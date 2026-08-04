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
