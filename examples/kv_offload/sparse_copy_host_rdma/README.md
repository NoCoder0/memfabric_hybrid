# CPU HOST_RDMA 稀疏拷贝示例

`001_host_rdma_bench.py` 是一个双角色 Python 示例：**MF 负责准备和显式轮询，offload 负责拷贝**。
不需要 Torch、NPU 或 `offload.initialize()`，不启动 C++ benchmark 可执行文件。

## 调用流程

```text
两端：bm.initialize → bm.create2 → join → prepare_host_rdma_sparse

local / rank 0                       remote / rank 1
准备目标数据区                        初始化源数据区
        ←──── 控制通道 case ready 屏障 ────→
offload.sparse_copy_host_rdma(...)    handle.poll_host_rdma_sparse(...)
校验、统计                            按需重复调用 poll，处理请求
        ──── case_done / ack ────→
        ←──── 最终 finished 屏障 ────→
两端：handle.destroy → bm.uninitialize
```

`prepare_host_rdma_sparse` 只准备工作区、交换配置并初始化算子上下文，**不会启动轮询线程**。
`gather` 模式仍会准备执行 CPU gather/scatter 的工作线程池。
线程池使用原 `hostrdma_batch_bench.cpp` 的 `ParallelCopyPool`：固定线程分片，
generation 原子变量派发，工作线程和调用线程自旋等待；不使用任务队列、条件变量或 future。
远端 gather 工作线程逐线程绑核，本地 scatter 工作线程沿用原实现，不主动绑核。
远端由 example 的主线程显式调用 poll；两次 poll 之间检查控制消息。
一个 BM 上串行处理请求，整个块数/块大小矩阵复用同一次建链和准备。
每个 case 在热循环前调用 MF 的 `prepare_host_rdma_sparse_case`，缓存目标 VA 数组并重建原线程池。
相同目标地址的后续请求复用数组；正常 copy 接口仍支持目标地址变化，并自动更新缓存。

## 模式

两端在启动前设置同一个环境变量，只运行所选的一种模式：

```bash
export MF_HOST_RDMA_SPARSE_MODE=gather  # baseline / cont / gather
```

| 模式 | 数据路径 |
|---|---|
| `baseline` | remote 离散源 → batch RDMA write → local 离散目标 |
| `cont` | remote 离散源 → batch RDMA write + 每链路进度水位 → local staging → 增量 scatter |
| `gather` | remote 线程池 gather → 一次整块 RDMA write → local staging → 线程池 scatter |

消息、staging、聚合区和最终目标仍然全部在 BM DRAM 池中。
用户数据区后预留一个独占工作区，工作区大小由 MF 查询接口计算；Python 不再手写内部协议布局。
cont 的目标地址每 16 块压缩描述；水位间隔由 `--chunk` 控制，两者含义不同。
cont 在所有水位消费完后等待服务端完成确认，再允许复用工作区。

没有 `--backend`、`--mode` 或 `MF_HOST_RDMA_SPARSE_ENABLE`。
环境变量缺失或非法会报错；准备时固定模式，切换模式需要两端重新运行。
旧 Python `Benchmark` 拷贝实现、Python gather/scatter 线程池和 `all` 模式已移除。

## 前置条件与构建

- 两台 Linux CPU 主机、可用的 RDMA 网卡及 RDMA 网络；同机双进程也需要可用 HOST_RDMA。
- 两端安装本分支重新构建的 Python 包及匹配的 HCOM 库，不能混用旧库或旧 Python 扩展。
- 此示例使用现有 HOST_RDMA/HCOM 传输路径，没有切换为 HCOMM 后端。

```bash
bash script/build_and_pack_run.sh --xpu_type NONE --build_hcom ON --build_hcom_rdma ON
```

按照项目安装说明安装构建产物；动态链接配置（如 `LD_LIBRARY_PATH`）应在启动前设置。
脚本默认设置 `MF_HYBM_ENABLE_4K_PAGE=1`，显式已有值优先。
`--env-file` 支持字面的 `KEY=VALUE` 或 `export KEY=VALUE`、引号及注释，不执行 shell 或变量展开。

## 双机运行

先启动 local，再启动 remote。两端都设置 `MF_HOST_RDMA_SPARSE_MODE`，替换下面的地址：

```bash
# local / rank 0
export MF_HOST_RDMA_SPARSE_MODE=gather
python3 examples/kv_offload/sparse_copy_host_rdma/001_host_rdma_bench.py \
  --role local --control-host LOCAL_CONTROL_IP --control-port 18581 \
  --store-url tcp://LOCAL_CONTROL_IP:18580 --hcom-url tcp://LOCAL_RDMA_IP:19000 \
  --warmup 10 --rounds 1000 \
  --stats-file local_results.json

# remote / rank 1
export MF_HOST_RDMA_SPARSE_MODE=gather
python3 examples/kv_offload/sparse_copy_host_rdma/001_host_rdma_bench.py \
  --role remote --control-host LOCAL_CONTROL_IP --control-port 18581 \
  --store-url tcp://LOCAL_CONTROL_IP:18580 --hcom-url tcp://REMOTE_RDMA_IP:19000 \
  --warmup 10 --rounds 1000 \
  --poll-timeout-ms 100 --stats-file remote_results.json
```

默认运行 `[100, 200, 300, 400, 600, 800, 1200, 1600, 2400, 3200, 4800, 6400, 9600, 12800, 19200, 25600]` 个块
× `[656, 1024]` 字节，共 32 组。
可用 `--counts`、`--sizes` 分别覆盖；例如 `--counts 100 --sizes 656` 只运行一组。
测试矩阵、模式、stride、rounds、warmup、chunk、links 和显式容量必须两端一致，开始前会核对。
多网卡时两端 URL 数量相同，例如 `--hcom-url 'tcp://IP0:19000;tcp://IP1:19000'`。
TCP 只承载配置核对、屏障、结果和退出消息；地址消息、request、数据、水位及完成状态仍走 RDMA。

## 参数

| 参数 | 用途 | 默认值 |
|---|---|---|
| `--role` | `local` 请求端 / `remote` 服务端 | 必填 |
| `--store-url` | MF 配置服务地址 | 必填 |
| `--hcom-url` | 本端 RDMA URL，多网卡用 `;` 分隔 | 必填 |
| `--control-host` | local 控制网地址 | 必填 |
| `--control-port` | 控制通道端口 | `18581` |
| `--counts` | 每次读取块数，支持列表；别名 `--segments` | 上述 16 个档位（100～25600） |
| `--sizes` | 每块字节数，支持列表；别名 `--segment-bytes` | `656 1024` |
| `--stride` | 相邻源/目标块起始地址间隔 | 当前块大小的两倍 |
| `--matrix` | 兼容保留；默认已运行 16 个块数 × 2 个块大小 | 无需指定 |
| `--warmup` | 每 case 预热次数；别名 `--warmup-rounds` | `10` |
| `--rounds` | 每 case 计时次数 | `1000` |
| `--chunk` | cont 每条链路发布水位的块数间隔 | `128` |
| `--gather-threads` | gather 模式的远端 CPU 工作线程数 | `16` |
| `--gather-cpus` | 远端 gather 逐线程 CPU 列表，例如 `0-15`、`0-7,16-23` | 自动选取允许的 CPU，优先当前 CPU |
| `--scatter-threads` | gather 模式的本地 CPU 工作线程数 | `6` |
| `--dram-mb` | 每端池容量，包括数据和工作区；正的 2 MiB 倍数 | 自动计算，至少 16 MiB |
| `--poll-timeout-ms` | 远端每次 poll 等待新请求的上限，范围 `[0, 60000]` | `100` |
| `--round-timeout` | 算子协议等待超时，秒 | `30` |
| `--timeout` | 整个测试的子进程看门狗，秒 | `600` |
| `--stats-file` | JSON 结果路径 | `host_rdma_<role>.json` |
| `--log-level` | MF 日志级别，0～4 | `3` |
| `--env-file` | 环境变量配置文件 | 无 |

`--poll-timeout-ms=100` 是 **最多 100 ms 的忙轮询加 yield**，不是 sleep，也不是网卡事件等待。
没有请求时返回，收到请求时立即处理并返回。它只限制等待新请求的时间，不能打断已经接受的拷贝。
设置 `0` 表示每次只检查一次，example 仍会反复调用，并在每次调用之间检查控制消息。
等待和轮询会占用 CPU；gather/scatter 工作线程池在等待任务时也自旋。
`--gather-cpus` 只需在 remote 端设置，通过 `MF_HOST_RDMA_GATHER_CPUS` 在准备时传给 offload，
不改变 MF 准备接口。也可直接设置该环境变量；命令行值优先于环境变量和 env-file。
未设置时从当前线程允许的 CPU 中自动选择，优先当前 CPU，再按 CPU 编号补齐。
CPU 数少于 gather 线程数、列表重复/非法或指定 CPU 不在允许集合内时，准备阶段报错。
列表多于线程数时使用前 N 个。线程数仍由 `--gather-threads` 控制，scatter 不主动绑核。

沿用原 C++ benchmark：每个 case 的预热及全部计时轮次结束后，再校验最终目标数据。
结果报告 Python 调用 C++ offload 的端到端 avg/p50/p95/p99，
包含 pybind 参数转换开销，不含数据校验及 case 控制握手；远端报告处理请求数及空轮询次数。

全部用例完成后，按 URMA 示例的样式打印平均耗时汇总表：

```text
| bytes/pkt | packets | E2E(us) | request(us) | wait remote(us) | host gather(us) | host write(us) | gather+write(us) | scatter(us) |
```

两端表格相同：E2E/request/wait remote/scatter 来自 local，host gather/write/gather+write 来自 remote。
cont 的 `wait remote(us)` 列替换为 `receive+scatter(us)`，包含等待进度和分段拷贝。
不适用的阶段显示 `-`：baseline 无 gather/scatter，cont 无 gather；CPU 路径没有 AICPU 和 launch 阶段。
JSON 保留原有字段并新增各阶段统计及 min/max；同目录 `<stats-file>.txt` 保存详细 ASCII 表格：
`bytes/pkt、packets、stage、avg(us)、min(us)、max(us)、P50(us)、P95(us)、P99(us)`，附各列含义。
例如 `--stats-file local_results.json` 同时生成 `local_results.json.txt`。预热不纳入任何阶段统计。

| 指标 | 计时范围 |
|---|---|
| E2E | 紧贴 Python 同步 offload 调用前后打点，包括 pybind、原生地址校验、发布请求、等待和数据搬运；不含 Python 返回码检查、结果校验和统计查询 |
| request | 传输地址消息并发布 request doorbell；不含地址消息构建 |
| wait remote | baseline/gather：等待 done 并检查 status；不含 gather 模式的本地 scatter |
| receive+scatter | cont：从等待进度、分段 CPU 拷贝到等待 done 并检查 status 的连续区间 |
| host gather | 仅原 `GatherAddresses()` 调用，含派发和等待完成；地址解析/换算在此计时外 |
| host write | 同步 MF copy/batch 调用；cont 包含进度水位发布，不含最终 status/done 发布 |
| gather+write | gather：从调用 `GatherAddresses()` 前到同步 MF copy 返回后的连续区间；不含源地址数组构建及最终 status/done 发布 |
| scatter | gather：仅原 `Scatter()` 调用，不含目标数组分配、换算和释放；cont：累加已有进度的 CPU 拷贝区间 |

阶段耗时由各端原生算子计时，每次成功请求后查询，按 sequence 校验匹配。
各主机只计算自身单调时钟的耗时，不跨主机相减时间戳。
`cont` 的 write/scatter 重叠；receive+scatter 包含 scatter，gather+write 包含 gather/write，
local 的等待与 remote 的处理重叠，request 和远端处理也可能重叠，**不能将各阶段相加作为 E2E**。
E2E 还包含未单列的协议等待、消息解析等开销；算子内读时钟的开销包含在本次测试中。
本次新增原生 V2 统计接口，旧 C 结构体和 getter 保留；Python getter 名称不变，增加字段。
两端需同步更新 example，并重新构建安装 offload 库及 Python 扩展；握手会拒绝旧统计版本的脚本。
case 准备接口也需要同步更新 SMEM 库和 Python 扩展。旧 copy/prepare 接口签名保持不变。
支持原 `MF_BENCH_APP_CPU`：通信线程和全局准备完成后绑定调用线程，再创建各 case 的线程池。
gather 仍显式逐线程绑核；scatter 沿用原实现，继承创建线程的 affinity。
因此若指定单个 `MF_BENCH_APP_CPU`，scatter 线程也会继承这个单核集合，这与原 benchmark 一致。
阶段计时口径已对齐；Python E2E 仍含 Python/pybind 调用开销，不等同于原纯 C++ E2E。
完整矩阵耗时超过默认 600 秒时，可在两端增加 `--timeout`，或减少 `--rounds`。

## 应用接口

```python
from memfabric_hybrid import bm, offload

# 两端用 MF initialize / create2 / join，预留不与用户数据重叠的工作区。
workspace_bytes = bm.host_rdma_sparse_workspace_size(max_blocks, max_block_bytes, links=1)
assert workspace_bytes > 0
ret = handle.prepare_host_rdma_sparse(
    workspace_gva, workspace_bytes, max_blocks, max_block_bytes,
    progress_interval=128, timeout_ms=30000, gather_threads=16, scatter_threads=6,
)
if ret != 0:
    raise RuntimeError(f"prepare failed: {ret}")

# 可选：每个 case 的热循环前准备目标地址与线程池，之后两端做 ready 屏障。
# rank 0 传本 case 的 dst_gvas；rank 1 传 []。无需重置请求序号。
ret = handle.prepare_host_rdma_sparse_case(dst_gvas if rank == 0 else [], count, block_bytes)
if ret != 0:
    raise RuntimeError(f"case prepare failed: {ret}")

# rank 1：应用在自己的线程/事件循环中按需调用。
ret = handle.poll_host_rdma_sparse(timeout_ms=100)
# 1：处理完成一条请求；0：没有请求；负数：错误，应停止并协调两端退出。

# rank 0：远端需要同时执行上述 poll。
ret = offload.sparse_copy_host_rdma(handle, src_gvas, dst_gvas, block_bytes)
# 0：同步拷贝成功；非零：错误。

# 可选：在当前端下一次请求前读取最近一次成功 copy / poll 的阶段耗时。
timing = offload.host_rdma_sparse_last_timing(handle)
# dict: sequence, request_ns, gather_ns, write_ns, scatter_ns,
#       wait_remote_ns, receive_scatter_ns, gather_write_ns
# rank 0 提供 request/scatter，以及 baseline/gather 的 wait_remote 或 cont 的 receive_scatter；
# rank 1 提供 gather/write，以及 gather 模式的 gather_write；其余字段为 0。
# 尚无成功请求或上下文已失效时抛出异常。
```

C 接口分别为 `smem_bm_poll_host_rdma_sparse`（`smem_bm_sparse.h`）和
`offload_sparse_copy_host_rdma`（`acc_offload.h`）。offload 直接绑定自己的 Python 入口，执行三种模式；
SMEM 负责准备、请求轮询和调用已注册的处理函数，不反向链接 offload。
直接使用 C/C++ 时，在准备前加载并保留 `libmf_acc_offload.so`，直到相关 BM 全部销毁。
Python 包自动完成库加载，无需 offload session 或 NPU 初始化。

首版限制：两端、等长块、源为 rank 1 池 GVA、目标为 rank 0 池 GVA；不接受池外地址或 HBM。
目标块不能重叠；源可以重复。两端工作区偏移相同、64 字节对齐，工作区不能被用户数据覆盖。
每 BM 一次准备；不支持准备后扩容或改模式。传输失败或算子等待超时后，应协调两端停止并重建 BM。
最后一次拷贝和 poll 返回后，先做应用退出屏障，再销毁 BM。轮询线程若由应用创建，也由应用停止和回收。
`timeout_ms` 无法强制中断底层 HCOM 阻塞调用；example 的进程看门狗提供整个测试的超时保护。
