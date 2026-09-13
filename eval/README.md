# eval/ — 阶段 0 测试装置

`docs/benchmark.md` 定义规则，这里是执行它的代码。

**全程零 API 成本**：GitHub GraphQL + git + ripgrep + universal-ctags。向量基线跑本地
Jina 权重，不调任何付费接口。

## 依赖

| | |
|---|---|
| Python | 3.12+，标准库即可（阶段 0 不需要 `requirements.txt`） |
| git | 2.30+ |
| ripgrep | grep 基线 |
| universal-ctags | ground truth 的符号边界，**必须是 Universal Ctags，不是 BSD ctags** |
| `requirements.txt` | 只有本地向量基线需要（torch / sentence-transformers） |

```bash
# macOS
brew install universal-ctags ripgrep
# Debian / Ubuntu / GitHub Actions
sudo apt-get install -y universal-ctags ripgrep
```

需要 `GITHUB_TOKEN`（只读 public repo 即可）：`export GITHUB_TOKEN=$(gh auth token)`

## 四步

```bash
cd eval

# 1. 先数再抓：每仓库到底有多少条满足过滤条件的 PR
python3 -m harness.run census --size medium

# 2. 抓 merged PR 与其关联 issue（写 raw/，可重跑）
python3 -m harness.run collect --repo prometheus/prometheus

# 3. 过滤 + 算 ground truth，冻结进 datasets/
python3 -m harness.run build --repo prometheus/prometheus --limit 100

# 4. 跑基线，出报分表
python3 -m harness.run run    --repo prometheus/prometheus --baselines grep,bm25
python3 -m harness.run report --split train
```

`--size medium|huge|all`、`--split train|holdout|all` 在每个子命令上都能用。

## 各步在做什么

**collect** 走 GraphQL 的 `closingIssuesReferences`，只收 merged 且关联了 issue 的 PR。
base commit 取 `mergeCommit.parents[0]`——squash 和 merge commit 两种情况都对。

**build** 先跑纯文本与元数据的过滤（不碰 git，所以 census 免费），再对活下来的 PR 算
ground truth：`git diff -U0` 拿 base 侧被改的行号区间，ctags 拿函数边界，两者相交。
新增的函数在 base 上不存在，自动落不到任何区间里，与「纯新增函数从函数级榜单剔除」
一致。

**run** 对每条 query 把它自己 base commit 的语料铺出来，跑基线，算分，删掉语料。

## 三个与 `benchmark.md` 不同的地方

文档说的是原则，这里是能在 CI 上跑的实现。三处偏离，都记在这里而不是藏在代码里：

**一、符号边界用 universal-ctags，不是 language server。** 文档写「git diff 行号区间 +
language server 的 `documentSymbol`」。七种语言在 CI 里拉起七个 language server 太重，
ctags 一个二进制覆盖全部七种。硬规则 3 要的是**与检索侧不同的实现**——检索侧是
tree-sitter，ctags 满足这一条，而且比 LSP 更满足：两者连解析器都不共享。

**二、向量基线是 `jina-embeddings-v2-base-code`，不是生产要用的 v4。** v4 是 3.8B，
2 核 runner 上跑不动。所以这个数字是**向量那一路的下限，不是生产质量的预测**，报分时
必须带这句话。超大仓库默认走 `pool` 模式（只嵌入 BM25 前 200 个函数），同时报 pool
本身的召回天花板；中等仓库走 `full`。

**三、语料铺开用 `git archive` 流式解包。** blobless partial clone 上逐个 `cat-file`
会为每个缺失 blob 发一次网络请求，慢到不可用；`git archive` 一次性批量取回整棵树。
超大仓库因此和中等仓库一样，每个快照只花「一次批量拉取 + 解包」。

## GitHub Actions

`.github/workflows/eval.yml`，手动触发（`workflow_dispatch`）：

| 输入 | 说明 |
|---|---|
| `stage` | `census` / `dataset` / `baselines` / `all` |
| `size` | `medium` / `huge` / `all`——两个规模档分开跑 |
| `split` | `train` / `holdout` / `all` |
| `limit` | 每仓库 query 上限，`0` 为不限 |
| `baselines` | `grep,bm25` 或加上 `vector` |

每个仓库一个 job（`fail-fast: false`），一个仓库炸不影响其他仓库。git 镜像、符号索引、
本地嵌入三份缓存按仓库分开。超大档会先清掉 runner 上的 Android SDK 和 .NET 腾磁盘。

**每周一自动跑一次 census**——可用 PR 数会随上游仓库变化，这个数掉下去要立刻知道。

## holdout

`repos.txt` 里 `split=holdout` 的 4 个仓库，调参期间不许跑。累计上限 5 次，每次跑完
写一行进 `holdout-log.md`。规则见 `docs/benchmark.md`「holdout 使用协议」。

## 目录

```
harness/
  common.py       repos.txt、路径、git 镜像、限定名归一化
  collect.py      GitHub GraphQL 抓取 + census
  build.py        过滤规则 + 数据集生成
  groundtruth.py  diff 行号区间 -> 函数
  symbols.py      universal-ctags 封装
  snapshot.py     base commit 语料铺开 + 符号索引缓存
  tokenize.py     两条基线的冻结分词（不与 lexical/ 共享）
  baselines.py    grep 基线、BM25 基线
  vector.py       本地 Jina 向量基线（无 API）
  score.py        Recall@k / Acc@k / MRR / NDCG，macro 平均，报分表
  run.py          CLI

datasets/   冻结的测试集，进 git
raw/        抓取缓存，不进 git
results/    分数，不进 git
.cache/     镜像、语料、符号索引、嵌入，不进 git
```
