# A2 单机三卡复现 HCOMM 通道混入其他 peer 的 MR

在同一台 A2 服务器上选三张有独立 RoCE IP 的空闲卡，每卡一个进程。rank 1、2 各提供一个 32 MiB 的 MemFabric DRAM 池，rank 0 通过 `bm.copy_data` 分别从两者读取 4 KiB、再分别写入 4 KiB。两个 peer 在自己的 DRAM 中填入不同字节，读结果在 rank 0 校验，写结果由每个 peer 直接检查本地 DRAM。Gloo 用于同步进程和交换校验状态。

A2 的 DEVICE_RDMA 路径会把池的 Host VA 转成设备可见的 DVA，再交给 HCOMM 注册。每个 rank 打印自己的 GVA、HVA 和注册 DVA，随后按 rank 1、2、0 的顺序入组。rank 0 对两个 peer 执行读写。

## 编译并安装本分支的 MemFabric

在 A2 Linux 构建机上，从 `feat/kunpeng-dram-v3` 的 MemFabric 仓库根目录执行。使用与待测 HCOMM 配套的 CANN Toolkit（本路径要求 CANN ≥ 9.1）及 AICPU 编译工具。同一套 Python 环境需要有支持 Gloo 的 PyTorch 和 Python 构建依赖。下例的 CANN 安装路径按机器实际情况替换。

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
: "${ASCEND_HOME_PATH:?请先加载待测 CANN 的 set_env.sh}"
git branch --show-current        # 应为 feat/kunpeng-dram-v3
git rev-parse HEAD              # 记录本次构建提交
python3 -c 'import torch, torch.distributed as d; print(torch.__version__); assert d.is_gloo_available()'
python3 -m pip install 'pybind11>=2.10.3' setuptools wheel
set -o pipefail
bash script/build_and_pack_run.sh --xpu_type NPU --build_python ON --build_hcom OFF 2>&1 | tee build-a2.log
grep -E 'stage HYBM OPS payload|without HYBM OPS payload|skip build HYBM OPS' build-a2.log
ls -lh output/memfabric_hybrid/wheel/*.whl
sha256sum output/memfabric_hybrid/wheel/*.whl
```

本例的 `DEVICE_RDMA` 在运行时加载 `libhcomm.so`。`--build_hcom` 控制的是另一套 Host 通信库，因此这里使用 `OFF`。构建日志应出现 `stage HYBM OPS payload for wheel`。若出现 `without HYBM OPS payload`，先按日志补齐 AICPU 编译器或 CANN 头文件，再重新构建。详见[仓库安装指南](../../../../docs/zh/installation.md)和[算子构建说明](../../../../docs/zh/installation_kernel.md)。

在这台服务器上，用运行测试的 Python 环境安装刚构建的 wheel 和 AICPU 算子。以下命令从仓库根目录执行，三个 worker 共用这次安装。

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
WHEEL_DIR="$PWD/output/memfabric_hybrid/wheel"
python3 -m pip install --no-index --no-deps --force-reinstall --find-links "$WHEEL_DIR" memfabric-hybrid
python3 -c 'import memfabric_hybrid as mf; print(mf.__file__)'
mfcli kernel install
mfcli kernel info
```

用 `mfcli kernel info` 核对目标 CANN 环境里的 HYBM 算子已安装。分别测试 HCOMM 9.2.0 和 master 时，每轮加载对应版本的 HCOMM 库及配套设备侧组件，单独保存该轮日志。

## 先检查卡间 RoCE

以下以物理卡 0、1、2 为例，按实际卡号替换。查询本机三张卡的 RoCE IP，以卡 0 和卡 1 为例验证双向可达性。

```bash
H=/usr/local/Ascend/driver/tools/hccn_tool
for dev in 0 1 2; do "$H" -i "$dev" -ip -g; done
IP0=192.0.2.10    # 换成卡 0 的实际 RoCE IP
IP1=192.0.2.11    # 换成卡 1 的实际 RoCE IP
"$H" -i 0 -ping -g address "$IP1"
"$H" -i 1 -ping -g address "$IP0"
```

在同一台服务器的两个终端打流，先启动服务端：

```bash
# 终端 1：卡 1 等待连接
/usr/local/Ascend/driver/tools/hccn_tool -i 1 -roce_test ib_write_bw -s 131072 -n 1000 -tcp
```

```bash
# 终端 2：卡 0 发起写，IP1 换成卡 1 的实际 RoCE IP
IP1=192.0.2.11
/usr/local/Ascend/driver/tools/hccn_tool -i 0 -roce_test ib_write_bw -s 131072 -n 1000 -tcp address "$IP1"
```

两端换为 `ib_read_bw` 再测读。按同样方式验证卡对 0↔2、1↔2，三对卡读写均正常结束且平均带宽非零后，运行下面的示例。

## 单机启动

在同一台服务器上打开三个终端。在每个终端使用同一 Python 和 CANN 环境，手动启动一个进程。`--rank` 指定 MemFabric 和 Gloo 共用的 rank；`--device` 指定该进程可见的 CANN 设备索引。三个终端使用相同的 `ASCEND_RT_VISIBLE_DEVICES`。例如选 `2,4,6` 时，索引 0、1、2 分别指向 CANN 逻辑设备 2、4、6。容器中逻辑 ID 与宿主机物理卡号可能不同。[CANN 设备 ID 映射说明](https://www.hiascend.com/doc_center/source/en/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_2057.html)

| 变量 | 当前示例的实际读取位置和作用 |
|---|---|
| `BM_STORE` | 示例必填，原样传给 `bm.initialize`。三个 worker 共用一个地址；rank 0 启动 MemFabric 配置服务并作为客户端，rank 1、2 连接它。启动前确保本机 8570 可供 rank 0 监听。 |
| `BM_NIC` | 示例必填，Python 展开 `{rank}` 后调用 `cfg.set_nic()`。字符串写入 `config.hcomUrl`，随端点信息交换；HCOMM 路由要求 peer 的 NIC 字段非空。在本例的 A2 DEVICE_RDMA/HCOMM 路径中，10000、10001、10002 **不会因这个设置而被监听**。实际 RoCE IP 由 MemFabric 按物理卡获取。日志里的 `nic: tcp://127.0.0.1:...` 不能用来判断数据走了哪个网口。 |
| `MF_HYBM_RDMA_USE_HCOMM` | 脚本校验字符串 `1`。MemFabric 的 DEVICE_RDMA 路径还要满足 CANN 版本门槛（本分支对应 CANN ≥ 9.1）才选 HCOMM。用日志里的 `useHcomm=true` 确认。 |
| `MF_HYBM_RDMA_SWAP_SPACE_SIZE` | 脚本校验字符串 `64`。Device RDMA 实现把该数值乘以 1024²，为每个实例分配 64 MiB 中转空间。 |

代码对照：[示例入口](peer_mr.py)、[配置服务](../../../../src/smem/csrc/smem_bm/smem_bm_entry_manager.cpp)、[NIC 字段和传输选择](../../../../src/hybm/csrc/transport/protocol/compose/compose_transport_manager.cpp)、[A2 RoCE 端点](../../../../src/hybm/csrc/transport/protocol/device/rdma/device_rdma_hcomm_transport_manager.cpp)、[中转空间](../../../../src/hybm/csrc/data_operation/host/hybm_data_op_device_rdma.cpp)。

**三个终端都执行一次以下设置**（从仓库根目录开始）。本轮共用同一个日志目录名，下次测试换一个新名字。

```bash
cd examples/memory_pool/02_scale_out/04_a2_peer_mr   # 从 MemFabric 仓库根目录进入
export ASCEND_RT_VISIBLE_DEVICES=0,1,2
export BM_STORE='tcp://127.0.0.1:8570'
export BM_NIC='tcp://127.0.0.1:1000{rank}'
export MF_HYBM_RDMA_USE_HCOMM=1 MF_HYBM_RDMA_SWAP_SPACE_SIZE=64
LOG_DIR="$PWD/logs/run-01"
mkdir -p "$LOG_DIR"
export ASCEND_GLOBAL_LOG_LEVEL=1 ASCEND_SLOG_PRINT_TO_STDOUT=1  # 文件模式见下节，替换这一行
set -o pipefail
```

**每个终端只执行对应的一行**。先启动 rank 0，再在另外两个终端启动 rank 1 和 2。三个进程应在 3 分钟内到齐。

```bash
# 终端 1
python3 -u peer_mr.py --rank 0 --device 0 2>&1 | tee "$LOG_DIR/rank0.log"
# 终端 2
python3 -u peer_mr.py --rank 1 --device 1 2>&1 | tee "$LOG_DIR/rank1.log"
# 终端 3
python3 -u peer_mr.py --rank 2 --device 2 2>&1 | tee "$LOG_DIR/rank2.log"
```

rank 0 默认用 `127.0.0.1:29501` 等待 Gloo 的另外两个进程，这与 BM 配置服务的 8570 端口独立。如果 29501 被占用，在三条命令中都加上相同的 `--sync-port 29502`。日志里的 `waiting for three processes` 表示正在会合；看到 `Gloo ready` 才进入 MemFabric 初始化。启动顺序和程序内的 BM 入组顺序（1、2、0）是两件事。

## 查看 MF 和 HCOMM 日志

`tee` 将每个进程的标准输出和错误写到对应的 `rank0.log`、`rank1.log` 或 `rank2.log`。`LOG_DIR` 只供 shell 使用。MF 的 BM 日志由示例中的 `mf.set_log_level(0)` 打开 DEBUG；`ASCEND_GLOBAL_LOG_LEVEL=1`（INFO）和 `ASCEND_SLOG_PRINT_TO_STDOUT=1` 由 CANN 日志系统读取，用于把 HCOMM Host 日志打印到终端。

```bash
# 在启动测试的同一个 shell 执行；新 shell 请先将 LOG_DIR 指向当轮目录。
tail -n 15 "$LOG_DIR"/rank*.log
grep -nE 'useHcomm=|registered DVA=|ImportPeerMems stored peer rank=|CreatePeerResources|READ rank|WRITE rank|DATA PASS' "$LOG_DIR"/rank*.log
# HCOMM：显示通道序列化边界，以及边界之间的地址、长度、key
grep -nE 'Serialize (start|done)|GetAllMemDetails.*\[remote\]|GetPeerMemDetails' "$LOG_DIR/rank0.log"
# 失败时查首个错误，再回看同一 worker、同一线程的上下文
grep -niE 'error|failed|timeout|mismatch|Traceback' "$LOG_DIR"/rank*.log | head -60
```

先在 rank 0 的 MF 日志确认 `useHcomm=true` 和 DATA MR 的 `peerDva`、`peerSize`。再按同一进程、线程及通道创建上下文，检查一次 `Serialize start` 到 `Serialize done` 之间的 remote 列表。不要把不同通道各自合法的 MR 合并计数。若需确认本轮实际加载的 HCOMM 库，可在进程运行期间查询：

```bash
pgrep -af '[p]ython.*peer_mr.py'
PID=12345   # 换成上面列出的 Python worker PID
grep -F 'libhcomm.so' "/proc/$PID/maps" | awk '{print $NF}' | sort -u
```

如需单独的 CANN plog，在三个终端的公共设置中，**把设置 `ASCEND_SLOG_PRINT_TO_STDOUT=1` 的整行替换成下面三行**，然后分别启动进程。文件模式下，HCOMM Host 日志写入指定目录，MF 和用例自身的输出仍在各 rank 的日志中。[CANN 日志环境变量说明](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/maintenref/envvar/envref_07_0001.html)

```bash
export ASCEND_SLOG_PRINT_TO_STDOUT=0 ASCEND_GLOBAL_LOG_LEVEL=1
export ASCEND_PROCESS_LOG_PATH="$LOG_DIR/cann"
mkdir -p "$ASCEND_PROCESS_LOG_PATH"
```

本轮启动后在日志目录中查询：

```bash
find "$ASCEND_PROCESS_LOG_PATH" -type f -name '*log*' | head -40
grep -rInE --include='*.log' 'Serialize (start|done)|GetAllMemDetails.*\[remote\]|GetPeerMemDetails' "$ASCEND_PROCESS_LOG_PATH"
```

## 判读

- 在 rank 0 日志确认 `useHcomm=true`，并用 `ImportPeerMems stored peer rank=` 查看两个 peer 的 `peerDva` 和 `peerSize`。这两个字段给出各自 DATA MR 的起始地址和长度，可用于判断地址区间是否重叠。中转区是另一个 MR，每实例 64 MiB。
- 检查 rank 0 的 HCOMM 日志（若未打印到终端，查看该进程的 CANN plog）。将单个通道 `Serialize` 的 remote MR 列表与目标 peer 对照。如果同一通道包含来自两个 peer 的 DATA MR，就复现了串表。若随后 RDMA 失败，对照该通道选用的 key 和目标 peer 实际导出的 key。
- `DATA PASS` 表示真实读写逐字节通过。若两张卡恰好产生了相同数值的 rkey，数据通过本身无法证明通道只含自己的 MR。修复版应同时满足数据通过和每个通道只含目标 peer 的 DATA MR。
