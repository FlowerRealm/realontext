# 实现技术栈

C++20。选择理由见 `../00-decisions.md` D1。

## 依赖

| 功能 | 库 | 原生语言 | 备注 |
|---|---|---|---|
| git 操作 | **libgit2** | C | 直接链接，不 fork 进程。C++ 包装可选 `cppgit2` |
| 代码解析 | **tree-sitter** | C | 本来就是 C 库，其他语言都在包装它 |
| HTTP 客户端 | **libcurl** 或 cpp-httplib | C / C++ | 调用嵌入、重排、摘要 API |
| 向量索引 | **usearch** 或 **hnswlib** | C++ header-only | usearch 支持 int8 量化；hnswlib 更简单 |
| BM25 / 全文 | **SQLite FTS5** | C | 内置 `bm25()` 排序函数 |
| 存储 | **SQLite** | C | WAL 模式，单写多读 |
| 位图 | **CRoaring** | C | 分支 manifest |
| 内容哈希 | **BLAKE3** | C | 官方 C 实现 |
| JSON | **nlohmann/json** 或 **simdjson** | C++ | API 交互与 MCP 协议 |
| LSP 客户端 | 自实现 JSON-RPC over stdio | C++ | 只需 definition / references / documentSymbol |
| HTTP 服务 | **cpp-httplib**（header-only）或 **Drogon** | C++ | MCP over HTTP |
| 并发 | `std::jthread` / **taskflow** | C++ | 索引流水线 |

整条链上没有一个需要跨语言 FFI 或外部运行时。

## 不需要的依赖

API-only 决策（D5）砍掉了推理侧的全部复杂度：

- 没有 llama.cpp
- 没有 GGUF 模型文件
- 没有 CUDA / ROCm / Metal / Vulkan 后端矩阵
- 没有 Python、没有 PyTorch

单二进制分发因此变得简单：全是小型 C/C++ 库，静态链接，一个文件，跨平台无 GPU 依赖。

## 已知妥协

`tantivy`（Rust）没有 C++ 对等物。用 SQLite FTS5 替代——BM25 内置，功能够用，而且少一个依赖。

这是整个栈里唯一需要妥协的地方。

## 工程约定

| 项 | 约定 |
|---|---|
| 标准 | **C++20**。`std::span`、`std::format`、concepts、`std::jthread` 都用得上。C++23 的 `std::expected` 诱人但编译器支持还不齐 |
| 构建 | **CMake**。header-only 依赖（usearch / hnswlib / cpp-httplib / nlohmann）走 FetchContent，其余走 vcpkg |
| 分发 | **静态链接单二进制**。SQLite、tree-sitter、CRoaring、BLAKE3 都是小 C 库，静态进去没有负担 |
| 错误处理 | 边界层用 expected 风格返回值。**索引流水线内不抛异常**——批处理作业里异常会吞掉进度 |
| 内存 | 索引是批处理，**arena 分配器**比引用计数合适。一个提交或一个文件一个 arena，处理完整块释放 |

## 外部依赖：Language Server

符号关系来自 language server（D15），不是预计算的索引文件。

| 语言 | Language Server | 前置条件 |
|---|---|---|
| C / C++ | `clangd` | `compile_commands.json` |
| Rust | `rust-analyzer` | Cargo 项目 |
| Python | `pyright` / `pylsp` | 虚拟环境 |
| TypeScript / JS | `typescript-language-server` | `tsconfig.json` / `package.json` |
| Go | `gopls` | Go module |
| Java | `jdtls` | Maven / Gradle |

**这些前置条件通常在用户机器上已经满足**——开发者为自己的项目早就配好了。realontext 复用这份已付的成本，不额外要求。

### 需要实现的 LSP 子集

```
initialize / initialized      握手
textDocument/didOpen          打开文件
textDocument/definition       跳转定义
textDocument/references       查找引用
textDocument/documentSymbol   文件内符号
shutdown / exit               生命周期
```

不实现补全、诊断、格式化、重命名等编辑器功能。

### 工程难点

- server 生命周期管理与崩溃重启
- 初次打开大项目时 server 自身建索引需要数分钟，需要就绪探测
- 每种语言的启动命令和初始化参数不同，需要一张配置表
- 只有已 checkout 的分支可查询

### 现成参考

- [multilspy](https://github.com/microsoft/multilspy)（微软）：Python LSP 客户端库，为向 LLM 提供静态分析结果而设计
- [Serena](https://github.com/oraios/serena)：MCP 工具包，LSP 后端，30+ 语言

早期可直接依赖 Serena 作为并列 MCP，待自建测试集证明符号信号的增益后再自行实现（见 `../benchmark.md`、`../roadmap.md`）。
