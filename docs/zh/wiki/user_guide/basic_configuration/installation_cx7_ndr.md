# Host RDMA 网卡驱动与 NDR 安装

本文档介绍在 **openEuler 22.04 SP3** 操作系统、昇腾950标卡硬件条件下，为 MemFabric Hybrid 的 `host_rdma` 传输模式安装 **CX7 网卡（Mellanox OFED）驱动** 与 **NDR（`npu_peermem` 内核模块）** 的流程。

`host_rdma` 是 MemFabric 提供的一种传输模式，数据通过 Host 侧 RDMA 网卡在主机内存（或 NPU 注册的 peer memory）之间直接传输，而非使用 NPU 内部 SDMA 引擎或 DEVICE_RDMA（基于 HCCL 的 NPU 间直接传输）。该特性依赖 HDK 提供的 NDR 能力与 HCOM 提供的使用接口：NDR 能力集成在 `npu_peermem.ko` 中，而该二进制依赖 RDMA 网卡驱动。因此需要先安装网卡驱动，再编译并加载 NDR 内核模块。

> [!NOTE] 说明
>
> 本文档流程提取自《Ascend NPU RDMA Host 传输适配报告》，基于 MemFabric hybrid 1.3.0、Ascend CANN 9.1.0，安装与验证步骤均在 **openEuler 22.04 SP3** 上执行。其它发行版或版本需按实际内核与驱动包自行适配。文档仅覆盖网卡驱动与 NDR 的安装，功能与性能测试流程请参见对应测试文档。

---

## 一、 环境准备

| 项目 | 说明 |
|------|------|
| 操作系统 | openEuler 22.04 SP3（x86_64） |
| 内核 | 与目标 OS 版本配套，需安装对应 `kernel-devel` / `kernel-headers` 以编译 `npu_peermem.ko` |
| 硬件 | 昇腾950标卡 + CX7 网卡 |

首次使用时需要编译，导出编译与运行时环境变量（示例路径以实际安装位置为准）：

```bash
export CPATH=/usr/local/Ascend/cann-9.1.0/x86_64-linux/pkg_inc/base:$CPATH
export LD_LIBRARY_PATH=/usr/local/memfabric_hybrid/1.3.0/x86_64-linux/lib64:$LD_LIBRARY_PATH
```

若编译时报缺少 `infiniband/verbs.h`，需要安装 RDMA 核心开发库，例如 CX7 网卡的完整 OFED 驱动。

---

## 二、 安装 CX7 网卡驱动（Mellanox OFED）

到官网选择对应操作系统的对应版本，执行安装（以 OFED 24.10 为例）。

openEuler 22.04 SP3 不在 MLNX_OFED 的官方支持发行版列表内，因此安装命令需带 `--skip-distro-check` 跳过发行版校验，并用 `--add-kernel-support` 重新编译内核模块：

```bash
sudo ./mlnxofedinstall --skip-distro-check --add-kernel-support \
  --without-depcheck --skip-repo --force \
  --without-fw-update --without-ucx --without-fwctl
```

> [!IMPORTANT] 重要
>
> `ib_register_peer_memory_client` 必须导出，这是 NPU 内存通过 `npu_peermem.ko` 注册到 RDMA 子系统的前提条件。

### 安装验证

| 检查项 | 命令 | 期望值 |
|--------|------|--------|
| OFED 版本 | `ofed_info -s` | 例如 24.10-5.1.6.1 |
| 网卡驱动 | `ethtool -i <网卡名> \| grep -E 'driver'` | mlx5_core |
| RDMA 设备 | `ibv_devinfo` | 状态 Active |
| Peer memory 接口 | `grep -E "ib_register_peer_memory_client\|ib_peer_memory" /proc/kallsyms` | 存在对应符号 |

---

## 三、 编译并加载 NDR 内核模块（npu_peermem.ko）

### 1. 编译

```bash
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules \
  OFA_SYMVERS_DIR=/usr/src/ofa_kernel/default \
  OFA_SYMVERS_FILE=/usr/src/ofa_kernel/default/Module.symvers \
  HIAI_DKMS_DIR=/usr/src/davinci_ascend-1.0 \
  DRIVER_KERNEL_DIR=/usr/src/davinci_ascend-1.0 \
  KBUILD_EXTRA_SYMBOLS=/usr/src/ofa_kernel/default/Module.symvers \
  KBUILD_MODPOST_WARN=1
```

编译参数依赖如下路径：

| 参数 | 说明 |
|------|------|
| `OFA_SYMVERS_DIR` / `OFA_SYMVERS_FILE` | 指向 OFED 的 `Module.symvers` |
| `HIAI_DKMS_DIR` / `DRIVER_KERNEL_DIR` | 指向 Ascend DKMS 包目录 |
| `KBUILD_EXTRA_SYMBOLS` | 引入 OFED 的 `Module.symvers` 解析符号 |

### 2. 加载

```bash
insmod npu_peermem.ko
```

### 3. 验证

```bash
lsmod | grep peermem
# → npu_peermem  16384  0
```

**作用**：将 NPU 显存注册为 RDMA peer memory，使 Host RDMA 网卡能够通过物理地址（如 0x120080000000）直接读写 NPU 内存，实现零拷贝传输。

---

## 四、 网卡 IP 配置

host_rdma 场景下两张 RDMA 网卡均参与 RDMA 通信，需分别为它们配置同网段 IP，网卡名以 `ip link` 查询结果为准：

```bash
ip addr add <网卡1_IP>/24 dev <网卡1>
ip link set <网卡1> up
ip addr add <网卡2_IP>/24 dev <网卡2>
ip link set <网卡2> up
```

---

## 五、 安装验证

### 1. 网卡与 RDMA 状态确认

网卡名与 RDMA 设备名以实际查询结果为准：

```bash
# 速率确认
ethtool <网卡名> | grep Speed
# → Speed: 100000Mb/s (100Gbps)

# RDMA 设备确认
ibstat <RDMA设备名>

# RDMA 统计
rdma statistic show link <RDMA设备名>/1
# → 可见 rx_write_requests / rx_read_requests 等计数器活跃
```

### 2. 环回带宽测试

```bash
# 终端 1 (Server)
ib_write_bw -d <RDMA设备名> -p 20000

# 终端 2 (Client) — 8MB 报文，1024 发送深度
ib_write_bw -d <RDMA设备名> -p 20000 -s 8388608 --tx-depth 1024 localhost
```

- `-s 8388608`：使用 8MB 超大报文，减少 PCIe 交互次数。
- `--tx-depth 1024`：增加发送队列深度，压满网卡缓存。

---

## 六、 安装流程一览

1. 安装 OFED 驱动（`mlnxofedinstall`）
   - 验证：`ofed_info -s` → 例如 24.10-5.1.6.1
   - 验证：`ethtool -i <网卡名>` → mlx5_core
   - 验证：`ibv_devinfo` → Active
   - 验证：`/proc/kallsyms` 中 `ib_register_peer_memory_client` 存在

2. 编译 `npu_peermem.ko` 内核模块
   - 需要 `OFA_SYMVERS_DIR` 指向 OFED `Module.symvers`
   - 需要 `HIAI_DKMS_DIR` 指向 Ascend DKMS
   - 验证：`lsmod | grep peermem` → `npu_peermem` 已加载

3. 配置网卡 IP
   - 为两张 RDMA 网卡分别配置同网段 IP，参见「四、 网卡 IP 配置」

4. 导出编译/运行时环境变量
   - `CPATH`、`LD_LIBRARY_PATH`

---

## 七、 常见问题排查

| 问题 | 现象 | 解决方法 |
|------|------|----------|
| 缺少 `infiniband/verbs.h` | 编译报头文件找不到 | 安装 RDMA 开发库；或完整安装 OFED |
| OFED 不导出 peer memory 符号 | `grep /proc/kallsyms` 无输出 | 使用带 `--add-kernel-support` 的 OFED 安装方式，确认内核版本匹配 |
| Core dump 分析 | 进程崩溃无痕迹 | `echo ".../core.%p" > /proc/sys/kernel/core_pattern` |
