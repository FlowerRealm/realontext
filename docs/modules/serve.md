# `serve/` 服务

对外契约与传输。不含业务逻辑。

**依赖**：全部模块
**被依赖**：无

## 客户端与服务端

三件事强制了拆分：

| 事实 | 归属 |
|---|---|
| `lsp/` 需要一棵真实工作树 | **客户端** |
| dirty overlay 按定义在本地 | **客户端** |
| 索引、知识库、API key 共享且昂贵 | **服务端** |

agent 通过 MCP 说话，MCP 端点需要在开发者本机——也落在客户端。

### 职责

**客户端**：MCP 端点、上报分支与 dirty overlay、`lsp/` 生命周期、符号扩散、回传会话轨迹。**不持有任何 API key，不做嵌入，不做重排。**

**服务端**：`ingest/` `parse/` `model/` `store/` `vector/` `lexical/` `match/` `curate/` `playbook/` 全部。

### 两种部署形态

```
单机                          团队
┌──────────────┐              ┌──────────────┐
│ agent        │              │ 服务端（一台）│
│  ↕ MCP       │              │ 索引 / 知识库 │
│ 客户端        │              │ API key      │
│  ↕ HTTP      │              └──────┬───────┘
│ 服务端        │                     │ HTTP
└──────────────┘              ┌──────┴───────┬──────────┐
                            客户端A      客户端B     客户端C
                          （各自的分支与 dirty 状态）
```

**同一个二进制，两个角色。**

```bash
realontext serve   --repo github.com/org/repo
realontext client  --server http://host:port
```

单机模式下客户端可直接拉起本地服务端，用户看到的仍是一条命令。

团队模式收益：**一份索引、一个 API key、一份知识库**，N 个开发者共享，成本不随人数增长。

## 检索的两阶段往返

`lsp/` 在客户端，`match/` 的其余部分在服务端：

```
① 客户端 → 服务端
   query, branch, dirty chunks, budget, session_id
        ↓  match/ 的 vector + lexical 召回、分支过滤
② 服务端 → 客户端
   候选列表（约 150）
        ↓  lsp/ 符号扩散
③ 客户端 → 服务端
   扩散后的候选集
        ↓  match/ 融合重排 → curate/ + playbook/
④ 服务端 → 客户端 → agent
   Context Package
```

两个来回是为了让符号扩散出的候选也进入重排和策展。若只在末尾拼接，它们既不参与排序也不占预算，等于绕过 `match/` 的排序和 `curate/` 的组装。

延迟预算是秒级（D12），可以接受。

**降级**：无可用 language server 时跳过 ②③，退化为单次往返、两路召回。

## MCP 工具

### `codebase-retrieval`

```
入参
  query          自然语言任务描述
  token_budget   本次调用的 token 上限
  session_id     跨调用去冗标识

出参
  Context Package
```

**`token_budget` 和 `session_id` 是架构性参数**，不是可选增强。

没有预算，`curate/` 没有优化目标；没有会话标识，`playbook/` 的跨调用去冗没有落点。

它们反向约束下游：`match/` 必须为 `curate/` 预留选择空间，所以交出约 200 条而非 5 条。

### `expand`

```
入参  id       Context Package 中某条目的标识
出参  该条目的完整原文
```

**这是 D17 的安全阀。**

`curate/` 会把部分内容降档到签名或引用，`playbook/` 会把重复命中的内容替换为条目加引用。两种情况下 agent 都能看到条目存在，需要时要回原文。

没有这个工具，降档就等于静默扣留。

## Context Package

返回物不是排序列表的前 N 项，而是**预算约束下的组装产物**。同一个包里可能同时含有：

```
· 全文档     auth.cpp:40-88   [620 token]
· 签名档     TokenCache 类的三个方法  [90 token]
· Playbook   已知坑：过期判断用 < 而非 <=  [40 token]
· 引用档     另外 6 处相关位置  [50 token]
```

每条带稳定 `id`，来自内容寻址的 `chunk_hash`，跨调用天然稳定。

## 为什么不是 LSP 那样的协议

Language Server Protocol 是给编辑器设计的：输入是 `(file, position)` 或符号名，**要求调用方已经知道要找什么**。

| | LSP | realontext |
|---|---|---|
| 输入 | `(file, position)` 或符号名 | 自然语言任务描述 |
| 前提 | 调用方已知目标 | 调用方不知道 |
| 输出 | 全部匹配项，无序 | 预算内组装的 Context Package |
| 相关性概念 | 无 | 有 |

`find_referencing_symbols("validate_token")` 要求你先知道这个符号存在。热门函数有 500 个引用，LSP 全给你，不排序。

## 协议

MCP 面向 agent（stdio 或 localhost HTTP）。

客户端与服务端之间 **HTTP + JSON**。不引入自定义二进制协议或 gRPC——调试成本、依赖成本，以及这条链路数据量本来就不大（一次查询几百 KB 量级）。

## dirty overlay 的传输

客户端把未提交文件内容随查询上传。

**隐私影响**：未提交代码会离开客户端机器。`ingest/` 的路径过滤和凭据扫描必须在**客户端发送前**执行，不能等到服务端。详见 [`../cross/privacy.md`](../cross/privacy.md)。

## 认证

团队模式下服务端需要区分客户端身份，用于会话轨迹归属、dirty overlay 隔离、Playbook 条目来源标注。

**不做权限隔离**（D11）——所有人能检索全部内容。身份只用于归属，不用于授权。

最简实现：共享 token + 客户端自报用户名，服务端不校验真实性。对团队内部工具够用。

## 与 D11 的关系

拆分的动因是**能力边界**（谁能看到工作树、谁持有密钥），不是安全边界。服务端仍假设所有客户端可信。

## 未决

- Context Package 的具体序列化格式
- `token_budget` 缺省时的行为（拒绝？用默认值？）
- 是否需要流式返回

## 相关决策

D11 · D12 · D16 · D17 · D18
