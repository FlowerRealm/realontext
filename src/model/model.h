#pragma once
// Every call to an external model goes through here; no other module speaks
// HTTP (docs/modules/model.md).
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "result.h"

namespace model {

// Where embeddings come from. All five fields are the index fingerprint (D8, D22):
// vectors produced under different values live in different spaces.
struct Embedding {
    std::string endpoint; // POST target speaking the Jina embeddings API shape
    std::string model;
    std::string task;     // adapter; the role is appended: code -> code.query / code.passage
    uint32_t dim;
};

enum class Role { Query, Passage };

struct Request {
    std::string url;
    std::string key; // empty: no Authorization header
    std::string body;
};

struct Response {
    long status;      // 0: the request never got an HTTP answer
    std::string body; // response body, or the transport error
    double retry_after = 0; // seconds, from Retry-After; 0 when absent
};

using Post = std::function<Response(const Request&)>;

// libcurl, one easy handle per call.
Post http_post();

// Time, injectable so tests do not sleep.
struct Clock {
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::duration<double>)> sleep;
    static Clock real();
};

// Two token buckets, requests per minute and tokens per minute, each holding at
// most one minute of budget. acquire() reserves first and sleeps after, so
// concurrent callers queue up behind each other instead of racing.
class Limiter {
public:
    Limiter(double rpm, double tpm, Clock clock);
    void acquire(double tokens);
    // A request's real token count is known only from the response.
    void settle(double estimated, double actual);

private:
    struct Bucket {
        double rate, level;
    };
    void refill();
    std::mutex mu_;
    Bucket requests_, tokens_;
    std::chrono::steady_clock::time_point last_;
    Clock clock_;
};

struct Vectors {
    std::vector<float> data; // n × dim, each row L2-normalised
    uint64_t tokens;         // billed by the provider
};

// Parses an embeddings response for `n` inputs of `dim` dimensions.
Result<Vectors> parse(const std::string& body, size_t n, uint32_t dim);

class Embedder {
public:
    Embedder(Embedding config, std::string key, Post post, Limiter& limiter, Clock clock)
        : config_(std::move(config)), key_(std::move(key)), post_(std::move(post)), limiter_(limiter),
          clock_(std::move(clock)) {}

    // 429, 5xx and transport failures back off and retry; anything else, or
    // running out of attempts, is an error. A failed batch job loses nothing:
    // what was not written is still pending on the next run.
    Result<Vectors> embed(const std::vector<std::string>& texts, Role role);

private:
    Embedding config_;
    std::string key_;
    Post post_;
    Limiter& limiter_;
    Clock clock_;
};

// Where reranking happens. Not part of the index fingerprint: reranking stores
// nothing, so nothing can be left behind in the wrong space (D27).
struct Rerank {
    std::string endpoint; // POST target speaking the Voyage rerank API shape
    std::string model;
};

// One document's place in the reranked order: where it sat in the caller's
// list, and what the model thought of it.
struct Relevance {
    size_t index;
    double score;
};

// Parses a rerank response for `n` documents, best first. Every document must
// come back exactly once: a response that scores a subset would silently drop
// candidates from the ranking.
Result<std::vector<Relevance>> parse_rerank(const std::string& body, size_t n);

class Reranker {
public:
    Reranker(Rerank config, std::string key, Post post, Limiter& limiter, Clock clock)
        : config_(std::move(config)), key_(std::move(key)), post_(std::move(post)), limiter_(limiter),
          clock_(std::move(clock)) {}

    // Best first. A failure is an error, never a quiet fall back to the order
    // the caller came in with: that order and this one rank by different things,
    // and nobody downstream could tell which one they got (D27).
    Result<std::vector<Relevance>> rank(const std::string& query, const std::vector<std::string>& documents);

private:
    Rerank config_;
    std::string key_;
    Post post_;
    Limiter& limiter_;
    Clock clock_;
};

// Bytes per token assumed before the provider reports the real count. Low on
// purpose: overestimating only slows the limiter, underestimating earns 429s.
inline constexpr double bytes_per_token = 3.0;

} // namespace model
