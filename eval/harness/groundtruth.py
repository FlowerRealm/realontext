"""Ground truth = what the PR actually changed. No annotation, no judgement."""
import os
import re
import shutil
import tempfile

from . import symbols
from .common import git, is_code, log

HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def changed_files(repo, base, merge):
    """[(status, path)] restricted to first-party source in the 7 languages."""
    p = git(repo, ["diff", "--name-status", "--no-renames", base, merge])
    out = []
    for line in p.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        out.append((parts[0][0], parts[-1]))
    return out


def base_line_ranges(repo, base, merge, paths):
    """Lines that existed on the base side and were touched. Pure insertions yield none."""
    if not paths:
        return {}
    args = ["diff", "-U0", "--no-renames", base, merge, "--"] + paths
    p = git(repo, args)
    ranges, cur = {}, None
    for line in p.stdout.splitlines():
        if line.startswith("+++ b/"):
            cur = line[6:]
            ranges.setdefault(cur, [])
        elif line.startswith("@@") and cur is not None:
            m = HUNK_RE.match(line)
            if not m:
                continue
            start = int(m.group(1))
            count = 1 if m.group(2) is None else int(m.group(2))
            if count > 0:
                ranges[cur].append((start, start + count - 1))
    return ranges


def materialize_blobs(repo, sha, paths, dest):
    """Write the base-side content of specific files. Only touches the blobs we need."""
    written = []
    for path in paths:
        p = git(repo, ["cat-file", "blob", "%s:%s" % (sha, path)], check=False, text=False)
        if p.returncode != 0:
            continue
        full = os.path.join(dest, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(p.stdout)
        written.append(path)
    return written


def ground_truth(repo, base, merge):
    """{files, functions, unmapped, file_count_all} for one PR."""
    changes = changed_files(repo, base, merge)
    all_paths = [p for _, p in changes]
    # A file the PR creates does not exist in the base tree, so it is not in the
    # corpus and no retriever can return it. Counted, not scored.
    code = [p for st, p in changes if is_code(p) and st != "A"]
    added = [p for st, p in changes if is_code(p) and st == "A"]
    modified = [p for st, p in changes if is_code(p) and st == "M"]

    functions, unmapped = [], 0
    if modified:
        tmp = tempfile.mkdtemp(prefix="gt-")
        try:
            have = materialize_blobs(repo, base, modified, tmp)
            tags = symbols.scan(tmp, rel_paths=have)
            by_file = symbols.index_by_file(tags)
            ranges = base_line_ranges(repo, base, merge, have)
            for path, spans in ranges.items():
                tof = by_file.get(path, [])
                for lo, hi in spans:
                    hit = False
                    for line in range(lo, hi + 1):
                        t = symbols.enclosing(tof, line)
                        if t:
                            functions.append(t["qname"])
                            hit = True
                    if not hit:
                        unmapped += 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    return {
        "files": sorted(set(code)),
        "functions": sorted(set(functions)),
        "hunks_outside_functions": unmapped,
        "changed_all": len(all_paths),
        "changed_code": len(code),
        "added_files_dropped": len(added),
    }
