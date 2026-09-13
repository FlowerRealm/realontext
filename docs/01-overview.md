# 模块分层

realontext 按技术关注点分层，每层对应一个代码模块和一份文档。

## 依赖方向

```
                    ┌──────────────┐
        对外         │   serve/     │  MCP · HTTP · 客户端/服务端
                    └──────┬───────┘
                           │
              ┌────────────┴────────────┐
        策展   │  curate/    playbook/   │  预算组装 · 知识库
              └────────────┬────────────┘
                           │
                    ┌──────┴───────┐
        匹配       │   match/     │  多路召回编排 · 融合 · 重排
                    └──────┬───────┘
                           │
         ┌─────────────────┼─────────────────┐
 检索后端 │   vector/     lexical/      lsp/  │  向量库 · 倒排 · 语言服务
         └─────────────────┼─────────────────┘
                           │
                    ┌──────┴───────┐
        存储       │   store/     │  SQLite · 内容寻址 · bitmap
                    └──────┬───────┘
                           │
         ┌─────────────────┼─────────────────┐
 基础能力 │   ingest/     parse/      model/  │  取数据 · 解析 · 调模型
         └───────────────────────────────────┘
```

上层只依赖下层，同层之间不互相依赖。

## 模块清单

| 模块 | 职责 | 文档 |
|---|---|---|
| `ingest/` | git 采集：裸镜像、分支枚举、过滤 | [ingest.md](modules/ingest.md) |
| `parse/` | 语法解析：切块、符号提取、签名提取 | [parse.md](modules/parse.md) |
| `model/` | 模型调用：嵌入、重排、摘要、限流重试 | [model.md](modules/model.md) |
| `store/` | 持久化：schema、内容寻址、bitmap、GC | [store.md](modules/store.md) |
| `vector/` | 向量库：HNSW、量化、ANN 查询 | [vector.md](modules/vector.md) |
| `lexical/` | 倒排：FTS5、BM25 | [lexical.md](modules/lexical.md) |
| `lsp/` | 语言服务客户端：生命周期、符号查询 | [lsp.md](modules/lsp.md) |
| `match/` | 匹配：查询理解、多路召回、融合、重排 | [match.md](modules/match.md) |
| `curate/` | 策展：预算分配、多级保真、去冗 | [curate.md](modules/curate.md) |
| `playbook/` | 知识库：条目、Reflector、Curator | [playbook.md](modules/playbook.md) |
| `serve/` | 服务：MCP 契约、HTTP、客户端/服务端 | [serve.md](modules/serve.md) |

跨模块关注点：[隐私](cross/privacy.md) · [C++ 技术栈](cross/stack.md)

测试标准：[benchmark.md](benchmark.md)

## 三个模块的特殊性

### `model/` 是基础能力，不是管道的一环

它被多处调用，时机完全不同：

| 调用方 | 时机 | 用途 |
|---|---|---|
| `ingest/` → `model/` | 离线批处理 | 提交摘要 |
| `store/` 建索引时 | 离线批处理 | Chunk 嵌入 |
| `match/` | 查询路径 | 查询嵌入、重排 |
| `playbook/` | 离线批处理 | Reflector / Curator |

所以它放在最底层作为能力提供者，而不是插在管道中间。**限流、重试、批处理、Provider 抽象全部收敛在这一个模块里**，其余模块不直接碰 HTTP。

### `lsp/` 只能在客户端

language server 需要一棵真实的工作树。这是客户端/服务端拆分的根本原因之一（D18）。

因此 `match/` 的召回是跨进程的——服务端出候选，客户端做符号扩散，再回服务端排序。

### `curate/` 与 `playbook/` 同层但职责相反

`curate/` 处理**单次调用内**的组装：预算、保真档、去冗、多样性。

`playbook/` 处理**跨会话累积**的知识：条目、计数器、增量合并。

两者都属于策展，但一个是瞬时的，一个是持久的。

## 模块的边界原则

| 模块 | 不做什么 |
|---|---|
| `ingest/` | 不解析代码内容，只取字节和元数据 |
| `parse/` | 不调模型，不访问存储，纯函数 |
| `model/` | 不理解业务语义，只管调用与配额 |
| `store/` | 不做检索决策，只管持久化 |
| `vector/` / `lexical/` | 不做多路协调，只答自己那一路 |
| `lsp/` | 不排序，只答符号关系 |
| `match/` | 不截断，只排序 |
| `curate/` | 不重新排序，只在预算下组装 |
| `serve/` | 不含业务逻辑，只是契约与传输 |

全系统贯穿一条：**realontext 不做任何语言分析。** 切块靠 tree-sitter，符号关系靠 language server，两者都是外部组件。

## 数据流

模块分层是静态依赖关系。运行时的数据流横穿这些模块：

```
索引路径（离线）
  ingest/ → parse/ → model/ → store/ → vector/ + lexical/

查询路径（在线）
  serve/ → match/ ─┬→ vector/
                   ├→ lexical/
                   ├→ lsp/       （客户端）
                   └→ model/     （查询嵌入、重排）
         → curate/ ←→ playbook/
         → serve/
```

一次查询是两阶段往返，因为 `lsp/` 在客户端而 `match/` 的其余部分在服务端。详见 [serve.md](modules/serve.md)。

## 阻塞项

`match/` 里有一块完全空白：**查询理解**。

agent 传来的自然语言直接拿去嵌入和 BM25，没有改写、扩展或子查询拆分。BM25 尤其受影响——「为什么加载 yaml 会崩」里没有一个词会出现在 `ConfigParser::parse` 里。

见 [open-questions.md](open-questions.md) C1。
