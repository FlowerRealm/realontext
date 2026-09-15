# 测试标准

本项目不使用公开基准作为验收标准，自建测试集。

理由与被否决的方案见文末。**先读「三条铁律」一节**——它决定了这套测试集是有信号还是自欺。

---

## 三条铁律

自建评测最容易死的地方不是指标选错，是**测试集本身没有信号**。以下三条任何一条破了，整套测试作废：

### 铁律一：query 由不知道答案的人写

用 **issue 正文**，不用 commit message，不用 PR 描述。

```
commit message   "Fix null deref in config parser"
真实 issue       "为什么加载这个 yaml 会崩"
```

前者是修复之后写的，作者已知答案，"null deref" 和 "config parser" 两个强信号直接喂给 BM25。用它当 query 会系统性高估检索质量，**且专门高估词法那一路**。

issue 正文是修复之前写的，由不知道答案的人写。这才是 agent 实际收到的输入分布。

### 铁律二：语料时间锚定

语料是 **PR 的 base commit 状态**，不是仓库当前状态。

修复之后的代码、提交、注释都不在语料里，检索器无法通过匹配「修复说明」抄答案。

开启提交摘要后（阶段 9），commit 历史同样必须截断在 base commit 之前。

### 铁律三：ground truth 是历史事实，不是人工标注

**答案 = 那个 PR 实际改了哪些文件和函数。**

不是「我认为应该改哪里」。历史事实不可争辩，也不会被无意识地往有利方向调整。

这一条同时消掉了人工标注的成本——整套测试集生成是纯 git + GitHub API 的自动过程，零标注。

---

## 数据来源

对每个选中的仓库：

```
枚举 closed + merged 的 PR
  ↓ 保留关联了 issue 的
  ↓ query        = issue title + body
  ↓ ground truth = PR diff 触及的文件与函数
  ↓ 语料状态      = PR 的 base commit
  ↓ 过滤（下节）
```

全部可从 GitHub API 与 git 取得，无 API 模型调用，构建阶段零成本。

### 过滤规则

过滤决定测试集的信号强度。以下全部剔除：

| 规则 | 理由 |
|---|---|
| issue 正文短于 200 字符 | 信息量不足以定位 |
| bot 开的 issue | 模板化，非自然语言 |
| diff 只触及测试 / 文档 / 格式 | 不是代码定位任务 |
| diff 触及文件数 > 10 | 意图不聚焦，ground truth 噪音大 |
| diff 触及文件数 = 0（纯配置） | 无代码可召回 |
| 触及 vendored / 生成文件 | 与 `ingest/` 的过滤规则一致 |
| issue 与 PR 间隔 < 1 小时 | 大概率是自己给自己开的事后补录 |
| diff 不修改任何已存在函数（纯新增文件 / 纯新函数） | 只从**函数级**榜单剔除，文件级保留。Agentless 在 SWE-bench Lite 上同样处理，300 条留 274 条 |
| 一个 issue 关联多个 merged PR | 只取最早 merge 的那个，否则同一 query 有多份互相矛盾的 GT |
| 同一 issue 出现多次 | 去重，query 唯一 |
| PR 与 issue 不在同一仓库 | 取不到一致的 base commit |

### 一个要报告而不是过滤的指标

**query 里直接出现了 ground truth 文件路径或函数名的比例。**

真实 issue 有时确实会写「`ConfigParser` 崩了」，这是真实分布的一部分，不该剔除。但如果这个比例很高，测试集就退化成字符串匹配任务，BM25 会虚高。

**每次报分必须同时报这个比例。** 它是测试集难度的体温计。

---

## 三级任务

分级对应本项目自己的模块分层，不照抄任何公开基准。

| 级 | 任务 | 测的模块 | ground truth |
|---|---|---|---|
| **L1 定位** | issue → 要改的文件与函数 | `ingest/` 到 `match/` 全栈 | PR diff 触及的文件与函数 |
| **L2 上下文** | issue → 为改好它还需要读什么 | `curate/` 的召回面 | L1 答案 + 其一跳调用邻居 |
| **L3 预算** | 固定 token 预算下的 L1 / L2 质量 | **`curate/` 的核心** | 同上 |

### L2 的 ground truth 是 proxy，必须写明

「为了改好它还需要看什么」没有历史事实可依——PR diff 只记录改了什么，不记录作者读了什么。

本项目用**一跳调用邻居**作为代理：对 L1 的答案函数，用 language server 取 `definition` 与 `references` 各扩一跳。

这是确定性的、可重现的，但**它是代理不是真值**。一个人类开发者实际读的东西可能更多或更少。

L2 的绝对分数没有意义，**只有同一套代理下的相对比较有意义**。文档里任何 L2 数字都必须带这个限定。

### L3 是本项目唯一真正的验收

`curate/` 的价值主张是「相同质量下更少 token」。协议只有一个方向：

```
固定    token 预算 ∈ {2K, 4K, 8K, 16K, ∞}
测量    该预算下的 L1 Recall@10 / L2 Recall@50
```

扫预算曲线：

| 预算 | L1 Recall@10 | L2 Recall@50 |
|---|---|---|
| 2K | | |
| 4K | | |
| 8K | | |
| 16K | | |
| 无限 | | |

**验收量是 B\***：满足 `Recall@10(B) ≥ Recall@10(∞) − 2 个百分点` 的最小预算档。B\* 越低，`curate/` 越有效。

容差写死 2 个百分点绝对值。没有容差，「达到无限预算档的质量」是一句可以事后解释的话。报分要同时给 `B*` 与 `Recall@10(∞)` 的绝对值——B\* 低但绝对质量崩了不算赢。

---

## 全分支测试

公开基准全部是单一 checkout 状态，测不了 D14。自建语料能测，这是自建的直接收益之一。

### 场景：重复劳动检测

「这个问题有人已经在别的分支上修好了，引擎找得到吗。」

这是索引全部分支的真实价值——不是抽象的「覆盖面更广」，而是**避免重复劳动**。只索引默认分支的系统在这个场景下必然失败。

### 构造

利用一个时间性质：**一个 PR 在 merge 之前，它的代码只存在于非默认分支。**

```
取一个 merged PR P，它关联 issue I
  语料    = P 的 base commit（默认分支）+ P 的分支 tip（此时尚未 merge）
  query   = I 的正文            ← 与 L1 同一批，不新造
  答案    = P 在分支 tip 上引入的代码
  对照组  = 只索引 base commit，召回必然为 0
```

**不需要为这个测试生成任何 query。** 复用 L1 的 issue 正文即可，三条铁律原样满足。

这一点很重要：如果 query 从目标代码本身推导（函数名、注释、docstring），query 和代码用的是同一批词，BM25 直接字符串匹配就命中——这个测试就算系统是垃圾也能通过，等于没测。复用 issue 正文绕开了这个陷阱。

### 指标是二元的，且判据必须是内容级

召回到 / 没召回到，不是 NDCG。价值主张是「能看到别人看不到的东西」，不是「排得更准」。

**命中 = 召回的 Chunk 的内容哈希等于分支 tip 上那一版。** 路径相同、函数名相同但内容还是 base 版本，不算命中。

这一条不能松。PR 大多修改的是已经存在的文件里已经存在的函数——按路径判或按函数名判，只索引 base commit 的对照组照样命中，分数不是 0。只有内容哈希判据下，「对照组为 0」才是构造保证的。

同时报一个文件级宽判据的数字作为对照，并注明它在对照组上不为 0，不能拿来证明能力。

这个测试**只能证明能力存在，不能证明能力好**。不要拿它当质量指标。

### 语料获取：fork 的 PR 分支也能拿到

开源项目的 PR 分支多在贡献者的 fork 上，不在 origin 的 `refs/remotes/origin/` 下。

GitHub 在 origin 仓库上暴露 `refs/pull/<n>/head`：

```bash
git fetch origin 'refs/pull/*/head:refs/remotes/pr/*'
```

fork 来的 PR 分支照样能从 origin 一次拉全，建语料不需要逐个 clone fork。

**这同时是 `ingest/` 的一个设计选项**：生产环境若也拉 `refs/pull/*/head`，「索引全部分支」在开源仓库上的覆盖率就从接近零变成基本完整。归 `ingest.md` 与 D14 决定，不在本文档范围内。

---

## 指标

| 指标 | 粒度 | 用在 | 定义 |
|---|---|---|---|
| Recall@k | 文件级 / 函数级 | L1 / L2 | 单 query `\|GT ∩ top-k\| / \|GT\|`，再对 query 取算术平均 |
| Acc@k | 文件级 / 函数级 | L1 | top-k 覆盖**全部** GT 记 1，否则 0，再平均 |
| MRR | 文件级 / 函数级 | L1 | 第一个命中 GT 的倒数排名 |
| NDCG@10 | 文件级 / 函数级 | L1 | 二元 gain（`rel ∈ {0,1}`） |
| 预算曲线 | Context Package token 数 | L3 | 见上节 |
| 内容级召回 | 二元 | 全分支 | 见上节 |

k 固定：文件级 `{1, 5, 10}`，函数级 `{5, 10, 50}`。L2 只报 Recall@50。

**平均方式是 macro**：先算每条 query 的分，再对 query 平均。不把所有 query 的命中池在一起算 micro——那样 GT 大的 query 会主导整体分数。这是 TREC / BEIR 的惯例。

**Recall@k 与 Acc@k 一起报。** Recall 宽松、方差小；Acc@k（LocAgent 的严格档）要求 top-k 覆盖全部 GT。agent 实际需要的是后者——漏掉一个改动点，补丁就是错的。

`|GT| > k` 的 query，Recall@k 的上界不足 1。不做修正，但报分时给出 `|GT|` 的中位数与分布。

NDCG@10 在二元 gain 下退化成位置加权的 Recall，只用于同一批 query 之间的横向比较，不与外部数字对照。

函数级和文件级都报。realontext 返回的是 Chunk（函数 / 类粒度），函数级是更直接的对照；文件级噪音更低、更稳定。

---

## 评分规则

指标名字不构成评分标准。**命中怎么判、基线怎么跑、验收怎么算，全部写死在这一节，与过滤规则同级冻结。** 留空的规则等于把打分权交回给判卷人，而判卷人就是出卷人。

### 命中判据

| 粒度 | 判据 |
|---|---|
| 文件级 | 仓库根相对路径**精确相等**。POSIX 分隔符，大小写敏感，不做重命名追踪 |
| 函数级 | **完全限定名精确相等**，格式 `path/to/file.ext::Outer::inner` |

不用行号区间重叠。行号随切块方案漂移，改一次切块历史分数就不可比；限定名对切块方案免疫。Agentless、LocAgent、SweRank 用的都是名字判据，不是行号判据。

两侧的限定名由不同实现产出（硬规则 3）：GT 侧是 git diff 的行号区间 ∩ **universal-ctags** 的符号边界，检索侧是 `parse/` 的 tree-sitter 切块自带的符号名。

（原方案写的是 language server 的 `documentSymbol`。改用 ctags 是为了能在 CI 里跑——七种语言拉七个 language server 太重，ctags 一个二进制全覆盖。隔离性反而更强：ctags 与 tree-sitter 连解析器都不共享。）两套实现要对得上，需要一份同样冻结的归一化规则：

- 去掉泛型参数、参数列表、返回类型：`Foo<T>::bar(int) -> R` → `Foo::bar`
- receiver 归一到类型名：Go 的 `(*T).M` → `T::M`，Rust 的 `impl Tr for U { fn m }` → `U::m`。每语言一行，写死在表里
- 嵌套一律用 `::` 连接，不管语言原生分隔符是 `.` 还是 `#`
- 大小写敏感，不做模糊匹配、不做编辑距离

**对不上的 GT 条目记为 `unmapped`，报告比例，不静默丢弃。** `unmapped` 超过 5% 说明归一化表有 bug，先修再报分——静默丢弃会系统性地丢掉最难的那批 query。

无法提取限定名的 Chunk（顶层语句、宏展开、模板实例、配置片段）只参与文件级评分，相应 query 从函数级榜单剔除并计数。

### ripgrep 基线

ripgrep 不排序，也不吃 200 字符的自然语言。从 issue 到 top-k 列表这一段构造**是基线本身的一部分**，冻结进 [`../eval/harness/baselines.py`](../eval/harness/baselines.py)，阶段 0 之后不许改：

1. **取词**：对 issue title + body 抽 `[A-Za-z_][A-Za-z0-9_]*`，长度 ≥ 4。保留代码块与 stack trace——它们是真实输入分布的一部分
2. **去噪**：英文停用词表 + 各语言关键字表，两张表进 git
3. **选词**：按 IDF 降序取前 20 个。IDF 在该 query 自己的 base commit 语料上算，纯统计，零成本
4. **检索**：`rg --no-heading --line-number --fixed-strings --ignore-case -w -f patterns.txt`
5. **排序**：文件得分 = Σ 命中词的 IDF，**同一个词在同一个文件只计一次**，否则一个高频词刷屏就赢。函数得分把命中行号映射进函数区间后同理。降序，平手按路径字典序

### BM25 基线

必须与 `lexical/` 完全隔离：不 import 其代码，不共用分词器，不共用切块，不共用数据库文件。基线表写「纯 FTS5」、阶段 4 也写 FTS5，那是同一条代码路径——切块或分词一改，基线跟着动，它就不再是锚。

| 项 | 固定值 |
|---|---|
| 文件级文档单位 | 整个文件（SWE-bench 的 BM25 baseline 即此） |
| 函数级文档单位 | GT 侧同一个 `documentSymbol` 切出的函数体，不走 `parse/` |
| 分词 | camelCase / snake_case 拆分，同时保留原词；停用词表同 ripgrep 基线 |
| 参数 | `k1 = 0.9`、`b = 0.4`（Pyserini / Anserini 默认，BEIR 通用取值） |
| query | issue title + body，同一套分词 |

参数写死不调。基线的价值在于它不动。

### holdout 使用协议

「只跑一次」执行不了——阶段 3、5、6、7 加最终验收就是 5 次。改成可执行的规则：

- 每个验收阶段允许跑一次，累计 ≤ 5 次
- 每次写一行进 `eval/holdout-log.md`：日期、测试集 commit、系统 commit、全部分数
- **不允许基于 holdout 结果回头改任何参数。** 改了就作废这批 holdout，重新划分
- train 与 holdout 的相对分差 > 15% 记为过拟合告警，写进报分

### 报分模板

一个分数单独出现没有意义。每次报分必须是这张表，缺项写「未跑」，不留空：

| 字段 | 说明 |
|---|---|
| 测试集版本 | `eval/` 的 git commit |
| 系统版本 | 代码的 git commit |
| 仓库数 / query 数 | train 与 holdout 分开计 |
| `\|GT\|` 中位数与分布 | 解释 Recall@k 的上界 |
| 标识符泄漏率 | query 里直接出现 GT 路径或函数名的比例 |
| `unmapped` 率 | 限定名对不上的 GT 比例 |
| ripgrep 基线 | 文件级 / 函数级 |
| BM25 基线 | 文件级 / 函数级 |
| 纯向量基线 | 阶段 3 起 |
| 本系统 | 同粒度 |
| holdout 分数 | 若本次跑了 |

### 这些规则的外部出处

自建测试集没有外部可比数字，但**规则可以不自创**。以下几条直接照搬业界通行做法，避免在评分口径上自己发明一套只对自己有利的：

| 规则 | 出处 |
|---|---|
| 文件路径精确匹配、函数限定名精确匹配 | Agentless / LocAgent / SweRank |
| Acc@k 要求 top-k 覆盖全部 GT | [LocAgent](https://arxiv.org/abs/2503.09089) |
| 纯新增函数的 PR 从函数级榜单剔除 | Agentless 在 SWE-bench Lite 上 300 → 274 |
| Recall@k 按 query macro 平均 | TREC / BEIR |
| BM25 `k1 = 0.9`、`b = 0.4` | Pyserini / Anserini 默认，BEIR 通用 |
| BM25 baseline 以整个文件为文档单位 | [SWE-bench 的 BM25 检索基线](https://huggingface.co/datasets/princeton-nlp/SWE-bench_bm25_27K) |
| issue 正文当 query | LocBench / SWE-bench（已由铁律一吸收） |

---

## 基线：外部的、免费的、作弊不了的锚

自建测试集最大的风险是没有外部参照——只跟自己比，永远看不出整条路走错了。

**每次报分必须同时报这三条基线：**

| 基线 | 成本 | 作用 |
|---|---|---|
| **ripgrep** | $0 | 下限。打不过它，整个项目没有存在理由 |
| **BM25（独立实现）** | $0 | 词法那一路单独的分数。**不共用 `lexical/` 的任何代码**，参数见「评分规则」 |
| **纯向量（无重排、无策展）** | $0（本地权重） | 隔离出 `match/` 与 `curate/` 的增量 |

第三条在阶段 0 就能跑，且不花钱：`jina-embeddings-v2-base-code`（161M）本地推理，不走 API。**它不是生产要用的 v4**（3.8B，CPU 上跑不动），所以这个数字是向量那一路的**下限**，不是生产质量的预测——报分时必须带这句话。

前两条不依赖任何 API，不依赖本项目的任何代码路径，**没有办法在调参时不小心把它们一起调高**。

分数上升而 ripgrep 基线同步上升 = 测试集变简单了，不是系统变好了。这是自建评测唯一可靠的自检手段。

---

## 防自欺的硬规则

自建评测的结构性风险：出卷、答题、判卷是同一个人。以下四条是补偿措施，不是建议。

### 1. 测试集冻结并进 git

仓库名单在 [`../eval/repos.txt`](../eval/repos.txt)，PR 列表与过滤规则同样进版本控制。改一次记一次，写明理由。

未冻结的测试集等于没有测试集——你会不自觉地往分数好看的方向漂。

**这一条由装置强制，不靠自觉。** `eval/datasets/MANIFEST.json` 钉住每个数据集文件的
内容哈希，打分前校验；对不上就拒绝出分。生成数据集的 `build` 只是提案，`freeze` 才让
它生效，且强制要求写明理由。CI 里重新抓取（`refresh`）与打分（`baselines`）是两个不
相连的阶段——现抓即打分等于每次换一套卷子。

每份报分带 `dataset_version`（全部冻结哈希的摘要）与 git / ripgrep / ctags 的版本号：
后两者会改变函数边界，属于必须记录的漂移源。

### 2. 留 holdout，调参期间不许看

**14 个仓库里 4 个划为 holdout**（见 `repos.txt` 的 `split` 字段）。调参、选模型、改切块全部只在另外 10 个上做。

具体跑几次、跑完记什么、分差多少算过拟合，见「评分规则 → holdout 使用协议」。

### 3. 测试集生成代码与检索代码不共享切块逻辑

ground truth 的函数边界由独立的实现确定（可以直接用 git diff 的行号区间 + language server 的 `documentSymbol`）。

共享切块逻辑的话，切块的 bug 会同时污染答案和作答，互相掩盖。

### 4. 每次报分带齐上下文

一个分数单独出现没有意义。必须同时给出：

```
测试集版本（git commit）
仓库数 / query 数
query 含 ground truth 标识符的比例
ripgrep 基线 / BM25 基线
holdout 分数（如果跑了）
```

---

## 测试仓库

名单冻结在 [`../eval/repos.txt`](../eval/repos.txt)。**改一次记一次**，理由写进该文件。

### 规格：每种语言两个，一个超大一个中等

| 语言 | 超大 | 中等 |
|---|---|---|
| C++ | `ClickHouse/ClickHouse` | `duckdb/duckdb` |
| Go | `kubernetes/kubernetes` | `prometheus/prometheus` |
| Rust | `rust-lang/rust` | `tokio-rs/tokio` |
| TypeScript | `microsoft/vscode` | `vuejs/core` |
| C | `systemd/systemd` | `redis/redis` |
| Python | `apache/airflow` | `pandas-dev/pandas` |
| Java | `elastic/elasticsearch` | `netty/netty` |

14 个仓库，7 种语言。

**两个规模档是有意的。** 只用超大项目，「小仓库也值得装」这个论点就没有对照——而它是明确的目标场景，不是边角案例。中等档同时用来观察检索质量随仓库规模怎么变。

### 语言分布是自建测试集最大的优势

公开基准（CORE-Bench、LocBench、SWE-bench 系列）以 Python 为主，测不出 C++ / Go / Rust / Java 的表现。自建语料没有这个限制，所以七种语言权重相同。

### 硬性准入条件

| 条件 | 为什么 |
|---|---|
| GitHub 原生工作流 | PR 在 GitHub 合并、issue 在 GitHub 开，否则抓不到数据 |
| PR 与 issue 有关联 | `Fixes #N` 或 GitHub linked issue，是 ground truth 的来源 |
| issue 正文是人写的自然语言 | 纯模板化的 issue 没有查询信号 |

被这三条排除的知名项目，记在 `repos.txt` 里防止有人再提：

```
git / linux / postgres / ffmpeg    只读镜像，开发在邮件列表
django                             issue 在 Trac，PR 关联的不是 GitHub issue
kafka                              issue 在 JIRA
curl / openssl                     PR 靠 cherry-pick 落地，GitHub 上显示 closed 不是 merged
```

curl 那一条是阶段 0 的 census 查出来的，不是事先想到的：它有 212 个 merged PR、15,923 个 closed PR，全部历史只筛得出 11 条可用 query。**「先数再抓」第一次跑就换掉了一个仓库**，理由记在 [`../eval/repos.txt`](../eval/repos.txt) 的变更记录里。

### holdout 划分

**4 个仓库（29%）划为 holdout**：`duckdb` · `rust-lang/rust` · `vuejs/core` · `apache/airflow`。

调参、选型、改切块全部只在另外 10 个上做，holdout 只在阶段验收时跑一次。

划分刻意跨了 4 种语言、两个规模档都有——这样 holdout 上的分差才能区分「过拟合到测试集」和「某个语言或某个规模档本来就不行」。

---

## 成本

构建测试集：**$0**。纯 git + GitHub API。

跑测试的成本 = 嵌入这 14 个仓库的语料。

每个 query 需要仓库在它自己 base commit 时刻的状态，但这些快照之间高度重叠，`store/` 的内容寻址去重直接吃掉——总量约等于「一份快照 + 时间跨度内的变更量」，不是「快照数 × 快照大小」。

按每仓库 100–200 个 query、base commit 跨 2–3 年估：

| 项 | 估算 |
|---|---|
| 语料 | 约 1.3 亿 token（去重后） |
| **嵌入成本** | **约 $2.3** |
| 耗时 | 付费档约 1 小时 |
| 索引 | int8 约 450 MB / fp32 约 1.8 GB |
| query 数 | 1,400 – 2,800 |

规模与 ContextBench（1,136 条人工标注）同级，成本是两位数美分量级。

**语料规模由仓库名单决定，完全可控**——这是自建相对公开基准的另一个收益。作为对照，CORE-Bench Level-2 全量语料 26.9 亿 token，约 22 小时、9.6 GB 索引，且名单不可裁剪。

### 什么时候需要重新掏钱

只有两件事会让已付的嵌入作废：

| 改动 | 是否重嵌 |
|---|---|
| **切块方案**（尺寸、合并规则、grammar 版本） | **是** |
| **provider / model / task / dim** | **是** |
| HNSW 参数、量化档位 | 否 |
| RRF 权重、MMR 参数 | 否 |
| 查询理解层（改写、HyDE、子查询） | 否，只重嵌 query，成本可忽略 |
| 重排开关或换型 | 否 |
| `curate/` 的全部参数 | 否 |

**两条推论：**

1. **切块方案必须在第一次付费嵌入之前定死。** 它现在挂在 [`open-questions.md`](open-questions.md) C6，应当在阶段 3 之前移入决策记录。
2. **D8 的索引指纹缺一项。** 它记录 `provider / model / task / dim`，但同样的四项配置换个切块方案，向量完全不同，指纹照样放行——正是 D8 自己描述的那种「静默且看似合理」的失败。指纹需要补上**切块方案版本**。

### 存储策略

开发用的小规模语料存 **fp32**，量化档位（C7）可以本地免费扫；正式验收的大规模语料直接存 **int8**。

`store/` 的 `chunks` 表里那份 embedding 不是 `vector/` 索引的冗余副本——**它是花钱买来的持久缓存**，usearch 索引是可随时从它重建的派生物。不要为了省空间删它。

---

## 执行顺序

| 阶段 | 跑什么 | 成本 |
|---|---|---|
| 0 | 建测试集；ripgrep 与 BM25 基线 | **$0** |
| 3 | L1，对照两条基线 | 一次嵌入 |
| 5 | L1 重排后重跑，量化重排增益 | 可忽略 |
| 6 | **L3 预算曲线** | $0（复用嵌入） |
| 7 | L2（需要 language server 算一跳邻居） | $0 |
| — | 全分支测试 | 视语料而定 |
| 验收 | 在 holdout 上跑一次 | 一次嵌入 |

**阶段 0 不可跳过，且完全免费。** 打不过 ripgrep 基线就停下来查原因，不要继续加功能。

装置在 [`../eval/`](../eval/README.md)，CI 在 [`.github/workflows/eval.yml`](../.github/workflows/eval.yml)。中等档与超大档分开跑，每个仓库一个 job。实现与本文档的三处偏离（ctags、本地 v2-base-code、`git archive` 铺语料）记在 `eval/README.md`，不藏在代码里。

### 阶段 0 的落地基准

- **仓库与题目规格**：经过全量普查（census）与独立子代理盲审（6 大缺陷报告质量准则），最终锁定为 **14 个仓库 × 50 道题 = 700 道题目**。
- **题目性质**：全部为真实的 **Bug 缺陷定位题**（排除了 feature request、功能提议和 RFC），排除模板噪音。
- **零泄露**：issue 正文直接点名答案文件或答案函数的题目全部剔除——这种题 grep 一搜就中，测不出语义检索的价值。盲审准则第 1 条要求评审拒掉它们，评审判断之后再由装置逐题机械复验（`leak_path` / `leak_symbol` 对着真实 ground truth 比对），没过的踢掉换备选。700 道题的泄露数为 0。
- **精选溯源**：题目明细及打分入选理由存放在 [`../eval/selections/`](../eval/selections/)，由内容哈希钉死在 `MANIFEST.json` 中统一锁定版本。

### 阶段 0 的前置事项

---

## 作为产品功能

这套测试不只是内部 QA。

```bash
realontext eval --repo github.com/org/repo
```

用户在**自己的仓库**上跑同一套流程：自动抓 issue-PR 对、建时间锚定语料、出 ripgrep 基线与 realontext 的对照。

README 已经承诺了这件事——「先跑基线再决定要不要用」。公开基准给不了这个答案，因为用户关心的不是 632 个 Python 仓库上的排名，是**自己这个仓库上值不值得装**。

这条把评测框架从成本变成了卖点。实现时与内部测试集共用同一套代码，只是仓库列表换成用户的。

---

## 被否决的方案

### 公开基准作为验收标准

调查过四个，全部不作为验收标准使用。

| 基准 | 是什么 | 否决理由 |
|---|---|---|
| **CORE-Bench** | 三级代码检索基准，5,061 query / 9.38M 块 / 632 仓库 | ground truth 标在它自己的块上（AST + LangChain，平均 1,005 字符）。要能算 NDCG@10 就得把 `parse/` 的切块改成迎合它——**让基准反过来决定架构**。且语料以 Python 为主 |
| **Agent Retrieval Bench** | 427 样本 / 25 仓库，BCY@4k/8k/16k/32k 预算指标 | BCY 是**文件级** token packing。`curate/` 的多级保真（20–60 token 的签名档 vs 全文档）在文件级指标下几乎不产生差异，测不出来 |
| **ContextBench** | 1,136 任务 / 66 仓库 / 8 语言，人工标注 gold context | 语言覆盖是唯一超过自建方案的地方。但指标是 context recall / precision，仍不是 token 效率 |
| **LocBench** | 560 issue / 163 仓库，函数级 Acc@k | 同样以 Python 为主。方法论（issue 正文当 query）已被本文铁律一吸收 |

**共同的问题**：三条铁律全都满足，但**测的都不是本项目的差异化**。`curate/` 的 token 效率和全分支能力，四个基准加起来覆盖不到。

### 保留的一项：CoIR 只作选型参考

选嵌入 Provider 时读 MTEB 榜上的 CoIR 分数（Jina vs Voyage）。

**读榜，不跑。** 零成本、零基础设施。且它是 query→snippet 级别，大致对应「表示质量」，**不能当系统级验收标准**——片段级分数高不代表仓库级不崩。

### commit message 当 query

本项目最早的方案。被铁律一和铁律二直接否决：

- commit message 是事后写的，已含答案信号，且专门虚高 BM25
- commit 历史同时被索引，构成数据泄漏

记在这里防止有人再走一遍。

### 人工标注 ground truth

标注质量高，但一个人标不出有统计意义的规模（ContextBench 有 1,136 条人工标注），且引入标注者自己的偏见——**出卷人就是答题人**。

用历史事实（PR diff）替代，规模无上限，且不可争辩。代价是 L2 只能用代理，已在上文写明。

---

## 已知缺口

| 缺口 | 说明 |
|---|---|
| 无外部可比数字 | 放弃公开基准的直接代价。不能声称 SOTA，只能声称「在这些仓库上比 ripgrep / BM25 好多少」 |
| L2 ground truth 是代理 | 一跳调用邻居 ≠ 开发者实际读的内容。绝对分数无意义 |
| 全分支测试只能证明能力存在 | 对照组分数由构造决定必然为 0，测不出质量高低 |
| 出卷答题同源 | 靠冻结、holdout、免费基线三条补偿，不能完全消除 |
| API-only 无法微调 | 公开数据显示 in-domain 微调对这类任务收益接近翻倍。本项目受 D5 约束无法微调，必须靠 `match/` 的多路召回重排与 `curate/` 的策展补回来 |

---

## 相关文档

[`roadmap.md`](roadmap.md) 阶段划分 · [`open-questions.md`](open-questions.md) 未决问题 · [`00-decisions.md`](00-decisions.md) D8 索引指纹 / D16 `curate/` 是核心交付物
