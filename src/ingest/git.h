#pragma once
#include <memory>
#include <string>
#include <vector>

#include "oid.h"
#include "result.h"

struct git_repository;

namespace ingest {

struct Ref {
    std::string name;
    Oid commit;
};

// A bare mirror. Read-only: bytes and metadata, never parsed content.
class Repo {
public:
    static Result<Repo> open(const std::string& path);

    // Every branch the mirror holds (D14: no freshness filter).
    Result<std::vector<Ref>> branches() const;
    Result<Oid> resolve(const std::string& rev) const;

    // The commit's tree, minus paths the filter rejects.
    Result<std::vector<FileVersion>> tree(const Oid& commit) const;

    // Blob content as indexable text. Oversized and binary blobs read as empty:
    // they produce no chunks, which is exactly what "not indexed" means downstream.
    Result<std::string> text(const Oid& blob) const;

private:
    struct Free {
        void operator()(git_repository* r) const;
    };
    std::unique_ptr<git_repository, Free> repo_;
};

} // namespace ingest
