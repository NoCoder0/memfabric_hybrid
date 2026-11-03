# Mooncake（MemFabric版）完整指南

本指南汇总 Mooncake 在昇腾 NPU 环境下使用基于MemFabric的Tent 传输编译、安装部署、运行验证与环境变量配置。

> 参考环境为昇腾 NPU、aarch64、CANN 9.x 和 Python 3.11/3.12，命令中的路径请按实际环境调整。构建选项和打包逻辑如有变化，以当前源码中的 `CMakeLists.txt` 和 `scripts/build_wheel.sh` 为准。

## 目录

- [一、安装与卸载](#一安装与卸载)
- [二、运行验证](#二运行验证)
- [三、关键环境变量](#三关键环境变量)
- [四、SGLang 配置 Mooncake 示例（PD 分离）](#四sglang-配置-mooncake-示例pd-分离)

---

## 一、安装与卸载

### 1.1 快速参考

| 阶段 | 主要命令 | 结果 |
|---|---|---|
| 编译 | `cmake --build build -j$(nproc)` | 在 `build/` 中生成库、二进制和 Python 扩展 |
| 打包 | `MEMFABRIC_BUILD=1 bash scripts/build_wheel.sh` | 在 `mooncake-wheel/dist/` 中生成 MemFabric 版 wheel |
| 安装 | `pip install mooncake-wheel/dist/mooncake_transfer_engine_memfabric-*.whl` | 安装到当前 Python 环境的 `site-packages/` |
| 卸载 | `pip uninstall mooncake-transfer-engine-memfabric` | 根据 wheel 的 `RECORD` 清理安装文件 |

### 1.2 获取代码与依赖

```bash
git clone https://github.com/kvcache-ai/Mooncake.git
cd Mooncake
git submodule update --init --recursive
bash scripts/ascend/dependencies_ascend_installation.sh
```

配置 CANN 环境。

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
```

依赖脚本会自动检测系统包管理器并安装编译依赖。已经安装依赖时可以跳过该脚本。脚本不会安装下列补充依赖，需要单独处理。

```bash
# CMake 配置阶段需要查找 CLI/CLI.hpp
apt-get install -y libcli11-dev

# MEMFABRIC_BUILD=1 打包阶段需要
apt-get install -y file patchelf
```

### 1.3 编译与主要产物

在源码根目录执行以下命令。

```bash
cmake -S . -B build -G Ninja \
    -DUSE_TENT=ON \
    -DUSE_MEMFABRIC=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_UNIT_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build -j$(nproc)
```

这些产物用于后续 wheel 打包。

| 产物 | 说明 |
|---|---|
| `mooncake-integration/engine.*.so` | Python 绑定（`mooncake.engine`） |
| `mooncake-integration/_store.*.so` | Store Python 绑定 |
| `mooncake-transfer-engine/src/libtransfer_engine.so` | 传输引擎（含 Tent + MemFabric 传输） |
| `mooncake-transfer-engine/tent/src/libtent_shared.so` | Tent 共享库 |
| `mooncake-transfer-engine/tent/src/metrics/libtent_metrics.so` | Tent metrics，`libtransfer_engine.so` 和 Tent Python 模块均依赖它 |
| `mooncake-transfer-engine/tent/src/python/tent.cpython-*.so` | Tent Python 模块 |
| `mooncake-common/src/libmooncake_common.so`、`mooncake-common/libasio.so` | 公共库 |
| `mooncake-store/src/libmooncake_store.so`、`mooncake_master`、`mooncake_client` | Store 库和二进制 |

### 1.4 wheel 打包与安装

#### 1.4.1 构建 MemFabric 版 wheel

在 Mooncake 源码根目录执行以下命令。

```bash
MEMFABRIC_BUILD=1 bash scripts/build_wheel.sh
```

`MEMFABRIC_BUILD=1` 会将 wheel 的发行包名设置为 `mooncake-transfer-engine-memfabric`，自动复制 `libtent_shared.so`、`libtent_metrics.so` 和 `tent.cpython-*.so`，并执行 MemFabric 所需的 `$ORIGIN` RPATH 和 auditwheel 处理。
wheel 输出到 `mooncake-wheel/dist/`。wheel 文件名会使用下划线形式的发行包名。

```bash
ls -lh mooncake-wheel/dist/mooncake_transfer_engine_memfabric-*.whl
```

#### 1.4.2 安装与卸载

使用 pip 安装刚生成的 wheel。

```bash
pip install mooncake-wheel/dist/mooncake_transfer_engine_memfabric-*.whl --force-reinstall
```

wheel 的包名为 `mooncake-transfer-engine-memfabric`，导入包名为 `mooncake`。pip 会将包安装到当前解释器对应的 `site-packages`，并生成包含 `RECORD` 的 `dist-info` 元数据。

运行 wheel 中的 NPU 组件仍需要匹配的 Ascend 驱动、CANN 和 MemFabric 运行时。厂商库不由该 wheel 提供。

需要卸载时执行以下命令。

```bash
pip uninstall mooncake-transfer-engine-memfabric
```

### 1.5 卸载自检

```bash
pip show mooncake-transfer-engine-memfabric 2>/dev/null
python3 -c "import mooncake; print(mooncake.__file__)"
```

`pip show` 无输出时，说明 pip 已不再记录该 wheel。如果仍能导入 `mooncake`，应继续检查输出路径，确认它是否来自 CANN 目录或其他 Python 环境中的副本。

### 1.6 常见问题

**Q1 安装 CLI11 后仍提示找不到开发文件**

此前的 `-NOTFOUND` 结果可能已写入 CMake 缓存。安装依赖后删除缓存并重新配置。

```bash
apt-get install -y libcli11-dev
rm -f build/CMakeCache.txt
```

**Q2 MemFabric 打包阶段提示缺少 file 或 patchelf**

```bash
apt-get install -y file patchelf
MEMFABRIC_BUILD=1 bash scripts/build_wheel.sh
```

**Q3 在仓库根目录的 dist 目录中找不到 wheel**

本文流程将 wheel 写入 `mooncake-wheel/dist/`。

```bash
find mooncake-wheel/dist -maxdepth 1 -name 'mooncake_transfer_engine_memfabric-*.whl' -print
```

---

## 二、运行验证

### 2.1 wheel 与模块检查

检查 wheel 是否包含三个 Tent 产物。

```bash
unzip -l mooncake-wheel/dist/mooncake_transfer_engine_memfabric-*.whl | grep -i tent
```

输出中应包含 `libtent_shared.so`、`libtent_metrics.so` 和 `tent.cpython-*.so`。Python 扩展文件名中的解释器版本和平台标签以实际构建结果为准。

安装后检查 Mooncake 本体、engine 扩展和 Tent 模块。

```bash
python3 -c "import mooncake; import mooncake.engine; print('mooncake OK', mooncake.__file__)"
python3 -c "from mooncake import tent; print('tent OK', tent.__file__)"
```

如果当前源码增加了顶层导入 shim，也可以按对应实现使用 `import tent`。导入方式应以实际包结构为准。

继续检查包内二进制和 pip 文件清单。

```bash
python3 -c "import mooncake, pathlib; print(list(pathlib.Path(mooncake.__file__).parent.glob('mooncake_*')))"
pip show -f mooncake-transfer-engine-memfabric
```

### 2.2 加载路径与动态库检查

CANN 环境可能通过 `PYTHONPATH` 或 `.pth` 文件引入另一份 `mooncake`。切换到源码目录之外检查实际导入位置。

```bash
cd /tmp
python3 -c "import mooncake; print(mooncake.__file__)"
```

预期结果应指向 wheel 所在的 `site-packages/mooncake/`，不应指向 `/usr/local/Ascend/cann-*/python/site-packages/mooncake` 中的旧副本。

继续检查 `libtransfer_engine.so` 的动态库解析结果。

```bash
python3 -c "import mooncake, pathlib, subprocess; d=pathlib.Path(mooncake.__file__).parent; print(subprocess.run(['ldd', str(d/'libtransfer_engine.so')], capture_output=True, text=True).stdout)"
```

重点检查 Tent 相关库是否从预期位置解析，以及输出中是否存在 `not found`。如果 wheel 已安装但仍导入 CANN 旧副本，应按照第三章配置 `PYTHONPATH`，并重新执行本节的检查。

### 2.3 Tent + MemFabric 预检

安装 `memfabric_hybrid` 并按照第三章完成配置后，可以执行以下独立脚本。脚本会检查核心环境变量、Tent 策略、engine 方法以及 `libmf_smem.so` 的加载和符号。

该脚本用于静态预检，不执行端到端数据传输。

```bash
cat > /tmp/check_tent_memfabric.py <<'EOF'
import ctypes, importlib, json, os, sys
errors = []

if os.environ.get("MC_USE_TENT") != "1":
    errors.append("MC_USE_TENT 必须为 1")
if os.environ.get("MF_PROTOCOL") not in ("device_sdma", "device_rdma"):
    errors.append("MF_PROTOCOL 应为 device_sdma 或 device_rdma")

conf_path = os.environ.get("MC_TENT_CONF", "")
if not conf_path or not os.path.isfile(conf_path):
    errors.append("MC_TENT_CONF 未设置或文件不存在: " + repr(conf_path))
else:
    try:
        conf = json.load(open(conf_path))
        enabled = [k for k, v in conf.get("transports", {}).items() if v.get("enable")]
        if enabled != ["memfabric"]:
            errors.append(f"Tent transports 必须仅启用 memfabric，当前: {enabled}")
    except Exception as e:
        errors.append("Tent 配置无法解析: " + str(e))

try:
    m = importlib.import_module("mooncake.engine")
    print("mooncake.engine ->", m.__file__)
    for name in ("initialize", "get_rpc_port", "batch_register_memory", "batch_transfer_sync_write"):
        if not hasattr(m.TransferEngine, name):
            errors.append("TransferEngine 缺少方法 " + name)
except Exception as e:
    errors.append("无法导入 mooncake.engine: " + str(e))

mf_dir = os.environ.get("MEMFABRIC_LIB_DIR", "")
try:
    lib = ctypes.CDLL(os.path.join(mf_dir, "libmf_smem.so"), mode=ctypes.RTLD_GLOBAL)
    for symbol in ("smem_trans_init", "smem_trans_create", "smem_trans_batch_register_mem",
                   "smem_trans_batch_copy", "smem_trans_batch_status_query"):
        if not hasattr(lib, symbol):
            errors.append("libmf_smem.so 缺少符号 " + symbol)
    print("libmf_smem.so ->", os.path.join(mf_dir, "libmf_smem.so"))
except Exception as e:
    errors.append(f"无法加载 libmf_smem.so，请检查 MEMFABRIC_LIB_DIR 和 LD_LIBRARY_PATH: {e}")

if errors:
    print("FAIL:")
    [print("  -", error) for error in errors]
    sys.exit(1)
print("OK: Tent + MemFabric 预检通过")
EOF
python3 /tmp/check_tent_memfabric.py
```

### 2.4 注意事项

1. 预检通过后仍需在实际业务中验证数据读写和传输性能。
2. `MF_PROTOCOL` 应根据硬件和部署方式选择 `device_rdma` 或 `device_sdma`。
3. `mooncake` 本体、Tent 扩展和 MemFabric 运行时库应来自预期安装位置，避免混合加载。

---

## 三、关键环境变量

### 3.1 Tent 与 MemFabric 核心开关

| 变量 | 说明 | 默认值 | 配置要求 |
|---|---|---|---|
| `MC_USE_TENT` | 启用 Tent 模式 | 未设置时使用经典 TransferEngine 路径 | 设置为 `1` |
| `MC_TENT_CONF` | 指定 Tent 策略 JSON 文件 | 无 | 替换为实际配置文件路径 |
| `MF_PROTOCOL` | 指定 MemFabric 设备传输协议 | 未设置时可能报告 `Transport memfabric skipped` | `device_sdma` / `device_rdma` / `host_rdma` / `device_urma` / `device_uboe` |

以 `device_rdma` 为例。

```bash
export MC_USE_TENT=1
export MC_TENT_CONF=/path/to/memfabric_tent.json
export MF_PROTOCOL=device_rdma
```

### 3.2 Tent 策略配置

`MC_TENT_CONF` 指向的 JSON 应只启用 MemFabric 传输。以下是一份最小配置，需将 `rpc_server_hostname` 替换为本机地址。

```json
{
    "metadata_type": "p2p",
    "rpc_server_hostname": "<本机 IP>",
    "rpc_server_port": 0,
    "log_level": "info",
    "transports": {
        "memfabric": { "enable": true }
    },
    "policy": [
        { "name": "default_memory", "segment_type": "memory", "transports": ["memfabric"] }
    ]
}
```

| 字段 | 说明 |
|---|---|
| `metadata_type` | `p2p` 表示点对点元数据交换，无需独立 metadata server |
| `rpc_server_hostname` | Tent RPC 绑定的本机主机名或 IP |
| `rpc_server_port` | `0` 表示随机端口 |
| `transports.memfabric.enable` | 启用 MemFabric，其余传输应显式设为 `false` |
| `policy[].transports` | 策略中只允许使用 `memfabric` |

### 3.3 运行时库路径与加载优先级

先将 `SP` 设置为 wheel 实际安装对应的 `site-packages`，再配置 MemFabric 运行时库和 Python 导入优先级。

```bash
SP=/path/to/site-packages
export MEMFABRIC_LIB_DIR="${SP}/memfabric_hybrid/lib"
export LD_LIBRARY_PATH="${MEMFABRIC_LIB_DIR}:${SP}/mooncake:${LD_LIBRARY_PATH}"
export PYTHONPATH="${SP}:${PYTHONPATH}"
```

`MEMFABRIC_LIB_DIR` 用于定位 `libmf_smem.so` 等运行时库。

---

## 四、SGLang 配置 Mooncake 示例（PD 分离）

以下示例使用两个昇腾 NPU 节点，Prefill 和 Decode 之间通过 Mooncake 的 Tent + MemFabric 后端传输 KV Cache。示例地址和端口如下，实际使用时请替换。

| 参数 | 示例值 |
|---|---|
| Prefill 节点 IP | `192.168.0.167` |
| Decode 节点 IP | `192.168.0.168` |
| Prefill HTTP 端口 | `30000` |
| Decode HTTP 端口 | `30001` |
| Mooncake bootstrap 端口 | `31000` |
| Router 服务端口 | `8000` |

### 4.1 在两个节点配置 Mooncake 环境

先按照第 3.2 节在两个节点分别创建 `memfabric_tent.json`，并将每个节点配置中的 `rpc_server_hostname` 改为该节点自己的 IP。随后在 Prefill 和 Decode 节点分别执行以下配置。示例保留 `MOONCAKE_LOCAL_HOSTNAME`，用于兼容读取该变量的 SGLang 版本和其他 Mooncake 接入路径。

```bash
# 模型路径和当前节点 IP，按实际环境修改
export MODEL_PATH=/path/to/model
export MOONCAKE_LOCAL_HOSTNAME=192.168.0.167

# 启用 Tent + MemFabric
export MC_USE_TENT=1
export MC_TENT_CONF=/path/to/memfabric_tent.json
export MF_PROTOCOL=device_sdma
export MF_TRANSFER_TIMEOUT=10000

# 配置 MemFabric、Mooncake 动态库和 Python 包路径
SP=/path/to/site-packages
export MEMFABRIC_LIB_DIR="${SP}/memfabric_hybrid/lib"
export LD_LIBRARY_PATH="${MEMFABRIC_LIB_DIR}:${SP}/mooncake:${LD_LIBRARY_PATH}"
export PYTHONPATH="${SP}:${PYTHONPATH}"

# 清理可能改变传输路径的遗留配置
unset MC_FORCE_TCP MF_DATA_OP_TYPE SGLANG_MF_SPLIT_PROTOCOLS
```

`MF_PROTOCOL=device_sdma` 仅为本示例配置；使用 RDMA 的节点应改为 `device_rdma`。启动 SGLang 前，先按第 2.3 节完成预检。读取 `MOONCAKE_LOCAL_HOSTNAME` 的版本应在两个节点分别配置本机地址。当前 PD 实现也可能通过 `get_local_ip_auto()` 自动检测本机 IP，多网卡节点需要在启动日志中确认实际使用的地址。

### 4.2 启动 Prefill 节点

在 `192.168.0.167` 上执行：

```bash
python3 -m sglang.launch_server \
    --model-path "${MODEL_PATH}" \
    --host 0.0.0.0 \
    --port 30000 \
    --disaggregation-mode prefill \
    --disaggregation-transfer-backend mooncake \
    --disaggregation-bootstrap-port 31000
```

### 4.3 启动 Decode 节点

在 `192.168.0.168` 上执行第 4.1 节的环境配置。如果使用 `MOONCAKE_LOCAL_HOSTNAME`，将其设置为 `192.168.0.168`，然后执行：

```bash
python3 -m sglang.launch_server \
    --model-path "${MODEL_PATH}" \
    --host 0.0.0.0 \
    --port 30001 \
    --disaggregation-mode decode \
    --disaggregation-transfer-backend mooncake
```

如果部署多个 Prefill 实例，应确保每个实例的 HTTP 和 bootstrap 端口在其所在节点上未被占用。

### 4.4 启动 Router 并验证请求

待 Prefill 和 Decode 服务均启动后，在安装了 `sglang-router` 且能够访问两个节点的机器上启动 PD Router：

```bash
python3 -m sglang_router.launch_router \
    --pd-disaggregation \
    --prefill http://192.168.0.167:30000 31000 \
    --decode http://192.168.0.168:30001 \
    --host 0.0.0.0 \
    --port 8000
```

`--prefill` 后的第二个参数 `31000` 是 Prefill 节点的 Mooncake bootstrap 端口，必须与 Prefill 启动命令中的 `--disaggregation-bootstrap-port` 保持一致。使用 Mooncake PD 传输时应显式提供该端口。省略后 Router 不会向 Prefill 传递 bootstrap 端口，可能导致 Prefill 与 Decode 无法建立 KV 传输连接。

向 Router 发送请求：

```bash
curl -s http://127.0.0.1:8000/generate \
    -H 'Content-Type: application/json' \
    -d '{
        "text": "请用一句话介绍 Mooncake。",
        "sampling_params": {
            "temperature": 0,
            "max_new_tokens": 32
        }
    }'
```

请求能够正常返回生成结果，说明 SGLang 的 PD 请求链路已打通。同时检查两个节点的日志，确认 Mooncake 使用 Tent + MemFabric 初始化，且没有出现 `Transport memfabric skipped`、动态库加载失败或传输超时等错误。
