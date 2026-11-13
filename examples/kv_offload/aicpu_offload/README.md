# A3/A5 图模式下的 AICPU 拷贝

在 Ascend910C（A3）上使用 SDMA，在 Ascend950（A5）上使用 URMA。`offload.Scene.AICPU` 使用本实例的 Host 池与本卡 NPU 内存进行 H2D/D2H 拷贝，支持 eager 和原生 `torch.npu.graph`。初始化在图外完成，A5 在进程内连接 Host rank 0 与 Device rank 1。

## 安装与运行

A3 需要支持 GVA V4/VMM 的驱动、提供 `aclnnShmemSdmaStarsQuery` 及其 workspace 查询接口的 CANN、设备侧 `libascend_hal.so` 的 `halSqCqQuery/halSqCqConfig`，以及匹配的 PyTorch 和 torch_npu。A5 需要匹配的 CANN 9.1+、HCOMM 和 UBC_CTP 链路，设置 MF_DEVICE_URMA_ENABLE_UBOE=0。Host EID 发现使用 libdcmi.so、libdrvdsmi_host.so 和 /sys/class/ubcore 或 /sys/class/uburma。Host 库、Python binding、launch 扩展和 AICPU 包须来自同一次构建。设置 `ASCEND_HOME_PATH`，安装新的 NPU wheel 后执行：

```bash
mfcli kernel install --soc-version A3  # A5 设备使用 A5
mfcli kernel info
python examples/kv_offload/aicpu_offload/aicpu_graph_offload.py --device 0 --records 65536
python examples/kv_offload/aicpu_offload/aicpu_graph_offload.py --device 0 --records 20000 --warmup-iters 0
```

示例使用显式 offload.Offload() 实例。第二条 Python 命令在新进程中验证无 eager 预热的首次捕获。示例在同一张图上改变方向、条数、地址顺序和长度；大批次示例还检查图内元数据生产、拷贝后消费、空批次、256/1000/8190 条附近的批次边界，以及超过 32 MiB 的单条记录。

## 调用约定

设置 `OffloadConfig.scene = offload.Scene.AICPU` 和 `device_id`。配置的 `flags` 为 0；`reserve_size`、`alloc_size` 均非零且向上按 GiB 对齐后相等。模块级 API 和显式 `offload.Offload()` 实例使用相同的五参数接口：

```python
pool = offload.Offload()
assert pool.initialize(config) == 0
ret = pool.sparse_copy(src_ptrs, dst_ptrs, lengths, count, device)
```

- `pool` 为已初始化的模块或实例。源、目的地址表是等容量的连续一维 NPU int64 Tensor。长度表是同容量的连续一维 NPU Tensor，每项按 `uint32_t` 读取，单位为字节。示例使用 int32 Tensor 存放可由 int32 表示的非负长度。count 是同设备的单元素 int32 Tensor。调用方保证 `0 <= count <= 实际表容量` 且不超过 65536。
- 每条非零长度记录必须恰有一端是本池的**原始 Host 地址**，完整区间在池内；另一端是本卡有效的 NPU 地址。不要传入 AIV 使用的 DVA。kernel 每次执行逐条推断方向，允许批内独立的双向记录，不保证重叠拷贝的顺序。零长度不访问 payload。
- 捕获固定的是四个元数据 Tensor 的地址。重放前可以原地修改内容；图中也可以先生成元数据，再执行拷贝。不要替换底层存储。元数据、payload、池和实例均须覆盖图的使用期。
- 每个实例独立持有池、设备上下文、工作区和通知资源。A3 占用一个 entity ID，A5 按需申请两个。同设备可创建多个实例。同实例的调用和 replay 须串行；跨 stream 须建立前次完成到下次开始的显式依赖。销毁前停止提交、同步在途工作并销毁图，再释放池。
- Host 返回 0 表示已提交。内核逐条校验，遇到非法记录时立即停止。此前已提交给 SDMA/URMA 的拷贝可能已生效，暂存的记录会被丢弃。输入错误或设备通信错误都可能使后续 notify 等待失败。通过 stream 同步检查结果，通信失败后重建资源。
- 仅支持本池内存的分配、释放、池内地址注册校验和 sparse copy；不支持外部 Host 内存注册、`get_dva`、group pack、KV exchange 或 entry gather。

A5 在图外建立 Host/Device 连接、注册并导入 Host 池，准备持久 context、8190 条描述符工作区和 notify。每次调用固定提交 9 组 HybmUrmaDeviceCopy 与 Notify Wait/Reset，覆盖最多 65536 条记录。首块读取设备 count，后续块复用该值；各块单趟校验并生成描述符，按最多 1000 条提交 HCOMM，再发送 fence 和完成 marker。空块也发送 marker，下一块在用户流的 Wait/Reset 后复用工作区。
