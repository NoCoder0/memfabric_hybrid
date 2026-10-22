# 环境与配置最小必要检查清单

**使用原则**：仅在日志已直接支持相应假设后，才执行对应的检查项；每次只做能判别候选假设的最小检查，
不得由应用层超时直接推断网络、PFC、容器或环境问题。每项检查记录：命令、输出关键值、与假设的对应关系。

## 一、版本与基础环境（几乎所有诊断都需要）

| 检查项 | 命令 | 期望/判读                                          |
|---|---|------------------------------------------------|
| 芯片型号 | `npu-smi info` | A2=910B、A3=910C、A5=Ascend950PR/Ascend950DT；决定是否需要 AICPU 算子与验签关闭 |
| MemFabric 版本（C API） | `cat /usr/local/memfabric_hybrid/latest/version.info` | 与 issue/日志日期对齐源码 ref                           |
| MemFabric 版本（Python） | `mfcli version` | 同上                                             |
| CANN 版本 | 日志中通常可见；或 `ls ${ASCEND_HOME_PATH}/version*` | DEVICE_RDMA HCOMM 路径要求 ≥ 9.1（ACL ≥ 1.17）       |
| 安装方式 | `pip show memfabric_hybrid` 或 `ls /usr/local/memfabric_hybrid/` | wheel vs run 包，决定 offload 支持与日志目录              |
| 环境路径 | `echo $ASCEND_HOME_PATH`（NPU）/ `echo $CUDA_HOME`（GPU） | 必填，否则动态库加载失败                                   |

## 二、AICPU 算子与驱动（A2/A5 device 路径问题时）

| 检查项 | 命令 | 期望/判读 |
|---|---|---|
| 算子安装状态 | `mfcli kernel info` | 扩展库 `libmf_hybm_accoffload.so` 安装状态与路径 |
| 算子版本 | `mfcli kernel version` | 与 MemFabric 版本匹配 |
| 驱动验签 | `npu-smi info -t custom-op-secverify -i 0`（或查询等价命令） | A2/A5 自定义 AICPU kernel 需已关闭验签（见下） |
| 大页内存 | `grep Huge /proc/meminfo` | A2 DRAM 池化要求 `HugePages_Total` 与池化大小匹配；不足则初始化失败 |

关闭验签（A2/A5 使用自定义 AICPU kernel 前置）：

```bash
npu-smi set -t custom-op-secverify-enable -i 0 -d 1
npu-smi set -t custom-op-secverify-mode -i 0 -d 0
```

## 三、config store 连通性（init / join 失败时）

| 检查项 | 命令 | 期望/判读 |
|---|---|---|
| URL 已设置且格式正确 | `echo $MF_CONFIG_STORE_URL` | 必填；`tcp://ip:port` / `etcd://...` / `reg://...` |
| server 端口在允许范围 | `echo $MF_CONFIG_STORE_PORT_START $MF_CONFIG_STORE_PORT_END` | 默认 9000-65535；session 未指定端口时自动选端口也用此范围 |
| server 是否侦听 | `ss -lntp \| grep <port>`（在 server 所在 rank0 执行） | 无 etcd 模式 server 由 rank0 自动启动 |
| 端口可达 | `nc -zv <ip> <port>`（client 侧） | 排除防火墙/路由 |
| etcd 可达（etcd 模式） | `etcdctl endpoint health` 或日志 `Get(ETCD_KEY_LEADER)` | Leader 4s 健康检查失败会主动降级停服 |
| 心跳超时配置 | `echo $MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S` | 默认 60s；0 表示永不超时；频繁掉线可适当调大 |
| socket 调试覆盖 | `echo $MF_SOCKET_URL` | 该变量会覆盖 config store 连接地址，调试后忘记清理会导致连错地址 |

etcd 关键 key（`/memfabric_hybrid/config_store/` 前缀）：`leader/elected`、`leader/elected_status`、
`data/world_size`、`data/alive_rank_list`。可用 `etcdctl get --prefix /memfabric_hybrid/config_store/` 直接核对集群状态。

## 四、传输路径配置（建链/传输失败时）

先从日志确认实际传输实现类，再核对该路径变量：

| 路径 | 检查项 | 命令/判读 |
|---|---|---|
| DEVICE_RDMA（HCOMM） | 开关与 CANN 匹配 | `echo $MF_HYBM_RDMA_USE_HCOMM`；=1 时要求 CANN ≥ 9.1，否则实际仍走 native |
| DEVICE_RDMA（HCOMM/A2） | MR 快照限制三件套 | `MF_HYBM_RDMA_USE_HCOMM=1` 时动态注册内存直传必失败，需同时 `MF_HYBM_RDMA_FORCE_UNREGISTERED=1` + `MF_HYBM_RDMA_SWAP_SPACE_SIZE=<MB>` |
| DEVICE_RDMA | swap 空间 | `echo $MF_HYBM_RDMA_SWAP_SPACE_SIZE`；device_rdma 默认 0（不分配），未注册路径会直接报错 |
| DEVICE_URMA/UBOE | swap 与 QoS | `MF_HYBM_URMA_SWAP_SPACE_SIZE`（默认 0，未注册路径直接报错）；`MF_DEVICE_UB_QOS`（0-7，非法回退 0） |
| 通用 RDMA | 4K 页拦截 | `echo $MF_HYBM_ENABLE_4K_PAGE`；默认拦截 4K 页，非 0 开放支持 |
| 通用 RDMA | 强制未注册路径 | `echo $MF_HYBM_RDMA_FORCE_UNREGISTERED`；非 0 时 `BatchDataCopy` 直接走未注册路径 |
| HOST_* | HCOM 是否编译 | wheel 已含；源码运行需 `--build_hcom ON`；RDMA 需 `libibverbs-dev` |
| HOST_* | 切片与收包缓冲 | `HCOM_MAX_SLICE_SIZE`（NPU 默认 1MB）、`HCOM_RECV_DATA_SIZE`（建议 slice+1024，默认 NPU 1MB+1KB） |
| HOST_SHM | memfd 与容器 | `MF_HOST_SHM_USE_MEMFD=1` 时跨进程 import 要求 `--pid=host` 且 uid 一致；`ls -l /proc/<pid>/fd` 看匿名 fd |
| join 慢/超时 | 超时与重试 | `MF_GROUP_JOIN_MAX_TIMEOUT`（600s）、`MF_GROUP_RETRY_TIME`（5）、`MF_QP_READY_CHECK_TIMEOUT_BASE`（100s） |
| QP/CQ 参数 | device RDMA 队列深度 | `MF_NPU_RDMA_SEND/RECV_CQ_DEPTH`（64-32768）、`MF_NPU_RDMA_MAX_SEND/RECV_WR`（64-32767） |

## 五、RDMA 底层（仅当日志已指向 RDMA 建链/收发失败）

```bash
ibv_devinfo                  # RDMA 设备状态、端口 UP、链路层（RoCE/IB）
ibv_devinfo -d <mlxdev> -p <port>   # GID/PKEY 细节
ss -lntp                     # 端口占用（对照 MF_CONFIG_STORE_PORT_*）
dmesg -T | grep -iE "rdma|roce|mlx|hns"   # 驱动层报错
```

## 六、容器 / 权限（部署类报错时）

| 检查项 | 命令 | 期望/判读 |
|---|---|---|
| PID namespace（memfd 场景） | 容器启动参数含 `--pid=host`；`readlink /proc/1/ns/pid` 与宿主对比 | HOST_SHM memfd import 需要 |
| 设备可见性 | `echo $ASCEND_RT_VISIBLE_DEVICES` | 物理/逻辑 device ID 映射是否与预期一致 |
| 大页挂载（非 memfd 场景） | `mount \| grep hugepages` | `/dev/hugepages` 可用性 |
| 构建与 ABI | `git log -1`、构建命令行是否带 `--build_open_abi` | 新 libstdc++ 发行版需 ON，否则 `undefined symbol` |

## 七、日志与观测面

| 检查项 | 命令 | 说明 |
|---|---|---|
| MemFabric 运行日志 | `ls -lt /var/log/memfabric_hybrid/` | 运行日志与 ptracer `ptracer_<pid>.dat` |
| MemCache 日志 | `ls -lt /var/log/memcache_hybrid/` | meta service |
| Ascend plog | `ls -lt ${ASCEND_PROCESS_LOG_PATH:-/root/ascend/log}` | 优先 `ASCEND_PROCESS_LOG_PATH` |
| 内核日志 | `dmesg -T` / `journalctl -k --since "6 hours ago"` | 驱动、大页、OOM |
| Python 日志级别 | `echo $MF_LOG_LEVEL`（0-4） | **仅 Python 接口生效**，bm/shm 不生效 |
| 一键采集 | `mfcli logs --hours=6 --max-size=2048` | 在业务容器内、程序结束后立即执行 |

## 八、故障注入验证（偶发问题复现时）

前提：目标程序为 debug/asan 构建（`MF_ENABLE_TRACEPOINT` 已定义）。

```bash
pgrep -af <进程名>                          # 找 PID
cat > /tmp/mf_failpoints_<pid>.conf <<'EOF'
ALLOC_LOCAL_MEMORY callback 1
MMAP pause 3 200
EOF
# 验证：看日志 Triggered ... / 复现目标操作
rm -f /tmp/mf_failpoints_<pid>.conf         # 取消
```

内置故障点：`ALLOC_LOCAL_MEMORY`（`src/hybm/csrc/entity/hybm_entity_default.cpp`）、
`MMAP`（`src/smem/csrc/smem_bm/smem_bm_entry.cpp`）。详细语义见 `docs/zh/fault_injection.md`。
