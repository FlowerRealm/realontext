#include "model/model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace model {

namespace {

constexpr int attempts = 8;
constexpr double backoff_cap = 60.0; // seconds

size_t collect(char* data, size_t size, size_t n, void* out)
{
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

size_t header(char* data, size_t size, size_t n, void* out)
{
    std::string_view line(data, size * n);
    constexpr std::string_view name = "retry-after:";
    if (line.size() > name.size() &&
        std::equal(name.begin(), name.end(), line.begin(), [](char a, char b) { return a == std::tolower(b); }))
        *static_cast<double*>(out) = std::atof(std::string(line.substr(name.size())).c_str());
    return size * n;
}

bool retryable(long status) { return status == 0 || status == 429 || status >= 500; }

// One retry policy for every endpoint: 429, 5xx and transport failures back off
// and retry, anything else is the provider saying no. The caller settles the
// token bucket once the response says what it really cost.
Result<Response> send(const Post& post, const Request& r, double estimate, Limiter& limiter, const Clock& clock)
{
    thread_local std::mt19937 rng{std::random_device{}()};
    Response last;
    for (int i = 0; i < attempts; i++) {
        limiter.acquire(estimate);
        last = post(r);
        if (last.status == 200)
            return last;
        if (!retryable(last.status))
            break;
        double cap = std::min(backoff_cap, std::ldexp(1.0, i));
        double delay = std::max(last.retry_after, std::uniform_real_distribution<>(0, cap)(rng));
        clock.sleep(std::chrono::duration<double>(delay));
    }
    return Err{"HTTP " + std::to_string(last.status) + ": " + last.body.substr(0, 300)};
}

} // namespace

Post http_post()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return [](const Request& r) {
        CURL* c = curl_easy_init();
        Response out{0, {}};
        curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
        if (!r.key.empty())
            headers = curl_slist_append(headers, ("Authorization: Bearer " + r.key).c_str());
        curl_easy_setopt(c, CURLOPT_URL, r.url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, r.body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out.body);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &out.retry_after);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        CURLcode rc = curl_easy_perform(c);
        if (rc == CURLE_OK)
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out.status);
        else
            out.body = curl_easy_strerror(rc);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        return out;
    };
}

Clock Clock::real()
{
    return {[] { return std::chrono::steady_clock::now(); },
            [](std::chrono::duration<double> d) { std::this_thread::sleep_for(d); }};
}

Limiter::Limiter(double rpm, double tpm, Clock clock)
    : requests_{rpm / 60, rpm}, tokens_{tpm / 60, tpm}, last_(clock.now()), clock_(std::move(clock))
{
}

void Limiter::refill()
{
    auto now = clock_.now();
    double secs = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    for (Bucket* b : {&requests_, &tokens_})
        b->level = std::min(b->rate * 60, b->level + b->rate * secs);
}

void Limiter::acquire(double tokens)
{
    double wait;
    {
        std::lock_guard lock(mu_);
        refill();
        requests_.level -= 1;
        tokens_.level -= std::min(tokens, tokens_.rate * 60); // one oversized request must still pass
        wait = std::max({0.0, -requests_.level / requests_.rate, -tokens_.level / tokens_.rate});
    }
    if (wait > 0)
        clock_.sleep(std::chrono::duration<double>(wait));
}

void Limiter::settle(double estimated, double actual)
{
    std::lock_guard lock(mu_);
    tokens_.level -= actual - estimated;
}

Result<Vectors> parse(const std::string& body, size_t n, uint32_t dim)
{
    auto doc = nlohmann::json::parse(body, nullptr, false);
    if (doc.is_discarded() || !doc.contains("data") || !doc["data"].is_array())
        return Err{"embeddings response without data: " + body.substr(0, 300)};
    const auto& data = doc["data"];
    if (data.size() != n)
        return Err{"embeddings response has " + std::to_string(data.size()) + " vectors for " + std::to_string(n) +
                   " inputs"};
    Vectors out{std::vector<float>(n * dim), 0};
    for (const auto& item : data) {
        size_t i = item.value("index", n);
        const auto& v = item["embedding"];
        if (i >= n || !v.is_array() || v.size() != dim)
            return Err{"embeddings response: bad vector at index " + std::to_string(i)};
        float* row = &out.data[i * dim];
        double norm = 0;
        for (uint32_t d = 0; d < dim; d++) {
            row[d] = v[d].get<float>();
            norm += double(row[d]) * row[d];
        }
        if (norm == 0)
            return Err{"embeddings response: zero vector at index " + std::to_string(i)};
        for (uint32_t d = 0; d < dim; d++)
            row[d] = static_cast<float>(row[d] / std::sqrt(norm));
    }
    out.tokens = doc.contains("usage") ? doc["usage"].value("total_tokens", uint64_t{0}) : 0;
    return out;
}

Result<Vectors> Embedder::embed(const std::vector<std::string>& texts, Role role)
{
    nlohmann::json input = nlohmann::json::array();
    double bytes = 0;
    for (const std::string& t : texts) {
        input.push_back(t);
        bytes += static_cast<double>(t.size());
    }
    nlohmann::json req = {{"model", config_.model},
                          {"task", config_.task + (role == Role::Query ? ".query" : ".passage")},
                          {"dimensions", config_.dim},
                          {"embedding_type", "float"},
                          {"input", std::move(input)}};
    Request r{config_.endpoint, key_, req.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)};
    double estimate = bytes / bytes_per_token;

    auto res = send(post_, r, estimate, limiter_, clock_);
    if (!res)
        return Err{"embeddings " + config_.endpoint + ": " + res.error()};
    auto v = parse(res->body, texts.size(), config_.dim);
    if (v && v->tokens)
        limiter_.settle(estimate, static_cast<double>(v->tokens));
    return v;
}

Result<std::vector<Relevance>> parse_rerank(const std::string& body, size_t n)
{
    auto doc = nlohmann::json::parse(body, nullptr, false);
    // Voyage calls the array "data", OpenRouter proxying it calls it "results".
    const char* field = doc.is_discarded() ? nullptr : doc.contains("results") ? "results" : "data";
    if (!field || !doc.contains(field) || !doc[field].is_array())
        return Err{"rerank response without results: " + body.substr(0, 300)};
    const auto& data = doc[field];
    if (data.size() != n)
        return Err{"rerank response scores " + std::to_string(data.size()) + " of " + std::to_string(n) +
                   " documents"};
    std::vector<Relevance> out;
    out.reserve(n);
    std::vector<bool> seen(n, false);
    for (const auto& item : data) {
        size_t i = item.value("index", n);
        if (i >= n || seen[i] || !item.contains("relevance_score"))
            return Err{"rerank response: bad entry at index " + std::to_string(i)};
        seen[i] = true;
        out.push_back({i, item["relevance_score"].get<double>()});
    }
    // The provider sorts, but the order is the whole product here, so it is
    // rebuilt from the scores rather than assumed.
    std::sort(out.begin(), out.end(), [](const Relevance& a, const Relevance& b) {
        return a.score != b.score ? a.score > b.score : a.index < b.index;
    });
    return out;
}

Result<std::vector<Relevance>> Reranker::rank(const std::string& query, const std::vector<std::string>& documents)
{
    if (documents.empty())
        return std::vector<Relevance>{};
    nlohmann::json docs = nlohmann::json::array();
    double bytes = 0;
    for (const std::string& d : documents) {
        docs.push_back(d);
        bytes += static_cast<double>(d.size());
    }
    nlohmann::json req = {{"model", config_.model},
                          {"query", query},
                          {"documents", std::move(docs)},
                          {"truncation", true}};
    Request r{config_.endpoint, key_, req.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)};
    // Billed as the query once per document plus the documents themselves, so
    // the token bucket counts what the bill counts.
    double estimate = (static_cast<double>(query.size()) * static_cast<double>(documents.size()) + bytes) /
                      bytes_per_token;

    auto res = send(post_, r, estimate, limiter_, clock_);
    if (!res)
        return Err{"rerank " + config_.endpoint + ": " + res.error()};
    auto doc = nlohmann::json::parse(res->body, nullptr, false);
    if (!doc.is_discarded() && doc.contains("usage"))
        limiter_.settle(estimate, static_cast<double>(doc["usage"].value("total_tokens", uint64_t{0})));
    return parse_rerank(res->body, documents.size());
}

} // namespace model
