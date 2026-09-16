# holdout 使用记录

`repos.txt` 里 `split=holdout` 的 4 个仓库——duckdb/duckdb、rust-lang/rust、
vuejs/core、apache/airflow——调参期间不许跑。累计上限 5 次。

规则见 [`../docs/benchmark.md`](../docs/benchmark.md)「holdout 使用协议」。

跑完一次，往下面加一行。没有记录的一次跑等于没跑过——**下一次照样算第一次**，
这条自欺的成本比多跑一次高得多。

| # | 日期 | dataset_version | code_commit | 跑了什么 | train 分 | holdout 分 | 结论 |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-16 | v1-part-8a417a2f9750c725 | 965597b | duckdb/duckdb, rust-lang/rust, vuejs/core, apache/airflow：阶段 1 验收：BM25 召回 + 测试降权 + 文件级 RRF（D21） | realontext file_recall@10 0.474 / func_recall@10 0.217（bm25 0.386 / 0.169） | realontext file_recall@10 0.306 / func_recall@10 0.204（bm25 0.252 / 0.205） | 文件级在 holdout 上保持领先（+5.4 个点，相对 bm25 +21%，train 为 +23%）。函数级没有泛化：train 领先 4.8 个点，holdout 与 bm25 持平（-0.1），func_mrr 0.120 低于 bm25 0.155。train→holdout 相对分差 file 35% 超过 15%，但 bm25 同幅下降，记为题目难度差异；func 的相对领先消失，记为过拟合告警 |

*（剩余 4 次。）*
