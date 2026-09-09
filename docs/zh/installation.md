# 软件安装

本文档介绍 MemFabric Hybrid 的安装方法，支持 **Python API** 和 **C API** 两种使用方式。请根据实际开发需求选择对应的安装路径。

---

## 一、 使用 Python API

推荐通过 `pip` 进行安装，支持在线安装与离线安装两种方式。

### 1. 在线安装

```bash
pip install memfabric_hybrid
```

**指定版本安装**
如需安装特定版本，请使用 == 指定版本号。

```bash
pip install memfabric_hybrid==1.0.0
```

### 2. 离线安装

在无网络环境中，需预先下载对应平台架构和 Python 版本的 .whl 包。

1. **下载 whl 包**
    从 [PyPI](https://pypi.org/project/memfabric-hybrid/?spm=a2ty_o01.29997173.0.0.36c455fbq3MLaA#files) 下载对应的 `.whl` 文件。
    <!-- 文件名示例：memfabric_hybrid-1.1.0-cp311-cp311-manylinux_2_27_aarch64.whl -->

2. **执行安装**
    将 .whl 包上传至目标环境，执行以下命令进行离线安装。

    ```bash
    pip install --no-index memfabric_hybrid-*.whl
    ```

### 3. AICPU算子安装

A2(device_rdma走hcomm)和A5需要安装一个AICPU算子。

安装 MemFabric Hybrid whl包后，执行 `mfcli kernel install` 完成部署。

详细说明见 [安装 HYBM AICPU OPS](installation_kernel.md)。

### 4. acc_offload扩展库安装

NPU 类型的 wheel 包内置 acc_offload 算子源码，安装编译为扩展库 `libmf_hybm_accoffload.so` 后，`memfabric_hybrid.offload` 的稀疏拷贝等能力才可用。

`.run` 包安装时不再编译该扩展库。安装 MemFabric Hybrid whl包后，执行 `mfcli kernel install` 完成编译安装；环境不支持时（如 910B 芯片）自动跳过。

通过 `mfcli kernel info` 查看扩展库的安装状态和安装路径。

> [!NOTE] 说明
>
> **纯 run 包（不含 whl）的场景不再支持 acc_offload 扩展库**，请安装 wheel 包后使用 `mfcli kernel install`。

---

## 二、 使用 C API

### 1. 编译环境要求

| 组件 | 建议版本/要求 |
| -- | -- |
| **OS** | Ubuntu 22.04 LTS 或更高版本；openEuler 24.03 LTS（aarch64）亦经验证支持 |
| **CMake** | 3.12.x 或更高（3.20.x 及以上推荐） |
| **GCC** | 11.4 或更高 |
| **pybind11** | 2.10.3 (仅编译 Python 绑定时需要) |
| **Make/Ninja** | Make 4.3+ 或 Ninja 1.10.1+ |

### 2. 获取源码

```bash
git clone https://gitcode.com/Ascend/memfabric_hybrid
cd memfabric_hybrid
```

`git checkout` 到稳定发布分支。建议根据[分支策略](https://gitcode.com/Ascend/memfabric_hybrid/wiki/%E5%88%86%E6%94%AF%E7%AD%96%E7%95%A5.md)选择正确分支。

```bash
git checkout release/1.1  # 这里以release/1.1为例
git clean -xdf
git reset --hard
```

### 3. 编译构建

使用封装脚本进行一键编译。脚本会自动处理依赖并生成安装包。

``` bash
bash script/build_and_pack_run.sh
```

`build_and_pack_run.sh` 脚本参数详解:

| 参数 | 选项 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `--build_mode` | `RELEASE` / `DEBUG` | `RELEASE` | 编译模式 |
| `--build_open_abi` | `ON` / `OFF` | `OFF` | 是否使用新 C++ ABI（`_GLIBCXX_USE_CXX11_ABI`）。GCC 12.x / 新 libstdc++ 发行版（如 openEuler 24.03）建议设为 `ON`，详见下方说明。 |
| `--build_python` | `ON` / `OFF` | `ON` | 是否编译 Python Wheel 包 |
| `--xpu_type` | `NPU` / `GPU` / `NONE` | `NPU` | 目标异构设备类型：<br>- `NPU`: 适配昇腾 CANN 环境<br>- `GPU`: 适配 CUDA 环境<br>- `NONE`: 无卡纯 CPU 环境 |
| `--build_test` | `ON` / `OFF` | `OFF` | 是否编译测试工具和样例代码。<br>**注意**：设为 `ON` 时需先执行 `git submodule update --recursive --init` 拉取第三方库。 |
| `--build_hcom` | `ON` / `OFF` | `OFF` | 是否编译 HCOM 通信库。<br>若数据传输类型涉及 `HOST_RDMA`, `HOST_TCP`, `HOST_URMA`，需设为 `ON`。 |
| `--build_hcom_rdma` | `ON` / `OFF` | `ON` | (仅当 `build_hcom=ON` 有效) 是否启用 RDMA 支持。<br>需先执行 `apt install libibverbs-dev`。 |
| `--build_hcom_ub` | `ON` / `OFF` | `OFF` | (仅当 `build_hcom=ON` 有效) 是否启用 UB (URMA) 支持。<br>RDMA 和 UB 可同时开启。|
| `--build_etcd_backend` | `ON` / `OFF` | `OFF` | 是否编译 etcd 后端 SO。<br>启用后支持 `etcd://` 配置存储 URL，用于 etcd HA 模式下的分布式选主和元数据存储。 |

> [!NOTE] 说明
>
> **设备端 DEVICE_RDMA 传输（基于 HCOMM）：** 需要 CANN >= 9.1。运行时根据 CANN 版本自动选择传输层实现：
>
> - CANN >= 9.1 → `DeviceRdmaHcommTransportManager`（AICPU kernel + HCOMM，支持 A2/A5 RoCE 跨节点传输）
> - CANN < 9.1  → native device RDMA（兼容旧 CANN 版本）
>
> [环境安装参考链接](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/latest/softwareinst/instg/instg_0000.html)
>
> [参考安装Toolkit开发套件包的第三步配置环境变量](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/latest/softwareinst/instg/instg_0000.html)

<!-- -->

> [!NOTE] C++ ABI 兼容性
>
> - 默认 `--build_open_abi OFF` 使用旧 C++ ABI（`_GLIBCXX_USE_CXX11_ABI=0`），用于与旧环境二进制兼容。
> - 在 GCC 12.x 或较新 libstdc++ 的发行版（如 **openEuler 24.03 LTS**）上，系统库与预置依赖通常使用新 C++ ABI，沿用旧 ABI 可能导致链接/运行期符号不兼容；此时请显式指定 `--build_open_abi ON`：
>
> ```bash
> bash script/build_and_pack_run.sh --build_open_abi ON
> ```

### 4. 安装

编译成功后，生成的安装包位于 `output/memfabric_hybrid-${version}_${os}_${arch}${xpu_suffix}.run`。

> 其中 xpu_suffix 为空（NPU）/_cpu（XPU_TYPE=NONE）/_gpu（XPU_TYPE=GPU）

运行以下命令默认安装至 `/usr/local/`。

```bash
bash memfabric_hybrid-*_*_*.run  # optional: --install-path=${your path}
source /usr/local/memfabric_hybrid/set_env.sh

# 查看 C API 版本信息
cat /usr/local/memfabric_hybrid/latest/version.info
```

### 5. AICPU算子安装

A2(device_rdma走hcomm)和A5需要安装一个AICPU算子。

安装 MemFabric Hybrid whl包后，执行 `mfcli kernel install` 完成部署。

详细说明见 [安装 HYBM AICPU OPS](installation_kernel.md)。

> **A2 环境使用 DEVICE_RDMA：** A2（Ascend 910B3）上使用自定义 AICPU kernel 需先关闭驱动验签：
>
> ```bash
> npu-smi set -t custom-op-secverify-enable -i 0 -d 1
> npu-smi set -t custom-op-secverify-mode -i 0 -d 0
> ```
>
> > 说明：A5（Ascend 950）使用 DEVICE_URMA 同样需关闭驱动验签（自定义 AICPU kernel 验签要求 A2/A5 一致）。
> **A2 环境使用 DRAM 池化：** 需要根据每台机器池化内存的大小来配置大页内存，否则初始化失败
>
> 检查是否配置大页:
>
> ```bash
> grep Huge /proc/meminfo
> ```
>
> 配置大页内存，以配置1024个大页为例:
>
> ```bash
> echo 1024 > /proc/sys/vm/nr_hugepages
> ```

---
**卸载 Run 包**

若为自定义路径，请替换为对应路径下的 uninstall.sh

```bash
bash /usr/local/memfabric_hybrid/latest/uninstall.sh
```

---

## 三、 包权限说明

Run 包安装后，动态库（`.so` 文件）权限默认为 `440`（属主和属组可读，其他用户无权限）。若以非安装用户运行程序，需将部署用户加入安装时指定的属组中。

```bash
# 查看 so 文件的属组
ls -l /usr/local/memfabric_hybrid/latest/aarch64-linux/lib64/*.so

# 将部署用户加入该属组
sudo usermod -a -G <group_name> <deploy_user>

# 退出当前会话后重新登录使组生效
```
