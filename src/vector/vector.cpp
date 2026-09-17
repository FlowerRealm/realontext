#include "vector/vector.h"

#include <filesystem>
#include <utility>

#include <usearch/index_dense.hpp>

#include "store/embed.h"

namespace vector {

namespace {

using unum::usearch::index_dense_t;
using unum::usearch::index_dense_config_t;
using unum::usearch::index_limits_t;
using unum::usearch::metric_kind_t;
using unum::usearch::metric_punned_t;
using unum::usearch::scalar_kind_t;

// Cosine, for every index precision. The stored vectors are already L2
// normalized (D22), so inner product would rank identically and cost less per
// comparison — but on an integer index it returns a dot product scaled by the
// quantization, which is no longer comparable with an exact scan's score. One
// metric keeps one scale.
constexpr metric_kind_t metric = metric_kind_t::cos_k;

// What the index was built from, so a stale one is an error rather than a
// silently short answer.
constexpr std::string_view built_chunks = "vector.chunks";
constexpr std::string_view built_last = "vector.last";

Result<scalar_kind_t> precision(const std::string& name)
{
    auto parsed = unum::usearch::scalar_kind_from_name(name.c_str());
    if (!parsed)
        return Err{"index precision '" + name + "': " + parsed.error.what()};
    return parsed.result;
}

} // namespace

struct Index::Impl {
    index_dense_t index;
};

Index::Index(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Index::~Index() = default;
Index::Index(Index&&) noexcept = default;
Index& Index::operator=(Index&&) noexcept = default;

std::string index_path(std::string_view db_path)
{
    return std::string(db_path) + ".usearch";
}

Result<BuildStats> build(store::Db& db, uint32_t dim, const Params& params, std::string_view db_path,
                         const std::function<void(size_t added, size_t total)>& progress)
{
    auto scalar = precision(params.scalar);
    if (!scalar)
        return Err{scalar.error()};
    auto covers = store::embedded(db);
    if (!covers)
        return Err{covers.error()};
    if (covers->chunks == 0)
        return Err{"no chunk has an embedding yet: run realontext embed"};

    auto made = index_dense_t::make(metric_punned_t(dim, metric, *scalar),
                                    index_dense_config_t(params.connectivity, params.expansion_add,
                                                         params.expansion_search));
    if (!made)
        return Err{std::string("usearch: ") + made.error.what()};
    index_dense_t index = std::move(made.index);
    if (!index.try_reserve(index_limits_t(covers->chunks)))
        return Err{"could not reserve room for " + std::to_string(covers->chunks) + " vectors"};

    std::string failure;
    size_t added = 0;
    auto scan = store::each_vector(db, dim, [&](uint32_t chunk, std::span<const float> v) {
        if (!failure.empty())
            return;
        // The chunk id is the key: content-addressed storage already numbers
        // chunks uniquely, so there is no side table to keep in step (D24).
        if (auto r = index.add(chunk, v.data()); !r) {
            failure = std::string("usearch add chunk ") + std::to_string(chunk) + ": " + r.error.what();
            return;
        }
        progress(++added, covers->chunks);
    });
    if (!scan)
        return Err{scan.error()};
    if (!failure.empty())
        return Err{failure};

    const std::string path = index_path(db_path);
    if (auto saved = index.save(path.c_str()); !saved)
        return Err{"writing " + path + ": " + saved.error.what()};
    // Recorded after the file lands: a crash mid-save leaves the old counts, and
    // the next open reports a stale index instead of trusting a half-written one.
    if (auto s = store::set_meta_int(db, built_chunks, static_cast<int64_t>(covers->chunks)); !s)
        return Err{s.error()};
    if (auto s = store::set_meta_int(db, built_last, covers->last); !s)
        return Err{s.error()};

    std::error_code ec;
    return BuildStats{added, std::filesystem::file_size(path, ec)};
}

Result<Index> Index::open(store::Db& db, std::string_view db_path, size_t expansion_search)
{
    auto chunks = store::meta_int(db, built_chunks);
    if (!chunks)
        return Err{chunks.error()};
    auto last = store::meta_int(db, built_last);
    if (!last)
        return Err{last.error()};
    const std::string path = index_path(db_path);
    if (*chunks == 0)
        return Err{"no vector index has been built: run realontext build-index"};

    auto covers = store::embedded(db);
    if (!covers)
        return Err{covers.error()};
    if (covers->chunks != static_cast<size_t>(*chunks) || covers->last != static_cast<uint32_t>(*last))
        return Err{path + " was built from " + std::to_string(*chunks) + " vectors, the database now holds " +
                   std::to_string(covers->chunks) + ": rerun realontext build-index"};

    if (!std::filesystem::exists(path))
        return Err{path + " is gone: rerun realontext build-index"};
    auto made = index_dense_t::make(path.c_str());
    if (!made)
        return Err{"reading " + path + ": " + made.error.what()};
    auto impl = std::make_unique<Impl>(Impl{std::move(made.index)});
    impl->index.change_expansion_search(expansion_search);

    auto pinned = store::pinned(db);
    if (!pinned)
        return Err{pinned.error()};
    if (impl->index.dimensions() != pinned->embedding.dim)
        return Err{path + " holds " + std::to_string(impl->index.dimensions()) + "-dimensional vectors, the database " +
                   std::to_string(pinned->embedding.dim) + ": rerun realontext build-index"};
    return Index(std::move(impl));
}

Result<std::vector<Hit>> Index::search(std::span<const float> query, size_t k) const
{
    if (query.size() != impl_->index.dimensions())
        return Err{"query has " + std::to_string(query.size()) + " dimensions, the index " +
                   std::to_string(impl_->index.dimensions())};
    auto found = impl_->index.search(query.data(), k);
    if (!found)
        return Err{std::string("usearch search: ") + found.error.what()};

    std::vector<Hit> hits;
    hits.reserve(found.size());
    for (size_t i = 0; i < found.size(); i++) {
        auto match = found[i];
        hits.push_back({static_cast<uint32_t>(match.member.key), 1.0f - static_cast<float>(match.distance)});
    }
    return hits;
}

size_t Index::size() const
{
    return impl_->index.size();
}

uint32_t Index::dim() const
{
    return static_cast<uint32_t>(impl_->index.dimensions());
}

} // namespace vector
