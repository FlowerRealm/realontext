# holdout 使用记录

`repos.txt` 里 `split=holdout` 的 4 个仓库——duckdb/duckdb、rust-lang/rust、
vuejs/core、apache/airflow——调参期间不许跑。累计上限 5 次。

规则见 [`../docs/benchmark.md`](../docs/benchmark.md)「holdout 使用协议」。

跑完一次，往下面加一行。没有记录的一次跑等于没跑过——**下一次照样算第一次**，
这条自欺的成本比多跑一次高得多。

| # | 日期 | dataset_version | code_commit | 跑了什么 | train 分 | holdout 分 | 结论 |
|---|---|---|---|---|---|---|---|

*（尚未使用。剩余 5 次。）*
