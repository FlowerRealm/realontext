"""universal-ctags wrapper.

Deliberately NOT tree-sitter: the retrieval side of realontext chunks with
tree-sitter, so ground truth must come from a different implementation or a
chunker bug would corrupt the answer and the response at the same time
(docs/benchmark.md -> 防自欺的硬规则 3).
"""
import json
import os
import shutil
import subprocess

from .common import CTAGS_LANG, lang_of, log, normalize_symbol, qualified_name, sh

FUNC_KINDS = {"function", "method", "func", "procedure", "subroutine",
              "member", "prototype", "interface", "singletonMethod"}

_checked = [False]


def ctags_bin():
    exe = os.environ.get("CTAGS", "ctags")
    if not shutil.which(exe):
        raise SystemExit("universal-ctags not found (apt install universal-ctags)")
    return exe


def preflight():
    if _checked[0]:
        return
    exe = ctags_bin()
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout
    if "Universal Ctags" not in out:
        raise SystemExit("need Universal Ctags, found:\n" + out.splitlines()[0])
    p = subprocess.run([exe, "--output-format=json", "--version"],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit("this ctags lacks JSON output (needs libjansson)")
    _checked[0] = True


def _langs_arg(languages):
    names = sorted({CTAGS_LANG[l] for l in languages if l in CTAGS_LANG})
    return ",".join(names)


def scan(root, rel_paths=None, languages=None, timeout=1800):
    """Return tags as dicts: path, name, scope, kind, line, end, qname."""
    preflight()
    languages = languages or list(CTAGS_LANG)
    args = [ctags_bin(), "--output-format=json", "--fields=+neKzS", "--extras=+q",
            "--languages=" + _langs_arg(languages), "-f", "-"]
    if rel_paths is None:
        args += ["-R", "."]
        stdin = None
    else:
        args += ["-L", "-"]
        stdin = "\n".join(rel_paths) + "\n"
    p = sh(args, cwd=root, check=False, timeout=timeout, stdin=stdin)
    if p.returncode != 0 and not p.stdout:
        raise RuntimeError("ctags failed: " + (p.stderr or "")[-1000:])

    raw = []
    for line in p.stdout.splitlines():
        if not line.startswith("{"):
            continue
        try:
            t = json.loads(line)
        except ValueError:
            continue
        if t.get("_type") != "tag" or t.get("kind") not in FUNC_KINDS:
            continue
        path = t.get("path", "").lstrip("./")
        if not path or lang_of(path) is None:
            continue
        raw.append({
            "path": path,
            "name": t.get("name", ""),
            "scope": t.get("scope") or "",
            "kind": t.get("kind"),
            "line": int(t.get("line") or 0),
            "end": int(t.get("end") or 0),
        })

    # ctags does not emit `end` for every parser: fall back to "next tag in the
    # same file, minus one". Last tag in a file runs to EOF (represented as 0).
    by_file = {}
    for t in raw:
        by_file.setdefault(t["path"], []).append(t)
    tags = []
    for path, group in by_file.items():
        group.sort(key=lambda t: t["line"])
        for i, t in enumerate(group):
            if not t["end"]:
                t["end"] = (group[i + 1]["line"] - 1) if i + 1 < len(group) else 10 ** 9
            if t["end"] < t["line"]:
                t["end"] = t["line"]
            t["qname"] = qualified_name(path, t["scope"], t["name"])
            tags.append(t)
    return tags


def index_by_file(tags):
    idx = {}
    for t in tags:
        idx.setdefault(t["path"], []).append(t)
    for v in idx.values():
        v.sort(key=lambda t: (t["line"], t["end"]))
    return idx


def enclosing(tags_of_file, line):
    """Innermost tag whose [line, end] contains `line`."""
    best = None
    for t in tags_of_file:
        if t["line"] <= line <= t["end"]:
            if best is None or (t["end"] - t["line"]) < (best["end"] - best["line"]):
                best = t
    return best
