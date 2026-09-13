"""CLI. `python -m harness.run <command>` from the eval/ directory."""
import argparse
import datetime as dt
import json
import os
import sys

from . import (baselines, build as buildmod, collect as collectmod,
               manifest as manifestmod, score as scoremod)
from .common import (DATASETS, RESULTS, ROOT, log, parse_repos, read_jsonl,
                     repo_meta, sh, slug, write_jsonl)
from .snapshot import Snapshot
from .tokenize import bm25_terms, grep_terms


def _repos(args):
    rows = parse_repos()
    if args.repo:
        want = set(args.repo.split(","))
        rows = [r for r in rows if r["repo"] in want]
        if not rows:
            raise SystemExit("no such repo in repos.txt: " + args.repo)
    if getattr(args, "split", None) and args.split != "all":
        rows = [r for r in rows if r["split"] == args.split]
    if getattr(args, "size", None) and args.size != "all":
        rows = [r for r in rows if r["size"] == args.size]
    return rows


def _git_commit(path):
    p = sh(["git", "-C", path, "rev-parse", "--short", "HEAD"], check=False)
    return p.stdout.strip() or "unknown"


# ---------------------------------------------------------------- commands

CENSUS_DIR = os.path.join(RESULTS, "census")


def _census_table(rows):
    out = ["| 仓库 | 语言 | 规模 | split | 关联 issue 的 PR | 过滤后可用 |",
           "|---|---|---|---|---|---|"]
    for r in sorted(rows, key=lambda r: (repo_meta(r["repo"])["size"],
                                         repo_meta(r["repo"])["language"])):
        m = repo_meta(r["repo"])
        out.append("| %s | %s | %s | %s | %d | %d |"
                   % (r["repo"], m["language"], m["size"], m["split"],
                      r["linked_prs"], r["kept"]))
    out.append("")
    out.append("合计可用 query：%d" % sum(r["kept"] for r in rows))
    out.append("")
    out.append("> 这里的「可用」只跑了文本与元数据过滤。`build` 之后还会掉一些"
               "（ground truth 算不出来的、base commit 取不到的）。")
    return "\n".join(out)


def cmd_census(args):
    """先数再抓：每仓库到底有多少条能用的 PR。规格站不站得住，先看这张表。"""
    os.makedirs(CENSUS_DIR, exist_ok=True)
    rows = []
    for r in _repos(args):
        row = collectmod.census([r["repo"]], want=args.want)[0]
        scoremod.save(os.path.join(CENSUS_DIR, slug(r["repo"]) + ".json"), row)
        rows.append(row)
    print()
    print(_census_table(rows))
    return 0


def cmd_census_report(args):
    """合并各仓库的 census 分片。一个仓库抓崩了不影响其余的数字。"""
    if not os.path.isdir(CENSUS_DIR):
        raise SystemExit("no results/census/ — run census first")
    known = {r["repo"] for r in _repos(args)}
    rows = []
    for name in sorted(os.listdir(CENSUS_DIR)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(CENSUS_DIR, name), encoding="utf-8") as f:
            row = json.load(f)
        if row["repo"] in known:
            rows.append(row)
    if not rows:
        raise SystemExit("no census shards for the requested repos")
    missing = sorted(known - {r["repo"] for r in rows})
    print(_census_table(rows))
    if missing:
        print()
        print("**没有数据的仓库**（抓取失败，需要重跑）：" + "、".join(missing))
    return 0


def cmd_collect(args):
    for r in _repos(args):
        info = collectmod.collect(r["repo"], want=args.want)
        log("[collect] %(repo)s scanned=%(prs_scanned)d linked=%(linked)d" % info)
    return 0


def cmd_build(args):
    for r in _repos(args):
        repo = r["repo"]
        raw = read_jsonl(os.path.join(ROOT, "raw", slug(repo) + ".jsonl"))
        if not raw:
            log("[build] %s has no raw/, run collect first" % repo)
            continue
        rows, stats = buildmod.build(repo, raw, limit=args.limit)
        path = buildmod.save(repo, rows)
        log("[build] %s -> %d queries (%s)" % (repo, len(rows), path))
        log("[build] dropped: " + json.dumps(stats, sort_keys=True))
        entry = manifestmod.load()["frozen"].get(repo)
        if entry and manifestmod.file_hash(path) != entry["sha256"]:
            log("[build] %s now differs from the frozen manifest. Scoring will "
                "refuse it until you re-freeze with a reason." % repo)
    log("build only proposes. `freeze` is what makes a test set real.")
    return 0


def cmd_freeze(args):
    """冻结：把当前 datasets/ 记进 MANIFEST.json。改一次记一次。"""
    if not args.reason:
        raise SystemExit("--reason is required: an unexplained test-set change is "
                         "indistinguishable from moving the goalposts")
    for r in _repos(args):
        manifestmod.freeze(r["repo"], args.reason)
    return 0


def _run_query(snap, row, which):
    paths, by_file = snap.paths, snap.by_file
    query = row["query"]
    gterms = sorted({t.lower() for t in grep_terms(query)})
    bterms = sorted(set(bm25_terms(query)))
    vocab = sorted(set(gterms) | set(bterms))
    cands = baselines.candidate_files(snap.root, vocab) if vocab else []
    stats = baselines.scan(snap.root, cands, vocab, by_file)

    out = {}
    if "grep" in which:
        out["grep"] = baselines.grep_baseline(snap.root, paths, query, by_file,
                                              stats=stats, corpus_files=paths)
    if "bm25" in which:
        out["bm25"] = baselines.bm25_baseline(snap.root, paths, query, by_file,
                                              stats=stats, corpus_files=paths)
    if "vector" in which:
        from . import vector
        pool = None
        if os.environ.get("EVAL_VECTOR_MODE", "pool") == "pool":
            base = out.get("bm25") or baselines.bm25_baseline(
                snap.root, paths, query, by_file, stats=stats, corpus_files=paths)
            pool = base[1][:vector.POOL_SIZE]
        out["vector"] = vector.vector_baseline(snap.root, snap.tags, query,
                                               row["repo"], pool=pool)
        if pool is not None:
            gt = set(row.get("gt_functions") or [])
            out["_pool_ceiling"] = (len(gt & set(pool)) / len(gt)) if gt else None
    return out


def cmd_verify(args):
    """报告冻结状态，不改任何东西。CI 的 refresh 阶段用它出提案摘要。"""
    man = manifestmod.load()
    bad = 0
    for r in _repos(args):
        repo = r["repo"]
        path = manifestmod.dataset_file(repo)
        entry = man["frozen"].get(repo)
        if not os.path.exists(path):
            print("%-28s MISSING" % repo)
            bad += 1
        elif entry is None:
            print("%-28s UNFROZEN  %s" % (repo, manifestmod.file_hash(path)[:12]))
            bad += 1
        else:
            actual = manifestmod.file_hash(path)
            state = "OK" if actual == entry["sha256"] else "DRIFTED"
            print("%-28s %-8s frozen=%s actual=%s queries=%d"
                  % (repo, state, entry["sha256"][:12], actual[:12], entry["queries"]))
            bad += (state != "OK")
    print("\ndataset_version = %s" % manifestmod.dataset_version(
        [r["repo"] for r in _repos(args)]))
    return 1 if bad else 0


def cmd_run(args):
    which = [w.strip() for w in args.baselines.split(",") if w.strip()]
    for r in _repos(args):
        repo = r["repo"]
        rows = read_jsonl(buildmod.dataset_path(repo))
        if not rows:
            log("[run] %s has no dataset, run build first" % repo)
            continue
        if args.limit:
            rows = rows[:args.limit]
        frozen = manifestmod.verify(repo, allow_drift=args.allow_drift)
        if frozen:
            log("[run] %s frozen at %s (%d queries)"
                % (repo, frozen["sha256"][:12], frozen["queries"]))
        per_query = {w: [] for w in which}
        ceilings = []
        for i, row in enumerate(rows, 1):
            log("[run] %s %d/%d pr=%d" % (repo, i, len(rows), row["pr"]))
            try:
                with Snapshot(repo, row["base_sha"]) as snap:
                    res = _run_query(snap, row, which)
            except Exception as e:
                log("[run] %s pr=%d failed: %s" % (repo, row["pr"], e))
                continue
            if res.get("_pool_ceiling") is not None:
                ceilings.append(res["_pool_ceiling"])
            for w in which:
                if w in res:
                    files, funcs = res[w]
                    per_query[w].append(scoremod.score_one(files, funcs, row))
        payload = {
            "repo": repo, "meta": r,
            "dataset_stats": scoremod.dataset_stats(rows),
            "systems": {w: scoremod.macro(per_query[w]) for w in which},
            "per_query_counts": {w: len(per_query[w]) for w in which},
        }
        if ceilings:
            payload["vector_pool_ceiling"] = sum(ceilings) / len(ceilings)
        out = os.path.join(RESULTS, slug(repo) + ".json")
        scoremod.save(out, payload)
        log("[run] %s -> %s" % (repo, out))
    return 0


def cmd_report(args):
    repos = _repos(args)
    rows, systems, ceilings = [], {}, []
    dataset_rows = []
    for r in repos:
        path = os.path.join(RESULTS, slug(r["repo"]) + ".json")
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            rows.append(json.load(f))
        dataset_rows += read_jsonl(buildmod.dataset_path(r["repo"]))
    if not rows:
        raise SystemExit("no results/ found — run `run` first")

    for payload in rows:
        if payload.get("vector_pool_ceiling") is not None:
            ceilings.append(payload["vector_pool_ceiling"])
        for name, m in payload["systems"].items():
            systems.setdefault(name, []).append((payload["dataset_stats"]["queries"], m))

    merged = {}
    for name, parts in systems.items():
        keys = set()
        for _, m in parts:
            keys.update(m)
        acc = {}
        for k in keys:
            num = sum(n * m[k] for n, m in parts if k in m)
            den = sum(n for n, m in parts if k in m)
            if den:
                acc[k] = num / den
        merged[name] = acc

    scored = [p["repo"] for p in rows]
    meta = {
        "dataset_version": manifestmod.dataset_version(scored),
        "unfrozen_repos": ", ".join(
            r for r in scored if r not in manifestmod.load()["frozen"]) or "none",
        "tools": json.dumps(manifestmod.tool_versions(), ensure_ascii=False),
        "dataset_commit": _git_commit(os.path.dirname(ROOT)),
        "code_commit": _git_commit(os.path.dirname(ROOT)),
        "repos": "%d (%s)" % (len(rows), ", ".join(p["repo"] for p in rows)),
        "split": args.split or "all",
        "generated_at": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
    }
    stats = scoremod.dataset_stats(dataset_rows)
    if ceilings:
        stats["vector_pool_ceiling"] = sum(ceilings) / len(ceilings)
    md = scoremod.markdown_report(meta, stats, merged)
    print(md)
    with open(os.path.join(RESULTS, "report.md"), "w", encoding="utf-8") as f:
        f.write(md + "\n")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="harness.run")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, split=True):
        p.add_argument("--repo", help="comma separated, defaults to all of repos.txt")
        if split:
            p.add_argument("--split", choices=["train", "holdout", "all"], default=None)
            p.add_argument("--size", choices=["huge", "medium", "all"], default=None)

    p = sub.add_parser("census", help="先数再抓：统计每仓库可用 PR 数")
    common(p)
    p.add_argument("--want", type=int, default=400)
    p.set_defaults(fn=cmd_census)

    p = sub.add_parser("collect", help="抓 merged PR 与关联 issue")
    common(p)
    p.add_argument("--want", type=int, default=400)
    p.set_defaults(fn=cmd_collect)

    p = sub.add_parser("build", help="过滤 + 生成 ground truth，冻结进 datasets/")
    common(p)
    p.add_argument("--limit", type=int, default=None)
    p.set_defaults(fn=cmd_build)

    p = sub.add_parser("census-report", help="合并 census 分片")
    common(p)
    p.set_defaults(fn=cmd_census_report)

    p = sub.add_parser("verify", help="报告冻结状态")
    common(p)
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("freeze", help="冻结数据集进 MANIFEST.json")
    common(p)
    p.add_argument("--reason", required=True, help="为什么改，会写进 history")
    p.set_defaults(fn=cmd_freeze)

    p = sub.add_parser("run", help="跑基线")
    common(p)
    p.add_argument("--baselines", default="grep,bm25")
    p.add_argument("--limit", type=int, default=None)
    p.add_argument("--allow-drift", action="store_true",
                   help="对未冻结或已漂移的数据集打分。只用于本地探索，出来的数不许报")
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("report", help="合并 results/ 出报分表")
    common(p)
    p.set_defaults(fn=cmd_report)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
