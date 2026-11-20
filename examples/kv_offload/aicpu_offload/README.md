# A3/A5 图模式下的 AICPU 拷贝

在 Ascend910C（A3）上使用 SDMA，在 Ascend950（A5）上使用 URMA。`scene=offload.Scene.LOCAL`、`executor=offload.Executor.AICPU` 使用本实例的 Host 池与本卡 NPU 内存进行 H2D/D2H 拷贝，支持 eager 和原生 `torch.npu.graph`。初始化在图外完成，A5 在进程内连接 Host rank 0 与 Device rank 1。

## 安装与运行

A3 需要支持 GVA V4/VMM 的驱动、提供 `aclnnShmemSdmaStarsQuery` 及其 workspace 查询接口的 CANN、设备侧 `libascend_hal.so` 的 `halSqCqQuery/halSqCqConfig`，以及匹配的 PyTorch 和 torch_npu。A5 需要匹配的 CANN 9.1+、HCOMM 和 UBC_CTP 链路，设置 MF_DEVICE_URMA_ENABLE_UBOE=0。Host EID 发现使用 libdcmi.so、libdrvdsmi_host.so 和 /sys/class/ubcore 或 /sys/class/uburma。Host 库、Python binding、launch 扩展和 AICPU 包须来自同一次构建。设置 `ASCEND_HOME_PATH`，安装新的 NPU wheel 后执行：

```bash
mfcli kernel install --soc-version A3  # A5 设备使用 A5
mfcli kernel info
python examples/kv_offload/aicpu_offload/aicpu_graph_offload.py --records 1000
```

默认使用逻辑设备 0、1、2、3，依次测试每条记录为 256、2048、4096、16384、524288、20971520 字节的六种规模，每卡每次图重放拷贝 1000 条记录。以上命令也可省略 `--records 1000`。每种规模下，4 张卡并发执行，先测试 H2D，再测试 D2H，每个方向默认计时 1 次图重放。可见设备不足 4 张时需显式减小 `--num-devices`。

| 参数 | 含义 | 默认值 / 范围 |
| --- | --- | --- |
| `--bytes` | 一个或多个每条记录的字节数，按输入顺序测试，所有卡使用相同大小 | `256 2048 4096 16384 524288 20971520`；每项 1～2147483647 |
| `--records` | 每卡每次图重放的记录条数，对所有字节数生效 | 1000；1～65536 |
| `--device` | 起始逻辑设备 ID，使用连续的设备 | 0；非负且设备范围有效 |
| `--num-devices` | 并发卡数，每卡一个独立进程 | 4；正整数 |
| `--iters` | 每个方向正式计时的图重放次数 | 100；正整数 |
| `--warmup-iters` | 捕获前的 eager 预热次数，以及每个方向的图重放预热次数 | 1；非负整数 |

自定义多个大小、单卡、无预热和大记录测试示例：

```bash
python examples/kv_offload/aicpu_offload/aicpu_graph_offload.py --bytes 256 2048 4096 --records 1000
python examples/kv_offload/aicpu_offload/aicpu_graph_offload.py --bytes 4096 --records 8192 --num-devices 1
python examples/kv_offload/aicpu_offload/aicpu_graph_offload.py --bytes 4 --records 65536 --warmup-iters 0
python examples/kv_offload/aicpu_offload/aicpu_graph_offload.py --bytes 33554433 --records 1
```

每种规模下，每卡创建独立的 `offload.Offload()` 实例、Host 池、NPU buffer、stream 和 graph，完成测试并释放该规模的资源后，所有卡集合，再进入下一种规模。数据使用 `uint8`，每条记录对应连续 buffer 中的一个切片，单次拷贝量为当前 `bytes × records`。Host 池按该数据量向上取整到 GiB，最少每卡 1 GiB；此外还需预留校验副本和框架工作区所需的内存。默认最大规模为每卡约 19.53 GiB 数据，对应每卡 20 GiB Host 池，4 卡的 Host 池合计 80 GiB。

每种规模各捕获一次图，图中只包含 `sparse_copy`，通过图外原地更新地址表，在同一张图上切换 H2D/D2H。各卡在每个方向计时前集合，全部完成计时后再开始校验。NPU Event 在同一 stream 上测量连续 `iters` 次图重放，输出的 `time` 是平均每次重放耗时，`data` 是每卡每次重放的数据量，`bandwidth` 是该数据量除以平均耗时，单位 GiB/s。初始化、预热、地址表更新和校验均不计入耗时；结果表示多卡并发负载下各卡的带宽，每个结果均带有当前的 `bytes` 和 `records`。

每个方向在计时前后分别生成新的、可复现的随机字节数据，目的端预置为期望数据的按位取反值，重放并同步后全量比较。成功时输出每卡每个方向的 `PASS` 和性能结果；失败时记录设备、方向、数据规模及首个错误字节的位置，终止其他 worker，并以非零状态退出。默认流程只测试传入的规模；批次边界可通过调整 `--records` 单独运行，不再自动执行动态 count、地址翻转、图内元数据生产与消费等回归场景。

## 调用约定

设置 `scene=offload.Scene.LOCAL`、`executor=offload.Executor.AICPU` 和 `device_id`。配置的 `flags` 为 0；`reserve_size`、`alloc_size` 均非零且向上按 GiB 对齐后相等。模块级 API 和显式 `offload.Offload()` 实例使用相同的五参数接口：

```python
config = offload.OffloadConfig()
config.scene = offload.Scene.LOCAL
config.executor = offload.Executor.AICPU
config.device_id = device.index
config.reserve_size = 1 << 30
config.alloc_size = 1 << 30
pool = offload.Offload()
assert pool.initialize(config) == 0
ret = pool.sparse_copy(src_ptrs, dst_ptrs, lengths, count, device)
```

- `pool` 为已初始化的模块或实例。源、目的地址表是等容量的连续一维 NPU int64 Tensor。长度表是同容量的连续一维 NPU Tensor，每项按 `uint32_t` 读取，单位为字节。示例使用 int32 Tensor 存放可由 int32 表示的非负长度。count 是同设备的单元素 int32 Tensor。调用方保证 `0 <= count <= 实际表容量` 且不超过 65536。
- 每条非零长度记录必须恰有一端是本池的**原始 Host 地址**，完整区间在池内；另一端是本卡有效的 NPU 地址。不要传入 AIV 使用的 DVA。kernel 每次执行逐条推断方向，允许批内独立的双向记录，不保证重叠拷贝的顺序。零长度不访问 payload。
- 捕获固定的是四个元数据 Tensor 的地址。重放前可以原地修改内容；图中也可以先生成元数据，再执行拷贝。不要替换底层存储。元数据、payload、池和实例均须覆盖图的使用期。
- 每个实例独立持有池、设备上下文、工作区和通知资源。A3 占用一个 entity ID，A5 按需申请两个。同设备可创建多个实例。同实例的调用和 replay 须串行；跨 stream 须建立前次完成到下次开始的显式依赖。销毁前停止提交、同步在途工作并销毁图，再释放池。
- Host 返回 0 表示已提交。内核逐条校验，遇到非法记录时立即停止。此前已提交给 SDMA/URMA 的拷贝可能已生效，暂存的记录会被丢弃。输入错误或设备通信错误都可能使后续 notify 等待失败。通过 stream 同步检查结果，通信失败后重建资源。
- 支持本池内存的分配、释放、池内地址注册校验、`get_dva` 和 sparse copy；不支持外部 Host 内存注册、group pack、KV exchange 或 entry gather。
- `pool.get_dva(host_ptr)` 返回本池地址对应的设备地址，供应用层自定义 MTE 算子访问。

A5 在图外建立 Host/Device 连接、注册并导入 Host 池，准备持久 context、8190 条描述符工作区和 notify。每次调用固定提交 9 组 HybmUrmaDeviceCopy 与 Notify Wait/Reset，覆盖最多 65536 条记录。首块读取设备 count，后续块复用该值；各块单趟校验并生成描述符，按最多 1000 条提交 HCOMM，再发送 fence 和完成 marker。空块也发送 marker，下一块在用户流的 Wait/Reset 后复用工作区。
