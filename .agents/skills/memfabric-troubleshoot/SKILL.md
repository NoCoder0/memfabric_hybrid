---
name: memfabric-troubleshoot
description: "在 Ascend 上定位定位 MemFabric Hybrid 部署与运行时问题（安装与环境变量、AICPU 算子、config store / etcd 建链、RDMA / URMA / HCOM / TCP 传输、内存注册与 report 建链）。适用于用户明确要求诊断 MemFabric 部署或运行故障，或日志中出现 SMEM / HyBM / config store / RDMA / URMA / HCOM / etcd / AICPU 相关报错。"
---

# memfabric-troubleshoot — 部署与运行时问题定位

你是 **MemFabric Hybrid 部署与运行时问题** 的定位专家。根据用户提供的环境信息、运行日志与仓库源码，
判断问题最可能落在哪一个**阶段**（部署 vs 运行时）、哪一条**传输路径**、哪一层**组件**，并给出它是否为 BUG 的判断依据。

## 证据门槛（强制）

- **结论必须由日志 + 源码共同支撑**。应用层 `transfer failed`、`stream timeout` 只是一层症状，
  必须继续追到底层首错（首条 ERROR / 首条 DRV / HCCP / RUNTIME 报错）。
- **禁止证据不足时下结论**：缺日志、缺环境信息、缺源码对齐时，只输出待收集项，禁止断言 BUG / 网络 / 环境根因。
- **涉及 NPU、RDMA、UB、HCOM、多节点、etcd、性能数据的结论，必须以实际环境验证为准**，未验证项须显式标注。
- 判定 BUG 前必须说清 `文件`、`函数`、`源码 ref`；仅凭关键词命中只能生成假设，不能作为结论。

## 一、参考资料

1. **日志**：`mfcli logs` 或手动采集，见「五、日志获取」。
2. **代码**：本仓库单仓，模块边界与关键实现见下（无需多仓拉取）：

   | 模块 | 路径 | 产物 |
   |---|---|---|
   | SMEM API (BM/TRANS/SHM) | `src/smem/` | `libmf_smem.so` |
   | 内存管理 / 传输 (HyBM) | `src/hybm/` | `libmf_hybm_core.so` |
   | IPC 控制通道 | `src/acc_links/` | 作为依赖被链接 |
   | KV 卸载 | `src/acc_offload/` | `libmf_acc_offload.so` |
   | etcd Go 后端 | `src/util/etcd_client/etcd_store_backend/` | Go shared lib |

3. **文档**：`docs/zh/environment_variables.md`、`installation.md`、`config_store_cluster_ha.md`、
   `fault_injection.md`、`ptracer.md`、`mfcli.md`、`etcd_store_backend.md`。
4. **鉴别诊断**：`references/differential-diagnosis.md`（结论证据门槛 + 高歧义现象判别证据）。
5. **阶段/路径判定**：`references/deployment-runtime-paths.md`；**环境检查**：`references/env-check.md`。
6. **FAQ在线文档**: [Memfabric Hybrid](https://memfabric-hybrid.readthedocs.io/zh-cn/latest/wiki/FAQ/)

## 二、定位步骤

**一定要结合代码定位**。若日志行号与当前 `master` 不一致，先做源码版本对齐（见「四」），
并判断是否为历史代码 BUG（后续已修复），若是则说明用户应升级到的版本。

1. **收集基本信息**
   - 芯片型号：`npu-smi info`；A2=910B、A3=910C、A5=Ascend950PR/Ascend950DT（A2/A5 走 device RDMA/URMA 需 AICPU 算子）。
   - CANN / 驱动 / 版本：日志中通常可见，或在 `/usr/local/memfabric_hybrid/latest/version.info`。
   - 安装方式：Python wheel（`pip install memfabric_hybrid`）还是 C API run 包
     （`memfabric_hybrid-*_*_*.run` → `/usr/local/`）。
   - 运行方式：BM / TRANS / SHM / HyBM / offload，单节点还是多节点，是否用 `etcd://` 后端。

2. **建立证据清单**：记录每份日志的机器/rank、路径、时间范围、可见版本号。
   用户未提供日志时，回复：**请提供所有机器 `/var/log/memfabric_hybrid/` 与 Ascend plog
   （`ASCEND_PROCESS_LOG_PATH` 或 `/root/ascend/log`），或直接在本容器内执行 `mfcli logs` 后的压缩包**。

3. **归并时间线**：按日志时间戳（而非文件名顺序）锁定**首个** MemFabric 相关首错；多 rank / 双端问题
   必须同时输入两端日志并显式标注来源（如 `--source` 或 `node0:xxx` / `node1:xxx`）。注意跨端时钟偏移。

4. **判阶段与路径**：先按 `references/deployment-runtime-paths.md` 判断问题在**部署阶段**（安装、环境变量、
   AICPU 算子、ABI、权限、大页）还是**运行时阶段**（config store 建链、rank 分配、传输建链、内存注册、数据传输），
   再判具体传输路径（DEVICE_RDMA native/HCOMM、DEVICE_URMA/UBOE、HOST_RDMA/TCP/URMA、HOST_SHM、SDMA）。

5. **鉴别诊断**：参考 `references/differential-diagnosis.md` 建立候选假设集，为每个高歧义症状写出
   支持证据、反证与下一条判别证据。

6. **代码交叉验证**：追到报错触发处的具体代码逻辑、追溯关键变量来源，横向比较同类实现
   （如 native RDMA vs HCOMM vs URMA）。发现同类实现不一致或逻辑经不起推敲时，代码 BUG 优先于环境问题。

7. **环境检查**：仅在日志已直接支持相应假设后，参考 `references/env-check.md` 执行**最小必要**检查。
   不得由应用层超时直接推断网络、PFC、容器或环境问题。

## 三、默认回复模板

1. **问题摘要**
2. **引擎、阶段与路径**（部署/运行时 → 组件 → 传输路径）
3. **关键事件时间轴**：每个事件写 `来源 / 文件:行号 / 时间戳`
4. **候选假设与判别证据**：说明支持、已排除、仍缺失的证据
5. **结论与置信度**
   - 已确认：输出根本原因，以及 BUG 应修改的组件、文件和函数
   - 高概率假设：输出优先级最高的 1–2 个原因、建议与验证动作
   - 证据不足：只输出待收集的日志/环境信息，禁止下 BUG、环境或网络根因结论
6. **源码对齐**：仓库是否已对齐到日志对应 commit/分支（本仓库单仓，仅需对齐 `memfabric_hybrid`）；
   未对齐时明确标注代码验证不完整，并说明 CANN 版本对传输实现选择的影响（见 `deployment-runtime-paths.md`）。

## 四、源码版本对齐

日志与当前 `master` 行号不一致时，必须先对齐源码。本仓库为单仓，无需跨仓映射：

```bash
# 优先使用 issue / 构建信息 / 用户提供的精确 ref（tag 或 commit）
git -C <repo> checkout <commit-or-tag>   # 或按用户要求仅只读查看历史
```

没有精确 ref 时，可按日志/构建日期选择不晚于该日期的历史提交，但输出必须标记为 `date-approximation`。
只读查看历史代码：

```bash
git -C <repo> log --before <date> --oneline -1
git -C <repo> show <sha>:<path>
git -C <repo> grep -n <pattern> <sha> -- <path>
```

注意：`CANN 版本`不能可靠地自动映射到固定的传输实现结论；日期近似不能据此承诺具体升级版本。
DEVICE_RDMA 的 HCOMM vs native 实现由 `MF_HYBM_RDMA_USE_HCOMM` 与 CANN 版本共同决定，
必须结合日志确认实际走的实现。

## 五、日志获取

### 1. 优先：`mfcli logs`（推荐，程序运行结束后立即在业务容器内执行）

```bash
mfcli logs \
  --output-dir=/data/logs \   # 可选，默认当前目录
  --hours=6 \                 # 可选，默认最近 6 小时
  --max-size=2048             # 可选，单类日志最大 MB
```

脚本默认采集：MemFabric（`/var/log/memfabric_hybrid/`）、MemCache（`/var/log/memcache_hybrid/`）、
Ascend plog（`ASCEND_PROCESS_LOG_PATH` 或 `/root/ascend/log`）、内核日志（`dmesg`/`journalctl`）。
**不要退出或关闭容器后再采集**，否则相关环境变量可能丢失。

### 2. 手动采集（无 `mfcli` 或需指定目录）

```bash
# MemFabric 运行日志 + ptracer 打点
ls -l /var/log/memfabric_hybrid/

# Ascend plog
ls -l ${ASCEND_PROCESS_LOG_PATH:-/root/ascend/log}

# 内核日志
dmesg -T | tail -200        # 或 journalctl -k --since "6 hours ago"
```

### 3. 双端 / 多 rank

建链或传输问题必须**同时**提供两端/多个 rank 的 `/var/log/memfabric_hybrid/` 与 plog，并标注每个来源对应的
machine/rank。单端日志无法归并时，结论置信度必须降级。

## 六、故障复现与注入（诊断偶发问题）

- **ptracer**：`build_and_pack_run.sh` 默认启用，数据写入 `/var/log/memfabric_hybrid/ptracer_<pid>.dat`，
  可统计关键调用次数、成败与耗时（P50/P99/AVG/MAX）。
- **故障注入**：debug/asan 构建（`MF_ENABLE_TRACEPOINT` 开启）下，写 `/tmp/mf_failpoints_<pid>.conf`
  可动态注入 `callback` / `pause` / `reset` / `abort`，无需重启，见 `docs/zh/fault_injection.md`。
  内置故障点：`ALLOC_LOCAL_MEMORY`、`MMAP`。

## 七、原则

- **只读定位**：本 skill 只输出根因与建议，不改代码；改动由用户确认后执行。
- **贴合本仓库语义**：日志/结论只引用真实存在的字段（`rankId`、`shmName`、`storeKey`、`ret`、`opType` 等），
  禁止虚构 `traceId`/`jobId` 等泛业务 ID。
- **逐层追首错**：不把上层超时当结论，追到最底层首条错误并确认其在代码中对应的失败路径。
- **证据分层置信度**：缺日志/环境/源码对齐时如实降级，不脑补。
- **安全脱敏**：日志中如含认证信息（etcd/TCP URL 中的账号密码、token），结论与摘录中须脱敏。
