# MF / HCOM 同层打点

用于解释已经复现的 `cont receiver e2e` 差距。默认 `--trace=0`；正式性能结果仍用关闭 trace 的多轮测试。
开启后会对每次 busy poll 读时钟，诊断时延包含采集开销，不能直接当作原始基线。

## 构建

修改涉及两个仓库：MF `testRDMA`，相邻 ubs-comm `oneside-msge-merge-old`。
MF 默认 FetchContent 拉远端分支，**不会自动包含相邻目录的未提交修改**。在 MF 仓库根目录执行：

```bash
export MF_HCOM_SOURCE_DIR="$(cd ../ubs-comm && pwd)"
# 在原构建命令中打开以下两项，XPU/Python/ABI 等其他选项保持基线设置。
# 下例使用 wrapper 的默认 XPU/Python/ABI 设置；原来有显式选项的请继续保留。
bash script/build_and_pack_run.sh --build_hcom ON --build_hcom_rdma ON

export HCOM_SOURCE_DIR="$MF_HCOM_SOURCE_DIR"
bash test/indirect_transport_test/build_hostrdma_bench.sh ./output
```

原构建 wrapper 会清理其 build/output 目录。若已有专用构建流程，可用 CMake 参数
`-DFETCHCONTENT_SOURCE_DIR_HCOM=/绝对路径/RDMA_DEMO/ubs-comm` 指向同一源码。
bench 编译需要新 `hcom_rdma_trace.h`，运行使用动态库，不静态链接另一份 HCOM。
不需要更改 SGE 上限、队列大小或原有传输设置。

两端运行前，将本次 MF/HCOM 产物目录放在 `LD_LIBRARY_PATH` 前面；不要混用另一份 `libhcom.so`。
开启 trace 时会输出 `mf_trace_library.path`，这就是本进程使用的 HCOM 路径；缺少新 hook 或事件结构大小
不匹配会在 MF 初始化前报错退出。可对该路径执行 `sha256sum` 并保存两端构建版本：

```bash
git rev-parse HEAD
git -C ../ubs-comm rev-parse HEAD
git diff --stat
git -C ../ubs-comm diff --stat
```

本次修改未提交时，commit ID 只标识基底，不能单独代表实际产物。

## 采集与分析

保留已复现基线中的物理端口、绑核、环境变量及其他参数。两端均改为：

```text
--mode=cont --count=1600 --size=656 --stride=4096 --chunk=960 --warmup=100 --rounds=3 --trace=1
```

trace 保留 warmup，但只记录之后的 rounds；两端必须使用相同配置。
`--trace=1` 仅支持 cont，rounds 为 1–32。先用 3 轮即可。
需 unset `MF_BENCH_RAIL_TRACE`，以免已有的轮内输出干扰打点。
将两端完整 stdout/stderr 分别保存为 `mf-local.log` / `mf-remote.log`，**等待正常退出**后运行：

```bash
python3 test/indirect_transport_test/analyze_hostrdma_trace.py mf-local.log > mf-local-summary.json
python3 test/indirect_transport_test/analyze_hostrdma_trace.py mf-remote.log > mf-remote-summary.json
```

分析器非零退出表示记录不完整或数据覆盖不符；缺少 CQE 不能当作更低时延。
原日志和分析结果都需保留。单个文件只放一个进程的一次运行，不要合并两台机器的日志。

### 日志不便传出时：设备上先生成紧凑统计

只需更新 Python 分析脚本，无需重新编译 benchmark 或 HCOM。完整日志留在设备上，进程正常退出后执行：

```bash
# local 设备，MF 仓库根目录
python3 test/indirect_transport_test/analyze_hostrdma_trace.py --compact mf-local.log > mf-local-compact.json
# remote 设备，MF 仓库根目录
python3 test/indirect_transport_test/analyze_hostrdma_trace.py --compact mf-remote.log > mf-remote-compact.json
```

带回两个 `*-compact.json` 即可。常见两三轮输出只有几 KB，不包含逐条 POST/CQE、地址或完整完成曲线。
保留配置、实际库路径、每轮阶段耗时、WR 形态、poll 调用/相邻 CQE/逐 WR 等待的 min/p50/p95/max，
以及完成 10/25/50/75/100% 数据的时间。单次 post_send 提交多个 WR 时，调用耗时按同线程相同起止时间
去重；不把两个 WR 算成两次 post_send。local 水位组最多展示首尾各四组，实际总组数仍保留。

紧凑模式额外对照采集器汇总的 POST/CQE 数量，并检查对应 poll 和 dispatch 记录是否保留。
`status=incomplete` 或非零退出码表示不能据此认定完整完成耗时；JSON 中仍保留可用的应用阶段信息和
完整性检查结果。不要先从终端复制被省略的文本再生成统计；预处理不能恢复丢失的 CQE。

口径：p50/p95 是**单轮内事件样本**的最近秩分位数，不是多轮 e2e 分位数；
`data_post_to_cqe_us` 包含排队时间；`reported_empty_polls` 和最大 poll 间隔来自返回数据 CQE 的 poll
所携带的累计窗口，该窗口可能从首个数据提交之前开始，不是严格数据传输区间的空轮询总数。
同一 poll 返回的多条 CQE 观察间隔可为 0。`first_data_post_to_batch_return_us` 包含同步返回开销，
不能当成精确 CQE 完成时间。

重点比较：

| 端 | 输出 | 含义 |
|---|---|---|
| remote | `request_to_first_data_post_us` | 看到地址请求至首个数据 verbs 提交开始 |
| remote | `first_data_post_to_last_cqe_us` | 首个数据 verbs 提交开始至最后数据 CQE 被观察到 |
| remote | `data_wr_shapes` / `wr_counts` | 实际 WR 的 SGE 数、字节数，以及数据/水位 WR 数 |
| remote | `data_completion_curve` | 相对首个数据提交的累计完成字节曲线 |
| remote | `successful_data_poll_median_us` | 返回数据 CQE 的 poll 调用耗时中位数 |
| local | `e2e_us` / `request_call_us` | 本端完整一轮和地址请求调用耗时 |
| local | `ready_groups` | 本端实际观察到的水位增量及时间；中间水位可能被后续值覆盖 |
| local | `scatter_sum_us` / `last_ready_to_end_us` | 实际各段 scatter 区间之和、末次水位可见后的尾部 |

比较 SGL 时应统一首个 POST 的起点口径：这里用 `POST_BEGIN`，不是 `POST_END`。
地址请求、数据、水位通过实际远端地址区间分类，不只按“8 字节”猜测。
水位 WRITE 的远端可见时间以 local 的 `watermark_observed` 为准，不用 remote 的发送 CQE 替代。

## 事件与完整性

- HCOM：两条 SGL 提交路径及普通 WRITE 的 POST_BEGIN/END；busy-poll 调用起止、空 poll 次数、
  poll 间最大间隔、CQE_OBSERVED、CQ_DISPATCH_BEGIN/END。WR opcode 和 WC opcode 使用不同 verbs 枚举。
- MF remote：request_observed、request_decoded、copy_batch_begin/end。
- MF local：local_round_begin/end、request_begin/end、watermark_observed、scatter_begin/end；水位与
  scatter 带 rail、`[from, upto)` 范围及原始水位值。
- 热路径使用预分配、预触页的线程独立缓冲区；没有逐条打印或分配。最多 16 个生产线程，每线程容量
  最大 65536 条；超量丢失会计数并返回 incomplete。只在停止 HCOM worker 后读取和输出缓冲区。
- `capture_round` 是包含 warmup 的一基轮次；CQ 上的值是**观察窗口**，未必是该 WR 的所属轮次。
  分析器按 QP + wr_id + 时间先后关联 POST_BEGIN 和 CQE，支持 wr_id 复用及 CQE 早于 POST_END。
- 日志按线程输出，分析前须按 timestamp 排序。时钟均为 CLOCK_MONOTONIC_RAW，只做同机相减。
  CQE 时间是软件观察时间，同一 poll 返回的 CQE 共享时间戳，不是硬件完成时间。
- `max_poll_gap_ns` 包含上次非空 poll 以来的回调与采集开销，不等于 CPU 调度停顿。
- MF 采集的是 CQ dispatch 区间，没有把整个 dispatch 命名成应用 callback；它们不能视作相同口径。

当前指定的旧 HCOM 分支 `NET_WR_MAX_SGE=16`，请求 IOV 上限为 30。
因此在远端目标连续、同 rkey 的情况下，1600 块、chunk=960、每次 PutV 最多 30 项对应：
**54 次数据 PutV，但通常为 107 个数据 WR + 2 个水位 WRITE**。
53 组 30 项各拆为 16+14 SGE，最后一组 10 项为一个 WR。实际以 trace 为准。
这与此前 30 SGE/WR 的 SGL trace 不是同一 WR 形态，不能把“54 次 PutV”当成“54 个 CQE”。

## 无网卡检查

```bash
python3 test/indirect_transport_test/test_analyze_hostrdma_trace.py
# 在 ubs-comm 根目录：
g++ -std=c++17 -Wall -Wextra -Werror -Isrc/hcom \
    test/rdma_trace_standalone/trace_test.cpp src/hcom/hcom_rdma_trace.cpp -o /tmp/hcom_trace_test
/tmp/hcom_trace_test
```

这些检查覆盖事件 ABI 拒绝、关闭采集、空 poll 统计、跨窗口 dispatch、WR ID 复用、提前 CQE、
缺失 CQE 与字节覆盖校验。它们不替代 Linux 全量编译和双机 RDMA 运行。
