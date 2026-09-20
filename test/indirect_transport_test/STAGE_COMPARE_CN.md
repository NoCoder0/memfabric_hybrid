# MF / SGL 同源阶段采集（2026-09-20）

当前状态：代码、离线验证和设备命令已完成；本机为 Windows，没有 RDMA 双机访问。
用户选择自行在设备执行并回传四份紧凑 JSON。**尚未取得本次同库实机 trace，不能据此宣布差距根因。**
已确认的 MF 300 多 / SGL 500 多 μs，以及 source_order=0/1 无收益，均保留为已完成实验。

本次只补测量：以 ubs-comm `740f0bb` 为基底保留 30 SGE、传输逻辑、源内容更新、通知和队列行为。
采集默认关闭；唯一新增的 trace 模式行为是 SGL 保留 `--warmup`，随后才采集少量轮次。
没有实现窗口限流、大页、源内容或通知机制实验。

## 1. 在两台设备获取修改

修改已提交到三个仓库的原分支。确认设备仓库分别处于下列分支后，可直接拉取：

```bash
git -C ubs-comm pull --ff-only origin oneside-msge-merge
git -C perf_test_duo_card_sgl pull --ff-only origin duo_card_sgl
git -C memfabric_hybrid pull --ff-only origin testRDMA
```

HCOM 采集提交为 `5810ef5`，SGL 应用阶段提交为 `2f2318d`；MF 工具位于 `testRDMA`。
拉取后直接执行第2节构建，不再应用下面的补丁。若设备已有未提交补丁或其他冲突，保留现场，不强制覆盖。

不能直接访问远端时，仍可使用此前交付包：

交付包包含三个仓库的独立 patch。设备目录应仍为三个相邻仓库；基底分别为：
MF `4b4d6f0b`，SGL `a858218`，ubs-comm `740f0bb`。
在父目录执行（`PATCH_DIR` 为解压目录）：

```bash
export PATCH_DIR=/实际路径/stage_compare_delivery
git -C memfabric_hybrid status --short
git -C perf_test_duo_card_sgl status --short
git -C ubs-comm status --short
git -C ubs-comm apply --check "$PATCH_DIR/ubs-comm.patch"
git -C memfabric_hybrid apply --check "$PATCH_DIR/memfabric_hybrid.patch"
git -C perf_test_duo_card_sgl apply --check "$PATCH_DIR/perf_test_duo_card_sgl.patch"
# 三个 check 均通过后应用；不会 reset、clean 或覆盖已有未跟踪分析文档。
git -C ubs-comm apply "$PATCH_DIR/ubs-comm.patch"
git -C memfabric_hybrid apply "$PATCH_DIR/memfabric_hybrid.patch"
git -C perf_test_duo_card_sgl apply "$PATCH_DIR/perf_test_duo_card_sgl.patch"
```

若 check 报冲突，保留输出；不要强制覆盖用户改动，也不要把 MF 切回 `616e018`。
旧 `TIMER_POOL_ANALYSIS_CN.md`、`V1_0_BATCH_PERFORMANCE_DIFF_CN.md`、`__cmake_systeminformation/` 未纳入补丁。

## 2. 同一次构建产出 HCOM shared / static

两端在 MF 仓库根目录执行，保留之前的 XPU / Python / ABI 构建选项：

```bash
export MF_HCOM_SOURCE_DIR="$(cd ../ubs-comm && pwd)"
export HCOM_SOURCE_DIR="$MF_HCOM_SOURCE_DIR"
# 以下默认选项与原 wrapper 一致；原基线有其他显式选项时原样追加。
bash test/indirect_transport_test/build_stage_compare.sh
```

此脚本沿用 MF wrapper（会重建 MF 的 build/output），然后直接让 SGL 链接这次 MF 构建树里的
`libhcom_static.a`。不再单独重建另一份 ubs-comm。输出：

- `memfabric_hybrid/output/stage-build.json`：三仓库身份、库/程序 SHA256、SGL 实际链接命令。
- `memfabric_hybrid/output/stage-build.env`：准确库/头文件路径。
- MF benchmark 和 SGL `build/rdma_600`。

HCOM 库内还嵌入 commit、tracked diff 摘要、编译配置摘要；MF 从实际 dlopen 库读取，SGL 从实际静态链接对象读取。
运行器核对二进制哈希、源码 commit/diff、MF 实际映射/加载库哈希；不匹配就拒绝建立可比结论。
QP 建立时输出 provider 返回的 SQ/RQ/SGE/inline/CQ 容量、device/port/QP；这和环境变量声明分开保存。
不要在构建与采集之间改源码，或用另一个 HCOM 安装覆盖当前产物。

若 Linux 构建失败，请回传第一个编译/链接错误附近的少量内容。Windows 已做的 CPU 检查不等同于 Linux 全量构建。

## 3. 设备运行

在两个终端分别加载**先前已复现基线的环境变量**。保留物理 NIC、同 NUMA 绑核、提交线程、内存/日志等设置。
运行脚本仅固定本次工作量和已确认的 SQ=1024；不会替你选择 CPU 或改其他性能变量。
脚本直接执行二进制，不能把旧 run wrapper 当作被采集程序。

两个终端都设置：

```bash
cd /实际父目录/RDMA_DEMO
export RUN=mf-sgl-stage-01                 # 两端、两个程序必须相同；重跑换新名称
export STAGE_RESULT_DIR="$PWD/stage-results"
RUNNER="$PWD/memfabric_hybrid/test/indirect_transport_test/run_stage_compare.sh"
# 此时应已加载原基线的 MF_BENCH_APP_CPU、MF_HYBM_HCOM_WORKER_CPU_RANGE、
# MF_HYBM_SUBMIT_CPU_RANGE、RDMA 单口选择、4K page 等环境。全部原样保留。
```

**MF local**：先启动 local 的内嵌 store，然后在另一台启动 remote。
下面 IP 是当前仓库 run_hostrdma_bench.sh 的值；若实际基线不同，使用原基线值。

```bash
MF_CONN=(--rank=0 --store-url=tcp://90.91.183.86:18580 \
         --hcom-url=tcp://192.168.75.86:19000 --with-store=1)
bash "$RUNNER" mf local baseline "$RUN" -- "${MF_CONN[@]}"
# 两端 baseline 正常结束后，两端分别执行 trace：
bash "$RUNNER" mf local trace "$RUN" -- "${MF_CONN[@]}"
```

**MF remote**：

```bash
MF_CONN=(--rank=1 --store-url=tcp://90.91.183.86:18580 \
         --hcom-url=tcp://192.168.75.87:19000 --with-store=0)
bash "$RUNNER" mf remote baseline "$RUN" -- "${MF_CONN[@]}"
bash "$RUNNER" mf remote trace "$RUN" -- "${MF_CONN[@]}"
```

**SGL remote**：先启动 remote，再启动 local。把 CPU 值设为之前使用的实际值；这里只填原有连接和绑核参数。

```bash
export APP_CPU=原remote应用核
export WORKER_CPU=原remote轮询核
# 沿用原先已经验证的 RDMA_600_QP_MAX_SEND_SGE 声明，勿凭猜测设置 cap。
SGL_CONN=(--rdma-ip 192.168.75.87 --listen 192.168.75.87:19000 \
          --app-cpu "$APP_CPU" --worker-cpu "$WORKER_CPU")
bash "$RUNNER" sgl remote baseline "$RUN" -- "${SGL_CONN[@]}"
bash "$RUNNER" sgl remote trace "$RUN" -- "${SGL_CONN[@]}"
```

**SGL local**：

```bash
export APP_CPU=原local应用核
export WORKER_CPU=原local轮询核
export RDMA_600_SOURCE_SEQUENTIAL=0       # 本次固定一种；不是重做0/1收益排除实验
SGL_CONN=(--rdma-ip 192.168.75.86 --peer 192.168.75.87:19000 \
          --app-cpu "$APP_CPU" --worker-cpu "$WORKER_CPU")
bash "$RUNNER" sgl local baseline "$RUN" -- "${SGL_CONN[@]}"
bash "$RUNNER" sgl local trace "$RUN" -- "${SGL_CONN[@]}"
```

每次必须先等两端上一轮正常退出。不同程序不要同时运行。
若原基线另有参数，只追加连接/日志等未被脚本固定的选项；SGL 不接受重复参数。
脚本固定：单口 SGL K=30/G=32/pipeline=on，MF chunk=960，1600×656B/stride4096，warmup=100；
baseline 为 trace 关闭的1000轮，trace 为3轮。SGL仍保留原有 verify 轮，内容更新逻辑不变。
baseline 用来核对补采集后的二进制仍复现差距，并与 trace 开销区分；不是重复前期排除实验。

运行器独立进程每250ms读取一次 `/proc`，保存观察到的线程 affinity 和动态库映射，不进入应用热路径。
`thread_affinity_last_observed` 是采样身份信息，不能当作逐轮 CPU 驻留证明。可将运行器本身钉在空闲同机核，
但不要让继承的单核 mask 妨碍原程序绑核；两边采集方式保持一致。

## 4. 只回传四份 JSON

进程结束后自动分析完整文件；原始逐事件日志不会经过终端复制。每个角色生成：

```text
stage-results/<RUN>-mf-local-trace.compact.json
stage-results/<RUN>-mf-remote-trace.compact.json
stage-results/<RUN>-sgl-local-trace.compact.json
stage-results/<RUN>-sgl-remote-trace.compact.json
```

四份 JSON 内附上一轮 trace-off 结果摘要及其身份哈希。请分别回传这四份，保留设备上的
`.log`、`.identity.json`、baseline compact、`.analysis.stderr`、`stage-build.json`。
末行会报告 process_exit / analysis_exit；不应忽略非零退出码。

默认使用紧凑 JSON，不带逐条 WR、地址或全部完成曲线。超过16KB会另外生成 `.round-N.compact.json`，
按完整轮次分消息传输；原完整 compact 保留，不做截断。针对同一角色自动合回：

```bash
python3 memfabric_hybrid/test/indirect_transport_test/compare_stage_trace.py merge \
  stage-results/$RUN-mf-remote-trace.round-*.compact.json > stage-results/$RUN-mf-remote-merged.compact.json
```

若需要更短传输，直接在设备用下面命令针对某轮重新提取（先分析整份原日志验证完整性，再选择该轮）：

```bash
T=memfabric_hybrid/test/indirect_transport_test
python3 "$T/compare_stage_trace.py" reduce stage-results/$RUN-mf-remote-trace.log \
  --identity stage-results/$RUN-mf-remote-trace.identity.json --app mf --role remote --round 101 \
  > stage-results/$RUN-mf-remote-round101.compact.json
```

MF 轮号通常101–103；SGL保留 verify轮，因此一般121–123。实际以JSON为准，不按原始轮号强行同序配对。
SGL默认20轮verify，对照文件按各自采集顺序展示。

四份 compact 放到同一设备/目录后生成完整表：

```bash
python3 "$T/compare_stage_trace.py" compare \
  stage-results/$RUN-mf-local-trace.compact.json stage-results/$RUN-mf-remote-trace.compact.json \
  stage-results/$RUN-sgl-local-trace.compact.json stage-results/$RUN-sgl-remote-trace.compact.json \
  > stage-results/$RUN-comparison.md
```

只要缺失CQE、字节未覆盖、collector计数不一致、dropped、post错误或产物身份不符，就不能把完成尾部当作有效性能结论。
日志截断时无法通过分析“恢复”尾部；从设备原文件重新生成即可。

## 5. 指标与证据边界

| 指标 | 口径 |
|---|---|
| 正式e2e | trace-off的MF cont receiver / SGL local sparse_copy；多轮分位数保持各原程序口径 |
| request prepare/submit | 本机轮起点→提交边界、提交边界→请求调用返回；SGL提交阶段包括分片编码与Send准备 |
| remote request observed | MF主线程发现请求；SGL主线程 acquire 发现完整请求，随后单列复制/解析/源内容准备/请求准备 |
| WR形态 | 实际verbs WR；按数据、请求、SEND通知、水位WRITE分类，包含SGE/bytes/flags；不是PutV次数 |
| 提交与完成尾部 | 首数据POST_BEGIN→末POST_END；末POST_END→末CQE；首POST_BEGIN→末CQE |
| 在途 | 成功提交WR的POST_BEGIN至软件CQE观察；ibv_post_send内部接纳时刻不可见；同时间戳并发事件合并 |
| 完成曲线 | 按完成数据字节累计25/50/75/100%；不按CQE个数比较不同WR形态 |
| poll | 非空数据poll调用分布、前一次poll间隔、携带窗口的空poll数、调用/间隔直方图、返回条数；窗口可能早于首数据post |
| poll直方图 | 同线程上一次非空poll后累计，桶上界125/250/500/1000/2000/4000/8000ns/∞；不是每一个空poll的原始列表 |
| dispatch/callback | 两边都有CQE→dispatch及dispatch时长；只有SGL提供应用data callback，不能与MF dispatch等同 |
| local就绪/scatter | MF实际水位观察；SGL callback-ready为发布前打点，另有发布后标记、scatter线程实际consumer观察；全部同local时钟 |
| last_ready_to_end | 最后一段被consumer观察可处理→结束；SGL另列最后通知发布→结束，不能把两者混用 |
| scatter累计 | 各真实memcpy区间求和；通知组scatter起止跨度包括组内间隙，不能再与remote传输时长相加 |

只做同机时间差。CQE是软件观察，不是硬件完成。trace轮内事件p50/p95不是多轮e2e分位数。
wr_id按QP与每次出现匹配，允许CQE早于POST_END；同一次post的多WR共享调用时间并去重。
失败链式post保留已提交前缀和call错误，整轮仍为incomplete。未知SEND保留为other，不冒充数据。
新同库数据到手前，只能说测量路径已准备好。历史616e018的247μs与740f0bb的405μs不进入本次新结果表。

## 6. 已做验证与待验证

Windows离线：MF旧分析器10项、统一分析器15项；实际post wrapper用fake verbs编译执行；
HCOM C++11 hook契约/ABI拒绝/字段/poll直方图；SGL四种buffer场景（正常、溢出、线程超限、缺post）；
Python语法与新增bash语法；git diff空白检查。测试时间全部为合成数据，不是实机性能。
Windows测试将CLOCK_MONOTONIC_RAW映射到可用的MONOTONIC，仅为CPU逻辑测试，生产Linux源码不作替换。

设备侧待验证：完整Linux构建、双机正常退出、四份新同库trace和关闭trace的正式e2e。
若失败，只需对应角色的analysis.stderr或首个编译错误；不要先上传整段被终端省略的原日志。
