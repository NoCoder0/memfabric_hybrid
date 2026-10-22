# 部署与运行时阶段 / 传输路径判定

用于把问题定位到「部署 vs 运行时」阶段，再落到具体组件与传输路径。路径均相对仓库根目录，
变量名以 `docs/zh/environment_variables.md` 为准。

## 一、两阶段总判定

| 判定特征 | 部署阶段 | 运行时阶段 |
|---|---|---|
| 发生时机 | 安装/构建/首次启动前 | 程序运行中（init、join、传输、退出） |
| 典型报错 | `pip install` 失败、`.run` 安装失败、`mfcli kernel install` 失败、`ASCEND_HOME_PATH` 未设置、so 权限拒绝、ABI 符号不兼容、大页不足初始化失败 | config store 连接失败、join 超时、rank 分配失败、传输建链失败、内存注册失败、copy/write 超时 |
| 日志特征 | 安装脚本输出、`ImportError`/`undefined symbol`、加载器报错 | `/var/log/memfabric_hybrid/` 运行日志、plog |
| 入口章节 | 「二、部署阶段判定」 | 「三、运行时阶段判定」 |

注意：`MF_LOG_LEVEL` **仅 Python 接口下生效，bm/shm 场景不生效**——用户抱怨"改了日志级别没变化"属于此类。

## 二、部署阶段判定

### 2.1 安装与包

| 症状 | 检查点 | 依据 |
|---|---|---|
| wheel 安装后 offload 不可用 | 是否执行 `mfcli kernel install`；`mfcli kernel info` 查看扩展库 `libmf_hybm_accoffload.so` 状态 | `docs/zh/installation.md`、`mfcli.md` |
| `.run` 包场景 offload 不可用 | 纯 run 包**不再支持** acc_offload 扩展库，须装 wheel | `installation.md` |
| so 权限拒绝（Permission denied） | run 包安装后 `.so` 默认 440，部署用户需加入安装属组（`usermod -a -G`） | `installation.md` 三 |
| 提示 wheel 中没有 AICPU 算子 | 换用包含 AICPU 算子的 wheel | `mfcli.md` FAQ |
| `mfcli` 提示未设置 `ASCEND_HOME_PATH` | `export ASCEND_HOME_PATH=/path/to/cann` | `mfcli.md` FAQ |
| GPU 环境加载 CUDA 库失败 | `CUDA_HOME` 必填；NPU 环境则不需要 | `environment_variables.md` |

### 2.2 构建问题（源码编译场景）

| 症状 | 检查点 |
|---|---|
| 新发行版（openEuler 24.03 等）链接/运行期符号不兼容 | 默认 `BUILD_OPEN_ABI=OFF`（旧 C++11 ABI）；GCC 12.x / 新 libstdc++ 需 `--build_open_abi ON` |
| HOST_RDMA/HOST_TCP/HOST_URMA 链接失败 | 需 `--build_hcom ON`；RDMA 需先 `apt install libibverbs-dev`；UB 需 `--build_hcom_ub ON` |
| `etcd://` URL 不支持 | 需 `--build_etcd_backend ON` 编译 Go 后端 |
| 测试/样例编译失败 | 需先 `git submodule update --recursive --init`（googletest、mockcpp） |
| whl 版本号为空 | 构建 Python wheel 需设置 `MEMFABRIC_VERSION` |

### 2.3 AICPU 算子与驱动（A2/A5 device 路径）

A2（910B，device_rdma 走 hcomm）和 A5（950，DEVICE_URMA）需要安装 AICPU 算子：

#### 2.3.1 `mfcli kernel install`（`--install-for-all` 供所有用户；`--soc-version A2/A3/A5` 指定芯片）

#### 2.3.2 A2/A5 使用自定义 AICPU kernel 需**先关闭驱动验签**

```bash
npu-smi set -t custom-op-secverify-enable -i 0 -d 1
npu-smi set -t custom-op-secverify-mode -i 0 -d 0
```

#### 2.3.3 `mfcli kernel info` / `mfcli kernel version` 验证安装状态与版本

### 2.4 大页内存（A2 DRAM 池化）

A2 使用 DRAM 池化需按池化内存大小配置大页，否则**初始化失败**：

```bash
grep Huge /proc/meminfo                 # 检查
echo 1024 > /proc/sys/vm/nr_hugepages   # 配置（按需）
```

### 2.5 版本核对

- C API：`cat /usr/local/memfabric_hybrid/latest/version.info`
- Python：`mfcli version`
- DEVICE_RDMA 走 HCOMM 要求 CANN ≥ 9.1（ACL ≥ 1.17）；版本不满足时即使 `MF_HYBM_RDMA_USE_HCOMM=1` 仍走 native。

## 三、运行时阶段判定

### 3.1 运行时判定顺序

```
init/失败退出 → ① config store（连接/选主/rank 分配）
             → ② join/建链（group、QP 就绪）
             → ③ 内存注册/导出导入
             → ④ 数据传输（按 DOP_TYPE 分路径）
             → ⑤ 退出/异常恢复（peer down、重连）
```

### 3.2 ① config store 建链

关键代码：`src/smem/csrc/config_store/`、`src/smem/csrc/config_store/backend/`（etcd key 前缀
`/memfabric_hybrid/config_store/`）、`src/util/etcd_client/etcd_store_backend/`。

| 症状 | 候选原因 | 判别证据 |
|---|---|---|
| 初始化即报错 | `MF_CONFIG_STORE_URL` 未设置（必填）或格式错 | 日志是否有 store URL 解析错误；`tcp://`/`etcd://`/`reg://` |
| 连不上 store | 端口不在 `MF_CONFIG_STORE_PORT_START/END`（默认 9000-65535）范围、防火墙、IP 错 | server 侧日志是否有 `ServerStart`/侦听记录；`ss -lntp` 对应端口 |
| 客户端反复重连后失败 | 无 etcd 模式 server（rank0）已退出（SPOF）；重连上限 60 次×1s | server 进程是否存活；日志 `LinkBrokenHandler`、`ReConnectAfterBroken` |
| etcd 模式选主失败 | etcd 不可达；Leader 与 etcd 失联会**主动降级**（停 TCP server，4s 健康检查，Lease TTL 5s） | `Get(ETCD_KEY_LEADER)` 失败记录；`StopServer` 降级日志 |
| rank 分配失败 | `worldSize` 已满（`aliveRankSet_.size() < worldSize` 不满足） | server 日志 `worldSize exceeded` |
| 心跳掉线 | `MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S`（默认 60s，0 永不超时）过短或网络抖动 | 心跳超时日志与时间差 |

时序参考：`docs/zh/config_store_cluster_ha.md`（含正常/降级/分区三类 mermaid 时序）。

### 3.3 ② join / 建链

| 变量 | 默认 | 说明 |
|---|---|---|
| `MF_GROUP_JOIN_MAX_TIMEOUT` | 600 | Join 最大超时（秒） |
| `MF_GROUP_RETRY_TIME` | 5 | GroupUpdate 重试次数 |
| `MF_QP_READY_CHECK_TIMEOUT_BASE` | 100 | device RDMA QP 就绪检查超时基数（秒） |

join 超时先确认：所有 rank 是否都到达 join、config store 是否正常、双端日志同一 `comm/group` 下发时间差。

### 3.4 ③ 内存注册 / 导出导入

| 症状 | 候选原因 | 判别证据 |
|---|---|---|
| 未注册内存直接报错（URMA） | `MF_HYBM_URMA_SWAP_SPACE_SIZE=0`（默认）时跳过交换空间分配，未注册路径直接返回错误 | 日志是否提示 swap 未分配 |
| 未注册内存走 RDMA 失败（device_rdma） | 需 swap 中转：`MF_HYBM_RDMA_SWAP_SPACE_SIZE`（device_rdma 默认 0） | swap 分配日志、报错 ret |
| 4K 页被拦 | RDMA 场景默认拦截 4K 页，需 `MF_HYBM_ENABLE_4K_PAGE=1` | 报错是否含页大小 |
| HOST_SHM import 失败（容器） | `MF_HOST_SHM_USE_MEMFD=1` 时跨进程 import 走 `/proc/<pid>/fd/`，要求共享 PID namespace（`--pid=host`）且 uid 一致 | 容器启动参数、`ls -l /proc/<pid>/fd` |
| 大页/memfd 申请失败 | 宿主未配 hugetlb；可回退非 memfd 路径 | `mmap`/`memfd_create` 失败 ret |

### 3.5 ④ 数据传输路径（DOP_TYPE）

枚举定义：`src/hybm/include/hybm_def.h`（`HYBM_DOP_TYPE_*`）。

| 路径 | 适用场景 | 关键实现位置 | 关键变量 |
|---|---|---|---|
| DEVICE_RDMA（native） | 默认 device 跨节点路径 | `src/hybm/csrc/transport/protocol/device/rdma/`（`RdmaTransportManager`） | `MF_NPU_RDMA_SEND/RECV_CQ_DEPTH`（16384）、`MF_NPU_RDMA_MAX_SEND/RECV_WR`（32767/32767） |
| DEVICE_RDMA（HCOMM） | `MF_HYBM_RDMA_USE_HCOMM=1` 且 CANN ≥ 9.1 | `device_rdma_hcomm_transport_manager.cpp` | 见下方 A2 限制 |
| DEVICE_URMA | A5（Ascend 950），支持运行时动态 MR 更新 | `src/hybm/csrc/transport/protocol/device/urma/` | `MF_DEVICE_UB_QOS`（0-7）、`MF_HYBM_URMA_SWAP_SPACE_SIZE` |
| DEVICE_UBOE | UB 网络 | 同 urma 目录附近 | `MF_DEVICE_UB_QOS` |
| HOST_RDMA / HOST_TCP / HOST_URMA | 主机侧路径（需 `--build_hcom ON`） | `src/hybm/csrc/transport/protocol/host/`（`host_hcom_transport_manager.cpp`） | `HCOM_MAX_SLICE_SIZE`（NPU 1MB）、`HCOM_RECV_DATA_SIZE`（建议 slice+1024）、`MF_HCOM_CQ_DEPTH` 等 |
| HOST_SHM | 同节点共享内存 | `src/hybm/csrc/mm/hybm_host_shm_segment.cpp` | `MF_HOST_SHM_USE_MEMFD` |
| SDMA / MTE / AIV_SDMA | 卡内/主机拷贝 | `src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp` | — |
| compose | 组合路径 | `src/hybm/csrc/transport/protocol/compose/compose_transport_manager.cpp` | — |

**DEVICE_RDMA HCOMM vs native 判定表**（依据 `environment_variables.md`）：

| `MF_HYBM_RDMA_USE_HCOMM` | CANN 版本 | 实际实现 | 动态注册内存 |
|---|---|---|---|
| 0（默认） | 任意 | native RDMA | ✅ 实时查 MR 表 |
| 1 | ≥ 9.1（ACL ≥ 1.17） | HCOMM | ❌ A2 需中转 |
| 1 | < 9.1 | native RDMA | ✅ |

**A2 + HCOMM 已知限制**：`AicpuTsRoceChannel` 的 MR 表为创建时快照、不支持运行时 `UpdateMemInfo`；
join 后动态注册的内存（如 vLLM KV cache）不能直传，**必须同时**：

```bash
export MF_HYBM_RDMA_USE_HCOMM=1
export MF_HYBM_RDMA_FORCE_UNREGISTERED=1   # 强制走 swap 中转
export MF_HYBM_RDMA_SWAP_SPACE_SIZE=2048   # MB，按需
```

诊断时先从日志确认实际走的实现类（`RdmaTransportManager` vs `DeviceRdmaHcommTransportManager` vs
`DeviceUrmaTransportManager`），再套用对应限制。

### 3.6 ⑤ 退出与恢复

- 对端下线回调：`smem_trans_set_peer_down_callback`（`src/smem/csrc/smem_trans/`）。
- server 掉线清理：config store `LinkBrokenHandler` → 从 `aliveRankSet_` 移除并通知 Watch。
- etcd 模式恢复时序见 `config_store_cluster_ha.md` 第三部分（降级/重选/恢复等待 5s）。

## 四、调试与复现工具

| 工具 | 用途 | 入口 |
|---|---|---|
| ptracer | 关键调用次数/成败/耗时统计（P50/P99/P999/AVG/MAX） | `/var/log/memfabric_hybrid/ptracer_<pid>.dat`；编译宏 `ENABLE_PTRACER` |
| 故障注入 | 动态注入 `callback`/`pause`/`reset`/`abort`，免重启复现偶发失败 | `/tmp/mf_failpoints_<pid>.conf`；仅 debug/asan 构建（`MF_ENABLE_TRACEPOINT`）；内置点 `ALLOC_LOCAL_MEMORY`、`MMAP` |
| `MF_SOCKET_URL` | 调试时覆盖 config store 连接地址 | `environment_variables.md` |
| `MF_TRANSPORT_MANAGER` | 调试时指定传输层实现 | `environment_variables.md` |
| `mfcli benchmark` | 数据读写性能基线 | `mfcli.md` |
| `mfcli mem scan` | NUMA 空闲内存扫描（host_rdma 大容量中转前置检查） | `mfcli.md` |

## 五、常见"假根因"提醒

- 应用层 `transfer failed` / 超时 ≠ 网络问题；先定位实际 DOP_TYPE 与实现类，再查该路径自身限制。
- `MF_LOG_LEVEL` 不生效 ≠ 日志系统坏了；bm/shm 场景本就不读该变量。
- HCOMM 下 A2 直传失败 ≠ RDMA 网络故障；先查 MR 快照限制与 `MF_HYBM_RDMA_FORCE_UNREGISTERED`。
- join 超时 ≠ 对端故障；先确认双端是否都进入 join、config store 是否健康。
