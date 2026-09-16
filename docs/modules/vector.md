# `vector/` 向量库

近似最近邻检索。只答自己这一路，不做多路协调。

**依赖**：usearch（或 hnswlib）、`store/`
**被依赖**：`match/`、`playbook/`

## 选型

**usearch**，C++ header-only，支持 int8 量化。

备选 hnswlib（更简单，无量化）。

### 明确避开 `sqlite-vec`

v0.1.10-alpha，只支持 f32，无量化，无 SIMD。本项目要存百万级向量并量化，它做不到。

## 索引形态：一个全局 HNSW

**不为每个分支建独立索引。**

关键观察：任意分支包含全部 Chunk 的绝大部分。选择性极高，所以过取加后过滤优于 filtered HNSW——后者在低选择性下严重退化，而这里根本不是低选择性场景。

```
ANN 取 top-800（4 倍过取）
  ↓
交给 match/ 做分支可见性测试
  ↓
保留 top-200
```

过滤本身不在本模块——本模块只负责「给我 top-K」，分支语义属于 `match/`。

副作用：切换分支等于换一个 bitmap 指针；新建分支等于拷贝 bitmap 再应用 diff。两者都不触发本模块重建。

## 维度与精度

维度 **1024**（D7）。理由是 Jina v4 与 voyage-code-4 的交集，切换 Provider 时存储布局不变。

存储精度 **int8**。850 MB 对 83 万 Chunk。

**量化档位尚未定死**——fp32 / int8 / 更激进的方案对召回率的影响需要用自建测试集实测。开发期语料存 fp32，量化档可本地免费扫；正式验收直接存 int8。见 [`../open-questions.md`](../open-questions.md) C7 与 [`../benchmark.md`](../benchmark.md) 存储策略。

## HNSW 参数

`M` / `efConstruction` / `efSearch` **全部未选定**。

影响召回率与内存，需要在自建测试集的 L1 上扫参。**扫参不重嵌**，从已存的向量重建索引即可，零额外成本。见 [`../open-questions.md`](../open-questions.md) C5。

## 增删与重建

- 新增：`store/` 写入新 Chunk 后追加
- 删除：GC 时从图中移除，产生碎片
- 重建：碎片累积到阈值后全量重建，与 GC 同期执行

## 另一个索引：Playbook 条目

`playbook/` 的条目也走本模块，与代码 Chunk 同一套嵌入和检索机制，但**是独立的索引实例**——条目和代码不在同一个向量空间里做排序，它们在 `curate/` 层才汇合。

条目的语义去重也用本模块（ACE 的 grow-and-refine 依赖嵌入相似度）。

## 相关决策

D7（维度 1024）
