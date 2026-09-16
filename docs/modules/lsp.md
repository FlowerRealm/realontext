# `lsp/` 语言服务客户端

符号关系查询。只答符号问题，不排序。

**依赖**：外部 language server 进程
**被依赖**：`match/`
**运行位置**：**仅客户端**

## 为什么只能在客户端

language server 需要一棵真实的工作树。服务端只有裸镜像，没有 checkout。

这是客户端/服务端拆分的根本原因之一（D18），也导致一次检索必须两阶段往返——服务端出候选，客户端做符号扩散，再回服务端排序。

## 不存储符号图

查询时现问，不预先计算也不持久化。

```
match/ 给来的候选 Chunk
  → 取其中的符号名（来自 parse/）
  → textDocument/references + definition
  → 得到邻接 Chunk
  → 回给 match/
```

**符号关系只对本次检索的候选查询，不需要全图。** top-200 候选查一轮。

### 为什么不用 SCIP

原方案是消费 SCIP（Sourcegraph 的 protobuf 索引格式），已被 D15 取代。

| | SCIP | LSP |
|---|---|---|
| 计算时机 | 预先算出全图 | 查询时按需问 |
| 全部分支（D14） | N 次全项目分析，不可行 | 只问已 checkout 的分支 |
| 持久化 | 需要 symbol 表 + edge 表 | 不存储 |
| 外部依赖 | 每语言一个 indexer 二进制 + protobuf | language server，用户机器上通常已在运行 |

D14 确定索引全部分支后，SCIP 的预计算模型直接失效——500 个分支就是 500 次全项目分析。

被消掉的：SCIP indexer 分发问题、protobuf 依赖、三张符号表、「边跨分支复用」的整套设计。

## 需要实现的 LSP 子集

```
initialize / initialized      握手
textDocument/didOpen          打开文件
textDocument/definition       跳转定义
textDocument/references       查找引用
textDocument/documentSymbol   文件内符号
shutdown / exit               生命周期
```

不实现补全、诊断、格式化、重命名——那些是编辑器功能。

`callHierarchy` 各语言支持度参差，不列入必需集。

## 支持的 language server

| 语言 | Server | 前置条件 |
|---|---|---|
| C / C++ | `clangd` | `compile_commands.json` |
| Rust | `rust-analyzer` | Cargo 项目 |
| Python | `pyright` / `pylsp` | 虚拟环境 |
| TypeScript / JS | `typescript-language-server` | `tsconfig.json` |
| Go | `gopls` | Go module |
| Java | `jdtls` | Maven / Gradle |

**这些前置条件通常在用户机器上已经满足**——开发者为自己的项目早就配好了。本模块复用这份已付的成本，不额外要求。

问题没有消失，只是移回它本来该在的地方。

## 工程难点

- server 生命周期管理与崩溃重启
- 初次打开大项目时 server 自身建索引需要数分钟，需要就绪探测
- 每种语言的启动命令和初始化参数不同，需要一张配置表
- 只有已 checkout 的分支可查询

## 限制与降级

**只有已 checkout 的分支能获得符号扩散。** 其余分支只有向量和 BM25 两路。

这与 Augment 持平——它的符号图同样只覆盖用户当前分支。

无可用 server 时本模块整体跳过，`match/` 退化为两路召回与单次往返，符号匹配回落到 `parse/` 的符号名兜底档。

## 现成参考

- [multilspy](https://github.com/microsoft/multilspy)（微软）：Python LSP 客户端库，为向 LLM 提供静态分析结果而设计
- [Serena](https://github.com/oraios/serena)：MCP 工具包，LSP 后端，30+ 语言

早期可直接依赖 Serena 作为并列 MCP，**待自建测试集证明符号信号的增益后再自行实现**。见 [`../benchmark.md`](../benchmark.md) 与 [`../roadmap.md`](../roadmap.md)。

## 相关决策

D15（用 LSP 不用 SCIP）· D18（运行在客户端）
