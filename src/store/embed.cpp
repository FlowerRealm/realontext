#include "store/embed.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace store {

namespace {

template <class T>
class Channel {
public:
    void push(T v)
    {
        {
            std::lock_guard lock(mu_);
            items_.push_back(std::move(v));
        }
        cv_.notify_one();
    }
    // Blocks until an item arrives; nullopt once closed and empty.
    std::optional<T> pop()
    {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return !items_.empty() || closed_; });
        if (items_.empty())
            return std::nullopt;
        T v = std::move(items_.front());
        items_.pop_front();
        return v;
    }
    void close()
    {
        {
            std::lock_guard lock(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<T> items_;
    bool closed_ = false;
};

using Fields = std::vector<std::pair<std::string, std::string>>;

Fields fields(const Fingerprint& fp)
{
    return {{"embed.endpoint", fp.embedding.endpoint},
            {"embed.model", fp.embedding.model},
            {"embed.task", fp.embedding.task},
            {"embed.dim", std::to_string(fp.embedding.dim)},
            {"embed.max_bytes", std::to_string(fp.max_bytes)},
            {"embed.input_version", std::to_string(input_version)}};
}

Result<std::map<std::string, std::string>> stored(Db& db)
{
    auto s = Stmt::prepare(db.handle(), "SELECT key, value FROM meta WHERE key LIKE 'embed.%'");
    if (!s)
        return Err{s.error()};
    std::map<std::string, std::string> out;
    int rc;
    while ((rc = s->step()) == SQLITE_ROW)
        out.emplace(s->text(0), s->text(1));
    if (rc != SQLITE_DONE)
        return Err{"meta: " + s->error()};
    return out;
}

} // namespace

std::string embed_text(std::string_view symbol, std::string_view content, size_t max_bytes)
{
    std::string text;
    text.reserve(symbol.size() + 1 + content.size());
    text.append(symbol).append("\n").append(content);
    if (text.size() > max_bytes) {
        size_t n = max_bytes;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) // never split a UTF-8 sequence
            n--;
        text.resize(n);
    }
    return text;
}

Status pin(Db& db, const Fingerprint& want)
{
    auto have = stored(db);
    if (!have)
        return Err{have.error()};
    Fields w = fields(want);
    // The fingerprint describes stored vectors. With none stored there is nothing
    // to protect, and a mistyped endpoint must not lock the database.
    auto count = Stmt::prepare(db.handle(), "SELECT (SELECT count(*) FROM chunks) - "
                                            "(SELECT count(*) FROM chunks WHERE embedding IS NULL)");
    if (!count)
        return Err{count.error()};
    if (count->step() != SQLITE_ROW)
        return Err{"count vectors: " + count->error()};
    if (count->int64(0) == 0) {
        count->reset();
        auto s = Stmt::prepare(db.handle(), "INSERT INTO meta (key, value) VALUES (?1, ?2) "
                                            "ON CONFLICT (key) DO UPDATE SET value = excluded.value");
        if (!s)
            return Err{s.error()};
        Transaction tx(db.handle());
        if (auto b = tx.begin(); !b)
            return b;
        for (const auto& [key, value] : w) {
            s->reset();
            s->bind(1, key).bind(2, value);
            if (s->step() != SQLITE_DONE)
                return Err{"meta: " + s->error()};
        }
        return tx.commit();
    }
    for (const auto& [key, value] : w) {
        auto it = have->find(key);
        std::string recorded = it == have->end() ? "(none)" : it->second;
        if (recorded != value)
            return Err{key + " is " + recorded + " in this database, " + value +
                       " requested: vectors from different sources do not mix (D8). Embed into a new database"};
    }
    return ok;
}

Result<Fingerprint> pinned(Db& db)
{
    auto have = stored(db);
    if (!have)
        return Err{have.error()};
    if (have->empty())
        return Err{"nothing embedded in this database: run realontext embed first"};
    auto get = [&](const char* key) { return (*have)[key]; };
    return Fingerprint{{get("embed.endpoint"), get("embed.model"), get("embed.task"),
                        static_cast<uint32_t>(std::stoul(get("embed.dim")))},
                       std::stoul(get("embed.max_bytes"))};
}

Result<Pending> pending(Db& db, size_t max_bytes)
{
    auto s = Stmt::prepare(db.handle(), "SELECT symbol_path, content FROM chunks WHERE embedding IS NULL");
    if (!s)
        return Err{s.error()};
    Pending out;
    int rc;
    while ((rc = s->step()) == SQLITE_ROW) {
        out.chunks++;
        out.bytes += embed_text(s->text(0), s->text(1), max_bytes).size();
    }
    if (rc != SQLITE_DONE)
        return Err{"pending: " + s->error()};
    return out;
}

Result<size_t> unembedded(Db& db)
{
    auto s = Stmt::prepare(db.handle(), "SELECT count(*) FROM chunks WHERE embedding IS NULL");
    if (!s)
        return Err{s.error()};
    if (s->step() != SQLITE_ROW)
        return Err{"unembedded: " + s->error()};
    return static_cast<size_t>(s->int64(0));
}

Result<Embedded> embedded(Db& db)
{
    auto s = Stmt::prepare(db.handle(), "SELECT count(*), coalesce(max(ord), 0) FROM chunks WHERE embedding IS NOT NULL");
    if (!s)
        return Err{s.error()};
    if (s->step() != SQLITE_ROW)
        return Err{"embedded: " + s->error()};
    return Embedded{static_cast<size_t>(s->int64(0)), static_cast<uint32_t>(s->int64(1))};
}

Result<EmbedProgress> embed(Db& db, const Fingerprint& fp, model::Embedder& embedder, const EmbedOptions& opt,
                            const std::function<void(const EmbedProgress&, size_t total)>& progress)
{
    if (auto s = pin(db, fp); !s)
        return Err{s.error()};
    auto queue = Stmt::prepare(db.handle(), "SELECT ord FROM chunks WHERE embedding IS NULL ORDER BY ord");
    auto read = Stmt::prepare(db.handle(), "SELECT symbol_path, content FROM chunks WHERE ord = ?1");
    auto write = Stmt::prepare(db.handle(), "UPDATE chunks SET embedding = ?1 WHERE ord = ?2");
    for (const std::string* e : {&queue.error(), &read.error(), &write.error()})
        if (!e->empty())
            return Err{*e};
    std::vector<uint32_t> ords;
    while (queue->step() == SQLITE_ROW)
        ords.push_back(static_cast<uint32_t>(queue->int64(0)));
    queue->reset();

    struct Work {
        std::vector<uint32_t> ords;
        std::vector<std::string> texts;
    };
    struct Done {
        std::vector<uint32_t> ords;
        Result<model::Vectors> vectors;
    };
    Channel<Work> work;
    Channel<Done> done;
    std::vector<std::jthread> workers;
    for (size_t i = 0; i < opt.jobs; i++)
        workers.emplace_back([&] {
            while (auto w = work.pop())
                done.push(Done{std::move(w->ords), embedder.embed(w->texts, model::Role::Passage)});
        });
    struct Close {
        Channel<Work>& c;
        ~Close() { c.close(); } // before the workers join, on every return
    } close{work};

    EmbedProgress prog;
    const size_t dim = fp.embedding.dim;
    size_t next = 0, inflight = 0;
    std::string failed;
    while (inflight > 0 || (failed.empty() && next < ords.size())) {
        while (failed.empty() && inflight < opt.jobs && next < ords.size()) {
            Work w;
            size_t bytes = 0;
            while (next < ords.size() && (w.ords.empty() || bytes < opt.batch_bytes)) {
                read->reset();
                read->bind(1, static_cast<int64_t>(ords[next]));
                if (read->step() != SQLITE_ROW)
                    return Err{"read chunk " + std::to_string(ords[next]) + ": " + read->error()};
                w.texts.push_back(embed_text(read->text(0), read->text(1), fp.max_bytes));
                w.ords.push_back(ords[next++]);
                bytes += w.texts.back().size();
            }
            read->reset();
            work.push(std::move(w));
            inflight++;
        }
        auto d = done.pop();
        inflight--;
        if (!d->vectors) {
            if (failed.empty())
                failed = d->vectors.error();
            continue; // keep writing what is already paid for
        }
        Transaction tx(db.handle());
        if (auto s = tx.begin(); !s)
            return Err{s.error()};
        for (size_t i = 0; i < d->ords.size(); i++) {
            write->reset();
            write->bind_blob(1, reinterpret_cast<const unsigned char*>(&d->vectors->data[i * dim]), dim * sizeof(float))
                .bind(2, static_cast<int64_t>(d->ords[i]));
            if (write->step() != SQLITE_DONE)
                return Err{"write embedding: " + write->error()};
        }
        write->reset();
        if (auto s = tx.commit(); !s)
            return Err{s.error()};
        prog.chunks += d->ords.size();
        prog.tokens += d->vectors->tokens;
        progress(prog, ords.size());
    }
    if (!failed.empty())
        return Err{failed + " (" + std::to_string(prog.chunks) + " chunks embedded before stopping; run again to resume)"};
    return prog;
}

Status each_vector(Db& db, uint32_t dim, const std::function<void(uint32_t chunk, std::span<const float>)>& visit)
{
    auto s = Stmt::prepare(db.handle(), "SELECT ord, embedding FROM chunks WHERE embedding IS NOT NULL");
    if (!s)
        return Err{s.error()};
    std::vector<float> row(dim); // blob memory is not float-aligned
    int rc;
    while ((rc = s->step()) == SQLITE_ROW) {
        std::string_view b = s->blob(1);
        if (b.size() != dim * sizeof(float))
            return Err{"chunk " + std::to_string(s->int64(0)) + " has a " + std::to_string(b.size()) +
                       "-byte embedding, expected " + std::to_string(dim) + " dimensions"};
        std::memcpy(row.data(), b.data(), b.size());
        visit(static_cast<uint32_t>(s->int64(0)), row);
    }
    if (rc != SQLITE_DONE)
        return Err{"vectors: " + s->error()};
    return ok;
}

} // namespace store
