# sparse_copy_urma examples

这些示例共用生产 `HybmBatchCopy`、固定 route 和
`mf_acc_offload.sparse_copy_urma`，不调用 `offload.initialize()`。建链使用
`bm.BmDataOpType.HOST_DEVICE_URMA`；route 首次发布后不增加内存区间、不替换 peer，调用方保证 entity
在同步拷贝完成前保持存活。

## 01：单机两卡 Device-HBM

需要两张 Ascend 950、可用 CANN/HCOMM/URMA 和已安装的 HYBM AICPU kernel。两个子进程分别绑定 device 0/1：

```bash
python3 examples/kv_offload/sparse_copy_urma/01_single_node_multi_device_urma.py
```

示例在每张卡的 MemFabric HBM 中初始化源数据，然后在对端通过真实框架 HBM tensor 的 `data_ptr()` 接收。
被测数据路径只有 `sparse_copy_urma`；`copy_data(H2G)` 仅用于初始化源 HBM。覆盖 1、999、1000、1001 条、
非零偏移和 range 尾部恰好结束，并打印远端 GVA 与本地 import view。

## 02：Host-DDR → NPU HBM

该示例需要分别在鲲鹏 Host 和 Ascend NPU 进程执行。Host 的 `--eid` 是 32 个十六进制字符，并用于设置
生产 Host manager 要求的 `MF_HOST_URMA_EID`。先启动 Host，再启动 NPU；`--head-ip` 必须是两端可达的
配置存储/控制网络地址：

Host：

```bash
python3 examples/kv_offload/sparse_copy_urma/02_host_device_urma.py \
  --rank 0 --head-ip <host-or-store-ip> --eid <32-hex-host-eid>
```

NPU：

```bash
python3 examples/kv_offload/sparse_copy_urma/02_host_device_urma.py \
  --rank 1 --head-ip <host-or-store-ip>
```

Host 使用固定 GVA DDR 区间并通过一次初始化 `copy_data(H2G)` 写入 pattern；NPU 侧检查 Python 可见的
`exported GVA == import view`，随后只用 `sparse_copy_urma` 读取到真实 HBM tensor。生产 Device manager
同时强制校验 `key.keys[1] == exportDesc.addr == view.addr`。该示例覆盖单条、999/1000/1001 条、非零偏移、
range 尾部和同步 completion。

如果修改 store/control 端口，Host 和 NPU 必须同时传入相同的 `--store-port`、`--ctrl-port`；不要在 route
发布完成后调用 `extend_local_mem`、替换 peer 或重新建链。

`--log-level` 同时控制 MemFabric 和 Python 脚本日志，取值与 `mf.set_log_level()` 一致：`0=DEBUG`、`1=INFO`、
`2=WARN`、`3=ERROR`、`4=OFF`。未指定时，local validation 默认 DEBUG，生产路径默认 INFO；两端建议传入相同值。

## 02：单机 DRAM → HBM 临时验证

该模式只用于同一台机器上的 1 个 Host/DRAM 进程和 1 个 NPU/HBM 进程。先构建打开验证宏的 NPU 版本；默认
构建和不带 `--local-dram-validation` 的跨节点路径不受影响：

```bash
bash script/build_and_pack_run.sh --build_local_dram_validation ON
```

EID 查询工具是 MemFabric 示例目录下的独立临时源文件，不接入 MemFabric 的 CMake、wheel 或 run 包；不依赖
`memfabric_hybrid` Python/C++ 库。以物理卡号查询并保存环境输出：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
  examples/kv_offload/sparse_copy_urma/urma_eid_query.cpp -ldl -o /tmp/mf_urma_eid_query

EID_TOOL=/tmp/mf_urma_eid_query
"${EID_TOOL}" --device-id 5 --format env --no-candidates > /tmp/mf-local-dram-eid.env
python3 examples/kv_offload/sparse_copy_urma/update_env_from_eid.py
```

工具的 `--device-id` 是物理卡号；工具输出的 `MF_LOCAL_DRAM_PHYSICAL_DEVICE_ID`、
`MF_LOCAL_DRAM_LOGICAL_DEVICE_ID` 和 EID 环境变量由更新脚本写入本目录的 `env` 文件。该脚本保留两端公共的
`ASCEND_RT_VISIBLE_DEVICES`、`MEMFABRIC_HYBRID_EXTEND_LIB_PATH` 和 `MF_LOG_LEVEL`；Python 脚本启动时读取
该文件并通过 `os.environ` 设置进程环境，不执行 shell。`MEMFABRIC_HYBRID_EXTEND_LIB_PATH` 应指向安装包的
`lib64` 目录；上例默认按 aarch64 Linux 安装路径填写，其他架构需修改 `env`。脚本根据物理卡号自动计算
ACL/Torch 可见索引；例如 `ASCEND_RT_VISIBLE_DEVICES=5,6` 且目标物理卡为 5 时，runtime index 自动为 `0`。
脚本根据显式的 `--role host|device` 设置 Host/Device 临时角色，覆盖 Host 的
`MF_LOCAL_DRAM_VALIDATION_ROLE=host`，并清理 Device 进程中的该变量；用户无需手动设置或清理环境变量。
`--rank` 仅表示当前协议 rank，不用于推断角色。启动顺序为 Host 后 NPU，两端使用同一个 `env` 文件和相同的
store/control 地址：

终端 1（进程 A，Host/DRAM，rank 0）：

```bash
python3 examples/kv_offload/sparse_copy_urma/02_host_device_urma.py \
  --local-dram-validation --role host --rank 0 --head-ip 127.0.0.1
```

终端 2（进程 B，NPU/HBM，rank 1）：

```bash
python3 examples/kv_offload/sparse_copy_urma/02_host_device_urma.py \
  --local-dram-validation --role device --rank 1 --head-ip 127.0.0.1
```

默认读取 `examples/kv_offload/sparse_copy_urma/env`，也可通过 `--env-file <path>` 指定公共环境文件。
`--physical-device-id`、`--device-id`、`--runtime-device-id`、`--host-eid` 和 `--device-eid` 仍可显式传入，
用于兼容旧命令或做一致性校验；local 模式下不再要求重复传递。`--rank`、`--role`、`--head-ip` 和其他两端
不一致的运行参数继续通过命令行传入。

Host 先初始化固定 8 MiB GVA DRAM 和连续 pattern；两端 `create2` 均使用
`max_dram_size=8 MiB`、`max_hbm_size=1 GiB` 保持相同的全局 GVA 布局。后者满足 Ascend 950 HBM VMM 的
GB 对齐要求，但实际 HBM 分配仍是 `local_hbm_size=8 MiB`；只有 `local_*_size` 按角色决定实际分配和导出。
该 pool 会在传输前注册，因此 Host validation 进程自动将未注册内存中转使用的
`MF_HYBM_RDMA_SWAP_SPACE_SIZE` 设为 `0`。NPU 通过现有 entity/key/endpoint/route 流程取得 Host DRAM，使用
本地 MemFabric HBM pool 的实际 device VA 调用 `sparse_copy_urma`，再用 `copy_data(G2H)` 回读验证。loopback
TCP 只传版本化 JSON 元数据和结果，不传 pattern、HCOMM descriptor 或 key。默认覆盖 1、4096、1 MiB 字节数以及
1/999/1000/1001 个 4 KiB item；可用 `--rounds`、`--sizes`、`--batch-counts` 调整。

EID、物理/逻辑卡映射、尺寸、范围和地址加法在可检查处先失败，HCOMM 返回值和清理错误保留
stage/rank/device/地址/长度上下文。不要为绕过
生产 key/type/address 门禁而设置额外变量。验证完成后删除该临时 Python 分支、构建宏/脚本参数和配套工具，
再以默认 `--build_local_dram_validation OFF` 重新构建。

## 03：单卡/多卡 Host DRAM → NPU HBM profiling

`03_host_device_urma_performance.py` 用本机 Host DRAM 进程模拟远端 DRAM，采集单张或多张 NPU 卡并发调用
`sparse_copy_urma` 的 profiling 数据。该用例沿用 02 的 local validation 建链方式，因此同样需要打开验证宏：

```bash
bash script/build_and_pack_run.sh --build_local_dram_validation ON
```

每张卡使用一个 Host/DRAM 进程和一个 Device/HBM 进程。脚本默认测试物理卡 `0,1,2,3`，共启动 8 个进程；
`--cards` 支持最少一张、最多八张卡。每一对进程组成独立的 `world_size=2` 通信域，Host 为 rank 0，Device
为 rank 1；不同 pair 使用独立的 store、control 和 NIC 端口，避免 rank、route 和控制消息互相干扰。

### 准备环境文件

创建一个独立目录，并为每张选中的物理卡准备 `card<物理卡号>.env`。每份文件都需要包含 02 local validation 所需的
Host/Device EID、物理/逻辑卡映射、`MEMFABRIC_HYBRID_EXTEND_LIB_PATH` 和 `ASCEND_RT_VISIBLE_DEVICES`。可以复制
同目录的 `env` 作为模板，再逐卡查询 EID 并更新目标文件；修改 `CARDS` 可准备单卡或最多八张卡：

```bash
EID_TOOL=/tmp/mf_urma_eid_query
ENV_DIR=/tmp/mf-urma-card-env
CARDS="0 1 2 3"
mkdir -p "${ENV_DIR}"

for card in ${CARDS}; do
  cp examples/kv_offload/sparse_copy_urma/env "${ENV_DIR}/card${card}.env"
  "${EID_TOOL}" --device-id "${card}" --format env --no-candidates > "/tmp/card${card}-eid.env"
  python3 examples/kv_offload/sparse_copy_urma/update_env_from_eid.py \
    --source "/tmp/card${card}-eid.env" \
    --target "${ENV_DIR}/card${card}.env"
done
```

更新后检查每个文件的 `ASCEND_RT_VISIBLE_DEVICES`：它必须包含该文件名对应的物理卡。可以让所有文件都配置
所选卡集合，也可以每份只暴露对应的一张卡；脚本会根据物理卡号计算进程内的 ACL/Torch runtime device id。
不要让不同 `card*.env` 复用另一张卡的 EID 或物理/逻辑卡映射。

### 运行

使用默认物理卡和端口时执行：

```bash
PROFILE_DIR=/tmp/mf-urma-card-profiling
python3 examples/kv_offload/sparse_copy_urma/03_host_device_urma_performance.py \
  --env-dir "${ENV_DIR}" \
  --profiling-dir "${PROFILE_DIR}" \
  --head-ip 127.0.0.1
```

单卡调试可传 `--cards 0`；多卡最多传入八个物理卡号，并提供相同编号的环境文件。例如：

```bash
python3 examples/kv_offload/sparse_copy_urma/03_host_device_urma_performance.py \
  --cards 0,1,2,3,4,5,6,7 \
  --env-dir "${ENV_DIR}" \
  --profiling-dir "${PROFILE_DIR}" \
  --head-ip 127.0.0.1
```

### 1/2/4 lane 对比

`--batch-copy-lanes` 只接受 `1`、`2` 或 `4`，默认 `1`。脚本会把该值同时传给 Host 和 Device 进程的
`ASCEND_MF_BATCH_COPY_LANES`；每条额外 lane 使用独立的 HCOMM channel，Device 侧还使用独立的 AICPU thread。
每次运行写入 `<profiling-dir>/lanes<lane 数>/card<物理卡号>`，可直接连续运行三组采样：

```bash
for lanes in 1 2 4; do
  python3 examples/kv_offload/sparse_copy_urma/03_host_device_urma_performance.py \
    --env-dir "${ENV_DIR}" \
    --profiling-dir "${PROFILE_DIR}" \
    --head-ip 127.0.0.1 \
    --batch-copy-lanes "${lanes}"
done
```

每个 pair 的端口为对应 base 加 pair 编号；八卡时编号为 `0..7`。端口被占用时可通过对应的
`--*-port-base` 整体平移：

| 通信用途 | pair `i` 端口 | CLI |
| --- | --- | --- |
| store | `8574 + i` | `--store-port-base` |
| control | `9877 + i` | `--ctrl-port-base` |
| NIC | `10005 + i` | `--nic-port-base` |

### Profiling 范围与输出

默认数据规格为 8192 tokens；每个 token 包含 512 个 FP8 E4M3 K 元素和 64 个 FP8 E4M3 V 元素。可通过
`--token-count`、`--k-dim` 和 `--v-dim` 调整，默认一次调用提交 16384 个 block、总传输量为 4.5 MiB。
`--v-dim 0` 时不创建 V tensor，也不提交 V descriptor，只拷贝 Key。Host 固定 DRAM pool 为 1 GiB，源数据为
带 token/component 标识的有限 FP8 原始字节 pattern；Device 在 profiling 结束后同步并将 K/V 拷回 CPU，逐字节
与预期 pattern 比较，任何不匹配都会带 pair、组件、字节偏移和期望/实际值报错。

每张卡总共执行 20 次 copy。前 6 次用于吸收共享库和 AICPU kernel 首次加载；所有选中卡完成这 6 次调用后通过
barrier 对齐，后 14 次调用前分别执行 `prof.step()`，用于推进与参考文件相同的 profiler schedule。当前
`sparse_copy_urma` 会在返回前同步 NPU stream，因此 profiler 能同时观察 Host API、kernel 启动、NPU timeline
和同步点。

脚本按照参考文件创建 `torch_npu.profiler.profile`：采集 CPU 和 NPU activity，使用 Level2、PipeUtilization，
并设置 `wait=1`、`warmup=1`、`active=10`、`repeat=1`、`skip_first=1`；同时打开 shape 和 memory 记录，关闭
stack、FLOPs 和 module 记录。profiler 在 20 次 copy 前启动，前 6 次 copy 后开始调用 `prof.step()`，最后一次
copy 完成并同步 NPU 后停止。每张选中卡分别写入
`<profiling-dir>/lanes<lane 数>/card<物理卡号>`，避免 trace 文件互相覆盖；
未指定 `--profiling-dir` 时默认写入本示例目录下的 `profiling`。

脚本不使用 Python wall-clock 计时，也不输出自行换算的平均时延或带宽。运行完成后只打印每张卡的
`profiling_path`、`profile_step_calls` 和整体完成状态。AICPU kernel、Host API、NPU timeline 及平均时延均以
各卡 profiling 目录中的采集结果为准。
