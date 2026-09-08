# mfcli 使用指南

`mfcli` 是 MemFabric Hybrid 提供的命令行工具。

## 使用前准备

安装 MemFabric Hybrid wheel：

```bash
pip3 install memfabric_hybrid-*.whl
```

使用 AICPU 命令前，请确认已加载 CANN 环境，或设置正确的 CANN 安装路径：

```bash
export ASCEND_HOME_PATH=/path/to/cann
```

## 查看版本

显示 MemFabric Hybrid 的版本：

```bash
mfcli version
```

## 安装 AICPU 算子

```bash
mfcli kernel install
```

如果安装后的算子需要供系统中的所有用户使用，请执行：

```bash
mfcli kernel install --install-for-all
```

## 查看 AICPU 算子状态

查看当前 CANN 环境中的 AICPU 算子安装状态和相关路径：

```bash
mfcli kernel info
```

## 查看 AICPU 算子版本

显示当前 CANN 环境中已安装的 AICPU 算子版本：

```bash
mfcli kernel version
```

## 卸载 AICPU 算子

```bash
mfcli kernel uninstall
```

## acc_offload 扩展库

NPU 类型的 wheel 包内置 acc_offload 算子源码，安装编译为扩展库 `libmf_hybm_accoffload.so` 后，`memfabric_hybrid.offload` 的稀疏拷贝等能力才可用。

`mfcli kernel install` 会在安装 AICPU 算子的同时编译安装该扩展库；环境不支持时（如 910B 芯片）自动跳过，不影响 AICPU 算子安装。

如果环境无法自动检测 SoC 类型，请通过 `--soc-version` 显式指定：

```bash
mfcli kernel install --soc-version A3
```

取值：`A2` / `A3` / `A5`。不指定时自动检测 SoC 类型。

通过 `mfcli kernel info` 查看扩展库的安装状态和安装路径。

`mfcli kernel uninstall` 会将扩展库还原到未安装状态。

## 扫描 NUMA 空闲内存

扫描所有 NUMA 节点：

```bash
mfcli mem scan
```

扫描指定 NUMA 节点：

```bash
mfcli mem scan --node 0
```

指定最小内存段大小和扫描线程数：

```bash
mfcli mem scan --min-mb 1024 --workers 8
```

## 查看帮助

```bash
mfcli --help
```

## 常见问题

### 提示未设置 `ASCEND_HOME_PATH`

设置正确的 CANN 安装路径后重新执行命令：

```bash
export ASCEND_HOME_PATH=/path/to/cann
```

### 提示 wheel 中没有 AICPU 算子

当前安装的 wheel 不包含 AICPU 算子，请安装包含 AICPU 算子的 wheel 后重试。

### 提示权限不足

请使用对 CANN 安装目录具有读写权限的用户执行命令。

### acc_offload 扩展库未安装

请执行 `mfcli kernel install`。若环境不支持（如 910B 芯片、CANN 或 torch/torch_npu 未安装），请按安装日志提示修正环境后重新执行。
