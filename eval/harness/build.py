"""Apply the frozen filter rules, then compute ground truth.

Cheap filters (text, metadata) run first so the census can answer
"how many usable PRs does this repo have" without touching git at all.
"""
import datetime as dt
import os

from . import groundtruth
from .common import (DATASETS, VENDOR_RE, GENERATED_RE, ensure_mirror, fetch_commit,
                     is_code, is_doc, is_test, log, slug, write_jsonl)

MIN_BODY = 200
MAX_FILES = 10
MIN_GAP_SECONDS = 3600
BOT_TYPES = {"Bot"}
BOT_NAME_HINTS = ("[bot]", "-bot", "bot-", "dependabot", "renovate", "mergify",
                  "codecov", "greenkeeper")


def _ts(s):
    if not s:
        return None
    return dt.datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)


def _is_bot(login, typename):
    if typename in BOT_TYPES:
        return True
    low = (login or "").lower()
    return any(h in low for h in BOT_NAME_HINTS)


def _pick_issue(row):
    """One PR may close several issues; take the lowest-numbered same-repo one."""
    same = [i for i in row["issues"] if i.get("repo") == row.get("_repo")]
    pool = same or []
    return min(pool, key=lambda i: i["number"]) if pool else None


def build(repo, raw_rows, dry_run=False, limit=None, only_prs=None):
    stats = {k: 0 for k in (
        "no_issue", "cross_repo", "bot_issue", "short_body", "gap_too_small",
        "too_many_files", "no_code_files", "docs_or_tests_only", "vendored",
        "later_pr_for_issue", "no_base_sha", "git_missing", "gt_empty")}

    for r in raw_rows:
        r["_repo"] = repo

    # merge order decides which PR owns an issue; pr number breaks ties so the
    # same raw/ always yields the same dataset, --limit included
    rows = sorted(raw_rows, key=lambda r: (r.get("merged_at") or "", r["pr"]))
    seen_issue = {}
    staged = []

    for r in rows:
        issue = _pick_issue(r)
        if not issue:
            stats["cross_repo" if r["issues"] else "no_issue"] += 1
            continue
        if _is_bot(issue.get("author"), issue.get("author_type")):
            stats["bot_issue"] += 1
            continue
        body = (issue.get("body") or "").strip()
        if len(body) < MIN_BODY:
            stats["short_body"] += 1
            continue
        gap = None
        if issue.get("created_at") and r.get("created_at"):
            gap = (_ts(r["created_at"]) - _ts(issue["created_at"])).total_seconds()
        if gap is not None and gap < MIN_GAP_SECONDS:
            stats["gap_too_small"] += 1
            continue
        if r.get("file_count", 0) > MAX_FILES:
            stats["too_many_files"] += 1
            continue
        paths = [f["path"] for f in r.get("files", [])]
        if any(VENDOR_RE.search(p) or GENERATED_RE.search(p) for p in paths):
            stats["vendored"] += 1
            continue
        code = [p for p in paths if is_code(p) and not is_test(p) and not is_doc(p)]
        if not code:
            stats["no_code_files" if not paths else "docs_or_tests_only"] += 1
            continue
        if not r.get("base_sha"):
            stats["no_base_sha"] += 1
            continue

        key = issue["number"]
        if key in seen_issue:
            stats["later_pr_for_issue"] += 1
            continue
        seen_issue[key] = True
        staged.append((r, issue))

    if dry_run:
        return staged, stats

    if only_prs is not None:
        want = set(only_prs)
        order = {pr: i for i, pr in enumerate(only_prs)}
        staged = sorted((t for t in staged if t[0]["pr"] in want),
                        key=lambda t: order[t[0]["pr"]])

    ensure_mirror(repo)
    kept = []
    for r, issue in staged:
        if limit and len(kept) >= limit:
            break
        if not (fetch_commit(repo, r["base_sha"]) and fetch_commit(repo, r["merge_sha"])):
            stats["git_missing"] += 1
            continue
        try:
            gt = groundtruth.ground_truth(repo, r["base_sha"], r["merge_sha"])
        except Exception as e:                      # a single bad PR must not kill a repo
            log("[build] %s#%d ground truth failed: %s" % (repo, r["pr"], e))
            stats["git_missing"] += 1
            continue
        gt_files = [p for p in gt["files"] if not is_test(p) and not is_doc(p)]
        if not gt_files:
            stats["gt_empty"] += 1
            continue
        # a function only counts if the file it lives in survived the same filter
        keep_paths = set(gt_files)
        gt_funcs = [q for q in gt["functions"] if q.split("::")[0] in keep_paths]

        query = (issue.get("title") or "") + "\n\n" + (issue.get("body") or "")
        kept.append({
            "repo": repo,
            "pr": r["pr"],
            "issue": issue["number"],
            "query": query,
            "base_sha": r["base_sha"],
            "merge_sha": r["merge_sha"],
            "head_sha": r.get("head_sha"),
            "gt_files": gt_files,
            "gt_functions": gt_funcs,
            "func_eligible": bool(gt_funcs),
            "hunks_outside_functions": gt["hunks_outside_functions"],
            "leak_path": _leak_path(query, gt_files),
            "leak_symbol": _leak_symbol(query, gt_funcs),
        })
        if len(kept) % 25 == 0:
            log("[build] %s kept=%d" % (repo, len(kept)))
    return kept, stats


def _leak_path(query, files):
    """Does the issue text hand over a ground-truth path outright?"""
    for p in files:
        if p in query or os.path.basename(p) in query:
            return True
    return False


def _leak_symbol(query, functions):
    for q in functions:
        name = q.split("::")[-1]
        if len(name) >= 4 and name in query:
            return True
    return False


def dataset_path(repo):
    return os.path.join(DATASETS, slug(repo) + ".jsonl")


def save(repo, rows):
    path = dataset_path(repo)
    write_jsonl(path, rows)
    return path
