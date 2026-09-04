# KERNEL OPS 编译与安装

KERNEL OPS 以 `cann-hybm-compat.tar.gz` 交付。本页介绍算子包的独立编译和安装方法。
`mfcli` 的完整命令和参数说明见 [mfcli 使用指南](mfcli.md)。

## 编译算子包

编译前设置 CANN 根目录：

```bash
export ASCEND_HOME_PATH=/path/to/cann
```

执行独立编译脚本：

```bash
bash script/kernel/hybm/build.sh
```

编译环境需要满足以下条件：

- CANN 版本不低于 `9.1.0`。

可通过环境变量指定编译模式：

```bash
BUILD_MODE=DEBUG bash script/kernel/hybm/build.sh
```

默认使用 `RELEASE` 模式。

## 安装

```bash
mfcli kernel install
```

## 卸载

```bash
mfcli kernel uninstall
```
