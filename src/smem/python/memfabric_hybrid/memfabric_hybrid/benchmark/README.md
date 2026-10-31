# mfcli benchmark

测量 NPU 与 BM 共享内存池之间的传输带宽。可见的卡两两配对，每次复制都访问**对端 rank** 的池，
所以测到的是真实的跨 rank 传输路径，而不是本地拷贝。需要在 NPU 环境下运行。

## 方向命名

一个方向写成“本地缓冲区 → 对端池”或“对端池 → 本地缓冲区”，`r` 表示对端 rank 的共享内存池：

- `D` = 本地 HBM，`H` = 本地主机内存；`rD` = 对端池的 HBM，`rH` = 对端池的 DRAM。
- 写方向 4 个：`D2rD`、`D2rH`、`H2rD`、`H2rH`；读方向 4 个：`rD2D`、`rH2D`、`rD2H`、`rH2H`。
- 一次默认跑全部 8 个方向。写方向由前一半卡发起，读方向由后一半卡发起，同一对卡轮流当发起端。

## 准备

安装 NPU 固件驱动和 CANN，并配置 CANN 环境变量
（[环境安装参考](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0000.html)、
[固件驱动下载](https://www.hiascend.com/hardware/firmware-drivers/community)）。
安装匹配的 torch/torch_npu，并配置所选协议的设备网络。

编译安装：

```bash
bash script/build_and_pack_run.sh --build_mode RELEASE --build_python ON --xpu_type NPU
bash output/memfabric_hybrid-*_linux_*.run --install-path="$HOME/mf-benchmark"
source "$HOME/mf-benchmark/memfabric_hybrid/set_env.sh"
```

使用 `device_urma` 时还需执行一次 `mfcli kernel install`。跨机测试时两台机器用相同版本的源码和安装包。

## 单机

```bash
mfcli benchmark                                        # 全部可见卡，8 个方向，16K-2M
mfcli benchmark --device 0,1                           # 只用 0、1 两张卡
mfcli benchmark --direction D2rD,rD2D --block-size 2M  # 指定方向和块大小
```

卡数为奇数时使用前 2⌊N/2⌋ 张，启动信息会列出未使用的卡。
未设置可见卡环境变量时，自动枚举优先累计 `npu-smi info -l` 的 `Chip Count`；A3 的 8 个双芯片板号对应 16 个设备。
卡按选择顺序分成前后两组，再按组内顺序配对；例如四个 rank 配成 `0↔2`、`1↔3`。
`--device` 使用逻辑卡号；例如 `ASCEND_RT_VISIBLE_DEVICES=2,5` 时，`--device 0,1` 对应物理卡 2、5。

## 跨机

两台机器执行**同一条命令**，`--ip` 填其中一台（记作 A）的地址：

```bash
mfcli benchmark --ip "tcp://<A_IP>:12060" --device 0,1
```

将 `<A_IP>` 替换为 A 机的可达地址，确保配置存储端口 12060 和同步端口 12061 对另一台可达。
A 机托管配置存储并发起写方向，另一台发起读方向。A 机最终打印完整结果，另一台只打印自己发起的方向。
两边的选卡数量、协议、方向、块大小、批量和复制/预热次数必须一致；物理卡号、本机 NIC 和日志开关可不同。
配置不一致会在初始化 BM 和加入池之前报 `configuration mismatch` 并退出。
先启动哪台都可以；另一台先启动且无法连接 A 的同步端口时，会提示 `Waiting for <IP>`。

## 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--direction` | `all` | `all`、`write`、`read`，或逗号列表如 `D2rD,rD2D`，不区分大小写 |
| `--device` | 全部可见卡 | 要使用的卡，如 `2,3` 或 `0-3,6`；卡号是 `ASCEND_RT_VISIBLE_DEVICES` 下的序号 |
| `--block-size` | `16K:2M` | 单值 `2M`、列表 `4K,64K,1M`，或倍增区间 `16K:2M`；需为 4K 的倍数，上限 1G |
| `--batch-size` | 自动 | 每次复制的块数；默认按 128M ÷ 块大小计算，最小 1 |
| `--copy-count` | `5` | 计时的批量复制次数，必须为正数 |
| `--warmup-count` | `1` | 不计时的预热次数，可以为 0 |
| `--protocol` | `auto` | 按机型自动选择：A2 为 `device_rdma`，A3 为 `sdma`，A5 为 `device_urma`。也可显式指定 `sdma`、`device_rdma`、`device_urma`、`host_rdma`、`host_urma`、`host_tcp` |
| `--ip` | `tcp://127.0.0.1:12050` | 配置存储地址，跨机时填 A 机地址；端口 +1 用于同步 |
| `--nic`（兼容 `--rdma`） | `tcp://0.0.0.0:10005` | 本机传输 NIC 地址；host 协议跨机时需填本机可达的 NIC IP，与两端相同的 `--ip` 不同 |
| `--group` | 自动 | 一般不需要。默认按 `--ip` 是否属于本机判断；同一台机器起两条命令时用 `0`/`1` 区分 |
| `--log` | 关闭 | 打印底层日志和每个 rank 的明细表 |

## 读结果

下面为表格格式示例，数值不代表特定硬件的性能：

```text
| Block | D2rD(GB/s) | D2rH(GB/s) | ... |
| 2M    | 23.776     | 20.727     | ... |
```

- 每格是同一方向、同一块大小下**各对卡带宽的算术平均值**，不是整机合计。1 GB 按 10^9 字节计算。
- 计时窗口只包含 `--copy-count` 次批量复制；准备数据、预热、同步和校验都不计时。
- 每个方向都会校验数据，有错误字节就判失败。退出码 0 表示全部通过，非 0 表示有失败，失败原因见 ERROR 日志。
- 预热后会重置目标或接收缓冲区，避免预热数据掩盖正式复制未生效的问题；写方向通过回读对端数据校验，读方向直接校验接收数据。

## 注意

- host 协议（`host_rdma`、`host_urma`、`host_tcp`）不能访问对端 HBM，只能选 `*2rH` 和 `rH2*` 方向。
- 不加 `--log` 时只打印 ERROR 及以上级别的底层日志。`--log` 配合默认的 8 方向 16K-2M 会产生很大的日志量，建议只配合单个方向和单个块大小使用。
- 默认端口 12050 被占用时，用 `--ip` 换一个端口。
- 自动协议选择仅依据机型，不会检查互联拓扑或在失败后切换协议；无法识别机型时需显式指定 `--protocol`。
- 每个 rank 贡献 4 GiB DRAM，HBM 池容量按测试数据量自动推导；`块大小 × batch` 不得超过 4 GiB。
