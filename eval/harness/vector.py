"""Pure-vector baseline, no API.

Runs jina-embeddings-v2-base-code from local weights on CPU. This is NOT the
production model (D7 picks jina-embeddings-v4 through the API) — v4 is 3.8B and
will not move on a 2-core runner. So this number is a floor for the vector
route, not a prediction of production quality, and the report labels it as such.

Embeddings are cached by content hash, which is the same content-addressed
dedup the product relies on: base-commit snapshots of one repo overlap almost
completely, so the cache absorbs nearly all of the cost after the first query.
"""
import os
import sqlite3
import struct

from .common import CACHE, log, sha256, slug

MODEL_ID = os.environ.get("EVAL_EMBED_MODEL", "jinaai/jina-embeddings-v2-base-code")
MAX_SEQ = int(os.environ.get("EVAL_EMBED_MAXSEQ", "1024"))
BATCH = int(os.environ.get("EVAL_EMBED_BATCH", "16"))
POOL_SIZE = int(os.environ.get("EVAL_POOL_SIZE", "200"))

_model = [None]


def model():
    if _model[0] is None:
        from sentence_transformers import SentenceTransformer      # lazy: heavy
        log("[vector] loading %s (cpu)" % MODEL_ID)
        m = SentenceTransformer(MODEL_ID, trust_remote_code=True, device="cpu")
        m.max_seq_length = MAX_SEQ
        _model[0] = m
    return _model[0]


class Cache(object):
    def __init__(self, repo):
        d = os.path.join(CACHE, "emb")
        os.makedirs(d, exist_ok=True)
        self.db = sqlite3.connect(os.path.join(d, slug(repo) + ".sqlite"))
        self.db.execute("CREATE TABLE IF NOT EXISTS emb"
                        "(h TEXT PRIMARY KEY, model TEXT, v BLOB)")
        self.db.commit()

    def get_many(self, hashes):
        out = {}
        cur = self.db.cursor()
        for i in range(0, len(hashes), 500):
            chunk = hashes[i:i + 500]
            q = "SELECT h,v FROM emb WHERE model=? AND h IN (%s)" % ",".join("?" * len(chunk))
            for h, blob in cur.execute(q, [MODEL_ID] + chunk):
                out[h] = struct.unpack("<%df" % (len(blob) // 4), blob)
        return out

    def put_many(self, items):
        self.db.executemany(
            "INSERT OR REPLACE INTO emb(h,model,v) VALUES(?,?,?)",
            [(h, MODEL_ID, struct.pack("<%df" % len(v), *v)) for h, v in items])
        self.db.commit()


def function_texts(root, tags, paths=None, limit=None):
    """qname -> source text, read straight off the materialised snapshot."""
    want = set(paths) if paths else None
    by_file = {}
    for t in tags:
        if want and t["path"] not in want:
            continue
        by_file.setdefault(t["path"], []).append(t)
    out = {}
    for path, group in by_file.items():
        full = os.path.join(root, path)
        try:
            with open(full, encoding="utf-8", errors="replace") as f:
                lines = f.read().split("\n")
        except OSError:
            continue
        for t in group:
            lo = max(1, t["line"]) - 1
            hi = min(len(lines), t["end"])
            body = "\n".join(lines[lo:hi]).strip()
            if body:
                out[t["qname"]] = body
            if limit and len(out) >= limit:
                return out
    return out


def embed(texts, cache):
    """Returns list of vectors aligned with `texts`, filling the cache as it goes."""
    hashes = [sha256(t) for t in texts]
    have = cache.get_many(list(dict.fromkeys(hashes)))
    todo = [(h, t) for h, t in zip(hashes, texts) if h not in have]
    todo = list({h: t for h, t in todo}.items())
    if todo:
        log("[vector] embedding %d new chunks (%d cached)" % (len(todo), len(have)))
        vecs = model().encode([t for _, t in todo], batch_size=BATCH,
                              normalize_embeddings=True, show_progress_bar=False)
        new = [(h, [float(x) for x in v]) for (h, _), v in zip(todo, vecs)]
        cache.put_many(new)
        have.update(dict(new))
    return [have[h] for h in hashes]


def _cos(a, b):
    return sum(x * y for x, y in zip(a, b))


def vector_baseline(root, tags, query, repo, pool=None, limit_chunks=None):
    """No rerank, no curation. `pool` restricts candidates to a shortlist, which is
    what makes huge repos tractable on a 2-core runner; the caller reports the
    pool's own recall ceiling alongside the score."""
    texts = function_texts(root, tags, paths=None, limit=limit_chunks)
    if pool:
        texts = {q: t for q, t in texts.items() if q in set(pool)}
    if not texts:
        return [], []
    cache = Cache(repo)
    names = list(texts)
    vecs = embed([texts[n] for n in names], cache)
    qv = embed([query], cache)[0]
    scored = sorted(zip(names, (_cos(qv, v) for v in vecs)),
                    key=lambda kv: (-kv[1], kv[0]))
    funcs = [n for n, _ in scored]

    seen, files = set(), []
    for q in funcs:
        p = q.split("::")[0]
        if p not in seen:
            seen.add(p)
            files.append(p)
    return files, funcs
