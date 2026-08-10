# sparse_copy_urma examples

这两个示例共用生产 `HybmBatchCopy`、固定 route 和
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
  --rank 1 --head-ip <host-or-store-ip> --device-id 0
```

Host 使用固定 GVA DDR 区间并通过一次初始化 `copy_data(H2G)` 写入 pattern；NPU 侧检查 Python 可见的
`exported GVA == import view`，随后只用 `sparse_copy_urma` 读取到真实 HBM tensor。生产 Device manager
同时强制校验 `key.keys[1] == exportDesc.addr == view.addr`。该示例覆盖单条、999/1000/1001 条、非零偏移、
range 尾部和同步 completion。

如果修改 store/control 端口，Host 和 NPU 必须同时传入相同的 `--store-port`、`--ctrl-port`；不要在 route
发布完成后调用 `extend_local_mem`、替换 peer 或重新建链。

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
  examples/kv_offload/urma_eid_query.cpp -ldl -o /tmp/mf_urma_eid_query

EID_TOOL=/tmp/mf_urma_eid_query
"${EID_TOOL}" --device-id 0 --format env --no-candidates > /tmp/mf-local-dram-eid.env
. /tmp/mf-local-dram-eid.env
```

工具的 `--device-id` 是物理卡号；Python 的 `--device-id` 是工具返回的逻辑卡号。启动顺序为 Host 后 NPU，
两端使用相同的 store/control 地址和 EID 元数据：

终端 1（进程 A，Host/DRAM，rank 0）：

```bash
export MF_LOCAL_DRAM_VALIDATION_ROLE=host
python3 examples/kv_offload/sparse_copy_urma/02_host_device_urma.py \
  --local-dram-validation --rank 0 --head-ip 127.0.0.1 \
  --physical-device-id "${MF_LOCAL_DRAM_PHYSICAL_DEVICE_ID}" \
  --device-id "${MF_LOCAL_DRAM_LOGICAL_DEVICE_ID}" \
  --host-eid "${MF_HOST_URMA_EID}" --device-eid "${USE_LOCAL_EID}"
```

终端 2（进程 B，NPU/HBM，rank 1）：

```bash
unset MF_LOCAL_DRAM_VALIDATION_ROLE
python3 examples/kv_offload/sparse_copy_urma/02_host_device_urma.py \
  --local-dram-validation --rank 1 --head-ip 127.0.0.1 \
  --physical-device-id "${MF_LOCAL_DRAM_PHYSICAL_DEVICE_ID}" \
  --device-id "${MF_LOCAL_DRAM_LOGICAL_DEVICE_ID}" \
  --host-eid "${MF_HOST_URMA_EID}" --device-eid "${USE_LOCAL_EID}"
```

Host 先初始化固定 8 MiB GVA DRAM 和连续 pattern；NPU 通过现有 entity/key/endpoint/route 流程取得 Host
DRAM，使用本地 MemFabric HBM pool 的实际 device VA 调用 `sparse_copy_urma`，再用 `copy_data(G2H)` 回读
验证。loopback TCP 只传版本化 JSON 元数据和结果，不传 pattern、HCOMM descriptor 或 key。默认覆盖 1、4096、
1 MiB 字节数以及 1/999/1000/1001 个 4 KiB item；可用 `--rounds`、`--sizes`、`--batch-counts` 调整。

负向检查使用 `--negative=bad-gva|cross-range|overflow-len|wrong-device`；EID、物理/逻辑卡映射、尺寸、范围
和地址加法在可检查处先失败，HCOMM 返回值和清理错误保留 stage/rank/device/地址/长度上下文。不要为绕过
生产 key/type/address 门禁而设置额外变量。验证完成后删除该临时 Python 分支、构建宏/脚本参数和配套工具，
再以默认 `--build_local_dram_validation OFF` 重新构建。
