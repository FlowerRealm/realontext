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

## 标准流程

```bash
cd eval

# 1. 先数再抓：每仓库到底有多少条满足过滤条件的 PR
python3 -m harness.run census --size medium

# 2. 抓 merged PR 与其关联 issue（写 raw/，可重跑）
python3 -m harness.run collect --repo prometheus/prometheus

# 3. 导出盲审候选，供子代理或人工独立挑选（输出 selections/<repo>.json）
python3 -m harness.run candidates --repo prometheus/prometheus

# 4. 过滤 + 算 ground truth（通过 --select 仅对选中的题目构建），写入 datasets/
python3 -m harness.run build --repo prometheus/prometheus --select

# 5. 冻结（计算内容与挑选哈希，没有这一步后续打分会直接拒绝）
python3 -m harness.run freeze --repo prometheus/prometheus --reason "v1 基准冻结"

# 6. 跑基线，出报分表
python3 -m harness.run run    --repo prometheus/prometheus --baselines grep,bm25
python3 -m harness.run report --split train
```

## 什么是固定的，什么会变

固定的部分靠装置强制，不靠自觉：

| | 怎么保证 |
|---|---|
| 语料 | `base_sha` 写死在数据集行里，那个 commit 的树不可变；基线的 ripgrep 带 `--hidden --no-ignore`，语料不受任何 `.gitignore` 摆布 |
| ground truth | 给定 `(base_sha, merge_sha)`，diff ∩ ctags 边界是确定性的 |
| 测试集组成 | `datasets/*.jsonl` 进 git，内容哈希钉在 `datasets/MANIFEST.json` |
| 评分与基线参数 | 常量写死在代码里 |

**`run` 会校验哈希，对不上就拒绝打分**，只有 `--allow-drift` 能绕过，绕过来的数不许报。
`build` 只是提案，`freeze` 才让它生效，而 `freeze` 强制要求 `--reason`——改一次记一次。

会变而且必须记录的：`collect` 抓到哪些 PR 取决于抓取时刻，ctags / ripgrep 版本随环境变。
所以每次报分都带 `dataset_version` 和三个工具的版本号。

```bash
python3 -m harness.run verify --split all     # 谁冻了、谁漂了、dataset_version 是多少
```

`--size medium|huge|all`、`--split train|holdout|all` 在每个子命令上都能用。

## 各步在做什么

**collect** 走 GraphQL 的 `closingIssuesReferences`，只收 merged 且关联了 issue 的 PR。
base commit 取 `mergeCommit.parents[0]`——squash 和 merge commit 两种情况都对。

**build** 先跑纯文本与元数据的过滤（不碰 git，所以 census 免费），再对活下来的 PR 算
ground truth：`git diff -U0` 拿 base 侧被改的行号区间，ctags 拿函数边界，两者相交。
新增的函数在 base 上不存在，自动落不到任何区间里，与「纯新增函数从函数级榜单剔除」
一致。PR 新增的**文件**同理从 `gt_files` 剔除：它们不在 base 树上，也就不在语料里，
留着只会给每个系统压一个够不到的天花板。

**泄露** issue 正文里是否直接出现了答案文件名或答案函数名。这种题 grep 一搜就中，测不
出语义检索的价值。挑题准则第 1 条要求评审拒掉它们，但**评审看不到答案，物理上判不了**
——所以这一步归装置：`build` 算完 ground truth 逐题机械复验，命中的直接丢弃，计入
`stats["leaked"]`。数据集里**不留标记字段**：留下来的题按定义全都没泄露，一个恒为 false
的字段和一个恒为 0 的 `leak_rate` 只会让人误以为那是个测出来的数。

判据分三档，都是纯字符串比对：

| 档 | 例 |
|---|---|
| 完整路径出现在正文 | `src/redis-cli.c` |
| 带扩展名的文件名出现在正文 | `SslHandler.java` |
| **去扩展名的主干**出现在正文，且主干是复合标识符 | `ByteToMessageDecoder`、`hive_partitioning`、`redis-cli` |

第三档是后加的，也是真正管用的那一档。没人在正文里写 `ByteToMessageDecoder.java`，都写
`ByteToMessageDecoder`——而 Java / C# / TypeScript 里类名就是文件名，这一个词就足以让任何
词法基线把答案排到第一。**复合**这个限定不能少：主干得含下划线、连字符或大小写驼峰。
`validation`、`connections` 这种单个小写词能匹配上百个文件，定位不了任何东西，当泄露处理
只会白扔好题。

实测这一档值多少：netty 50 道题里 21 道中招，把它们和干净题分开跑基线——

| | 泄露组 | 干净组 |
|---|---|---|
| bm25 file_mrr | 0.531 | 0.262 |
| grep file_recall@10 | 0.619 | 0.239 |
| grep func_recall@10 | 0.364 | 0.074 |

题面上写着答案，基线分就是两倍到三倍。

全量复验丢掉 51 道，700 → 638。**丢掉的不补**：候选池里排序靠后的题没经过盲审，拿它们
填数等于用没评过的题凑一个整数。要补回 50 道/仓库，得重跑一轮盲审。

丢了多少只在 `build` 的日志里（`"leaked": 21`）和 `MANIFEST.json` 的冻结理由里，那才是
它该待的地方——它是一次过滤动作的记录，不是数据集的属性。

**run** 对每条 query 把它自己 base commit 的语料铺出来，跑基线，算分，删掉语料。顺带
核对 ground truth 在不在这份快照里，两级各记一个数写进报分表：

- `gt_funcs_unreachable`——答案函数不在符号索引里。不为 0 就是限定名归一化两侧不一致。
- `gt_files_unreachable`——答案文件不在语料里。不为 0 就是有一批题谁都做不对。

这两个数不为 0，分数会安静地掉下去，看指标本身是看不出来的。

失败的 query 同理：抛异常的题不进平均，而失败与难度相关（快照越大越容易炸），只对活
下来的题取均值等于中途换卷子。所以 `report` 在失败率超过 2% 时直接拒绝出表，
`--allow-failures` 能绕过，绕过来的数不许报。

## 五个与 `benchmark.md` 不同的地方

文档说的是原则，这里是能在 CI 上跑的实现。五处偏离，都记在这里而不是藏在代码里：

**一、符号边界用 universal-ctags，不是 language server。** 文档写「git diff 行号区间 +
language server 的 `documentSymbol`」。七种语言在 CI 里拉起七个 language server 太重，
ctags 一个二进制覆盖全部七种。硬规则 3 要的是**与检索侧不同的实现**——检索侧是
tree-sitter，ctags 满足这一条，而且比 LSP 更满足：两者连解析器都不共享。

**二、向量基线是 `jina-embeddings-v2-base-code`，不是生产要用的 v4。** v4 是 3.8B，
2 核 runner 上跑不动。所以这个数字是**向量那一路的下限，不是生产质量的预测**，报分时
必须带这句话。超大仓库默认走 `pool` 模式（只嵌入 BM25 前 200 个函数），同时报 pool
本身的召回天花板；中等仓库走 `full`。

**三、Rust 与 TypeScript 的函数边界由花括号扫描器给出，不是 ctags。** ctags 的这两个
parser 一个 `end` 字段都不输出，`--fields=+ne` 在它们身上是空的。用「下一个 tag 的行号
减一」去补，等于凭空编造边界。所以 `symbols.py` 带一个花括号扫描器：它在 44,498 个
ctags 确实给了 `end` 的 C / Go / Java tag 上验证过，偏差 1 个；读不准的构造（raw string、
跨行模板字面量、含花括号的正则字面量）一律返回 `None`，对应的 tag 被丢弃并计数，绝不
猜测。丢弃率：kubernetes 0.00%、ClickHouse 0.03%、pandas 0.3%、tokio 1.4%。

**四、语料铺开用 `git archive` 流式解包。** blobless partial clone 上逐个 `cat-file`
会为每个缺失 blob 发一次网络请求，慢到不可用；`git archive` 一次性批量取回整棵树。
超大仓库因此和中等仓库一样，每个快照只花「一次批量拉取 + 解包」。

`git archive` 会执行被归档那棵树自己的 `.gitattributes` 里的 `export-ignore`——pandas 有
57 条，airflow 有 35 条，语料会被仓库自己的归档规则悄悄截断。`ensure_mirror` 因此往镜像的
`info/attributes` 写一行 `* -export-ignore`：该文件的优先级高于树内的 `.gitattributes`，
语料重新等于整棵树。

**五、基线的 ripgrep 必须带 `--hidden --no-ignore`。** 这两个不是调参，是把 ripgrep 的
默认行为关掉：它默认跳过点开头的路径，并且服从搜索根之上每一层的 `.gitignore`。而语料铺
在 `eval/.cache/snap/` 下，**在 realontext 自己的工作树里面**，于是 `eval/.gitignore` 的
`results/`、`raw/`、`candidates/`（都没有前导斜杠，匹配任意深度）会作用到语料上：

| 被吃掉的语料文件 | 原因 |
|---|---|
| elasticsearch 133 个（`.../inference/results/*.java`） | `eval/.gitignore` 的 `results/` |
| rust 28 个（`library/std/src/os/raw/`） | 同上，`raw/` |
| vscode 109 个、elasticsearch 46 个、ClickHouse 25 个 | 点开头的路径 |

换句话说：**改一行 `eval/.gitignore`，14 个仓库的语料一起缩水，而 `MANIFEST.json` 的哈希
纹丝不动**——`datasets/` 没变。冻结机制防的就是测试集悄悄变，这是它的后门。

当前 638 道题的 ground truth 没有一个落在这些黑洞里，所以历史分数没被污染；`run` 现在
把 `gt_files_unreachable` 算出来，下次再有就会当场叫。

## GitHub Actions

`.github/workflows/eval.yml`，手动触发（`workflow_dispatch`）：

| 输入 | 说明 |
|---|---|
| `stage` | `census` / `refresh` / `baselines` |
| `size` | `medium` / `huge` / `all`——两个规模档分开跑 |
| `split` | `train` / `holdout` / `all` |
| `limit` | 每仓库 query 上限，`0` 为不限 |
| `baselines` | `grep,bm25` 或加上 `vector` |

**`refresh` 只出提案，不喂分数。** 它重新抓取并生成数据集、打印与冻结版的差异，然后
停在那里——人来提交 `datasets/` 并跑 `freeze --reason`。`baselines` 阶段只读已提交的
冻结数据集，绝不现抓。两个阶段不相连，是刻意的：现抓即打分等于每次换一套卷子。

每个仓库一个 job（`fail-fast: false`），一个仓库炸不影响其他仓库。git 镜像、符号索引、
本地嵌入三份缓存按仓库分开。超大档会先清掉 runner 上的 Android SDK 和 .NET 腾磁盘。

**每周一自动跑一次 census**——可用 PR 数会随上游仓库变化，这个数掉下去要立刻知道。

## holdout

`repos.txt` 里 `split=holdout` 的 4 个仓库，调参期间不许跑。累计上限 5 次。规则见
`docs/benchmark.md`「holdout 使用协议」。

这条以前只写在文档里，靠自觉。现在 `run` 自己拦：碰到 holdout 仓库必须给
`--holdout-reason`，否则退出；`holdout-log.md` 里已有的行数就是计数器，满 5 次直接拒绝
跑。跑之前先把那一行写进日志，分数栏留给人跑完 `report` 再填。

```bash
python3 -m harness.run run --repo duckdb/duckdb --holdout-reason "阶段 1 验收"
```

## 目录

```
harness/
  common.py       repos.txt、路径、git 镜像、限定名归一化
  collect.py      GitHub GraphQL 抓取 + census
  build.py        过滤规则 + 数据集生成
  groundtruth.py  diff 行号区间 -> 函数
  symbols.py      universal-ctags 封装 + 花括号边界扫描（Rust / TypeScript）
  snapshot.py     base commit 语料铺开 + 符号索引缓存
  tokenize.py     两条基线的冻结分词（不与 lexical/ 共享）
  baselines.py    grep 基线、BM25 基线
  vector.py       本地 Jina 向量基线（无 API）
  manifest.py     冻结、校验、dataset_version
  score.py        Recall@k / Acc@k / MRR / NDCG，macro 平均，报分表
  run.py          CLI

datasets/         冻结的测试集 + MANIFEST.json，进 git
selections/       各仓库精选的高质量 Bug 题目定义及评审理由，进 git
curation-brief.md 题目双盲挑选的 6 大质量准则
candidates/       导出的候选 issue（不含答案，供子代理挑选），不进 git
raw/              抓取缓存，不进 git
results/          分数，不进 git
.cache/           镜像、语料、符号索引、嵌入，不进 git
```
