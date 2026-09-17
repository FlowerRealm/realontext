"""realontext as the system under test.

Talks to the built binary only. Nothing here imports realontext code or shares
its tokenizer or chunker with the baselines (docs/benchmark.md 隔离规则); the
function names it scores are the ones realontext itself produced.

Every query's base commit is registered as a pseudo branch in one database per
repository (docs/modules/match.md), all of them before the first query runs:
IDF is corpus-wide, so indexing between queries would let query order move scores.

`realontext-vector` is the same database ranked by exact cosine over embedded
chunks (D22). Embedding runs after indexing; REALONTEXT_EMBED carries the embed
flags (endpoint, model, limits), REALONTEXT_DB_TAG keeps databases embedded
under different fingerprints apart.

`realontext-ann` is that same database through vector/'s index (D24), so the
gap between the two is what the approximation costs. REALONTEXT_ANN carries the
index flags; the same string reaches both build-index and query, which each
read the ones they know.
"""
import json
import os
import shlex
import subprocess

from .common import CACHE, ROOT, ensure_mirror, log, mirror_path, slug

BIN = os.environ.get("REALONTEXT", os.path.join(os.path.dirname(ROOT), "build", "realontext"))
K = 200


def db_path(repo):
    d = os.path.join(CACHE, "realontext")
    os.makedirs(d, exist_ok=True)
    tag = os.environ.get("REALONTEXT_DB_TAG")
    return os.path.join(d, slug(repo) + ("." + tag if tag else "") + ".db")


def branch(row):
    return "eval/%s/pr-%d" % (slug(row["repo"]), row["pr"])


def _run(args, stdin=None):
    return subprocess.run([BIN] + args, input=stdin, capture_output=True, text=True)


def _hydrate(repo, sha):
    """Blobless mirror: `git archive` pulls the whole tree's blobs in one fetch."""
    with open(os.devnull, "wb") as sink:
        subprocess.run(["git", "-C", mirror_path(repo), "archive", "--format=tar", sha],
                       stdout=sink, stderr=subprocess.DEVNULL, check=True)


def index(repo, rows):
    if not os.path.exists(BIN):
        raise SystemExit("realontext binary not found at %s (cmake --build build)" % BIN)
    ensure_mirror(repo)
    for i, row in enumerate(rows, 1):
        args = ["index", "--db", db_path(repo), "--git", mirror_path(repo),
                "--rev", row["base_sha"], "--as", branch(row)]
        p = _run(args)
        if p.returncode != 0 and "missing from a partial clone" in p.stderr:
            _hydrate(repo, row["base_sha"])
            p = _run(args)
        if p.returncode != 0:
            raise RuntimeError("realontext index pr=%d: %s" % (row["pr"], p.stderr[-500:]))
        log("[realontext] %s %d/%d %s" % (repo, i, len(rows), p.stderr.strip().splitlines()[0]))


def embed(repo):
    """Embeds whatever indexing left pending. Progress goes straight to stderr: it runs for a while."""
    args = ["embed", "--db", db_path(repo)] + shlex.split(os.environ.get("REALONTEXT_EMBED", ""))
    if subprocess.run([BIN] + args).returncode != 0:
        raise RuntimeError("realontext embed %s failed" % repo)


def build_index(repo):
    """Rebuilds the ANN index from the vectors already stored. Seconds, not hours."""
    args = ["build-index", "--db", db_path(repo)] + shlex.split(os.environ.get("REALONTEXT_ANN", ""))
    if subprocess.run([BIN] + args).returncode != 0:
        raise RuntimeError("realontext build-index %s failed" % repo)


def query(row, route="lexical"):
    extra = shlex.split(os.environ.get("REALONTEXT_ANN", "")) if route == "ann" else []
    p = _run(["query", "--db", db_path(row["repo"]), "--branch", branch(row), "--k", str(K),
              "--route", route] + extra, stdin=row["query"])
    if p.returncode != 0:
        raise RuntimeError("realontext query: " + p.stderr[-500:])
    # Ground truth is where the fix went: the code group. Tests rank apart (D23).
    out = json.loads(p.stdout)["code"]
    files = [f["path"] for f in out["files"]]
    funcs, seen = [], set()
    for c in out["chunks"]:
        if c["kind"] != "function" or not c["symbol"]:
            continue
        for loc in c["locations"]:
            q = "%s::%s" % (loc["path"], c["symbol"])
            if q not in seen:
                seen.add(q)
                funcs.append(q)
    return files, funcs


def function_names(row):
    p = _run(["symbols", "--db", db_path(row["repo"]), "--branch", branch(row)])
    if p.returncode != 0:
        raise RuntimeError("realontext symbols: " + p.stderr[-500:])
    return set(p.stdout.split("\n")) - {""}
