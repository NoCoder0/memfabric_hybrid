# 硬件支持

本文按产品系列和具体机型列出 MemFabric Hybrid 支持的传输协议。

MemCache Prefix Cache 池化特性的配套硬件与 MemFabric Hybrid 一致。

⭐ 表示推荐协议。

本文列出的机型均支持 `host_rdma`、`host_shm` 和 `host_tcp`；其中 `host_rdma` 需要 CX 网卡，`host_shm` 只支持单机访问。

以下主要介绍昇腾 Device 侧网络的支持情况。

## A2

### 传输协议支持

| 机型 | 支持协议 | 访问范围与限制 |
| --- | --- | --- |
| Atlas 800T A2 /<br>Atlas 800I A2 | `device_rdma` ⭐ | — |
| Atlas 200T A2 Box16 | `device_rdma` ⭐ | — |

### 配套底软

#### For device_rdma

| 配套要求 | 要求 |
| --- | --- |
| 灵衢 L1 LingQu Computing Network | 不依赖 |
| HDK | >=25.5.1 |
| HDK-firmware | 不涉及 |
| PageSize | 4K；64K 自 v1.3.1 起默认支持 |
| OS kernel | 不涉及 |
| BIOS | 不涉及 |
| 集群规模 | 16 节点 |

## A3

### 传输协议支持

| 机型 | 支持协议 | 访问范围与限制 |
| --- | --- | --- |
| Atlas 900 A3 SuperPoD /<br>Atlas 800T A3 SuperPoD /<br>Atlas 800I A3 SuperPoD | `device_sdma` ⭐<br>`device_rdma` | `device_rdma`：需要全卡 RoCE 互联 |
| Atlas 800I A3 Server（背靠背） | `device_sdma` ⭐<br>`device_rdma` | `device_sdma`：只支持双机内访问<br>`device_rdma`：需要全卡 RoCE 互联 |

### 配套底软

#### For device_sdma

| 配套要求 | 要求 |
| --- | --- |
| 灵衢 L1 LingQu Computing Network | >=1.5.0，优选 1.5.1 |
| HDK | >=25.5.1 |
| HDK-firmware | >=7.8.0.6.201 [^1] |
| PageSize | 不涉及 |
| OS kernel | >=5.10.0 |
| BIOS | 参考内存容量 [^3] |
| 集群规模 | 超节点内 |

#### For device_rdma

| 配套要求 | 要求 |
| --- | --- |
| 灵衢 L1 LingQu Computing Network | 不依赖 |
| HDK | >=25.5.1 |
| HDK-firmware | 不涉及 |
| PageSize | 4K；64K 自 v1.3.1 起默认支持 |
| OS kernel | 不涉及 |
| BIOS | 不涉及 |
| 集群规模 | 8 节点 |

## A5

### 传输协议支持

| 机型 | 支持协议 | 访问范围与限制 |
| --- | --- | --- |
| Atlas 350（标卡） | `host_rdma` ⭐ | `host_rdma`：自 v1.3.0 起支持，参见 [Host RDMA 网卡驱动与 NDR 安装指南](installation_cx7_ndr.md) |
| Atlas 850 /<br>Atlas 850E（Server） | `device_urma` ⭐<br>`device_rdma` | UBoE：自 v1.3.0 起支持。v1.3 版本选择 `device_uboe`；v1.5 及以上版本选择 `device_urma` 并设置 [`MF_DEVICE_URMA_ENABLE_UBOE`](environment_variables.md)`=1`<br>`device_rdma`：自 v1.3.0 起支持，需要 1825 RoCE 组网 |
| Atlas 650 /<br>Atlas 650E（FM16） | `device_urma` ⭐<br>`device_rdma` | `device_urma`：自 v1.3.0 起支持，未启用 UBoE 时只支持双机内访问；启用 UBoE 后不再限制双机内访问<br>UBoE：v1.3 版本选择 `device_uboe`；v1.5 及以上版本选择 `device_urma` 并设置 [`MF_DEVICE_URMA_ENABLE_UBOE`](environment_variables.md)`=1`<br>`device_rdma`：自 v1.3.0 起支持，需要 1825 RoCE 组网 |
| Atlas 950 SuperPoD | `device_urma` ⭐ | `device_urma`：自 v1.3.0 起支持 |
| A5 UBX | `device_rdma` ⭐ | `device_rdma`：自 v1.3.0 起支持，需要 1825 RoCE 组网，参见 [1825 网卡端口桥配置](../../FAQ/FAQ.md#rdma-copy-port-bridge) |
| A5 PC16 | `device_urma` ⭐ | `device_urma`：自 v1.3.0 起支持，通过 UBG 网络传输，已根据 PC16 机型自动适配。 |

### 配套底软

当前兼容性基线提供的 A5 的底软版本仅供参考，其余机型的要求尚待补充。

#### Atlas 350（标卡）

| 配套要求     | 要求    |
| ------------ | ------- |
| HDK          | >= 26.2 |
| HDK-firmware | 不涉及  |
| PageSize     | 4K；64K |
| OS kernel    | 不涉及  |
| BIOS         | 不涉及  |
| 集群规模     | /       |

#### Atlas 850(E) （Server）

| 配套要求     | 要求    |
| ------------ | ------- |
| HDK          | >= 25.6 |
| HDK-firmware | 不涉及  |
| PageSize     | 4K；64K |
| OS kernel    | 不涉及  |
| BIOS         | 不涉及  |
| 集群规模     | /       |

#### Atlas 950 SuperPoD

| 配套要求     | 要求     |
| ------------ | -------- |
| HDK          | >= 25.1  |
| HDK-firmware | 不涉及   |
| PageSize     | 4K；64K  |
| OS kernel    | 不涉及   |
| BIOS         | 不涉及   |
| 集群规模     | 超节点内 |

## 底软检查方法

| 配套要求 | 版本链接 | 检查方法 |
| --- | --- | --- |
| HDK | [HDK 版本][hdk-version] | `npu-smi info` |
| HDK-firmware | - | [^2] |
| PageSize | - | `getconf PAGESIZE` |
| OS kernel | [兼容性查询][os-compatibility] | `uname -r` |
| BIOS | [升级参考][bios-upgrade]（仅参考，以指导书为准） | `dmidecode -t bios` |
| 集群规模 | - | - |

[hdk-version]: https://support.huawei.com/enterprise/zh/undefined/ascend-hdk-pid-252764743/software
[os-compatibility]: https://www.hiascend.com/hardware/compatibility
[bios-upgrade]: https://gitcode.com/Ascend/memcache/wiki/MemCache扩大A3可用内存空间.md

[^1]: 一般正常安装 HDK 后，此配套是准确的；如果 HDK 版本、灵衢交换机版本均正确，
    但出现 `sdma move comperr`，可检查此配套。
[^2]: 检查命令：
    `/usr/local/Ascend/driver/tools/upgrade-tool --device_index -1 --component -1 --version`
[^3]: 典型内存配置：1 TB（必须升级）；1.5 TB/2 TB（建议升级扩展更大空间）；
    3 TB（无需升级）。
