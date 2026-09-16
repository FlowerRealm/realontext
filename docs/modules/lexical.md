# `lexical/` 倒排

词法匹配。只答自己这一路。

**依赖**：SQLite FTS5、`store/`
**被依赖**：`match/`

## 选型

**SQLite FTS5**，内置 `bm25()` 排序函数。

与 `store/` 共用同一个 SQLite 文件，少一个依赖、少一份数据同步逻辑。

### 已知妥协

`tantivy`（Rust）功能更强但没有 C++ 对等物。FTS5 够用，且这是整个技术栈里唯一需要妥协的地方。

## 为什么不能省

向量检索有确定的盲区，BM25 正好补上：

| 找得到 | 例子 |
|---|---|
| 错误字符串 | `"connection reset by peer"` |
| 日志文本 | `LOG(ERROR) << "retry exhausted"` |
| 配置键 | `max_retry_interval_ms` |
| 标识符 | `ConfigParser::parse` |

这些都是精确字面量，嵌入向量会把它们抹平成「大致关于错误处理」。

## 代码分词

自然语言分词器对代码是错的：

```
ConfigParser::parse   应该切成 config / parser / parse
max_retry_interval_ms 应该切成 max / retry / interval / ms
```

自定义 FTS5 tokenizer（C 接口），规则是**拆分并保留原词**：

```
ConfigParser          → configparser  config  parser
max_retry_interval_ms → max_retry_interval_ms  max  retry  interval  ms
```

全部小写。原词保证整词查询精确命中，子词保证自然语言里的 `parser` 能对上标识符。

query 走同一个 tokenizer，去掉停用词后全部词 OR 起来，交给 `bm25()` 的 IDF 决定权重。不做改写、不选词。

分词器不与 `../../eval/harness/tokenize.py` 共享任何代码或词表（`../benchmark.md` BM25 基线的隔离要求）。

## 索引单位与排序

同一份文本建两张 FTS 表：

| 表 | 文档单位 | 列 | 存储 |
|---|---|---|---|
| `chunk_fts` | Chunk | `symbol_path`、`content` | 外部内容，文本在 `chunks` |
| `file_fts` | 一个 `(blob_sha, lang)`，即它全部 Chunk 的拼接 | `content` | 无内容表，只存倒排 |

Chunk 用来定位；整文件用来收集散落在大文件多个函数里的证据。两者的融合在 `match/`。

`chunk_fts` 的`bm25()` 给 `symbol_path` 更高的列权重——命中定义名比命中函数体里的一次调用更说明问题。

**IDF 按全库统计**，不按分支。FTS5 的统计量覆盖整张表，也就是全部分支的并集。分支之间高度重合，偏差小，记为已知误差。按分支统计需要自己实现排序函数并维护 BM25，复杂度不由这个误差支撑。

**路径不是召回信号。** Chunk 跨路径共享，路径放不进 Chunk 的列。要用文件名信号，得给 `files` 单独建索引再融合，当前不做。

## 与查询理解的关系

即使分词正确，还有一个上游问题：**自然语言描述里的词和代码标识符往往对不上。**

「为什么加载 yaml 会崩」里没有一个词会出现在 `ConfigParser::parse` 里。

这属于 `match/` 的查询理解，不是本模块能解决的。见 [`../open-questions.md`](../open-questions.md) C1。

## 相关决策

D21
