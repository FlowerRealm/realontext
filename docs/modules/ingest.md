# `ingest/` 采集

取原始字节和元数据，不解析内容。

**依赖**：libgit2。不依赖任何其他模块。
**被依赖**：`parse/`、`model/`（提交摘要）

## 裸镜像

服务端维护一份 bare mirror clone。

```
bare mirror clone
  ↓ git fetch --all --prune
  ↓ git for-each-ref --format='%(refname) %(objectname)'
  ↓ 与上次索引的 ref SHA 比对
  ↓ git diff --name-status <old> <new> 取变动 blob
```

**不靠 IDE 插件采集。** 插件模型有根本限制：没人 checkout 的分支它看不见。本项目索引全部分支（D14），必须服务端持有镜像。

同一份镜像同时供给代码和提交历史。

`--prune` 必须带：远端删除的分支要触发索引回收。

## 分支枚举

`git for-each-ref refs/remotes/origin/` 返回什么就要什么。**不做新鲜度过滤**（D14）。

不按提交时间过滤，不按 PR 状态过滤，不要求配置 allowlist。

代价：成本随分支陈旧程度上升，不是常数。系数可用纯 git 免费测出，命令见 README。启动时输出实测的分支数与唯一 blob 数——这是运行日志，不做美元换算（D10）。

## 增量：git 本身就是 merkle tree

Cursor 为增量索引建了一棵 merkle tree 每 10 分钟扫描，那是因为它索引未提交的工作区状态。

本模块只取 HEAD，dirty 文件走另一条路径，所以直接用 git 对象哈希。

所有变化收敛到一条命令：

```
git diff --name-status <old_sha> <new_sha>
```

新增、修改、删除、重命名、切分支——**同一个代码路径**。blob SHA 直接交给 `store/` 做缓存 key。

特殊情况不是被 if 处理掉的，是不存在。

## 触发方式

| 方式 | 配置成本 | 实时性 | 适用 |
|---|---|---|---|
| **PAT + 轮询** | 最低 | 分钟级 | 默认 |
| GitHub App + webhook | 需建 App + 隧道 | 秒级 | 团队 |
| Deploy key + 轮询 | 中 | 分钟级 | 单仓库只读 |

**默认轮询**（D9）。webhook 打不到本地服务，要接收就得配隧道。让用户为装一个索引器先配 cloudflared，采纳率归零。

轮询开销极小：`git ls-remote` 只拉 ref 列表，SHA 变了才真 fetch。

### GitHub 权限

```
Contents: Read          仓库内容与全部分支
Metadata: Read          必需
Webhooks: Read/Write    仅 webhook 模式
```

**只申请只读。** 任何写权限都会让谨慎的用户放弃。

## 过滤

在数据离开本模块之前执行，两类。

### 不索引（成本）

```
third_party/  vendor/  external/  node_modules/
*.min.js  *_generated.*  *.pb.cc  *.pb.go  moc_*.cpp
```

vendored 目录在大型项目里常占一半以上代码量。

超过 1 MB 的 blob 同样不索引——到这个尺寸是数据，不是源码。

**评测时同一套规则照常生效。** 测的是产品的真实行为；被过滤掉的 ground truth 文件计入 `gt_files_unreachable`，分数代价公开。

### 不发送（安全）

```
.env  .env.*  *.pem  *.key  id_rsa  credentials  secrets/
```

外加发送前的凭据形态扫描。详见 [`../cross/privacy.md`](../cross/privacy.md)。

这一类强制执行。**客户端上传 dirty overlay 前也必须跑同一套规则**——不能等到服务端。

## 提交历史采集

`git log --all`，覆盖全部分支。

过滤规则决定成本：

| 规则 | 理由 |
|---|---|
| 跳过 merge commit | 无实质 diff |
| 跳过纯格式化提交 | 改动大、信息量零 |
| 跳过 vendored 目录的提交 | 同代码路径规则 |
| 跳过生成文件 | `.pb.cc`、`moc_*.cpp` |
| 大 diff 截断 | 超阈值只留文件列表加前 N 行 |

通常砍掉 30–50% 的提交量。

### 深度策略

| 策略 | 建议 |
|---|---|
| 全部历史 | 十年以上老项目不划算 |
| **最近 3 年** | **默认** |
| 最近 N 条 | 配置项 |
| 仅默认分支 | 起步阶段 |

## dirty overlay

未提交文件由**客户端**采集，随查询上传（[`serve.md`](serve.md)）。

按定义在开发者本机，服务端的裸镜像拿不到。这是本模块唯一不经镜像的数据源，也是唯一在客户端执行的部分。

## 失败处理

首次拉取是长时间作业，必须断点续传：

```
已处理的 blob_sha 集合
每个分支上次索引的 head SHA
```

## 未建模的数据源

Augment Context Engine 宣称索引但本项目当前没有的：external docs、issue / ticket 正文、风格指南、tribal knowledge。

见 [`../open-questions.md`](../open-questions.md) B1–B3。issue/PR 正文可从 GitHub API 免费取得，是最低成本的扩展方向。

## 相关决策

D3 · D9 · D10 · D13 · D14
