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
chunks          : chunk_ord   -> (chunk_hash, content, embedding, symbol_path)  全局去重
blob_chunks     : (blob_sha, lang) -> [(chunk_ord, start_line, end_line)]       切块结果缓存
files           : file_ord    -> (path, blob_sha)                               文件版本，全局去重
branch_manifest : (repo, branch) -> roaring_bitmap<file_ord>
```

索引新分支：遍历 tree，每个 `(path, blob_sha)` 查 `files`，每个 `(blob_sha, lang)` 查是否切过。只对没切过的送 `parse/` 切块、送 `model/` 嵌入。

切块缓存的键带语言：语言由扩展名决定，同一份字节在 `.ts` 与 `.tsx` 下切出来不一样。

### 位图按文件版本编号，不按 Chunk

同一个 Chunk 可以出现在不同路径、不同分支。位图若按 Chunk 编号，检索结果拿不到路径。

按 `(path, blob_sha)` 编号（Zoekt 的分支掩码模型），可见性判定和路径解析是同一次查找：

```
chunk_ord → blob_chunks 里含它的 blob → files 里指向这些 blob 的 file_ord → ∩ 分支位图
```

交集非空即可见，交集里的每个 file_ord 直接给出 `path`，`blob_chunks` 给出行号。行号跟着 blob 走而不跟着 Chunk 走：内容相同的 Chunk 在不同 blob 里行号不同。

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
| `chunks` | chunk_ord（chunk_hash 唯一） | content, embedding, symbol_path | 全局去重 |
| `parsed` | ord（(blob_sha, lang) 唯一） | — | 已切过的标记，切出零块的 blob 也记；ord 是 `lexical/` 整文件文档的编号 |
| `blob_chunks` | (blob_sha, lang) | [(chunk_ord, start_line, end_line)] | 切块缓存 |
| `files` | file_ord（(path, blob_sha) 唯一） | path, blob_sha | 全局去重 |
| `branch_manifest` | (repo, branch) | roaring bitmap\<file_ord\> | 每分支 |
| `chunk_fts` | rowid = chunk_ord | symbol_path, content | FTS5（`lexical/`） |
| `commits` | commit_sha | summary, embedding | 全局去重 |
| `commit_refs` | (repo, branch) | roaring bitmap | 每分支 |
| `bullets` | bullet_id | content, counters, embedding | 全局（`playbook/`） |
| `meta` | key | `embed.*` 六项指纹；`lexical/` 的同步水位 | 全局 |

**符号关系不在表中。** 不预计算也不持久化，查询时由 `lsp/` 现问（D15）。

## 提交摘要的去重

**commit SHA 内容寻址且不可变。** 一条提交出现在 10 个分支只摘要一次，用 SHA 做主键自动获得。

比代码块的去重还干净——不需要额外逻辑。

## 索引指纹

`meta` 表的 `embed.*` 记录六项指纹（D22）。库里还没有向量时跟随请求改写；有了向量后，请求的配置任何一项不一致就拒绝嵌入（D8）。`input_version` 与二进制不同时，数据库直接拒绝打开。

## 嵌入队列

待嵌入的 Chunk 就是 `embedding IS NULL` 的行，由部分索引 `chunks_pending` 支撑。没有单独的队列表，中断后重跑 `realontext embed` 即续跑。

理由见 [`model.md`](model.md) Provider 指纹与切换。

## 垃圾回收

没有任何活跃分支引用的文件版本可回收，blob 与 Chunk 跟着引用计数走：

```
live_files  = OR(所有 branch bitmap)
dead_files  = ALL ANDNOT live_files
live_blobs  = live_files 指向的 blob
dead_chunks = 不被任何 live_blob 引用的 Chunk
```

前两步是位运算，与 `git gc` 同思路。

HNSW 增量删除产生碎片，GC 时通知 `vector/` 重建。

## 并发

SQLite WAL 模式。单写多读——索引作业是唯一写者，查询全是读者。

团队模式下多个客户端并发查询同一个服务端，读并发由 WAL 保证。

## 相关决策

D7 · D8 · D13 · D14 · D21 · D22
