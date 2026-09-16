#include "ingest/git.h"

#include <cstring>

#include <git2.h>

#include "ingest/filter.h"

namespace ingest {

namespace {

Err libgit2_error(const std::string& what)
{
    const git_error* e = git_error_last();
    return Err{what + ": " + (e && e->message ? e->message : "unknown libgit2 error")};
}

Oid to_oid(const git_oid* id)
{
    Oid out;
    std::memcpy(out.data(), id->id, out.size());
    return out;
}

git_oid from_oid(const Oid& id)
{
    git_oid out;
    std::memcpy(out.id, id.data(), id.size());
    return out;
}

void init_once()
{
    static const bool done = [] {
        git_libgit2_init();
        // Eval mirrors are blobless partial clones. libgit2 cannot fetch missing
        // objects, but it can read what is present once the extension is allowed;
        // a blob that was never fetched fails loudly in text().
        const char* extensions[] = {"partialclone"};
        git_libgit2_opts(GIT_OPT_SET_EXTENSIONS, extensions, 1);
        return true;
    }();
    (void)done;
}

struct TreeWalk {
    std::vector<FileVersion>* out;
};

int collect(const char* root, const git_tree_entry* entry, void* payload)
{
    if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB || git_tree_entry_filemode(entry) == GIT_FILEMODE_LINK)
        return 0;
    std::string path = std::string(root) + git_tree_entry_name(entry);
    if (indexable(path))
        static_cast<TreeWalk*>(payload)->out->push_back({std::move(path), to_oid(git_tree_entry_id(entry))});
    return 0;
}

} // namespace

void Repo::Free::operator()(git_repository* r) const { git_repository_free(r); }

Result<Repo> Repo::open(const std::string& path)
{
    init_once();
    git_repository* raw = nullptr;
    if (git_repository_open_bare(&raw, path.c_str()) != 0)
        return libgit2_error("open " + path);
    Repo repo;
    repo.repo_.reset(raw);
    return repo;
}

Result<std::vector<Ref>> Repo::branches() const
{
    git_reference_iterator* it = nullptr;
    if (git_reference_iterator_glob_new(&it, repo_.get(), "refs/heads/*") != 0)
        return libgit2_error("list branches");
    std::vector<Ref> out;
    git_reference* ref = nullptr;
    while (git_reference_next(&ref, it) == 0) {
        git_object* obj = nullptr;
        if (git_reference_peel(&obj, ref, GIT_OBJECT_COMMIT) == 0) {
            out.push_back({git_reference_shorthand(ref), to_oid(git_object_id(obj))});
            git_object_free(obj);
        }
        git_reference_free(ref);
    }
    git_reference_iterator_free(it);
    return out;
}

Result<Oid> Repo::resolve(const std::string& rev) const
{
    git_object* obj = nullptr;
    if (git_revparse_single(&obj, repo_.get(), (rev + "^{commit}").c_str()) != 0)
        return libgit2_error("resolve " + rev);
    Oid id = to_oid(git_object_id(obj));
    git_object_free(obj);
    return id;
}

Result<std::vector<FileVersion>> Repo::tree(const Oid& commit) const
{
    git_oid id = from_oid(commit);
    git_commit* c = nullptr;
    if (git_commit_lookup(&c, repo_.get(), &id) != 0)
        return libgit2_error("commit " + hex(commit));
    git_tree* t = nullptr;
    int rc = git_commit_tree(&t, c);
    git_commit_free(c);
    if (rc != 0)
        return libgit2_error("tree of " + hex(commit));

    std::vector<FileVersion> files;
    TreeWalk walk{&files};
    rc = git_tree_walk(t, GIT_TREEWALK_PRE, collect, &walk);
    git_tree_free(t);
    if (rc != 0)
        return libgit2_error("walk " + hex(commit));
    return files;
}

Result<std::string> Repo::text(const Oid& blob) const
{
    git_odb* odb = nullptr;
    if (git_repository_odb(&odb, repo_.get()) != 0)
        return libgit2_error("odb");
    git_oid id = from_oid(blob);
    size_t size = 0;
    git_object_t type = GIT_OBJECT_INVALID;
    int rc = git_odb_read_header(&size, &type, odb, &id);
    git_odb_free(odb);
    if (rc != 0)
        return libgit2_error("blob " + hex(blob) + " (missing from a partial clone?)");
    if (size > max_blob_bytes)
        return std::string();

    git_blob* b = nullptr;
    if (git_blob_lookup(&b, repo_.get(), &id) != 0)
        return libgit2_error("blob " + hex(blob));
    std::string content(static_cast<const char*>(git_blob_rawcontent(b)), git_blob_rawsize(b));
    git_blob_free(b);
    if (!is_text(content))
        return std::string();
    return content;
}

} // namespace ingest
