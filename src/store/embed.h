#pragma once
// The paid half of indexing: send pending chunks to model/, store the vectors.
// Separate from index_branch because it costs money and runs for hours; the
// queue is the chunks table itself, so an interrupted run resumes by running again.
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "model/model.h"
#include "result.h"
#include "store/store.h"

namespace store {

// Embedding source plus how chunks become input text. The index fingerprint (D8, D22).
struct Fingerprint {
    model::Embedding embedding;
    size_t max_bytes; // input is cut here, on a UTF-8 boundary
};

// The text embedded for a chunk. No path: a chunk is shared by every path that
// holds its content, and its vector must be too.
std::string embed_text(std::string_view symbol, std::string_view content, size_t max_bytes);

// Records `want` as the database's fingerprint while no vector is stored;
// afterwards checks it against the recorded one. Any difference is an error
// naming the field.
Status pin(Db& db, const Fingerprint& want);

// The recorded fingerprint; an error when nothing has been embedded yet.
Result<Fingerprint> pinned(Db& db);

struct Pending {
    size_t chunks = 0;
    size_t bytes = 0; // input bytes after truncation
};
Result<Pending> pending(Db& db, size_t max_bytes);

// Chunks without a vector. Nonzero means a vector ranking would silently skip them.
Result<size_t> unembedded(Db& db);

struct EmbedOptions {
    size_t batch_bytes; // one request carries at least this much input, unless the queue runs out
    size_t jobs;        // concurrent requests
};

struct EmbedProgress {
    size_t chunks = 0;
    uint64_t tokens = 0; // as billed
};

// Embeds every pending chunk under the pinned fingerprint. Each response is
// written in its own transaction, so a crash loses only requests in flight.
Result<EmbedProgress> embed(Db& db, const Fingerprint& fp, model::Embedder& embedder, const EmbedOptions& opt,
                            const std::function<void(const EmbedProgress&, size_t total)>& progress);

// Calls `visit` with every stored vector.
Status each_vector(Db& db, uint32_t dim, const std::function<void(uint32_t chunk, std::span<const float>)>& visit);

} // namespace store
