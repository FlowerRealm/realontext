"""The freeze. A test set that can silently change is not a test set.

datasets/*.jsonl is the only thing allowed to feed scoring, and MANIFEST.json
pins its content hash. Regenerating is an explicit, recorded act — never a side
effect of running the evaluation.
"""
import datetime as dt
import hashlib
import json
import os
import subprocess

from .common import DATASETS, log, slug

PATH = os.path.join(DATASETS, "MANIFEST.json")


def file_hash(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def tool_versions():
    out = {}
    for name, args in (("git", ["git", "--version"]),
                       ("ripgrep", [os.environ.get("RG", "rg"), "--version"]),
                       ("ctags", [os.environ.get("CTAGS", "ctags"), "--version"])):
        try:
            p = subprocess.run(args, capture_output=True, text=True, timeout=30)
            out[name] = (p.stdout or p.stderr).splitlines()[0].strip()
        except Exception:
            out[name] = "unavailable"
    return out


def load():
    if not os.path.exists(PATH):
        return {"version": 1, "frozen": {}, "history": []}
    with open(PATH, encoding="utf-8") as f:
        return json.load(f)


def save(man):
    with open(PATH, "w", encoding="utf-8") as f:
        json.dump(man, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")


def dataset_file(repo):
    return os.path.join(DATASETS, slug(repo) + ".jsonl")


def freeze(repo, reason):
    """Record the current datasets/<repo>.jsonl as *the* test set for that repo."""
    path = dataset_file(repo)
    if not os.path.exists(path):
        raise SystemExit("no dataset for %s — run build first" % repo)
    man = load()
    digest = file_hash(path)
    rows = sum(1 for line in open(path, encoding="utf-8") if line.strip())
    prev = man["frozen"].get(repo)
    if prev and prev["sha256"] == digest:
        log("[freeze] %s unchanged (%s)" % (repo, digest[:12]))
        return man
    man["frozen"][repo] = {"sha256": digest, "queries": rows,
                           "frozen_at": _now(), "reason": reason,
                           "tools": tool_versions()}
    man["history"].append({"repo": repo, "at": _now(), "queries": rows,
                           "sha256": digest, "reason": reason,
                           "replaces": prev["sha256"] if prev else None})
    save(man)
    log("[freeze] %s -> %s (%d queries)" % (repo, digest[:12], rows))
    return man


def verify(repo, allow_drift=False):
    """Refuse to score against anything the manifest does not pin."""
    man = load()
    entry = man["frozen"].get(repo)
    path = dataset_file(repo)
    if not os.path.exists(path):
        raise SystemExit("no dataset for %s" % repo)
    if entry is None:
        msg = "%s is not frozen — run `freeze --repo %s --reason ...`" % (repo, repo)
        if not allow_drift:
            raise SystemExit(msg)
        log("[verify] WARNING " + msg)
        return None
    digest = file_hash(path)
    if digest != entry["sha256"]:
        msg = ("%s drifted from the manifest\n  frozen %s\n  actual %s\n"
               "Either restore the frozen file, or re-freeze with a written reason."
               % (repo, entry["sha256"][:16], digest[:16]))
        if not allow_drift:
            raise SystemExit(msg)
        log("[verify] WARNING " + msg)
    return entry


def dataset_version(repos):
    """One short hash standing for the exact set of frozen datasets scored."""
    man = load()
    parts = []
    for r in sorted(repos):
        e = man["frozen"].get(r)
        parts.append("%s:%s" % (r, e["sha256"] if e else "UNFROZEN"))
    return hashlib.sha256("|".join(parts).encode()).hexdigest()[:12]


def _now():
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
