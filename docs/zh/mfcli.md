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
