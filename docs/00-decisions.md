# 决策记录

按时间顺序记录已定的技术决策。每条包含选择、理由，以及被否决的方案。

否决理由和选择理由同样重要——它们防止后来的人重走一遍已经走过的死路。

---

## D1. 实现语言：C++

C++20。

### 理由

项目的依赖几乎全是原生 C/C++ 库：tree-sitter 是 C，SQLite 是 C，libgit2 是 C，CRoaring 是 C，BLAKE3 是 C，usearch / hnswlib 是 C++ header-only，protobuf 有一流 C++ 支持。

其他语言实现这个项目，大部分时间在包装这些库。C++ 直接用原版。

### 否决：Rust

最初倾向 Rust，唯一理由是符号图方案打算用 stack-graphs（Rust 实现）。

D4 把符号图改成消费 SCIP（protobuf，语言无关）之后，这个理由消失了。

### 代价

`tantivy` 没有 C++ 对等物。用 SQLite FTS5 替代，BM25 内置，功能够用，而且少一个依赖。这是唯一需要妥协的地方。

---

## D2. 目标语言：多语言，不限定

工具本身用 C++ 写，但索引的目标仓库可以是任意语言。

早期讨论中一度误设为"只服务 C++ 仓库"，已纠正。

---

## D3. 分支策略：索引项目的全部分支

不是"跟随用户当前 checkout 的分支"，而是把一个项目的所有分支都建立索引。

### 这比 Augment Context Engine 更进一步

Augment 是 per-user per-branch，只跟着开发者当前的工作状态走。没人 checkout 的分支它看不见。

### 可行性

朴素实现会乘以分支数，经济上不成立（大型仓库 20 个分支的嵌入费用约 $600）。

内容寻址去重后成本降到 1.04 倍（约 $31）。实现方案见 `modules/store.md`。

---

## D4. 符号图：消费 SCIP，不自己做语言分析

> **状态：实现选择已被 D15 取代。** 边界原则（不自己做语言分析）继续有效，数据来源从 SCIP 改为 language server。以下内容保留作为决策历史。

系统边界：**realontext 不理解任何编程语言，它只理解 SCIP。**

外部 indexer 产出 `index.scip`（protobuf），realontext 解析、存储、查询。

### 现成的 indexer

`scip-typescript`、`scip-java`、`scip-python`、`scip-clang`、`rust-analyzer`（原生输出 SCIP）、`scip-ruby`、`scip-dotnet`、`scip-dart`、`scip-php`、`scip-go`。

一个都不用自己写。

### 这个边界的价值

新增一门语言支持等于新增一条 indexer 调用配置，不碰核心代码。语言分析的复杂度全部留在边界之外。

反面是在核心里写 `if (lang == "cpp") {...} else if (lang == "python") {...}`，那条路会长出无穷的分支。

### 否决：stack-graphs

GitHub 开源，增量、免构建，是理论上更优的方案。

但官方只支持 Python / JavaScript / TypeScript / Java，覆盖面不够。且它是 Rust 实现，与 D1 冲突。

### SCIP 的已知缺陷及其无关性

SCIP indexer 分析整个项目构建完整符号图，改一个文件需要重跑全项目。

对增量索引是硬伤——但本项目接受"重跑整个仓库"的成本，所以不构成阻碍。

### 降级档

没有对应 indexer、或拿不到构建配置的仓库，退到 tree-sitter `tags.scm`（按符号名匹配，精度低但零配置）。

**只做两档：精确（SCIP）和兜底（tree-sitter tags）。** 不要做中间态，中间态只会在代码里长出分支。

---

## D5. 模型：全部走 API，不自部署

无自部署推理能力，嵌入、重排、摘要全部调用第三方 API。

### 影响

**正面。** llama.cpp 依赖整条砍掉，没有 GGUF、没有 GPU 后端矩阵（CUDA / ROCm / Metal / Vulkan）、没有分发噩梦。单二进制目标反而更容易达成。

**负面。** 成本变成真实约束，速率限制成为一等工程问题，代码内容离开用户机器。

---

## D6. Provider：Jina 优先，Voyage 备选，DeepSeek 做摘要

| 用途 | 选择 | 备选 |
|---|---|---|
| 嵌入 | `jina-embeddings-v4` + `task=code` | `voyage-code-4` |
| 重排 | `jina-reranker-v3.5` | Cohere Rerank v3.5 |
| 摘要 | `deepseek-flash` | Claude Haiku 4.5（贵 15 倍） |

Jina 一个 API key 通用于 Embeddings 和 Reranker，额度共享——比原计划的 Jina + Cohere 少一个 provider、一套凭据、一套限流逻辑。

详见 `modules/model.md`。

### 否决：SweRankEmbed / SweRankLLM

Salesforce 的方案，SWE-Bench-Lite Acc@10 达 82.12%，是 issue localization 的 SOTA。

否决原因：训练集 SweLoc 只用公开 Python 仓库构建，对多语言场景不适用（D2）。且只有开放权重，无托管 API，与 D5 冲突。

### 注意：两个同名的 Jina 模型不是一回事

- `jina-code-embeddings-0.5b` / `1.5b`：HuggingFace 开放权重，代码检索 79.23%，持平 voyage-code-3。**未确认在 Jina 托管 API 上提供。**
- `jina-embeddings-v4` + `task=code`：托管 API 上的代码适配器。**本项目用的是这个。**

---

## D7. 嵌入维度固定为 1024

`jina-embeddings-v4` 默认 2048 维，可截断至 128。`voyage-code-4` 支持 Matryoshka 2048 / 1024 / 512 / 256。

两边的交集里选 1024。

### 理由

将来从 Jina 切到 Voyage 时，存储布局、HNSW 参数、磁盘占用完全不变，只需要重新嵌入，不动 schema。

用默认 2048 的话，切换时连存储层都要改。

顺带比 2048 省一半存储和内存，Matryoshka 的设计目的就是让这种截断的质量损失极小。

---

## D8. 索引带 provider 指纹，拒绝混用

索引元数据记录四项：`provider` / `model` / `task` / `dim`。

启动时与配置比对，任何一项不一致则**拒绝启动**，明确报错要求全量重嵌。

### 理由

不同 provider 的嵌入向量在不同的空间里。混用能算出余弦相似度，但物理意义为零。

失败形态是静默的：检索照常返回结果，不报错，质量下降但看起来合理。没有 eval 的话永远发现不了。

**静默且看似合理的错误比崩溃危险得多。**

### 明确不做

不做自动迁移，不做混合模式兼容。本项目接受重跑仓库的成本，让它硬失败是唯一正确的行为。

---

## D9. 接入方式：默认轮询，webhook 可选

GitHub webhook 打不到用户的本地服务。

三条路：轮询、隧道（cloudflared / tailscale funnel / ngrok）、混合。

**默认轮询。** 让用户为了装个索引器先配隧道，采纳率直接归零。webhook 作为可选加速。

详见 `modules/ingest.md`。

---

## D10. 不写成本计算代码

一度计划做 `--estimate` 干跑模式，扫描仓库、统计 token、报价。

**否决。** 成本数字写进 README 就够了，用户按自己的仓库规模比照即可。写一套代码去算一个用户一辈子只看一次的数字，不值。

运行时的 token 计数器仍然保留（见 `roadmap.md`），那是运维需要，不是报价需要。

---

## D11. 不做多租户隔离

团队多人共享同一份索引，所有人看到全部内容。

### 被消掉的复杂度

Augment 的 Proof of Possession（客户端发密码学哈希证明持有代码）、租户级索引分片共享、BigTable 存储层——这些的存在前提是多租户 SaaS，本项目没有这个前提。

不是降级，是前提不成立。

### 每用户私有的部分

只有两样：当前分支指针，以及未提交文件的临时覆盖层（dirty overlay）。量很小，不进全局索引。

---

## D12. 不追求亚秒延迟

Augment 的补全延迟低于 220ms、TTFT 低于 120ms，为此建了自研推理栈和 GCP AI Hypercomputer。

那是"每次击键都要补全"的要求。本项目服务的是 agent 检索——一次任务几次查询，延迟预算是秒级。

整块砍掉。

---

## D13. Project 与 Repository 一一对应

一个 Project 就是一个 git Repository。跨仓库不在范围内。

### 影响

- `Chunk` 去重作用域限于单个 Repository
- 符号图不跨 Repository 连边
- 数据结构键保持 `(repo, branch)`，不引入 `project_id`
- 微服务场景：每个服务起一个实例，实例之间互不可见

### 否决：Project 作为多个 Repository 的集合

跨仓库去重（共享库只嵌入一次）和跨仓库符号图确实有价值，SCIP 本身也支持跨仓库符号。

否决原因：「当前分支」会从单值变成每仓库一个分支的元组，检索前必须先确定这个元组。这个元组需要一个新概念、一套新的选择 UI、以及一套新的失效规则。复杂度不由当前需求支撑。

### 否决：预留 project_id 列

为尚未确定要做的功能预留 schema，属于臆想出来的需求。真要做多仓库时再迁移，本项目接受重跑仓库的成本（见 D8），schema 迁移同理。

### 代价

README 原先把「跨服务」列为该工具值得存在的场景之一，此说法已删除。

---

## D14. 索引全部分支，不做新鲜度过滤

`git for-each-ref refs/remotes/origin/` 返回什么就索引什么。不按提交时间过滤，不按 PR 状态过滤，不要求用户配置 allowlist。

### 理由

D3 的差异化卖点是「索引项目的全部分支」。加任何过滤条件都会让这句话变成有条件的，而条件本身需要用户理解和调参——那是把工具的复杂度转嫁给用户。

零配置开箱可用优先于成本可预测。

### 代价：成本模型不再是常数

去重率取决于分支的陈旧程度。陈旧分支持有已不在 main 上的旧版本文件，独有 chunk 随分支年龄增长。

| 分支状态 | 相对单分支的总量 |
|---|---|
| 全部近期拉出 | 约 1.0 倍 |
| 混合，最老约一年 | 1.5–2 倍 |
| 大量多年残留分支 | 3 倍以上 |

原先文档里写的「20 个分支 = 1.04 倍」只在第一行成立，已从 README 和架构文档中删除。

### 补偿措施

系数可以用纯 git 免费测出，不需要任何 API 调用。README 提供命令，用户在花钱前自行测量。

启动时输出实测的分支数和唯一 blob 数。这是运行日志，不是成本预估器——不做美元换算，与 D10 不冲突。

### 否决的三个方案

**按提交时间窗口过滤**（如 90 天内有提交）：成本可预测，纯 git 实现。否决原因是引入了一个需要调参的旋钮，且休眠后被拾起的分支要等重建。

**默认分支 + 有开放 PR 的分支**：语义最准，噪音最低。否决原因是绑定 GitHub API，GitLab / Gitea 要另写适配，且看不到未推 PR 的分支。

**用户配置 pattern allowlist**：否决原因是 pattern 不限制数量（`feat/*` 可能展开成 300 个分支），仍然需要时间窗口兜底，等于两套机制。

### 遗留风险

分支数极多的仓库（数百个以上）首次索引时间和费用都会显著上升。这是明确接受的取舍，写在 README 里让用户自己判断。

---

## D15. 符号关系用 Language Server，不用 SCIP

撤销 D4 的实现选择，保留 D4 的边界原则。

### 边界原则不变

realontext 不做任何语言分析，符号关系从外部获取。

### 实现改为 LSP

| | SCIP | LSP |
|---|---|---|
| 计算时机 | 预先算出全图 | 查询时按需问 |
| 全部分支（D14） | N 次全项目分析，不可行 | 只问已 checkout 的分支 |
| 持久化 | 需要 symbol 表 + edge 表 | 不存储 |
| 外部依赖 | 每语言一个 indexer 二进制 + protobuf | language server，用户机器上通常已在运行 |

### 决定性理由

符号关系只对**本次检索的候选**查询，不需要全图。top-50 候选查一轮 definition / references，毫秒级。

D14 确定索引全部分支后，SCIP 的预计算模型直接失效——500 个分支就是 500 次全项目分析。

### 被消掉的东西

- SCIP indexer 的分发问题（原文档最大的待定风险）
- protobuf 依赖
- `symbols` / `symbol_defs` / `edges` 三张表
- 「边跨分支复用」的整套设计
- `compile_commands.json` 在核心路径上的硬要求

最后一条需要说明：问题没有消失，只是移回用户机器。clangd 同样需要 `compile_commands.json`，但那是开发者为自己的项目早已配好的东西。这是复用一份已付的成本，不是要求用户为本工具再付一次。

### 代价

- 只有已 checkout 的分支能获得符号扩散。其余分支只有向量和 BM25 两路召回
- language server 首次打开大项目需要数分钟建立自身索引
- `definition` 和 `references` 普遍支持，`callHierarchy` 各语言支持度参差

第一条与 Augment 持平——ACE 的符号图同样只覆盖用户当前分支。

### 现成的参考实现

- [multilspy](https://github.com/microsoft/multilspy)（微软）：Python LSP 客户端库，明确为向 LLM 提供静态分析结果而设计
- [Serena](https://github.com/oraios/serena)：MCP 工具包，LSP 后端，30+ 语言，暴露符号级操作

本项目用 C++ 自行实现精简客户端（只需 `definition` / `references` / `documentSymbol`），或在早期直接依赖外部工具。取舍见 `roadmap.md`。

---

## D16. `curate/` 策展是核心交付物，不是后续增强

目标是 Augment Context Engine 的开源平替，不是一个精确的代码导航工具。

### 模块中的位置

```
serve/
  ↑
curate/  playbook/     ← 本决策
  ↑
match/
  ↑
vector/  lexical/  lsp/
  ↑
store/
  ↑
ingest/  parse/  model/
```

Language Server 只覆盖 `lsp/` 一个模块。只做到 `match/` 得到的是一个检索器，不是上下文引擎。

### 证据

Augment 在 Terminal Bench 2.0 上相对 Claude Code：token 少 32%、花费低 33%、**准确率打平**。

准确率打平意味着召回没有赢。省下的 32% 来自不返回无用内容，即 `curate/` 的贡献。

### 对工具契约的影响

`codebase-retrieval` 的入参不能只有 query：

```
query          任务描述
token_budget   调用方给出的上限
session_id     跨调用去冗的标识
```

没有预算，`curate/` 没有优化目标；没有会话标识，跨调用去冗没有落点。

这两个参数是架构性的，不能事后添加——`match/` 的输出量要为 `curate/` 预留选择空间（交出约 50 条而非 5 条）。

### 压缩用语法树，不用模型

多级保真中的签名档和引用档由 `parse/` 直接提取，确定性、零成本、零延迟。

在查询路径上调用模型做压缩，要为每次检索付钱和付延迟，方向错误。摘要档只用于已有离线摘要的对象（commit 记录）。

---

## D17. 会话级去冗靠知识库蒸馏，不靠记账扣留

### 问题

agent 的上下文由 agent 自己管理，realontext 看不到。若引擎记账认为某内容「已提供过」而略去，但 agent 已经压缩掉了那段上下文，结果是引擎静默扣留了 agent 实际需要的信息。

失败静默、不报错、会话越长越容易触发——而长会话恰恰最需要这个工具。

### 决定

不记录「发过哪些 chunk」。服务端维护一份持久知识库（playbook），重复命中时返回**提炼后的条目加引用**，而不是原文重发，也不是略去。

**永不扣留，只做替换。** 条目比原文短一个数量级，但信息密度更高；agent 看到引用即知原文存在，可通过 `expand` 要回。

失败模式从「静默丢失」降级为「给了一个更浓缩的版本」，后者可恢复。

### 方法来源

[ACE 论文](https://arxiv.org/abs/2510.04618)（Stanford + UC Berkeley + SambaNova，ICLR 2026，[开源实现](https://github.com/ace-agent/ace)）。

核心论点：**上下文不该变得更短，该变得组织更好。**

与 Augment Context Engine 撞名，是两个不同的东西。详见 `modules/playbook.md`。

### 关键约束：知识库允许变大

论文对整体重写的批判有具体数据：一次单步重写把上下文从 18,282 token（准确率 66.7%）压到 122 token（准确率 57.1%）。这个现象叫 context collapse。

因此**不要为了保持简洁而定期总结压缩知识库**——那正是论文测出会掉分的做法。只做增量追加与原地更新，去重靠语义嵌入。

### 否决的三个方案

**服务端记账后直接略去**：省 token 最多，但就是上述静默扣留。

**调用方声明 `already_have`**：状态归属正确，但要求 agent 侧配合。大多数 agent 不会配合，等于功能不存在。

**完全不做跨调用去冗**：引擎无状态、最好测试，但长会话下同一文件会被完整返回多次，反复吃掉预算。

### 遗留风险

计数器的信号来源未解决（见 `open-questions.md` A1）。没有信号，grow-and-refine 退化为只增不减。

---

## D18. 客户端 / 服务端拆分

### 强制拆分的三个事实

| 事实 | 归属 |
|---|---|
| language server 需要真实工作树（D15） | 客户端 |
| 未提交文件按定义在本地 | 客户端 |
| 索引、知识库、API key 共享且昂贵 | 服务端 |

### 划分

**客户端**：MCP 端点、上报分支与 dirty overlay、管理 language server、符号扩散、回传会话轨迹。**不持有 API key。**

**服务端**：`ingest/` `parse/` `model/` `store/` `vector/` `lexical/` `match/` `curate/` `playbook/` 全部。

### 同一个二进制，两个角色

```
realontext serve   --repo github.com/org/repo
realontext client  --server http://host:port
```

单机模式下客户端可直接拉起本地服务端，用户看到的仍是一条命令。

团队模式收益：一份索引、一个 API key、一份知识库，N 个开发者共享，成本不随人数增长。

### 检索是两阶段往返

符号扩散在客户端、召回重排在服务端，所以一次检索两个来回。这样符号扩散出的候选才能进入重排和策展；若只在末尾拼接，它们既不参与排序也不占预算，等于绕过 `match/` 的排序和 `curate/` 的组装。

延迟预算是秒级（D12），可以接受。无 language server 时退化为单次往返。

### 与 D11 的关系

拆分的动因是**能力边界**（谁能看到工作树、谁持有密钥），不是安全边界。服务端仍假设所有客户端可信，不做权限隔离。

### 隐私影响

未提交代码会离开客户端机器。`cross/privacy.md` 的路径过滤与凭据扫描必须在**客户端发送前**执行。

详见 `modules/serve.md`。

---

## D19. 知识库存在服务端，不写进用户仓库

grill-with-docs 把 `CONTEXT.md` 和 `docs/adr/` 写进用户的 git 仓库，作为用户资产接受 review。本项目不这么做。

### 理由

知识库是自动累积的机器产物，条目数会到千级，且带计数器等运行时元数据，不适合进 git diff。

### 代价

用户无法直接 review 和修订。**一条错误条目会永久污染检索结果。**

必须提供导出、搜索、删除、封禁接口，否则这是一个无法排查的质量黑洞。接口尚未设计，见 `open-questions.md` A4。

---

## D20. 自建测试标准，不用公开基准验收

测试集从 14 个仓库的 issue-PR 对自动生成，不使用 CORE-Bench 等公开基准作为验收标准。

完整方法见 `benchmark.md`，仓库名单见 `../eval/repos.txt`。

### 方法论不自造，语料自造

三条铁律直接来自 CORE-Bench 与 LocBench 的共同做法：

| 铁律 | 内容 |
|---|---|
| 一 | query 用 **issue 正文**，修复之前写的，由不知道答案的人写 |
| 二 | 语料锚定在 **PR 的 base commit**，修复后的代码不在里面 |
| 三 | ground truth 是**历史事实**（那个 PR 实际改了什么），不是人工标注 |

放弃公开基准不等于放弃它们验证过的方法论。换掉的只有语料。

### 否决公开基准的四个理由

**1. ground truth 的粒度会反向决定架构。**

CORE-Bench 的答案标在它自己切的块上（AST + LangChain，平均 1,005 字符）。要能算 NDCG@10，就得把 `parse/` 的切块改成迎合它——**让基准决定架构**，而不是让现实决定架构。

**2. 测不到差异化。**

`curate/` 的 token 效率和全分支能力（D14），调查过的四个公开基准加起来覆盖不到。Agent Retrieval Bench 的 BCY 最接近，但它是**文件级** packing，多级保真（20–60 token 的签名档 vs 全文档）在文件级指标下几乎不产生差异。

**3. 语言偏向。**

CORE-Bench、LocBench、SWE-bench 系列均以 Python 为主。本项目声称支持多语言（D2），这些基准测不出其中六种。自建语料七种语言权重相同。

**4. 自建评测本来就是产品功能。**

README 承诺「先跑基线再决定要不要用」。`realontext eval --repo <url>` 让用户在**自己的仓库**上跑同一套流程。用户关心的不是别人 632 个 Python 仓库上的排名，是自己这个仓库上值不值得装。与内部测试集共用一套代码。

### 代价：没有外部可比数字

**不能声称 SOTA。** 只能声称「在这 14 个仓库上比 ripgrep / BM25 好多少」。

D5（API-only 无法微调）造成的差距也因此无法直接量化——没有微调模型的对照组。

### 补偿：三条免费基线 + 四条硬规则

自建评测的结构性风险是出卷、答题、判卷同一个人。补偿措施：

- **ripgrep / BM25 / 纯向量三条基线**，每次报分必带。前两条不碰本项目任何代码路径，调参时调不动。分数涨而基线同步涨 = 测试集变简单了
- **测试集冻结进 git**，改一次记一次
- **4/14 仓库划为 holdout**，调参期间不许看
- **切块逻辑隔离**：测试集生成与检索实现不共享切块代码，否则切块的 bug 会同时污染答案和作答

### 否决的其他方案

**人工标注 ground truth**：质量高，但一个人标不出有统计意义的规模（ContextBench 有 1,136 条人工标注），且出卷人就是答题人，引入标注者偏见。用历史事实替代，规模无上限。

**commit message 当 query**：本项目最早的方案。commit message 是事后写的、已含答案信号、专门虚高 BM25；且 commit 历史同时被索引，构成数据泄漏。被铁律一与铁律二同时否决。

**保留 CoIR 作选型参考**：选嵌入 Provider 时读 MTEB 榜上的分数，**读榜不跑**。零成本零基础设施，但它是 query→snippet 级别，不能当系统级验收标准。

---

## 待定

- 项目名称（`realontext` 是占位）
- 许可证
- 文档语言：当前为中文，公开发布时是否补英文版
- 知识库条目的人工纠正接口形态（见 `open-questions.md` A4）
