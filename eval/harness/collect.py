"""Enumerate merged PRs with linked issues via the GitHub GraphQL API.

Nothing here judges quality — collection is deliberately dumb and wide,
filtering happens in build.py so the filter can be re-run without re-fetching.
"""
import json
import os
import time
import urllib.error
import urllib.request

from . import common
from .common import RAW, log, read_jsonl, slug, write_jsonl

API = "https://api.github.com/graphql"

QUERY = """
query($owner:String!,$name:String!,$cursor:String){
  repository(owner:$owner,name:$name){
    pullRequests(states:MERGED, first:%(page)d,
                 orderBy:{field:UPDATED_AT,direction:DESC}, after:$cursor){
      pageInfo{hasNextPage endCursor}
      nodes{
        number title mergedAt createdAt
        headRefOid
        author{login __typename}
        mergeCommit{ oid parents(first:2){nodes{oid}} }
        closingIssuesReferences(first:5){
          nodes{ number title body createdAt
                 author{login __typename}
                 repository{nameWithOwner} }
        }
        files(first:100){ totalCount nodes{path additions deletions changeType} }
      }
    }
  }
}
""" % {"page": 50}


def _token():
    for k in ("GITHUB_TOKEN", "GH_TOKEN"):
        if os.environ.get(k):
            return os.environ[k]
    raise SystemExit("need GITHUB_TOKEN (repo-read scope is enough)")


def _post(payload, retries=5):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(API, data=data, method="POST")
    req.add_header("Authorization", "bearer " + _token())
    req.add_header("Content-Type", "application/json")
    req.add_header("User-Agent", "realontext-eval")
    delay = 2.0
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                body = json.loads(r.read().decode())
            if "errors" in body:
                msg = json.dumps(body["errors"])[:400]
                if "RATE_LIMITED" in msg or "secondary rate" in msg.lower():
                    log("[api] rate limited, sleeping %.0fs" % delay)
                    time.sleep(delay)
                    delay *= 2
                    continue
                raise RuntimeError("graphql: " + msg)
            return body["data"]
        except urllib.error.HTTPError as e:
            if e.code in (403, 429, 500, 502, 503, 504):
                log("[api] http %d, sleeping %.0fs" % (e.code, delay))
                time.sleep(delay)
                delay *= 2
                continue
            raise
        except (urllib.error.URLError, TimeoutError):
            time.sleep(delay)
            delay *= 2
    raise RuntimeError("graphql: giving up after %d retries" % retries)


def _flatten(pr):
    mc = pr.get("mergeCommit") or {}
    parents = [p["oid"] for p in (mc.get("parents") or {}).get("nodes", [])]
    issues = []
    for i in (pr.get("closingIssuesReferences") or {}).get("nodes", []):
        issues.append({
            "number": i["number"],
            "title": i.get("title") or "",
            "body": i.get("body") or "",
            "created_at": i.get("createdAt"),
            "author": (i.get("author") or {}).get("login"),
            "author_type": (i.get("author") or {}).get("__typename"),
            "repo": (i.get("repository") or {}).get("nameWithOwner"),
        })
    files = (pr.get("files") or {})
    return {
        "pr": pr["number"],
        "title": pr.get("title") or "",
        "merged_at": pr.get("mergedAt"),
        "created_at": pr.get("createdAt"),
        "author": (pr.get("author") or {}).get("login"),
        "author_type": (pr.get("author") or {}).get("__typename"),
        "merge_sha": mc.get("oid"),
        "base_sha": parents[0] if parents else None,
        "head_sha": pr.get("headRefOid"),
        "issues": issues,
        "file_count": files.get("totalCount", 0),
        "files": [{"path": f["path"], "add": f["additions"], "del": f["deletions"],
                   "change": f["changeType"]}
                  for f in (files.get("nodes") or [])],
    }


def collect(repo, want=400, max_pages=200):
    """Walk merged PRs newest-first until `want` linked-issue PRs are banked."""
    owner, name = repo.split("/")
    out, cursor, pages, seen_prs = [], None, 0, 0
    while pages < max_pages:
        data = _post({"query": QUERY,
                      "variables": {"owner": owner, "name": name, "cursor": cursor}})
        conn = data["repository"]["pullRequests"]
        for pr in conn["nodes"]:
            seen_prs += 1
            if not (pr.get("closingIssuesReferences") or {}).get("nodes"):
                continue
            out.append(_flatten(pr))
        pages += 1
        log("[collect] %s page=%d prs_seen=%d linked=%d" % (repo, pages, seen_prs, len(out)))
        if len(out) >= want or not conn["pageInfo"]["hasNextPage"]:
            break
        cursor = conn["pageInfo"]["endCursor"]
    path = os.path.join(RAW, slug(repo) + ".jsonl")
    write_jsonl(path, out)
    return {"repo": repo, "prs_scanned": seen_prs, "linked": len(out), "path": path}


def census(repos=None, want=400):
    """先数再抓: how many usable PRs does each repo actually have?

    Collection is the expensive half, so census reuses whatever raw/ already holds.
    """
    from .build import build            # local import: build imports common only
    rows = []
    for r in (repos or [x["repo"] for x in common.parse_repos()]):
        raw = os.path.join(RAW, slug(r) + ".jsonl")
        if not os.path.exists(raw):
            collect(r, want=want)
        raw_rows = read_jsonl(raw)
        kept, stats = build(r, raw_rows, dry_run=True)
        rows.append({"repo": r,
                     "linked_prs": len(raw_rows),
                     "kept": len(kept),
                     **{"drop_" + k: v for k, v in stats.items()}})
        log("[census] %-28s linked=%4d kept=%4d" % (r, len(raw_rows), len(kept)))
    return rows
