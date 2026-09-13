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

## 代码的分词问题

**这是本模块最大的未解决项。**

自然语言分词器对代码是错的：

```
ConfigParser::parse   应该切成 config / parser / parse
max_retry_interval_ms 应该切成 max / retry / interval / ms
```

需要自定义 tokenizer：驼峰拆分、下划线拆分、保留原形。FTS5 支持自定义 tokenizer，但具体规则未设计。

不做这个，BM25 那一路对标识符查询基本失效。

## 与查询理解的关系

即使分词正确，还有一个上游问题：**自然语言描述里的词和代码标识符往往对不上。**

「为什么加载 yaml 会崩」里没有一个词会出现在 `ConfigParser::parse` 里。

这属于 `match/` 的查询理解，不是本模块能解决的。见 [`../open-questions.md`](../open-questions.md) C1。

## 相关决策

无独立条目。
