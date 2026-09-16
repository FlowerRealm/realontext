"""Shared helpers: repo list, paths, git, jsonl, qualified-name normalisation."""
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))          # eval/
CACHE = os.environ.get("EVAL_CACHE", os.path.join(ROOT, ".cache"))
RAW = os.path.join(ROOT, "raw")
DATASETS = os.path.join(ROOT, "datasets")
RESULTS = os.path.join(ROOT, "results")
MIRRORS = os.path.join(CACHE, "mirrors")

for _d in (CACHE, RAW, DATASETS, RESULTS, MIRRORS):
    os.makedirs(_d, exist_ok=True)


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def sh(args, cwd=None, check=True, text=True, timeout=None, stdin=None):
    p = subprocess.run(args, cwd=cwd, capture_output=True, text=text,
                       timeout=timeout, input=stdin)
    if check and p.returncode != 0:
        raise RuntimeError("cmd failed %s\n%s" % (args[:4], (p.stderr or "")[-2000:]))
    return p


def read_jsonl(path):
    if not os.path.exists(path):
        return []
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def write_jsonl(path, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False, sort_keys=True) + "\n")


def slug(repo):
    return repo.replace("/", "__")


def sha256(text):
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


# ---------------------------------------------------------------- repos.txt

def parse_repos(path=None):
    path = path or os.path.join(ROOT, "repos.txt")
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 4:
                raise ValueError("bad repos.txt line: %r" % line)
            repo, lang, size, split = parts
            rows.append({"repo": repo, "language": lang, "size": size, "split": split})
    return rows


def repo_meta(repo):
    for r in parse_repos():
        if r["repo"] == repo:
            return r
    raise KeyError(repo)


# ---------------------------------------------------------------- languages

EXT_LANG = {
    ".c": "c", ".h": "c",
    ".cc": "cpp", ".cpp": "cpp", ".cxx": "cpp", ".hpp": "cpp", ".hh": "cpp", ".hxx": "cpp",
    ".go": "go",
    ".rs": "rust",
    ".ts": "typescript", ".tsx": "typescript", ".js": "typescript", ".mjs": "typescript",
    ".py": "python",
    ".java": "java",
}

# ctags' own language names, for --language-force
CTAGS_LANG = {
    "c": "C", "cpp": "C++", "go": "Go", "rust": "Rust",
    "typescript": "TypeScript", "python": "Python", "java": "Java",
}


def lang_of(path):
    _, ext = os.path.splitext(path)
    return EXT_LANG.get(ext.lower())


# ---------------------------------------------------------------- path filters

VENDOR_RE = re.compile(
    r"(^|/)(vendor|third_party|thirdparty|node_modules|deps|external|"
    r"generated|gen|dist|build|target|\.git)(/|$)|^contrib(/|$)")
GENERATED_RE = re.compile(r"(\.pb\.go|_pb2\.py|\.generated\.[a-z]+|\.min\.js|"
                          r"\.g\.dart|_generated\.[a-z]+)$|(^|/)zz_generated[^/]*$")
TEST_RE = re.compile(
    r"(^|/)(tests?|testing|testsuite|spec|specs|__tests__|testdata|fixtures|e2e|"
    r"benchmarks?|asv_bench)(/|$)"
    r"|(^|/)tests?[._-][^/]+$|_test\.[a-z]+$|\.test\.[a-z]+$|\.spec\.[a-z]+$"
    r"|(^|/)Test[A-Z][^/]*\.java$|[A-Za-z0-9]Tests?\.java$|[A-Za-z0-9]IT\.java$")
DOC_RE = re.compile(r"\.(md|rst|txt|adoc|png|jpg|svg|gif|pdf)$|(^|/)docs?/")


def is_code(path):
    """Corpus membership: a real, first-party source file in one of the 7 languages."""
    if VENDOR_RE.search(path) or GENERATED_RE.search(path):
        return False
    return lang_of(path) is not None


def is_test(path):
    return bool(TEST_RE.search(path))


def is_doc(path):
    return bool(DOC_RE.search(path))


# ---------------------------------------------------------------- qualified names
#
# Frozen normalisation rules (docs/benchmark.md -> 评分规则 -> 命中判据).
# Both sides of the comparison run through this and nothing else.

_GENERIC_RE = re.compile(r"<[^<>]*>")
_ARGS_RE = re.compile(r"\([^()]*\)")
_PTR_RE = re.compile(r"[*&]")
_RECEIVER_RE = re.compile(r"^\(\s*\*?\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\.")


def normalize_symbol(name):
    """Strip generics, argument lists, pointers; unify separators to '::'."""
    s = name.strip()
    s = s.split("->")[0]                    # drop trailing return type
    s = _RECEIVER_RE.sub(r"\1.", s)          # Go: (*T).M / (T).M -> T.M
    for _ in range(3):                      # nested generics/args
        s = _GENERIC_RE.sub("", s)
        s = _ARGS_RE.sub("", s)
    s = _PTR_RE.sub("", s)
    s = s.replace("#", "::").replace(".", "::")
    s = re.sub(r":{3,}", "::", s)
    s = re.sub(r"\s+", "", s)
    return s.strip(":")


def qualified_name(path, scope, name):
    """path/to/file.ext::Outer::inner — the one and only function-level identity."""
    parts = []
    if scope:
        parts.append(normalize_symbol(scope))
    parts.append(normalize_symbol(name))
    tail = "::".join(p for p in parts if p)
    return "%s::%s" % (path, tail)


# ---------------------------------------------------------------- git mirrors

def mirror_path(repo):
    return os.path.join(MIRRORS, slug(repo) + ".git")


def ensure_mirror(repo, with_pull_refs=False):
    """Blobless partial clone. Huge repos stay in the tens of MB until objects are touched."""
    path = mirror_path(repo)
    if not os.path.exists(os.path.join(path, "HEAD")):
        log("[mirror] cloning %s" % repo)
        sh(["git", "clone", "--bare", "--filter=blob:none",
            "https://github.com/%s.git" % repo, path])
        sh(["git", "-C", path, "config", "remote.origin.promisor", "true"])
        sh(["git", "-C", path, "config", "remote.origin.partialclonefilter", "blob:none"])
    # `git archive` obeys export-ignore from the tree's own .gitattributes, which
    # would silently cut files out of the corpus (pandas ships 57 such rules).
    # info/attributes outranks the in-tree file, so this restores the full tree.
    info = os.path.join(path, "info")
    os.makedirs(info, exist_ok=True)
    attrs = os.path.join(info, "attributes")
    want = "* -export-ignore\n"
    if not os.path.exists(attrs) or open(attrs, encoding="utf-8").read() != want:
        with open(attrs, "w", encoding="utf-8") as f:
            f.write(want)
    if with_pull_refs:
        marker = os.path.join(path, ".pull-refs-fetched")
        if not os.path.exists(marker):
            log("[mirror] fetching refs/pull/*/head for %s" % repo)
            sh(["git", "-C", path, "fetch", "--filter=blob:none", "origin",
                "+refs/pull/*/head:refs/remotes/pr/*"], check=False)
            open(marker, "w").close()
    return path


def git(repo, args, **kw):
    return sh(["git", "-C", mirror_path(repo)] + args, **kw)


def commit_exists(repo, sha):
    p = git(repo, ["cat-file", "-e", sha + "^{commit}"], check=False)
    return p.returncode == 0


def fetch_commit(repo, sha):
    if commit_exists(repo, sha):
        return True
    p = git(repo, ["fetch", "--filter=blob:none", "origin", sha], check=False)
    return p.returncode == 0
