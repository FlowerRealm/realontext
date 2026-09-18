# `model/` 模型调用

全部外部模型调用收敛在这一个模块。**其余模块不直接碰 HTTP。**

**依赖**：libcurl。不依赖任何其他模块。
**被依赖**：`store/`（嵌入）、`match/`（查询嵌入、重排）、`ingest/`（提交摘要）、`playbook/`（Reflector / Curator）

## 为什么是基础层而不是管道一环

它被多处调用，时机完全不同：

| 调用方 | 时机 | 用途 |
|---|---|---|
| `store/` 建索引 | 离线批处理 | Chunk 嵌入 |
| `ingest/` | 离线批处理 | 提交摘要 |
| `match/` | 查询路径 | 查询嵌入、重排 |
| `playbook/` | 离线批处理 | Reflector / Curator |

所以它是能力提供者，不插在管道中间。限流、重试、批处理、Provider 抽象全部在这里。

## 当前配置

```yaml
embedding:
  provider: jina
  model:    jina-embeddings-v4
  task:     code
  dim:      1024
  api_key:  $JINA_API_KEY

rerank:
  provider: jina                  # 与嵌入共用同一个 key
  model:    jina-reranker-v3.5

summarize:
  provider: deepseek
  model:    deepseek-flash
  api_key:  $DEEPSEEK_API_KEY
  schedule: "0 1-4,6-10 * * 1-5"  # UTC 谷时窗口
  prompt_prefix_frozen: true      # 缓存命中的前提
```

三个用途，两个 key。

---

## 嵌入

### 选定：`jina-embeddings-v4` + `task=code`

| 参数 | 值 |
|---|---|
| 参数量 | 3.8B |
| 默认维度 | 2048 |
| **本项目使用** | **1024**（Matryoshka 截断，D7） |
| 可截断下限 | 128 |
| task 取值 | `retrieval` / `text-matching` / `code` |
| 代码用法 | 索引用 `code.passage`，查询用 `code.query`（非对称） |

### 为什么不用 v5

`jina-embeddings-v5-text-small`（677M，1024 维，32K 上下文）和 `v5-text-nano`（239M，768 维，8K）更新更小更快。

但官方列出的 adapter 是 `retrieval` / `text-matching` / `clustering`，**不含 `code`**。代码适配器目前只在 v4 上。

实现时重新确认。若 v5 补上 code adapter，换过去更划算。

### 价格对比

| 模型 | $/1M token | 上下文 | 代码专用 |
|---|---|---|---|
| **jina-embeddings-v4 (task=code)** | **$0.018** | — | 是 |
| voyage-4-lite | $0.02 | 32K | 否 |
| OpenAI text-embedding-3-small | $0.02 | 8K | 否 |
| voyage-4 | $0.06 | 32K | 否 |
| **voyage-code-4** | **$0.12** | **32K** | 是 |
| OpenAI text-embedding-3-large | $0.13 | 8K | 否 |

### 备选：`voyage-code-4`

明确为编码 agent 设计，32K 上下文，Matryoshka 2048/1024/512/256，另有量化选项。比 Jina 贵 6.7 倍。

**质量是否值这个差价用自建测试集实测**，不照搬别人的 benchmark。见 [`../benchmark.md`](../benchmark.md)。

### 免费额度

| 服务 | 额度 |
|---|---|
| Voyage | **2 亿 token**（voyage-4 代） |
| Jina | 1000 万 token |

### 命名陷阱

| | `jina-code-embeddings-0.5b/1.5b` | `jina-embeddings-v4` + `task=code` |
|---|---|---|
| 形态 | HuggingFace 开放权重 | 托管 API |
| 托管 API 可用 | **未确认** | 是 |
| 本项目使用 | 否 | **是** |

---

## 重排

### 选定：`jina-reranker-v3.5`

| 参数 | 值 |
|---|---|
| 参数量 | 0.6B |
| 类型 | **listwise** |
| 上下文 | **131,072 token**（query 加全部候选合计） |
| 截断 | 自动 |

131K 意味着 150 个 400-token 候选只占约 60K，一次调用全塞进去还有余量。

listwise 意味着所有候选一起评估再排序，优于 pairwise cross-encoder。

### 价格

| 服务 | 价格 | 计价方式 |
|---|---|---|
| Jina reranker | $0.02 / 1M token | 按 token |
| Cohere Rerank v3.5 | $0.001 / 次搜索 | 按次 |
| Voyage rerank-2.5 | $0.05 / 1M token | 按 token，Batch 减 33% |

选 Jina 是为了与嵌入共用一个 key 和额度池。

**重排是查询侧最大的成本项**，因为它是唯一随查询次数线性增长的部分。若成本成为问题，Cohere 按次计价可预测性更好，且不会逼你为省钱调小召回数（那是错误的优化方向）。

### 评测期改走 `voyage rerank-3`（D27）

**Jina key 拿不到**，阶段 2 的付费嵌入卡的是同一件事。阶段 5 的重排改走 Voyage：真托管 API，代码路径与将来接 v3.5 一致——换 endpoint 与响应解析，`Limiter`、重试分类、`Post` 全部复用。顺带验证 Voyage 的接入，嵌入备选 `voyage-code-4` 是同一家。

**取 rerank-3 不取 rerank-2.5**：上面价格表里的 2 亿免费 token 只给 **rerank-3 系列**，2.5 那一代是 0。两个都实测可用。

**生产选型不动**：`jina-reranker-v3.5` 仍是上面表里的选择。本节只记「key 的可得性是一个实际约束」。

类型差别要记住，它改的是成本曲线：

| | `jina-reranker-v3.5` | `voyage rerank-3` |
|---|---|---|
| 类型 | listwise | cross-encoder（pairwise） |
| 200 条候选 | 一次调用 | 200 次前向 |
| 候选之间 | 互相可见，一起排 | 各算各的分 |

所以池深在 Voyage 上是一个真旋钮（D27 定起步 50），换回 listwise 时它基本免费。

### Voyage rerank 实测（2026-09-18）

```
POST https://api.voyageai.com/v1/rerank
{"query": ..., "documents": [...], "model": "rerank-3", "truncation": true}
→ {"data": [{"index": i, "relevance_score": s}, ...], "usage": {"total_tokens": n}}
```

单次至多 1,000 个文档；计费 token = **query token × 文档数 + 文档 token 之和**，限流器按这个公式估算。上下文 32K。

**免费档的限流会挡住跑分**：没绑支付方式的账号是 **3 RPM / 10K TPM**，一条 2.5 KB 的 query 配池深 10 就超过单分钟额度，重试耗尽后硬报错。绑支付方式后恢复标准限流，2 亿免费 token 仍然生效。

---

## 摘要

### 选定：`deepseek-flash`

两个可叠加的折扣。

### 折扣一：自动上下文缓存

> "Context caching is on by default across all tiers."

不需要 API 改动。**唯一前提是 prompt 前缀完全固定，变量只放最后。**

这是设计约束不是优化项。模板一旦定下不要再改——改一次前缀，全部历史缓存作废。

### 折扣二：峰谷差价

价格（$ / 1M token）：

| | 缓存命中输入 | 未命中输入 | 输出 |
|---|---|---|---|
| **谷时** | **$0.003** | $0.15 | $0.60 |
| 峰时 | $0.006 | $0.30 | $1.20 |

谷时窗口：**UTC 01:00–04:00 和 06:00–10:00，周一至周五。**

`deepseek-v4-pro` 谷时 $0.022 / $0.66 / $1.98，峰时翻倍。

### 效果

50,000 条提交，输入 2000 token、输出 150 token：

| 场景 | 成本 |
|---|---|
| 谷时 + 80% 缓存命中 | **约 $7.7** |
| 峰时无缓存 | 约 $40 |

**差 5 倍，靠两个不改业务逻辑的配置项。**

### 其他候选

| 模型 | 输入 $/1M | 输出 $/1M |
|---|---|---|
| Qwen3.7 Flash | $0.03 | $0.13 |
| Gemini 2.0 Flash-Lite | $0.08 | $0.30 |
| GLM-5.3-Flash | $0.15 | $0.50 |
| Claude Haiku 4.5 | $1.00 | $5.00 |

Haiku 4.5 跑同样回填约 $137（Batch 减半后 $69），贵约 15 倍。摘要是提取任务不是推理任务。

---

## Provider 指纹与切换

**本模块最重要的约束。**

### 嵌入空间不可混用

不同 Provider 的向量在不同空间。混用能算出余弦相似度，但物理意义为零。

失败静默：

```
切换 Provider
  ↓ 新增量用新模型嵌入，老索引仍是旧模型
  ↓ 检索照常返回结果，不报错
  ↓ 质量下降，但结果看起来合理
  ↓ 没有 eval 就永远发现不了
```

### 强制约束

`store/` 的 `meta` 表记录六项（D22）：

```
endpoint / model / task / dim / max_bytes / input_version
```

前四项是向量从哪来，后两项是 Chunk 怎么变成输入文本。只记前四项的话，改了切块方案指纹照样放行。

启动时与本模块配置比对，**任何一项不一致则拒绝启动**，明确报错要求全量重嵌。

不做自动迁移，不做混合兼容。硬失败是唯一正确行为。

### 维度固定 1024 的作用

Jina v4 默认 2048、可截断 128；voyage-code-4 支持 2048/1024/512/256。取交集中的 1024。

从 Jina 切到 Voyage 时：存储布局不变、HNSW 参数不变、磁盘占用不变，**只需重新嵌入，不动 schema**。

顺带比 2048 省一半存储和内存。

---

## 配额治理

### 速率限制

Jina 按 key 限流：

| 档位 | RPM | TPM |
|---|---|---|
| 免费 | 100 | 100K |
| 付费 | 500 | 2M |
| 高级 | 5,000 | 50M |

另有 IP 级 10,000 请求 / 60 秒。

需要**双令牌桶**（请求数 + token 数），参数从配置读。首次索引会持续打满配额数小时，限流器实现不当就是一路 429。

### 不需要自己分批

> "There is no batch size limit for either the Embeddings or Reranker APIs."

两个 API 都不限单次请求条目数，内部按 token 数自动分批。这消掉了一整块打包逻辑，按 TPM 限流即可。

### 重试

429 和 5xx 指数退避加 jitter。

**批处理作业里失败不能丢**，要进重试队列并持久化。跑了三小时因为一次网络抖动重头来，用户会直接卸载。

### 批处理折扣

| 服务 | 折扣 |
|---|---|
| Voyage Batch | -33% |
| OpenAI Batch | -50% |
| Anthropic Batch | -50% |

异步作业的 **job id 必须落盘**，不然作业跑完找不回结果，钱白花。

### 谷时调度

DeepSeek 的摘要作业和 `playbook/` 的 Reflector / Curator 共用同一个调度器和队列：

```
schedule: "0 1-4,6-10 * * 1-5"   # UTC
```

队列持久化，跨窗口续跑。窗口结束暂停，下一窗口继续。

---

## 数据来源

价格与规格采集于 2026 年 9 月，各家调价频繁，实现前重新核对官方定价页。

- Jina：[Embedding API](https://jina.ai/embeddings/) · [Reranker API](https://jina.ai/reranker/) · [jina-embeddings-v4](https://jina.ai/models/jina-embeddings-v4/)
- Voyage：[voyage-code-4](https://openrouter.ai/voyageai/voyage-code-4) · [价格表](https://embeddingcost.com/voyage)
- DeepSeek：[定价](https://www.aipricing.guru/deepseek-pricing/) · [上下文缓存](https://deepseek-usa.ai/docs/deepseek-context-caching/)

## 相关决策

D5（全部走 API）· D6（Provider 选型）· D7（维度 1024）· D8（索引指纹）· D22（嵌入输入）· D27（重排接入）
