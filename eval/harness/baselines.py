"""The two free baselines. Frozen at stage 0; never tuned afterwards.

They exist to catch the one failure self-built evaluation cannot otherwise see:
scores going up because the test set got easier. Neither imports realontext code,
neither shares a tokenizer or an index with `lexical/`.
"""
import math
import os
import re
import subprocess
from collections import defaultdict

from .common import log
from .tokenize import IDENT_RE, bm25_terms, grep_terms

RG = os.environ.get("RG", "rg")

BM25_K1 = 0.9          # Pyserini / Anserini default, BEIR convention
BM25_B = 0.4
GREP_TOP_TERMS = 20
GREP_MIN_LEN = 4


# ---------------------------------------------------------------- shared scan

def _line_owner(path_tags, nlines):
    """line number -> innermost enclosing qname."""
    owner = [None] * (nlines + 2)
    for t in sorted(path_tags, key=lambda t: -(t["end"] - t["line"])):
        lo = max(1, t["line"])
        hi = min(nlines, t["end"])
        for i in range(lo, hi + 1):
            owner[i] = t["qname"]
    return owner


def _rg(root, patterns, extra):
    """One ripgrep invocation. Returns raw stdout lines."""
    pat = "\n".join(patterns) + "\n"
    args = [RG, "--no-messages", "--no-heading", "--fixed-strings",
            "--ignore-case", "--word-regexp", "--max-columns", "500",
            "-f", "-"] + extra + ["."]
    p = subprocess.run(args, cwd=root, input=pat, capture_output=True, text=True)
    if p.returncode not in (0, 1):
        raise RuntimeError("rg failed: " + (p.stderr or "")[-500:])
    return p.stdout.splitlines()


def candidate_files(root, terms):
    if not terms:
        return []
    out = []
    for line in _rg(root, terms, ["--files-with-matches"]):
        out.append(line.lstrip("./"))
    return out


def scan(root, paths, terms, by_file):
    """One pass over the files that can possibly score. Everything else is zero.

    Returns tf/df at file and function granularity plus the length statistics
    BM25 needs.
    """
    terms = set(terms)
    tf_file = defaultdict(lambda: defaultdict(int))
    tf_func = defaultdict(lambda: defaultdict(int))
    dl_file, dl_func = {}, defaultdict(int)
    tokens_seen = bytes_seen = 0

    for path in paths:
        full = os.path.join(root, path)
        try:
            with open(full, "rb") as f:
                raw = f.read()
        except OSError:
            continue
        text = raw.decode("utf-8", "replace")
        lines = text.split("\n")
        owner = _line_owner(by_file.get(path, []), len(lines))
        total = 0
        for i, line in enumerate(lines, 1):
            who = owner[i] if i < len(owner) else None
            for m in IDENT_RE.finditer(line):
                w = m.group(0).lower()
                total += 1
                if who:
                    dl_func[who] += 1
                if w in terms:
                    tf_file[w][path] += 1
                    if who:
                        tf_func[w][who] += 1
        dl_file[path] = total
        tokens_seen += total
        bytes_seen += len(raw)

    ratio = (bytes_seen / tokens_seen) if tokens_seen else 6.0
    return {"tf_file": tf_file, "tf_func": tf_func, "dl_file": dl_file,
            "dl_func": dl_func, "bytes_per_token": ratio}


# ---------------------------------------------------------------- grep

def grep_baseline(root, paths, query, by_file, stats=None, corpus_files=None):
    """1 取词 2 去噪 3 IDF 前 20 4 rg 5 Σ IDF，同词同文件只计一次。"""
    cands = grep_terms(query, GREP_MIN_LEN)
    if not cands:
        return [], []
    lows = sorted({c.lower() for c in cands})
    st = stats or scan(root, candidate_files(root, lows) or [], lows, by_file)
    n_docs = max(1, len(corpus_files if corpus_files is not None else paths))

    idf = {}
    for t in lows:
        df = len(st["tf_file"].get(t, {}))
        idf[t] = math.log(1.0 + (n_docs - df + 0.5) / (df + 0.5))
    chosen = sorted(lows, key=lambda t: (-idf[t], t))[:GREP_TOP_TERMS]
    if not chosen:
        return [], []

    file_terms = defaultdict(set)
    func_terms = defaultdict(set)
    matcher = [(t, re.compile(r"(?<![A-Za-z0-9_])" + re.escape(t) + r"(?![A-Za-z0-9_])",
                              re.IGNORECASE)) for t in chosen]
    for out in _rg(root, chosen, ["--line-number"]):
        head, _, text = out.partition(":")
        num, _, text = text.partition(":")
        if not num.isdigit():
            continue
        path = head.lstrip("./")
        line = int(num)
        hit = [t for t, rx in matcher if rx.search(text)]
        if not hit:
            continue
        file_terms[path].update(hit)
        tags = by_file.get(path)
        if tags:
            for t in tags:
                if t["line"] <= line <= t["end"]:
                    func_terms[t["qname"]].update(hit)

    files = sorted(file_terms, key=lambda p: (-sum(idf[t] for t in file_terms[p]), p))
    funcs = sorted(func_terms, key=lambda q: (-sum(idf[t] for t in func_terms[q]), q))
    return files, funcs


# ---------------------------------------------------------------- BM25

def _bm25(tf_map, dl_map, idf, avgdl):
    scores = defaultdict(float)
    for term, w in idf.items():
        for doc, tf in tf_map.get(term, {}).items():
            dl = dl_map.get(doc, 0) or 1
            denom = tf + BM25_K1 * (1.0 - BM25_B + BM25_B * dl / avgdl)
            scores[doc] += w * tf * (BM25_K1 + 1.0) / denom
    return sorted(scores, key=lambda d: (-scores[d], d))


def bm25_baseline(root, paths, query, by_file, stats=None, corpus_files=None):
    """File-level doc = whole file (SWE-bench convention). Function-level doc =
    the ctags function body. k1/b nailed to the Pyserini defaults."""
    qterms = bm25_terms(query)
    if not qterms:
        return [], []
    vocab = sorted(set(qterms))
    st = stats or scan(root, candidate_files(root, vocab) or [], vocab, by_file)

    n_docs = max(1, len(corpus_files if corpus_files is not None else paths))
    idf = {}
    for t in vocab:
        df = len(st["tf_file"].get(t, {}))
        if df:
            idf[t] = math.log(1.0 + (n_docs - df + 0.5) / (df + 0.5))
    if not idf:
        return [], []

    scanned_len = sum(st["dl_file"].values())
    scanned_files = max(1, len(st["dl_file"]))
    avgdl_file = scanned_len / scanned_files
    files = _bm25(st["tf_file"], st["dl_file"], idf, avgdl_file)

    n_func = max(1, len(st["dl_func"]))
    avgdl_func = max(1.0, sum(st["dl_func"].values()) / n_func)
    funcs = _bm25(st["tf_func"], st["dl_func"], idf, avgdl_func)
    return files, funcs
