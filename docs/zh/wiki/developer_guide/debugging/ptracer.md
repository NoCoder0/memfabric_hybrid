# ptracer性能打点工具简介

## 功能概述

MemFabric内置的ptracer工具支持通过如下方式对代码进行打点，对关键函数调用的次数、成功失败情况、耗时等进行统计。

```c++
TP_TRACE_BEGIN(TP_HYBM_RDMA_LH_TO_GD);
ret = CopyLH2GD(params.src, params.dest, params.dataSize, options);
TP_TRACE_END(TP_HYBM_RDMA_LH_TO_GD, ret);
```

内置线程每隔固定周期将打点数据输出到ptracer独立日志中，为分析性能瓶颈、测试性能数据提供了一种简便的方法。

## 使用方法

ptracer是否开启受编译宏ENABLE_PTRACER控制，使用build_and_pack_run.sh构建脚本时默认开启。

启用ptracer时，会在/var/log/memfabric_hybrid下生成ptracer_\<pid\>.dat形式的文件用于保存打点数据。

## 输出样例

以下是一次测试中ptracer输出的一个周期内的打点数据示例。

```text
TIME                   NAME                                    BEGIN          GOOD_END       BAD_END        ON_FLY         P50(us)        P99(us)        P999(us)       AVG(us)        MAX(us)
2026-01-05 14:57:03    TP_HYBM_RDMA_LH_TO_GD                   117            117            0              0              120.830        8127.460       15984.310      713.658        16193.220
2026-01-05 14:57:03    TP_MMC_LOCAL_BATCH_PUT                  30             30             0              0              124.620        9218.540       16312.450      3353.871       16400.930
2026-01-05 14:57:03    TP_ACC_SEND_ALLOC                       2              2              0              0              564.440        598.120        602.310        583.605        602.770
```

## 字段描述

| keyword   | description   |
|-----------|---------------|
| TIME      | 时间            |
| NAME      | Tracepoint ID |
| BEGIN     | 调用次数          |
| GOOD_END  | 执行成功次数        |
| BAD_END   | 执行失败次数        |
| ON_FLY    | 执行中的数量        |
| P50(us)   | P50 分位耗时（微秒）  |
| P99(us)   | P99 分位耗时（微秒）  |
| P999(us)  | P999 分位耗时（微秒） |
| AVG(us)   | 平均耗时（微秒）      |
| MAX(us)   | 最高耗时（微秒）      |
| TOTAL(us) | 该周期累计耗时（微秒）   |

> `*_SIZE` / `*_COUNT` 这类不耗时、只记量的埋点，`AVG` / `MAX` / `TOTAL` 列里存的分别是
> 平均字节数 / 单次最大字节数 / 累计字节数（`*_COUNT` 则为条数），列名沿用了耗时列。

## 分析打点数据

统计结果按整点小时输出 `batch_read`、`batch_write` 两个数据面埋点，每个埋点每小时一行：

```text
file    : /var/log/memfabric_hybrid/ptracer_12345.dat
source  : 1 file(s), 1 snapshot(s), period 10.0s
traffic : 2026-10-17 06:27:06 -> 2026-10-17 06:27:16
window  : 3600s per row, starting on the whole hour

WINDOW                    TRACEPOINT         SIZE     ELAPSED       AVG BW       MIN BW       MAX BW
----------------------------------------------------------------------------------------------------
2026-10-17 06:00-07:00    batch_read     8.00 MiB      507 us   16.56 GB/s   16.56 GB/s   16.56 GB/s
2026-10-17 06:00-07:00    batch_write    8.00 MiB      454 us   18.49 GB/s   18.49 GB/s   18.49 GB/s
```

| 列 | 含义 |
|----|------|
| WINDOW | 整点小时窗口 `[HH:00, HH+1:00)`，dump 周期按其所覆盖的时间段归入对应小时 |
| TRACEPOINT | 固定为 `batch_read` 或 `batch_write` |
| SIZE | 该小时累计搬运字节数（1024 进制） |
| ELAPSED | 该小时累计**实际拷贝耗时**，即 latency 埋点 `TOTAL(us)` 之和 |
| AVG / MIN / MAX BW | 该小时内逐周期带宽的均值 / 最小 / 最大 |

流量起止时间不在表格里，打印在表格上方，且只认**真的报出字节**的周期。

## 指标口径

### 流量起止时间

dump 每行的时间戳是**写出该行那一刻**的墙上时间，行内数据是**相对上一次 dump 的增量**
（`ptracer_tracepoint.h` 的 `ToPeriodString()` 用当前累计值减去上次累计值）。因此：

```text
一行时间戳 T  =>  它统计的是 [上一次 dump, T) 这个区间
                  T 是周期终点，不是起点
```

采集器在 workload 前后都会持续 dump，所以文件开头和结尾往往是一串 0 字节周期。工具计算
`traffic` 时只认报出非零字节的那一行：起始取该周期的起点，结束取最后一个有流量周期的时间戳。
精度上限就是打点周期，dump 里没有比"某个周期内出现过字节"更细的信息。

### 带宽

```text
SIZE            = Σ 该小时 <name>_SIZE.TOTAL        （字节）
ELAPSED         = Σ 该小时 <name>.TOTAL(us)         （实际拷贝耗时）
每周期带宽       = 周期字节 / 周期耗时               （byte/us 数值上等于 MB/s）
AVG / MIN / MAX = 该小时内各有效周期带宽的算术均值 / 最小 / 最大
```

- 分母是 op **真正执行的时间**，不是墙上时间：空闲周期不参与统计，调用方在算别的事情的时间
  也不计入，因此这个数反映"搬运时的实际速率"，不会被空闲期稀释。
- `batch_read` / `batch_write` 分别对应 `TP_TRANS_BATCH_READ` + `TP_TRANS_BATCH_READ_SIZE`、
  `TP_TRANS_BATCH_WRITE` + `TP_TRANS_BATCH_WRITE_SIZE`，两者缺一不可：缺字节计数那一半没有
  可统计的量，缺耗时那一半时 `ELAPSED` 为 0、带宽显示 `--`。
