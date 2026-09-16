"""universal-ctags wrapper.

Deliberately NOT tree-sitter: the retrieval side of realontext chunks with
tree-sitter, so ground truth must come from a different implementation or a
chunker bug would corrupt the answer and the response at the same time
(docs/benchmark.md -> 防自欺的硬规则 3).

Every tag this module returns has an exact [line, end]. ctags supplies it for
C, C++, Go, Java and Python; for Rust and TypeScript its parsers emit no `end`
at all, so the brace scanner below recovers it. A tag whose extent cannot be
established either way is dropped and counted — a fabricated boundary would
silently move ground truth.
"""
import json
import os
import re
import shutil
import subprocess

from .common import CTAGS_LANG, lang_of, log, qualified_name, sh

# ctags kind names that denote a callable, per language. Read off each parser's
# own output, not guessed: everything outside this table (struct members,
# interface and prototype declarations, fields) is not a function and must not
# reach the function-level ranking on either side.
FUNC_KINDS = {
    "c": {"function"},
    "cpp": {"function"},
    "go": {"func"},
    "java": {"method"},
    "python": {"function", "member"},
    "rust": {"function", "method"},
    "typescript": {"function", "method"},
}

# The TypeScript parser occasionally emits control-flow keywords as methods.
JUNK_NAMES = {"if", "else", "for", "while", "do", "switch", "case", "try",
              "catch", "finally", "return", "this", "super"}

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


# ---------------------------------------------------------------- brace scan
#
# Validated against the 44,498 C / Go / Java tags where ctags does report `end`:
# one disagreement, and every construct it cannot read exactly returns None
# rather than a guess.

_RE_LIT = re.compile(
    r"(?:^|[=(,:\[!&|?+;{}]\s*)(/(?:[^/\\\n\[]|\\.|\[(?:[^\]\\\n]|\\.)*\])+/[a-z]*)")


def _regex_braces(line):
    """A regex literal only threatens the brace count when it contains a brace."""
    for m in _RE_LIT.finditer(line):
        if "{" in m.group(1) or "}" in m.group(1):
            return True
    return False


def brace_end(lines, start_line, max_span=4000):
    """Line of the closing brace of the block opening at `start_line`, or None."""
    depth = 0
    started = False
    in_block = False
    for idx in range(start_line - 1, min(len(lines), start_line - 1 + max_span)):
        line = lines[idx]
        if ("{" in line or "}" in line) and _regex_braces(line):
            return None
        i, width = 0, len(line)
        while i < width:
            c = line[i]
            if in_block:
                if c == "*" and i + 1 < width and line[i + 1] == "/":
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if c == "/" and i + 1 < width:
                if line[i + 1] == "/":
                    break
                if line[i + 1] == "*":
                    in_block = True
                    i += 2
                    continue
            if c == "r" and i + 1 < width and line[i + 1] in '"#':
                return None                                  # rust raw string
            if c in "\"'`":
                if c == "'":
                    j = line.find("'", i + 1)
                    if j == -1 or j - i > 4:                 # rust lifetime, not a char
                        i += 1
                        continue
                    i = j + 1
                    continue
                j = i + 1
                while j < width:
                    if line[j] == "\\":
                        j += 2
                        continue
                    if line[j] == c:
                        break
                    j += 1
                if j >= width:
                    return None                              # multi-line string/template
                i = j + 1
                continue
            if c == ";" and not started:
                return None                                  # declaration, no body
            if c == "{":
                # Go signatures carry empty type literals (`struct{}`, `interface{}`)
                # ahead of the body brace; they are not the block being matched.
                if (not started and line[i:i + 2].replace(" ", "") == "{}"
                        and line[:i].rstrip().endswith(("struct", "interface"))):
                    i = line.index("}", i) + 1
                    continue
                depth += 1
                started = True
            elif c == "}":
                depth -= 1
                if started and depth == 0:
                    return idx + 1
                if depth < 0:
                    return None
            i += 1
    return None


# ---------------------------------------------------------------- scan

def _parse(stdout):
    out = []
    for line in stdout.splitlines():
        if not line.startswith("{"):
            continue
        try:
            t = json.loads(line)
        except ValueError:
            continue
        if t.get("_type") != "tag":
            continue
        path = t.get("path", "").removeprefix("./")
        lang = lang_of(path)
        if not path or lang is None:
            continue
        if t.get("kind") not in FUNC_KINDS.get(lang, ()):
            continue
        name = t.get("name", "")
        if not name or name.lower() in JUNK_NAMES:
            continue
        out.append({
            "path": path,
            "name": name,
            "scope": t.get("scope") or "",
            "kind": t.get("kind"),
            "line": int(t.get("line") or 0),
            "end": int(t.get("end") or 0),
        })
    return out


def scan(root, rel_paths=None, languages=None, timeout=1800):
    """Return tags as dicts: path, name, scope, kind, line, end, qname."""
    preflight()
    languages = languages or list(CTAGS_LANG)
    args = [ctags_bin(), "--output-format=json", "--fields=+neKzS",
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

    by_path = {}
    for t in _parse(p.stdout):
        by_path.setdefault(t["path"], []).append(t)

    tags, dropped = [], 0
    for path, group in sorted(by_path.items()):
        lines = None
        for t in group:
            if t["end"] < t["line"]:
                if lines is None:
                    lines = _read_lines(os.path.join(root, path))
                t["end"] = brace_end(lines, t["line"]) or 0
            if t["end"] < t["line"]:
                dropped += 1
                continue
            t["qname"] = qualified_name(path, t["scope"], t["name"])
            tags.append(t)
    if dropped:
        log("[symbols] %d tag(s) dropped: no exact extent (%.2f%% of %d)"
            % (dropped, 100.0 * dropped / max(1, dropped + len(tags)), dropped + len(tags)))
    return tags


def _read_lines(full):
    try:
        with open(full, encoding="utf-8", errors="replace") as f:
            return f.read().split("\n")
    except OSError:
        return []


# ---------------------------------------------------------------- lookup
#
# Ground truth walks single lines through `enclosing`; the baselines paint whole
# files through `line_owner`. Both order tags by the same key, so the two sides
# can never pick different names for the same position.

def _rank(t):
    return (t["end"] - t["line"], t["qname"])


def index_by_file(tags):
    idx = {}
    for t in tags:
        idx.setdefault(t["path"], []).append(t)
    for v in idx.values():
        v.sort(key=lambda t: (t["line"], t["end"], t["qname"]))
    return idx


def enclosing(tags_of_file, line):
    """Innermost tag whose [line, end] contains `line`."""
    best = None
    for t in tags_of_file:
        if t["line"] <= line <= t["end"] and (best is None or _rank(t) < _rank(best)):
            best = t
    return best


def line_owner(tags_of_file, nlines):
    """line number -> innermost enclosing qname, by the same rule as `enclosing`."""
    owner = [None] * (nlines + 2)
    for t in sorted(tags_of_file, key=_rank, reverse=True):
        lo = max(1, t["line"])
        hi = min(nlines, t["end"])
        for i in range(lo, hi + 1):
            owner[i] = t["qname"]
    return owner
