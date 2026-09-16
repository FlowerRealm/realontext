"""realontext as the system under test.

Talks to the built binary only. Nothing here imports realontext code or shares
its tokenizer or chunker with the baselines (docs/benchmark.md 隔离规则); the
function names it scores are the ones realontext itself produced.

Every query's base commit is registered as a pseudo branch in one database per
repository (docs/modules/match.md), all of them before the first query runs:
IDF is corpus-wide, so indexing between queries would let query order move scores.
"""
import json
import os
import subprocess

from .common import CACHE, ROOT, ensure_mirror, log, mirror_path, slug

BIN = os.environ.get("REALONTEXT", os.path.join(os.path.dirname(ROOT), "build", "realontext"))
K = 50


def db_path(repo):
    d = os.path.join(CACHE, "realontext")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, slug(repo) + ".db")


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


def query(row):
    p = _run(["query", "--db", db_path(row["repo"]), "--branch", branch(row), "--k", str(K)],
             stdin=row["query"])
    if p.returncode != 0:
        raise RuntimeError("realontext query: " + p.stderr[-500:])
    out = json.loads(p.stdout)
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
