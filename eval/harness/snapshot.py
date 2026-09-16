"""Materialise a repository at one base commit, plus its symbol index.

Corpus = the base commit's tree, nothing later. Materialising is a plain
cat-file stream: no working tree, no checkout, so a huge repo costs the same
per snapshot as a medium one plus bytes.
"""
import json
import os
import shutil
import subprocess
import tarfile

from . import symbols
from .common import CACHE, ensure_mirror, is_code, log, mirror_path, slug

MAX_FILE_BYTES = 1 << 20            # 1 MB: past this it is data, not source


def materialize(repo, sha, dest):
    """Write every source blob of the tree into `dest`. Returns the path list.

    `git archive` streamed through tarfile, deliberately: on a blobless partial
    clone it resolves the whole tree in one bulk fetch, where `cat-file --batch`
    would issue one network round trip per missing blob and take hours.
    """
    os.makedirs(dest, exist_ok=True)
    ensure_mirror(repo)                       # also neutralises export-ignore
    proc = subprocess.Popen(
        ["git", "-C", mirror_path(repo), "archive", "--format=tar", sha],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    written = []
    try:
        with tarfile.open(fileobj=proc.stdout, mode="r|*") as tar:
            for member in tar:
                if not member.isfile():
                    continue
                path = member.name
                if not is_code(path) or member.size > MAX_FILE_BYTES:
                    continue
                src = tar.extractfile(member)
                if src is None:
                    continue
                full = os.path.join(dest, path)
                if not os.path.abspath(full).startswith(os.path.abspath(dest) + os.sep):
                    continue                       # never trust a path from a tar
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "wb") as f:
                    shutil.copyfileobj(src, f)
                written.append(path)
    finally:
        if proc.stdout:
            proc.stdout.close()
        err = proc.stderr.read().decode("utf-8", "replace")[-500:] if proc.stderr else ""
        proc.wait(timeout=300)
        if proc.returncode not in (0, None) and not written:
            raise RuntimeError("git archive failed for %s@%s: %s" % (repo, sha[:8], err))
    return written


def tags_cache_path(repo, sha):
    d = os.path.join(CACHE, "tags", slug(repo))
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, sha + ".json")


def build(repo, sha, dest, use_cache=True):
    """(paths, tags) for one snapshot. Symbol index is cached by commit sha."""
    paths = materialize(repo, sha, dest)
    cache = tags_cache_path(repo, sha)
    if use_cache and os.path.exists(cache):
        with open(cache, encoding="utf-8") as f:
            tags = json.load(f)
    else:
        tags = symbols.scan(dest)
        with open(cache, "w", encoding="utf-8") as f:
            json.dump(tags, f)
    log("[snapshot] %s@%s files=%d tags=%d" % (repo, sha[:8], len(paths), len(tags)))
    return paths, tags


class Snapshot(object):
    """Context manager so a 1.3 GB corpus never outlives the query that needed it."""

    def __init__(self, repo, sha, root=None, keep=False):
        self.repo, self.sha, self.keep = repo, sha, keep
        self.root = root or os.path.join(CACHE, "snap", slug(repo), sha[:12])
        self.paths, self.tags = [], []

    def __enter__(self):
        shutil.rmtree(self.root, ignore_errors=True)
        os.makedirs(self.root, exist_ok=True)
        self.paths, self.tags = build(self.repo, self.sha, self.root)
        self.by_file = symbols.index_by_file(self.tags)
        return self

    def __exit__(self, *exc):
        if not self.keep:
            shutil.rmtree(self.root, ignore_errors=True)
        return False
