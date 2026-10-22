# mfcli工具 benchmark命令介绍

本 benchmark 基于 smem_bm 层的 Python 接口，测试 NPU 与共享内存池的读写性能。
本样例需要在 NPU 环境下编译运行。

首先，请在环境上提前安装 NPU 固件驱动和 CANN 包
（[环境安装参考链接](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0000.html)）。
固件驱动可参考 [社区版 HDK 下载链接](https://www.hiascend.com/hardware/firmware-drivers/community)。

安装完成后需要配置 CANN 环境变量
（[参考安装 Toolkit 开发套件包的第三步配置环境变量](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0008.html?Mode=PmIns&OS=Ubuntu&Software=cannToolKit)）。
两台机器需要安装匹配的 torch/torch_npu，并配置所选协议的设备网络。
使用 device_urma 时，还需按环境执行 `mfcli kernel install` 安装所需算子。

## 编译

打包安装时同源码一起编译，在源码根目录执行：

```bash
bash script/build_and_pack_run.sh --build_mode RELEASE --build_python ON --xpu_type NPU
```

运行 benchmark 无需打开 `--build_test`，默认 OFF 即可。

安装 whl：

```bash
pip install --force-reinstall output/memfabric_hybrid/wheel/mem*.whl
```

两台机器使用相同版本的源码和 whl；更新源码后重新编译、安装。

## benchmark 命令详解

命令格式为 `mfcli benchmark [read|write] [options]`，方括号表示可选参数。
操作类型默认为 read；`trans` 暂不支持。其余参数通过 `--参数名` 指定：

| 可选参数 | 默认值 | 说明 |
|----------|--------|------|
| `--framework` | `vllm` | 框架类型，可选 vllm、sglang |
| `--model` | `glm5.2` | 模型名称，可选 glm5.2、dsv4 |
| `--protocol` | `auto` | 根据本机机型选择传输协议；显式指定协议可覆盖，支持类型见下文 |
| `--ip` | `tcp://127.0.0.1:12050` | 配置存储地址；跨机测试两端都填写 write 机器的地址 |
| `--nic`（兼容 `--rdma`） | `tcp://0.0.0.0:10005` | 本机传输 NIC 地址；host_rdma 需指定本机实际 NIC，与 `--ip` 不同 |
| `--copysize` | `2097152` | 每个 block 的字节数，即 2 MiB；配对测试要求正数且为 4096 的倍数 |
| `--batchsize` | `1` | 每次批量 copy 的 block 数，必须为正数 |
| `--copy-count` | `1000` | 计时 copy 次数，必须为正数 |
| `--device` | `0` | 起始虚拟 device ID，由 ASCEND_RT_VISIBLE_DEVICES 映射到物理卡 |
| `--local-rank-size` | 可见 NPU 数 | 当前命令启动的 NPU 进程数 |
| `--world-size` | 自动推导 | 回环 IP 默认取 local rank 数，其他 IP 默认取两倍；两倍布局执行 write/read 配对 |
| `--rank-start` | 自动推导 | write 和同操作测试默认从 0 开始，配对 read 默认从 local rank 数开始 |
| `--peer-mem-type` | `host` | 池类型，可选 host（DRAM）、device（HBM） |
| `--local-hbm-size` | `0` | 每个 rank 贡献的 HBM 字节数；使用 HBM 池需至少容纳整个 batch |
| `--sync-port` | `--ip` 的端口加 1 | 配对测试同步端口，两端需一致，且与配置存储端口不同 |
| `--sync-timeout` | `3600` | 配对测试每个同步阶段的等待超时秒数，即一小时 |
| `--warmup-count` | `10` | 配对测试不计时的预热次数，可以为 0 |

`framework` 和 `model` 当前仅记录配置，不会启动推理框架或模型负载。
省略 `--protocol` 或指定 `--protocol auto` 时，按本机机型选择协议：

| 机型 | 自动选择协议 |
|------|--------------|
| A2 | `device_rdma` |
| A3 | `sdma` |
| A5 | `device_urma` |

仅通过 Python ACL 的 `acl.get_soc_name()` 获取完整 SoC 名称，
识别明确的 910B、910_93、950 或 910_95 型号；
通用 `Ascend910`、未知机型或无法检测时返回非零，提示通过 `--protocol` 手动指定。
启动日志记录检测到的 SoC、机型及实际协议，性能表和配对配置校验使用实际协议。
显式指定协议和帮助命令不进行机型检测。
自动选择不会检测或修复互联拓扑，也不会在失败后切换协议。

benchmark 默认启用 56 位 GVA，自动选择和显式指定协议均适用，
覆盖单机同操作及 write/read 配对流程。A5 的 DRAM 池可在预留大页不足时尝试普通页 `halMemAlloc`，
无需仅为该路径预先手动配置大页；分配仍受可用内存和驱动能力限制。

显式协议可选 sdma、device_rdma、device_urma、host_rdma、host_urma、host_tcp，
需匹配硬件、编译依赖和网络配置；host 协议在配对测试中只支持 DRAM 池。
SDMA 可用于具备 HCCS 互联且底层支持对应内存池访问的跨机拓扑；普通网络跨机需使用网络协议。
配对两端的协议、池类型、local rank 数、HBM 容量、copy 大小、批量、次数和预热次数必须一致。
每个 rank 贡献 4 GiB DRAM，`copysize * batchsize` 不得超过单 rank 的池容量。
通过 `export ASCEND_RT_VISIBLE_DEVICES=...` 设置各自机器的物理卡号，两端卡号可以不同，
但可见卡数须相同；默认使用全部可见卡，write/read 的 rank 自动一一配对。
未指定 `--ip` 或使用回环地址且未显式设置 world size 时，默认执行单机同操作测试，测完退出；
显式设置 `world_size == local_rank_size` 时也使用该流程。是否跨机取决于进程的实际部署位置。
完整帮助可通过 `mfcli benchmark --help` 查看。

## 跨机测试

机器 A 执行 write，机器 B 执行 read。以下使用自动协议，两端应为同代机型且自动结果相同；
也可在两端显式指定相同的 `--protocol`。自动与显式配置只要最终协议相同即可配对。
将 `10.0.0.1` 替换为 **机器 A 的可达 IP**，两端 `--ip` 都指向 A。
A 的 12050（配置存储）和 12051（默认同步）端口需对 B 可达。

机器 A：

```bash
export ASCEND_RT_VISIBLE_DEVICES=0
mfcli benchmark write --ip tcp://10.0.0.1:12050
```

机器 B：

```bash
export ASCEND_RT_VISIBLE_DEVICES=0
mfcli benchmark read --ip tcp://10.0.0.1:12050
```

默认使用 DRAM 池。A 测量 HBM → A 自己的 DRAM 池（D2RH），
完成写入和回读校验后立即输出写入性能表，再保持池存活等待读端。
看到 `Write benchmark complete` 后，在默认一小时等待时间内启动 B 即可。
B 测量 A 的 DRAM 池 → B 的 HBM（RH2D），完成读取和校验后输出读取性能表；
A 随后释放池并退出。表头显示所选协议，`WrongBytes(B)` 应为 0。

没有读端或校验失败时，配对流程返回非零；已输出的写入测量结果仍有效。
