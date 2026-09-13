# `store/` 持久化

SQLite 之上的内容寻址存储。不做检索决策。

**依赖**：SQLite、CRoaring、BLAKE3、`parse/`、`model/`
**被依赖**：`vector/`、`lexical/`、`playbook/`

## 核心问题：N 个分支不能是 N 份索引

10 万文件的仓库切出约 80 万 Chunk，20 个分支：

```
20 × 800,000 × 1024 dim × 4 bytes = 65 GB
```

存储不可行，嵌入费用同样乘以 20。

## 解法：抄 git 的内容寻址

分支之间大量内容相同。git 早就解决了——blob 按内容哈希存一份，tree 指过去。

```
chunks          : chunk_hash  -> (content, embedding, symbols)   全局去重
blob_chunks     : blob_sha    -> [chunk_hash]                    切块结果缓存
branch_manifest : (repo, branch) -> roaring_bitmap<chunk_ordinal>
```

索引新分支：遍历 tree，每个 `blob_sha` 查 `blob_chunks`。只对没见过的 blob 送 `parse/` 切块、送 `model/` 嵌入。

```
832,000 × 1024 × 1 byte = 850 MB   int8
```

Roaring bitmap 开销：每分支约 100 KB，可忽略。

### 去重率不是常数

**这组数字假设分支都是近期从 main 拉出的。** 陈旧分支持有已不在 main 上的旧版本文件，独有 Chunk 随年龄增长：

| 分支状态 | 相对单分支总量 |
|---|---|
| 全部近期拉出 | 约 1.0 倍 |
| 混合，最老约一年 | 1.5–2 倍 |
| 大量多年残留分支 | 3 倍以上 |

**不要在代码或文档里写死这个系数。** 逐仓库不同，可用纯 git 免费测出（命令见 README），启动时输出实测值。

即使最坏的 3 倍，相对朴素实现的 20 倍仍省 85%。去重是必需的。

## 表结构

| 表 | 键 | 值 | 作用域 |
|---|---|---|---|
| `chunks` | chunk_hash | content, embedding, symbols | 全局去重 |
| `blob_chunks` | blob_sha | [chunk_hash] | 切块缓存 |
| `branch_manifest` | (repo, branch) | roaring bitmap | 每分支 |
| `commits` | commit_sha | summary, embedding | 全局去重 |
| `commit_refs` | (repo, branch) | roaring bitmap | 每分支 |
| `bullets` | bullet_id | content, counters, embedding | 全局（`playbook/`） |
| `index_meta` | — | provider, model, task, dim | 全局 |

**符号关系不在表中。** 不预计算也不持久化，查询时由 `lsp/` 现问（D15）。

## 提交摘要的去重

**commit SHA 内容寻址且不可变。** 一条提交出现在 10 个分支只摘要一次，用 SHA 做主键自动获得。

比代码块的去重还干净——不需要额外逻辑。

## 索引指纹

`index_meta` 记录 `model/` 的四项配置。启动时比对，**任何一项不一致则拒绝启动**（D8）。

理由见 [`model.md`](model.md) Provider 指纹与切换。

## 垃圾回收

没有任何活跃分支引用的 Chunk 可回收：

```
live = OR(所有 branch bitmap)
dead = ALL ANDNOT live
```

两个位运算，与 `git gc` 同思路。

HNSW 增量删除产生碎片，GC 时通知 `vector/` 重建。

## 并发

SQLite WAL 模式。单写多读——索引作业是唯一写者，查询全是读者。

团队模式下多个客户端并发查询同一个服务端，读并发由 WAL 保证。

## 相关决策

D7 · D8 · D13 · D14
