# 单机多卡 Memory Pool 快速开始

本文介绍完成 MemFabric Hybrid 安装后，如何从零开始运行
`examples/memory_pool/02_scale_out/01_single_node_multi_device_dram` 中的单机双卡示例。

示例会在一台机器上启动两个进程，分别使用逻辑设备 0 和 1，创建由两个 rank 组成的 DRAM 内存池，
并依次验证两个 rank 之间的双向数据复制。

## 前置条件

- 已按照[软件安装](../../installation.md)完成 MemFabric Hybrid Python 包和运行环境安装。
- 机器上至少有两张可用 NPU，逻辑设备编号为 0 和 1。
- 当前机型支持示例使用的 `device_rdma` 协议，详见[硬件支持](../硬件支持.md)。
- 已安装 PyTorch，并可以在当前 Python 环境中导入 `torch` 和 `memfabric_hybrid`。
- 本机端口 8572 和 10005 未被其他进程占用。

如果当前机型和传输方式需要部署 AICPU 算子，请先参考
[KERNEL OPS 编译与安装](../../installation_kernel.md)完成安装。

## 1. 进入代码仓

如果本机还没有代码仓，先获取源码；已有代码仓时直接进入仓库根目录。

```bash
git clone https://gitcode.com/Ascend/memfabric_hybrid.git
cd memfabric_hybrid
```

## 2. 加载运行环境

根据实际安装路径加载 CANN 和 MemFabric Hybrid 环境变量。以下命令使用默认安装路径：

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh

# 使用 run 包安装时加载；仅使用 wheel 安装且环境已经生效时可跳过。
source /usr/local/memfabric_hybrid/set_env.sh

export ASCEND_RT_VISIBLE_DEVICES=0,1
```

确认设备和 Python 包可用：

```bash
npu-smi info
python3 -c "import torch; import memfabric_hybrid; print('Python dependencies are ready')"
```

## 3. 了解示例配置

示例文件 `01_single_node_multi_device_dram.py` 已包含运行所需的最小配置：

- `WORLD_SIZE=2`：启动两个 rank。
- rank 0 使用设备 0，rank 1 使用设备 1。
- `STORE_URL=tcp://127.0.0.1:8572`：rank 0 在本机启动 ConfigStore，无需单独启动服务。
- 每个 rank 申请 1 GiB 本地 DRAM，测试数据大小为 4 MiB。
- 数据传输协议为 `bm.BmDataOpType.DEVICE_RDMA`。

如果设备编号或端口与当前环境冲突，请先修改示例顶部的对应常量。

## 4. 运行示例

在仓库根目录执行：

```bash
cd examples/memory_pool/02_scale_out/01_single_node_multi_device_dram
python3 01_single_node_multi_device_dram.py
```

示例成功时会依次打印两个 rank 的初始化信息、双向复制结果，并以如下信息结束：

```text
[rank 1] (2/4) Phase A OK (r0→r1)
[rank 0] (3/4) Phase B OK (r1→r0)
(4/4) 01_single_node_multi_device_dram: all ranks OK
```

## 5. 继续运行批量复制示例

基础示例通过后，可以在同一目录运行批量数据复制版本：

```bash
python3 02_single_node_multi_device_dram_batch_data_copy.py
```

该示例使用 `BigMemory.copy_data_batch(...)` 替换单次复制接口，其余初始化和双 rank 流程相同。

## 常见问题

- 提示 Python 模块不存在：确认执行命令的 Python 环境与安装 MemFabric Hybrid、PyTorch 时使用的环境一致。
- 提示设备不可用：执行 `npu-smi info` 检查设备状态，并确认 `ASCEND_RT_VISIBLE_DEVICES` 包含两个设备。
- 提示端口占用：释放 8572 或 10005 端口，或修改示例中的 `STORE_URL` 和 `cfg.set_nic(...)` 端口。
- `bm.initialize` 或 `join` 失败：检查当前机型的协议支持、CANN/驱动环境及所需算子是否已正确安装。
- DRAM 内存池初始化失败：根据机型要求检查大页内存配置，相关说明见[软件安装](../../installation.md)。
