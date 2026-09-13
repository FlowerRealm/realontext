"""Human/agent curation of the query set.

The selection step reads the full issue text and is told nothing about what the
selection is for. Objectivity comes from that: a reviewer who does not know the
purpose cannot select toward it. The answer — which files and functions the PR
touched — is withheld regardless, since that is the marking scheme.

Every selection is written down with a reason and hashed, so a later reader can
see exactly which queries were picked and why.
"""
import hashlib
import json
import os
import re

from .common import ROOT, log, read_jsonl, slug, write_jsonl

CANDIDATES = os.path.join(ROOT, "candidates")
SELECTIONS = os.path.join(ROOT, "selections")
TARGET = 50

for _d in (CANDIDATES, SELECTIONS):
    os.makedirs(_d, exist_ok=True)

_TRACE = re.compile(r"(Traceback|panicked at|SIGSEGV|Exception|\bat [\w.]+\(|"
                    r"#\d+ 0x[0-9a-f]+|thread '.*' panicked)")
_REPRO = re.compile(r"(?i)(steps? to reproduce|to reproduce|reproduc|minimal example|"
                    r"expected behaviou?r|actual behaviou?r|what happened|"
                    r"how to reproduce)")
_FEATURE = re.compile(r"(?i)(feature request|proposal|rfc\b|would be nice|"
                      r"it would be great|enhancement request)")


def shortlist(staged, cap=150):
    """Cheap pre-trim so the agent reads a sane number of candidates.

    Ordering is deterministic and by textual signal only. It is a pre-trim, not
    a judgement: the agent still makes the call.
    """
    scored = []
    for row, issue in staged:
        body = issue.get("body") or ""
        score = 0
        if _REPRO.search(body):
            score += 2
        if _TRACE.search(body):
            score += 2
        if 400 <= len(body) <= 6000:
            score += 1
        if _FEATURE.search(body[:400]):
            score -= 2
        if body.count("```") >= 2:
            score += 1
        scored.append((-score, row["pr"], row, issue))
    scored.sort(key=lambda t: (t[0], t[1]))
    return [(row, issue) for _, _, row, issue in scored[:cap]]


def write_candidates(repo, staged, cap=150):
    """Emit the reviewer's reading material: the whole issue, answers withheld."""
    rows = []
    for row, issue in shortlist(staged, cap):
        body = (issue.get("body") or "").strip()
        rows.append({
            "pr": row["pr"],
            "issue": issue["number"],
            "issue_title": issue.get("title") or "",
            "issue_body": body,
            "body_chars": len(body),
        })
    path = os.path.join(CANDIDATES, slug(repo) + ".jsonl")
    write_jsonl(path, rows)
    return path, len(rows)


def selection_path(repo):
    return os.path.join(SELECTIONS, slug(repo) + ".json")


def load_selection(repo):
    path = selection_path(repo)
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        sel = json.load(f)
    if not isinstance(sel.get("selected"), list):
        raise SystemExit("%s: missing a `selected` list" % path)
    return sel


def validate(repo, sel, staged):
    """A selection may only name PRs that were actually offered."""
    offered = {row["pr"] for row, _ in staged}
    chosen, seen, problems = [], set(), []
    for item in sel["selected"]:
        pr = item["pr"] if isinstance(item, dict) else item
        if pr in seen:
            problems.append("duplicate pr %s" % pr)
            continue
        if pr not in offered:
            problems.append("pr %s was never a candidate" % pr)
            continue
        seen.add(pr)
        chosen.append(pr)
    return chosen, problems


def selection_hash(repo):
    path = selection_path(repo)
    if not os.path.exists(path):
        return None
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()
