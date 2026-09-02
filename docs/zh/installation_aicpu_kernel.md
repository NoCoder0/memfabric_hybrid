# 安装 HYBM AICPU OPS

HYBM AICPU OPS 以 `cann-hybm-compat.tar.gz` 交付。有效的 CANN 环境下，主构建会生成 OPS，并在签名环境可用时完成验签，然后将同一份载荷同时放入 wheel 和主 run 包。

## 构建

```bash
bash script/build.sh

# 或同时生成主 run 包
bash script/build_and_pack_run.sh
```

构建 OPS 需要：

- `XPU_TYPE=NPU`；
- CANN 版本大于等于 `9.1.0`；
- 已设置 `ASCEND_HOME_PATH`；
- CANN 交叉编译器和头文件可用；
- 如需签名，`/opt/signaturst` 下的签名客户端、`client.toml` 和 `MF-CRL.crl` 可用。

如果 `/opt/signaturst` 目录不存在，构建流程会跳过验签并继续打包；目录存在时，缺少签名客户端、配置或 CRL 会导致签名失败。

CANN 版本低于 `9.1.0` 时，不支持构建或安装 HYBM AICPU OPS。构建脚本无法识别版本或检测到低版本时会跳过 OPS；安装器遇到相同情况会拒绝安装。

如果未检测到有效 CANN 环境，主构建会提示跳过 OPS，不会报错。此时仍可生成普通 wheel 或 run 包，但其中不包含 OPS 载荷，不能执行 OPS 安装。

构建成功后，OPS 产物位于：

```text
output/hybm/aicpu_kernel/
├── cann-hybm-compat.tar.gz
├── cann_hybm_kernel_version
├── install.sh
└── libcann_hybm_kernel.json
```

## 从主 run 包安装

主 run 包默认只安装 MemFabric Hybrid。需要同时安装 OPS 时增加 `--install-ops`：

```bash
./output/memfabric_hybrid-*.run --install-ops
```

为所有用户安装时：

```bash
./output/memfabric_hybrid-*.run --install-ops --install-for-all
```

如果该 run 包是在无 CANN 环境下构建的，使用 `--install-ops` 会明确提示包内没有 OPS 载荷。

## 从 wheel 安装

安装 wheel 后，Python 的脚本目录会生成 `mfcli` 命令：

```bash
pip3 install output/memfabric_hybrid/wheel/memfabric_hybrid-*.whl
mfcli aicpu install
```

其他可用命令：

```bash
mfcli aicpu info
mfcli aicpu uninstall
```

使用 `--install-ops` 安装的主 run 包会在自身卸载时检测 CANN 中的 tar、JSON、版本文件和加载配置；检测到 HYBM OPS 后先卸载 OPS，再卸载 MemFabric Hybrid 主程序和 wheel。

`info` 会展示以下文件的完整路径和存在状态：

- `${ASCEND_HOME_PATH}/conf/ascend_package_load.ini`；
- `${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/kernel/cann-hybm-compat.tar.gz`；
- `${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/config/libcann_hybm_kernel.json`。

同时会输出 `ascend_package_load.ini` 中 `cann-hybm-compat.tar.gz` 对应的 MemFabric Hybrid 配置块；尚未安装时显示 `not configured`。

覆盖已有 OPS 或为所有用户安装：

```bash
mfcli aicpu install --force
mfcli aicpu install --install-for-all
```

## 安装位置

安装器将载荷部署到 `${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/`，并更新 `${ASCEND_HOME_PATH}/conf/ascend_package_load.ini`。卸载时会恢复首次安装前创建的备份。

安装后可检查：

```bash
ls -l ${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/kernel/cann-hybm-compat.tar.gz
ls -l ${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/config/libcann_hybm_kernel.json
grep -A4 '^name:cann-hybm-compat.tar.gz$' ${ASCEND_HOME_PATH}/conf/ascend_package_load.ini
```

## 验签配置说明

自定义 AICPU kernel 的验签策略由运行环境决定。A2/A5 调试环境如需关闭验签，可使用对应驱动版本提供的 `npu-smi` 或 `drv_hlt_dsmi_test` 工具；生产环境不建议关闭验签，且工具与驱动包版本必须匹配。

使用 `npu-smi` 的环境可按设备执行：

```bash
npu-smi set -t custom-op-secverify-enable -i 0 -d 1
npu-smi set -t custom-op-secverify-mode -i 0 -d 0
```

常见失败处理：

- `ASCEND_HOME_PATH is not set`：先加载 CANN 环境或设置正确的 CANN 根目录；
- `target files already exist`：先执行 `mfcli aicpu uninstall`，或使用 `mfcli aicpu install --force`；
- `Permission denied`：使用对 CANN 安装目录有写权限的用户执行；
- 验签失败：确认包已由构建流程成功签名，并核对设备的自定义算子验签策略。
