---
name: memfabric-log-audit
description: "当用户要求扫描/巡检代码仓日志代码、排查关键执行分支中缺失或过弱的日志、输出日志修改方案（日志整改）时使用。"
---

# memfabric-log-audit — 关键执行分支日志审计

## 目标

对指定范围（整个仓库或某模块，如 `src/smem/`、`src/hybm/`、`src/acc_links/`、`src/acc_offload/`、`app/zbal`）的日志代码做**只读扫描**，
找出**关键执行分支中缺失或过弱的日志**，并按优先级输出可直接执行的**日志修改方案**（做法/改法 + 建议代码）。

本 skill 只产出**修改方案**，不直接改代码；需要落地时由用户确认后再改。

## 与 `memfabric-code-review` 的区别

| | `memfabric-code-review` | `memfabric-log-audit` |
|---|---|---|
| 范围 | 本次变更（diff） | 全仓/指定模块（存量 + 增量） |
| 关注点 | 错误退出路径的错误日志 | **关键执行分支**的缺失/过弱日志（含正常关键节点） |
| 产物 | 审查问题清单 | **按优先级排序的日志修改方案** |

两者可组合：先 `memfabric-log-audit` 出方案，改完再用 `memfabric-code-review` 复查变更。

## 范围与输入

1. 默认扫描 `src/` 和 `app/` 下的 C/C++ 源码；用户指定路径或模块时以用户为准。
2. 若用户给出具体文件/函数/commit，则限定在该范围，不扩散为全量重构。
3. 扫描是**只读**的：允许读调用方、被调用方、资源释放与加锁上下文以判断日志是否重复或足够，但不得顺带重构无关代码。

## 一、识别「关键执行分支」

关键执行分支不限于错误退出，包含下表全部类别。命中任一类别即纳入检查：

| 分支类别 | 识别特征 | 期望最低日志 |
|---|---|---|
| 错误/异常退出 | `return` 错误码、`throw`、`goto error`、`if (ret != OK)` | ERROR |
| 资源获取/释放失败 | `malloc`/`alloc`/`open`/`mmap`/`create`/`register` 失败 | ERROR |
| 参数校验失败 | 空指针、越界、非法组合、前置状态不满足 | ERROR（非法）或 WARN（可容忍） |
| 外部调用返回码判断 | ACL/URMA/HCOM/etcd/RPC 返回 `!= 0` | ERROR（带 API 名与返回码） |
| 状态机/协议跳转 | 状态迁移、握手、重连、心跳超时 | INFO（关键节点）/ WARN（异常） |
| 并发/超时/重试 | `timeout`、`retry`、加锁失败、CAS 失败、队列满 | WARN/ERROR（高频处须限流） |
| 配置解析/生效 | store key 缺失、配置回退默认值、开关切换 | INFO/WARN |
| 关键成功里程碑 | 初始化完成、连接建立、内存注册完成、传输启动 | INFO |
| 热点循环内部 | `for`/`while` 内的失败或逐元素打印 | 聚合统计或用 `*_LIMIT` |

## 二、判定「缺失」

满足以下任一即判定为**缺失**，写明分支位置：

- 关键分支**完全没有任何日志语句**（如 `if (ret != OK) { return ret; }`）。
- 只有 `return`/`goto`/`continue`，没有失败原因与关键参数。
- `catch`/`else`/`default` 分支为空，吞掉了异常或非法状态。
- 使用 `printf`/`std::cout`/裸 `perror` 代替项目日志宏，导致无级别、无统一前缀、无法限流。

## 三、判定「过弱」并分级

日志存在但不满足关键分支要求时，判定为**过弱**，按下表分级（等级用于排优先级）：

| 等级 | 名称 | 判定标准 | 典型反例 |
|---|---|---|---|
| S1 | 完全缺失 | 关键分支无任何日志 | `if (ret != OK) return ret;` |
| S2 | 无失败原因/来源 | 无原始错误信息、无外部 API 名、无返回码 | `SM_LOG_ERROR("failed");` |
| S3 | 无关键参数 | 缺 `rankId`/`ret`/`handle`/端点等定位参数 | `LOG_ERROR("register failed");` |
| S4 | 级别不当 | 错误路径用 DEBUG/TRACE；正常高频路径用 ERROR 制造噪声 | 错误返回处 `SM_LOG_DEBUG` |
| S5 | 表述不可定位 | 泛化文案，不含变量与实际取值 | `"something went wrong"` |
| S6 | 热点过载 | 高频循环内逐次 INFO/ERROR，或同一错误重复刷屏 | `for` 内 `SM_LOG_ERROR(...)` 未限流 |
| S7 | 泄露风险 | 打印了敏感信息（密钥/token/证书/原始 buffer/完整 store value） | 打印 `ASCEND_MF_STORE_URL` 中含认证信息 |

判定 S2/S3 时，参数按下方「关键参数优先级」选取，**只打代码中真实存在的字段**，禁止照搬泛业务示例虚构 `traceId`/`jobId`。

### 关键参数优先级

| 优先级 | 参数类型 | 本仓库示例 |
|---|---|---|
| 高 | 标识符 | `rankId`、`localRank`、`remoteRank`、`deviceId`、`pid`、`threadId`、`channel`、`handle` |
| 高 | 资源名/句柄 | `shmName`、`shmKey`、`storeKey`、`bufferId`、`memHandle`、队列名、文件名 |
| 高 | 通信/任务定位 | `traceId`、`requestId`、`jobId`、`opId`（仅当真实存在） |
| 中 | 路径/端点 | 文件路径、`ASCEND_MF_STORE_URL`、etcd endpoint、TCP/HCOM/RDMA 地址与端口 |
| 中 | 配置项 | 超时、重试次数、阈值、`worldSize`、`rankSize`、`opType`、`xpu_type`、HCOM/RDMA 开关 |
| 中 | 外部调用目标 | `smem_bm_*`/`hybm_*` 调用点、ACL/URMA/HCOM/etcd 接口名、RPC 方法名 |
| 中 | 状态码/错误码 | `ret`、`errno`、`StoreErrorCode`、`BM_*` 错误码、ACL/HCOM/URMA 错误码 |
| 低 | 内部中间值 | `idx`、`offset`、`batchSize`、`len`/`length`、`srcVA`/`dstVA` |

## 四、扫描方法

使用 Grep 工具（不用 shell grep）按下列模式初筛，再人工读上下文确认：

- **错误退出候选**：`return\s+(-?\d+|ret|err|error|ERR_)`、`throw`、`goto\s+\w*error`
- **返回码判断分支**：`if\s*\(.*(ret|rc|status|result).*(!=|==|>|<)`
- **空分支/吞异常**：`catch\s*\([\s\S]*?\)\s*\{\s*\}`、`else\s*\{\s*\}`
- **裸输出**：`printf\s*\(`、`std::cout`、`perror\s*\(`（应改用日志宏）
- **热点与限流**：`for\s*\(`/`while\s*\(` 体内含 `LOG_(INFO|DEBUG|ERROR)`，检查是否应使用 `*_LIMIT`
- **定时/重连/心跳路径**：`timeout`、`retry`、`reconnect`、`heartbeat` 附近的分支

本仓库日志宏（优先沿用所在模块的宏，不要跨模块混用，不要自造宏）：

| 模块 | ERROR | WARN | INFO | DEBUG | 限流 |
|---|---|---|---|---|---|
| `src/smem/` | `SM_LOG_ERROR` | `SM_LOG_WARN` | `SM_LOG_INFO` | `SM_LOG_DEBUG` | `SM_LOG_WARN_LIMIT` |
| `src/hybm/` | `BM_LOG_ERROR` | `BM_LOG_WARN` | `BM_LOG_INFO` | `BM_LOG_DEBUG` | `BM_LOG_ERROR_LIMIT` |
| `src/acc_links/` | `LOG_ERROR` | `LOG_WARN` | `LOG_INFO` | `LOG_DEBUG` | `LOG_ERROR_LIMIT` / `LOG_INFO_LIMIT` |
| `src/smem/csrc/config_store/` | `STORE_LOG_ERROR` | `STORE_LOG_WARN` | `STORE_LOG_INFO` | `STORE_LOG_DEBUG` | `STORE_LOG_ERROR_LIMIT` |
| `src/acc_offload/` | `OFFLOAD_LOG_ERROR` | `OFFLOAD_LOG_WARN` | `OFFLOAD_LOG_INFO` | `OFFLOAD_LOG_DEBUG` | — |
| `src/smem/.../mk_transfer_adapter/` | `ADAPTER_LOG_ERROR` | `ADAPTER_LOG_WARN` | `ADAPTER_LOG_INFO` | `ADAPTER_LOG_DEBUG` | — |

规范依据：`docs/zh/contributions/coding_standards/docs_specification.md`、
`docs/zh/contributions/coding_standards/cpp-secure.md`。

## 五、输出：日志修改方案

按严重度（S1 → S7）和分支重要性排序，逐条给出。每条建议必须可直接落地：

````markdown
### [LGA-001] S1 完全缺失 · P0

**位置**：`src/smem/csrc/xxx/yyy.cpp:123`

**关键分支**：错误退出（外部调用返回码判断）

**现状**：`if (ret != OK) { return ret; }` — 无任何日志

**影响**：该分支失败时无根因、无定位参数，线上只能看到上层错误码。

**建议**：补齐 ERROR 日志，包含错误原因、外部 API 名、返回码与关键标识。

```cpp
if (ret != OK) {
    SM_LOG_ERROR("smem_bm_create failed, rankId=" << rankId << ", shmName=" << shmName
                 << ", ret=" << ret);
    return ret;
}
```

**参数说明**：`rankId`（标识符）、`shmName`（资源句柄）、`ret`（返回码）。
````

方案末尾附汇总表与分批建议：

| 编号 | 等级 | 位置 | 分支类别 | 优先级 | 建议级别 |
|---|---|---|---|---|---|
| LGA-001 | S1 | `...yyy.cpp:123` | 错误退出 | P0 | ERROR |
| LGA-002 | S6 | `...zzz.cpp:88` | 热点循环 | P1 | ERROR（改 `*_LIMIT`） |

- **P0**：错误退出/参数校验失败/外部调用失败且完全无日志（S1、S2）。
- **P1**：有日志但缺关键参数或级别不当（S3、S4）。
- **P2**：表述不可定位、热点过载、缺失关键成功里程碑（S5、S6）。
- **P3**：仅可读性/一致性优化（如统一文案），可选。

## 六、例外（可接受不打日志）

以下情形判定为**可接受例外**，需在方案中注明，不强制修改：

- **下层已记录**：下层已记录失败原因与关键参数，本层仅透传错误。
- **上层统一记录**：错误逐层上抛，由顶层中间件/框架统一记录（须确认上层确有记录逻辑）。
- **测试代码**：`test/`、`*_test.cpp` 等中的错误路径，`EXPECT`/`ASSERT` 已足够。
- **快速失败辅助函数**：如 `mustXxx`（约定 panic/abort），语义上不要求日志。
- **高频数据面**：性能敏感路径（每次传输/每元素）不逐次打印；改为聚合统计或 `*_LIMIT`。

## 七、原则

- **只读扫描**：本 skill 只产出方案，不改代码；改动由用户确认后执行。
- **最小化建议**：只针对已确认的缺失/过弱点，不顺带重构、重命名或格式化无关代码。
- **贴合本仓库语义**：只打代码中真实存在的字段，禁止虚构泛业务 ID；沿用模块现有日志宏。
- **可定位优先**：优先打印能直接定位到请求/任务/资源的参数，而非泛泛的 "failed"。
- **不制造噪声**：新增 ERROR/WARN 意味着失败或异常真实发生；正常高频路径不得用 ERROR。
- **不泄露**：禁止打印密钥、token、证书、原始 buffer/共享内存内容、完整 store value。
- **格式约束**：新增/修改日志行不超过 120 字符；保持与所在文件现有日志风格一致。
