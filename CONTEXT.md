# realontext

给编码 agent 提供代码检索与上下文策展的引擎。

本文件是词汇表，只定义概念，不记录实现决策——实现决策在 `docs/00-decisions.md`，分层结构在 `docs/01-overview.md`。

## Language

**Project**:
一个被索引的代码库单位，与一个 **Repository** 一一对应。
_Avoid_: 工程、codebase、workspace

**Repository**:
一个 git 仓库，是 **Branch** 命名空间的拥有者。
_Avoid_: 仓库集合、repo group

**Branch**:
一个 git 分支的当前 tip 状态；**Repository** 下的每一个 remote 分支都是索引对象，无新鲜度筛选。
_Avoid_: 活跃分支、feature branch、工作分支

**Chunk**:
一段按语法边界切出的代码，是嵌入和检索的最小单位。
_Avoid_: 代码块、block、fragment、片段

**Symbol**:
代码中一个有名字的定义（函数、类、变量），是符号图的节点。
_Avoid_: 标识符、identifier

**Context Package**:
一次检索调用的返回物，是在 token 预算内组装完成的结果集合。
_Avoid_: 结果列表、搜索结果、results

**Fidelity**:
一个 **Chunk** 在 **Context Package** 中的呈现档位：全文、签名、摘要、引用。
_Avoid_: 详细程度、级别、level

**Budget**:
调用方在单次检索中声明的 token 上限，是策展层的优化约束。
_Avoid_: 限制、配额、limit

**Session**:
一串共享去冗状态的连续检索调用。
_Avoid_: 会话上下文、对话

**Playbook**:
服务端持久维护的、关于一个 **Project** 的知识库，由 **Bullet** 组成。
_Avoid_: 知识库文档、memory、笔记

**Bullet**:
一条可独立寻址的知识：可复用的策略、领域概念或失败模式，带有用/有害计数器，隶属于某个 **Playbook**。
_Avoid_: 条目、记录、entry、note

**Provider**:
提供嵌入、重排或摘要能力的第三方 API 服务商。
_Avoid_: 厂商、vendor、后端、backend

## Relationships

- 一个 **Project** 恰好对应一个 **Repository**
- 一个 **Repository** 拥有多个 **Branch**
- 一个 **Chunk** 属于一个或多个 **Branch**（内容相同则共享同一个 **Chunk**）
- 一个 **Symbol** 定义在恰好一个 **Chunk** 中
- 跨 **Project** 的 **Chunk** 不共享，**Symbol** 不连边
- 一个 **Context Package** 由多个 **Chunk** 组成，每个 **Chunk** 带一个 **Fidelity**
- 一个 **Context Package** 的总开销不超过调用方声明的 **Budget**
- 一个 **Session** 包含多次检索，**Context Package** 之间共享去冗状态
- 一个 **Project** 对应一份 **Playbook**
- 一个 **Playbook** 由多个 **Bullet** 组成，**Bullet** 可被检索并进入 **Context Package**
- 重复命中的 **Chunk** 在 **Context Package** 中被替换为相关 **Bullet** 加引用，不被略去

## Example dialogue

> **Dev:** 我们的微服务有三个仓库，怎么让 agent 同时看到它们？
> **Domain expert:** 看不到。一个 **Project** 就是一个 **Repository**，三个仓库要起三个实例，互相不可见。
> **Dev:** 那 shared-lib 在三个服务里都被引用，会被嵌入三次？
> **Domain expert:** 会。**Chunk** 去重只在单个 **Repository** 内生效。跨 **Project** 没有共享。

## Flagged ambiguities

- 「ACE」同时指 Augment Context Engine（商业产品）和 Agentic Context Engineering（论文方法）——已解决：文档中一律写全称，不使用缩写 ACE。本项目对标前者，**Playbook** 层采用后者的方法。

- 「项目」曾被用来暗示跨仓库范围（`docs/00-decisions.md` D3 提到「跨服务」），但数据结构以 repo 为键——已解决：**Project** 与 **Repository** 一一对应，跨仓库不在范围内。
- 「indexer」曾同时指本工具和外部 SCIP indexer——已解决：SCIP 已出局（D15），外部组件统一称 **Language Server**，本工具称 realontext，不使用 indexer 一词。
- 「index」曾同时指整个系统、向量索引、全文索引——已解决：**Index** 专指持久化存储整体，其组成部分分别称 **Vector Index** 和 **FTS Index**。
- 「返回结果」曾被当作一个列表讨论——已解决：返回物是 **Context Package**，它是预算约束下的组装产物，不是排序列表的前 N 项。
- 「活跃分支」一词曾出现在成本表中但从未定义——已解决：不存在「活跃分支」这个概念，全部 remote **Branch** 一律索引。
