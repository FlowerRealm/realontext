"""Metrics. Macro-averaged over queries, per docs/benchmark.md 指标 / 评分规则."""
import json
import math
import os
import statistics

FILE_KS = (1, 5, 10)
FUNC_KS = (5, 10, 50)


def recall_at_k(ranked, gt, k):
    if not gt:
        return None
    return len(set(ranked[:k]) & set(gt)) / float(len(gt))


def acc_at_k(ranked, gt, k):
    """LocAgent's strict tier: every ground-truth location inside the top k."""
    if not gt:
        return None
    return 1.0 if set(gt) <= set(ranked[:k]) else 0.0


def mrr(ranked, gt):
    if not gt:
        return None
    gts = set(gt)
    for i, d in enumerate(ranked, 1):
        if d in gts:
            return 1.0 / i
    return 0.0


def ndcg_at_k(ranked, gt, k=10):
    """Binary gain: with rel in {0,1} this is position-weighted recall, and is
    only ever compared against other runs over the same query set."""
    if not gt:
        return None
    gts = set(gt)
    dcg = sum(1.0 / math.log2(i + 1) for i, d in enumerate(ranked[:k], 1) if d in gts)
    ideal = sum(1.0 / math.log2(i + 1) for i in range(1, min(len(gts), k) + 1))
    return dcg / ideal if ideal else 0.0


def score_one(ranked_files, ranked_funcs, row):
    out = {}
    gtf = row["gt_files"]
    for k in FILE_KS:
        out["file_recall@%d" % k] = recall_at_k(ranked_files, gtf, k)
        out["file_acc@%d" % k] = acc_at_k(ranked_files, gtf, k)
    out["file_mrr"] = mrr(ranked_files, gtf)
    out["file_ndcg@10"] = ndcg_at_k(ranked_files, gtf, 10)

    gtn = row.get("gt_functions") or []
    if gtn:
        for k in FUNC_KS:
            out["func_recall@%d" % k] = recall_at_k(ranked_funcs, gtn, k)
            out["func_acc@%d" % k] = acc_at_k(ranked_funcs, gtn, k)
        out["func_mrr"] = mrr(ranked_funcs, gtn)
        out["func_ndcg@10"] = ndcg_at_k(ranked_funcs, gtn, 10)
    return out


def macro(per_query):
    """Average each metric over the queries where it is defined. Never pool."""
    keys = set()
    for r in per_query:
        keys.update(k for k, v in r.items() if isinstance(v, (int, float)) and v is not None)
    out = {}
    for k in sorted(keys):
        vals = [r[k] for r in per_query if isinstance(r.get(k), (int, float))]
        if vals:
            out[k] = sum(vals) / len(vals)
    return out


def dataset_stats(rows):
    gt_sizes = [len(r["gt_files"]) for r in rows]
    fn_sizes = [len(r.get("gt_functions") or []) for r in rows if r.get("gt_functions")]
    leaked = sum(1 for r in rows if r.get("leak_path") or r.get("leak_symbol"))
    outside = sum(r.get("hunks_outside_functions", 0) for r in rows)
    func_rows = sum(1 for r in rows if r.get("gt_functions"))
    return {
        "queries": len(rows),
        "func_eligible": func_rows,
        "gt_files_median": statistics.median(gt_sizes) if gt_sizes else 0,
        "gt_files_p90": (sorted(gt_sizes)[int(0.9 * (len(gt_sizes) - 1))] if gt_sizes else 0),
        "gt_funcs_median": statistics.median(fn_sizes) if fn_sizes else 0,
        "leak_rate": leaked / len(rows) if rows else 0.0,
        "hunks_outside_functions_total": outside,
    }


HEADLINE = ["file_recall@10", "file_acc@10", "file_mrr", "file_ndcg@10",
            "func_recall@10", "func_acc@10", "func_recall@50", "func_mrr"]


def markdown_report(meta, stats, systems):
    """systems: {name: macro_dict}. One score alone means nothing — this is the
    fixed shape every report has to come in (docs/benchmark.md 报分模板)."""
    lines = []
    lines.append("## 评测报告")
    lines.append("")
    lines.append("| 字段 | 值 |")
    lines.append("|---|---|")
    for k in ("dataset_commit", "code_commit", "repos", "split", "generated_at"):
        if meta.get(k) is not None:
            lines.append("| %s | %s |" % (k, meta[k]))
    for k, v in stats.items():
        v = ("%.3f" % v) if isinstance(v, float) else v
        lines.append("| %s | %s |" % (k, v))
    lines.append("")
    header = "| 系统 | " + " | ".join(HEADLINE) + " |"
    lines.append(header)
    lines.append("|---" * (len(HEADLINE) + 1) + "|")
    for name, m in systems.items():
        cells = []
        for k in HEADLINE:
            cells.append("%.3f" % m[k] if k in m else "—")
        lines.append("| %s | %s |" % (name, " | ".join(cells)))
    lines.append("")
    lines.append("> 基线不是陪跑。realontext 涨而基线同步涨 = 测试集变简单了，不是系统变好了。")
    return "\n".join(lines)


def save(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2, sort_keys=True)
