# Host URMA Discrete Small-Packet Bandwidth Test

该程序使用 `HOST_DEVICE_URMA` 的 `copy_data_batch` 测试离散 Host URMA 写带宽。每个包默认位于独立的
4 KiB stride 上。`--concurrency N` 会创建 N 个 worker 线程，并让每个 worker 同时执行独立的
`copy_data_batch`；每个 worker 都传输 `--packet-counts` 指定数量的包。

默认测试：

- 包大小：576、1152 字节
- 包数量：2K、4K、8K、16K、32K
- 并发 worker 数：1、2、4、8
- 每个组合预热 1 次、测量 3 次，每次循环完整包集合 10 次
- 输出每个组合的 GB/s、Gbps、Mpps，并汇总每个包大小和每 worker 包数量下的最高带宽

例如并发为 8、包数量为 2K 时，会同时执行 8 个 2K `copy_data_batch`，每轮总计传输 16K 个包。
不同 worker 使用互不重叠的远端地址区。

先在 rank 0 执行：

```shell
python 04_host_urma_small_packet_bw.py \
  --rank 0 --head-ip <rank0_ip> --eid <rank0_eid>
```

再在 rank 1 执行：

```shell
python 04_host_urma_small_packet_bw.py \
  --rank 1 --head-ip <rank0_ip> --eid <rank1_eid>
```

调整并发数或测试规模：

```shell
python 04_host_urma_small_packet_bw.py \
  --rank <0_or_1> --head-ip <rank0_ip> --eid <local_eid> \
  --concurrency 1 2 4 8 16 \
  --packet-counts 2k 4k 8k 16k 32k \
  --packet-sizes 576 1152 --loops 20 --repeats 5
```

以上自定义参数需要分别在 rank 0 和 rank 1 执行时传入。

rank 0 和 rank 1 的端口、`--packet-counts`、`--concurrency` 和 `--stride` 必须保持一致，以保证两端创建
相同大小的内存池。测试运行时，进程线程数会在固定后台线程基础上增加当前并发 worker 数。

程序默认将 `MF_HYBM_RDMA_SWAP_SPACE_SIZE` 设为 1024 MiB。每个 worker 的 batch 会占用约
`packet_count * packet_size` 字节 swap；自定义更高并发或更大 batch 时，应确保 swap 大于所有 worker
同时占用的总量，并保留额外余量。例如 8 workers、32K 包、1152 字节至少需要 288 MiB。
