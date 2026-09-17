# realontext

给编码 agent 用的代码上下文引擎。语义索引 + 符号图 + 提交历史，通过 MCP 暴露给任意 agent。

自部署，开源，无收费机制。用户自带 API key。

## 它解决什么问题

编码 agent 靠 grep 找代码，在大仓库上会失效：搜索空间太大、跟不动跨文件的引用链、处理不了模糊的自然语言输入。

realontext 给 agent 一个检索工具，输入自然语言任务描述，返回一个在 token 预算内组装好的上下文包。

```
serve/                         MCP 契约 · HTTP · 客户端/服务端
  ↑
curate/   playbook/            预算组装 · 知识库
  ↑
match/                         查询理解 · 多路召回 · 融合 · 重排
  ↑
vector/   lexical/   lsp/      向量库 · 倒排 · 语言服务
  ↑
store/                         SQLite · 内容寻址 · bitmap
  ↑
ingest/   parse/   model/      git 采集 · 语法解析 · 模型调用
```

与同类方案的两点区别：

- **索引一个项目的全部分支**，不只是当前 checkout 的那个
- **有 `curate/` 策展模块**。没有它，整个系统就是一个检索器，不是上下文引擎。Augment 在这一层拿到的是 token 少 32%、准确率打平——省下的全部来自不返回无用内容

Language Server 类工具（Serena、multilspy）只覆盖 `lsp/` 一个模块。它们是本系统的数据源，不是替代方案。

## 它不解决什么问题

十万行以下的单体仓库，`ripgrep` 加上 agent 自己的探索循环已经够用，装这个是给自己找事。

Augment 官方在 SWE-bench 的技术报告里写过同样的话：

> "for SWE-bench tasks this was not the bottleneck – 'grep' and 'find' were sufficient."

索引的价值在用户输入本身模糊、以及反复在同一个仓库上工作的场景。项目提供 eval 框架（`realontext eval --repo <url>`），在你自己的仓库上跑出 ripgrep 基线的对照，再决定要不要装。

跨仓库不在范围内。一个 Project 对应一个 Repository，微服务场景需要每个服务起一个实例，实例之间互不可见。

## 快速开始

```bash
# 服务端：索引、知识库、API 调用
realontext serve --repo github.com/org/repo --token $GITHUB_TOKEN

# 客户端：跑在开发者机器上，提供 MCP 端点
realontext client --server http://localhost:7777
```

同一个二进制，两个角色。单机使用时客户端会自动拉起本地服务端。

然后把客户端的 MCP 端点接到 Claude Code / Cursor / 任意 agent。

### 构建

```bash
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
cmake --preset default && cmake --build build && ctest --preset default
```

当前进度见 `docs/roadmap.md`：阶段 1 完成，阶段 2 进行中。只有 CLI，还没有 MCP。

```bash
realontext index --db repo.db --git repo.git            # 索引镜像的全部分支，免费
realontext embed --db repo.db --dry-run 1               # 待嵌入的块数、字节、估计 token
JINA_API_KEY=... realontext embed --db repo.db          # 嵌入，随时中断，重跑即续跑
echo "为什么加载 yaml 会崩" | realontext query --db repo.db --branch main                 # 词法
echo "为什么加载 yaml 会崩" | realontext query --db repo.db --branch main --route vector  # 向量
```

`query` 的结果分两组：`code` 是修复该改的地方，`tests` 是覆盖这块行为的测试（D23）。

没有 API key 时，`eval/embed_server.py` 在本机提供同样接口的嵌入服务（开放权重模型，开发用），`embed` 加 `--endpoint http://127.0.0.1:8484/v1/embeddings --model jina-code-embeddings-0.5b --dim 896` 指过去。

团队模式下服务端部署一台，每个开发者跑自己的客户端——一份索引、一个 API key、一份知识库，成本不随人数增长。

需要两个 API key：

| 用途 | 服务 | 环境变量 |
|---|---|---|
| 嵌入 + 重排 | Jina | `JINA_API_KEY` |
| 提交摘要 | DeepSeek | `DEEPSEEK_API_KEY` |

Jina 一个 key 同时用于 Embeddings 和 Reranker，额度共享。

## 成本

用户自己付 API 费用。以下是实测口径的估算，按中型仓库（约 10,000 文件、2500 万 token 代码、50,000 条提交）计算。

### 首次索引

| 项目 | 用量 | 成本 |
|---|---|---|
| 代码嵌入 | 25M token × $0.018 | $0.45 |
| 提交摘要 | 50,000 条，谷时 + 缓存命中 | $7.70 |
| **合计** | | **约 $8** |

Jina 新账号有 1000 万 token 免费额度，实际首次支出通常低于这个数。

### 稳态月度

| 项目 | 用量 | 成本 |
|---|---|---|
| 增量嵌入 | 每天约 200 个文件变动 | $0.27 |
| 增量提交摘要 | 每月约 1,500 条 | $0.23 |
| 查询重排 | 每天约 100 次检索 | $1.20 |
| 查询嵌入 | 每次约 50 token | 可忽略 |
| **合计** | | **约 $1.7 / 月** |

### 大型仓库

10 万文件、2.5 亿 token 的仓库，首次嵌入约 $4.50（Jina）或 $30（voyage-code-4）。voyage 新账号有 2 亿 token 免费额度，通常能覆盖整个首次索引。

### 分支数量对成本的影响

realontext 索引仓库的**全部分支**，不做时间或活跃度过滤。

内容寻址去重让分支之间共享相同内容，但**去重率取决于你的分支有多陈旧**，逐仓库差异很大：

| 分支状态 | 去重后的额外成本 |
|---|---|
| 全部从近期 main 拉出 | 接近 1.0 倍 |
| 混合，最老约一年 | 1.5–2 倍 |
| 大量多年未动的残留分支 | 3 倍以上 |

原因：陈旧分支持有的是已经不在 main 上的旧版本文件。分支越老，与当前 main 重合的内容越少，独有 chunk 越多。

**在花任何钱之前，用纯 git 把这个比值量出来：**

```bash
# 去重后的唯一 blob 数
git for-each-ref --format='%(objectname)' refs/remotes/origin/ \
  | while read sha; do git ls-tree -r "$sha" | awk '{print $3}'; done \
  | sort -u | wc -l

# 不去重的总 blob 数
git for-each-ref --format='%(objectname)' refs/remotes/origin/ \
  | while read sha; do git ls-tree -r "$sha" | awk '{print $3}'; done \
  | wc -l
```

第一个数除以单分支的 blob 数，就是你要乘在上面成本表上的系数。完全本地，零 API 调用，几分钟出结果。

分支数量多的仓库请先跑这两条命令。

### 另一条省钱的路径

DeepSeek 谷时窗口：提交摘要是无时限的批处理作业，调度到 UTC 01:00–04:00 / 06:00–10:00（周一至周五）直接省一半。叠加自动前缀缓存后，整体差 5 倍。详见 `docs/modules/store.md`。

提交摘要的成本与分支数无关——commit SHA 天然去重，一条提交出现在多少个分支里都只摘要一次。

## 隐私

**代码内容会发送到第三方 API。** 嵌入、重排、提交摘要三处需要联网，符号图和存储完全在本地。

默认行为：

- 敏感路径不索引（`.env`、`*.pem`、`*.key`、`id_rsa`、`credentials`、`secrets/`）
- 发送前扫描常见凭据形态，命中则跳过该片段并告警

选择 provider 前请确认其数据留存政策是否符合你的合规要求。详见 `docs/cross/privacy.md`。

## 文档

| 文件 | 内容 |
|---|---|
| [`CONTEXT.md`](CONTEXT.md) | 词汇表 |
| [`docs/01-overview.md`](docs/01-overview.md) | **模块分层与依赖关系，从这里开始读** |
| [`docs/00-decisions.md`](docs/00-decisions.md) | 决策记录，含被否决的方案 |
| [`docs/benchmark.md`](docs/benchmark.md) | **测试标准** |
| [`docs/roadmap.md`](docs/roadmap.md) | 阶段划分 |
| [`docs/open-questions.md`](docs/open-questions.md) | **未决问题与待研究** |

### 模块

| 模块 | 职责 | 文档 |
|---|---|---|
| `serve/` | MCP 契约、HTTP、客户端/服务端 | [serve.md](docs/modules/serve.md) |
| `curate/` | 预算分配、多级保真、去冗 | [curate.md](docs/modules/curate.md) |
| `playbook/` | 知识库：条目、Reflector、Curator | [playbook.md](docs/modules/playbook.md) |
| `match/` | 查询理解、多路召回、融合、重排 | [match.md](docs/modules/match.md) |
| `vector/` | 向量库：HNSW、量化、ANN | [vector.md](docs/modules/vector.md) |
| `lexical/` | 倒排：FTS5、BM25 | [lexical.md](docs/modules/lexical.md) |
| `lsp/` | 语言服务客户端 | [lsp.md](docs/modules/lsp.md) |
| `store/` | 持久化：内容寻址、bitmap、GC | [store.md](docs/modules/store.md) |
| `ingest/` | git 采集、分支枚举、过滤 | [ingest.md](docs/modules/ingest.md) |
| `parse/` | tree-sitter 切块、符号与签名提取 | [parse.md](docs/modules/parse.md) |
| `model/` | 嵌入、重排、摘要、限流重试 | [model.md](docs/modules/model.md) |

### 跨模块

[隐私与凭据防护](docs/cross/privacy.md) · [C++ 技术栈](docs/cross/stack.md)

## 测试标准

**自建测试集，不用公开基准验收。**

测试样本从 14 个仓库（7 种语言，每种一个超大项目加一个中等项目）的 issue-PR 对自动生成：

- **query** = 修复之前写的 issue 正文，由不知道答案的人写
- **语料** = PR 的 base commit 状态，修复后的代码不在里面
- **答案** = 那个 PR 实际改了哪些文件和函数

三条都是历史事实，零人工标注。

三级任务对应模块分层：

```
L1  定位      issue → 要改哪些文件和函数
L2  上下文    issue → 为改好它还需要读什么
L3  预算      固定 token 预算下的 L1 / L2 质量    ← curate/ 的验收
```

外加一项公开基准测不了的：**全分支测试**——这个问题有人已经在别的分支上修好了，引擎找不找得到。

每次报分都带 **ripgrep** 和 **BM25** 两条基线。它们免费、外部、调参时调不动，是自建测试集唯一可靠的自检手段。

不用 CORE-Bench / LocBench / ContextBench 等公开基准做验收的理由，以及它们各自的否决记录，见 [`docs/benchmark.md`](docs/benchmark.md)。仓库名单在 [`eval/repos.txt`](eval/repos.txt)。

## 状态

设计阶段，尚无实现。

## 许可

待定。
