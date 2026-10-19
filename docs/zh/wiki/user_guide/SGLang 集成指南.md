# SGLang 集成指南

## 概述

本文档介绍如何将 MemFabric 作为传输后端集成到 [SGLang](https://github.com/sgl-project/sglang) 的 PD 分离（Prefill-Decode Disaggregation）场景中。SGLang 通过 `AscendTransferEngine` 封装 `memfabric_hybrid.TransferEngine`，实现跨节点的 KV Cache 高效传输。

SGLang 的 PD 分离架构中：

- **Prefill 节点**：处理大量 Prefill 计算，将 KV Cache 传输给 Decode 节点
- **Decode 节点**：接收 KV Cache，执行自回归解码产出 Token

MemFabric 提供 sdma, rdma 等多种传输协议，能够高效支撑上述数据传输。

## 核心机制一句话

MemFabric 通过 Config Store（ASCEND_MF_STORE_URL）做 Prefill/Decode 的相互发现，每个 TransferEngine 实例用 ip:port 作为 session_id 唯一标识——多实例共存的全部麻烦，本质上都源于三件事：端口唯一、协议一致、设备隔离。

部署前必查的 5 项

| 检查项 | 关键点 | 踩坑后果 |
| --- | --- | --- |
| Config Store URL | 同一 PD 集群内所有节点必须相同；多组集群用不同端口隔离 | Prefill/Decode 无法互相发现 |
| session_id 端口 | 每个实例唯一；不指定或传 0 时从 MF_CONFIG_STORE_PORT_START~END 自动分配 | 第二个实例初始化失败，返回非 0 |
| 传输协议 | 同一集群内所有实例必须一致（A2 推荐 device_rdma，A3 推荐 sdma/host_rdma） | 内存注册分布不匹配，传输失败 |
| NPU 隔离 | ASCEND_RT_VISIBLE_DEVICES + --base-gpu-id 不能重叠；容器里另加 ASCEND_NPU_PHY_ID | 多实例抢占同一设备 |
| RDMA swap 空间 | MF_HYBM_RDMA_SWAP_SPACE_SIZE × 实例数 总量 ≤ 可用 DRAM | 内存不足，传输性能下降或 OOM |

## 环境要求

### 硬件与软件配套

| 组件 | 版本要求 |
|------|---------|
| CANN | 8.1.RC1 及以上 |
| HDK | 24.1.RC2（HBM 池化）/ 25.5.0（DRAM 池化） |
| memfabric-hybrid | 1.0.0 及以上 |
| Python | 3.9+ |
| 昇腾硬件 | Atlas 800I A2 / Atlas 800 A3 / Atlas 900 A3 SuperPoD |

### 安装

```bash
pip install memfabric-hybrid==1.0.0
source /usr/local/Ascend/ascend-toolkit/set_env.sh
```

## 快速开始：单机 PD 分离

### 基础环境变量

```bash
# 配置存储地址（必须，所有节点指向同一地址）
export ASCEND_MF_STORE_URL="tcp://<PREFILL_IP>:<PORT>"

# 传输协议（可选，默认 SDMA）
#   A2 推荐: device_rdma
#   A3 推荐: sdma（默认）或 host_rdma
export ASCEND_MF_TRANSFER_PROTOCOL="sdma"

# 容器内需设置 NPU 物理 ID
export ASCEND_NPU_PHY_ID=0
```

### 启动 Prefill 节点

```bash
python -m sglang.launch_server \
    --model-path /path/to/model \
    --attention-backend ascend \
    --device npu \
    --tp-size 8 \
    --port 30000 \
    --disaggregation-mode prefill \
    --disaggregation-transfer-backend ascend \
    --disaggregation-bootstrap-port 8996 \
    --host 127.0.0.1
```

### 启动 Decode 节点

```bash
python -m sglang.launch_server \
    --model-path /path/to/model \
    --attention-backend ascend \
    --device npu \
    --tp-size 8 \
    --port 30001 \
    --base-gpu-id 8 \
    --disaggregation-mode decode \
    --disaggregation-transfer-backend ascend \
    --disaggregation-bootstrap-port 8996 \
    --host 127.0.0.1
```

### 启动 Router

```bash
python -m sglang_router.launch_router \
    --pd-disaggregation \
    --prefill http://127.0.0.1:30000 \
    --decode http://127.0.0.1:30001 \
    --host 0.0.0.0 --port 8000
```

---

## 多实例场景详解

以下章节重点介绍多 TransferEngine 实例共存场景下的关键配置与注意事项。

### 场景分类

| 场景 | 描述 |
|------|------|
| **单机多卡 PD 分离** | 同一机器的不同 NPU 分属 Prefill/Decode 两个角色，各自初始化 TransferEngine |
| **多 DP rank 共机** | 同机多个 DP rank 各自拥有 TransferEngine |
| **多组 PD 集群共享机器** | 多套 PD 独立部署共享物理机，端口与 Config Store 需要隔离 |
| **跨机多节点部署** | Prefill / Decode 分布在多台机器，各自的 TransferEngine 需相互发现 |

### 核心组件与资源

每个 `TransferEngine` 实例占用以下资源：

| 资源 | 说明 | 多实例冲突风险 |
|------|------|---------------|
| **Config Store 连接** | 通过 `store_url`（即 `ASCEND_MF_STORE_URL`）连接到配置存储 | 多个实例可共享，但需确保端口不冲突 |
| **RPC 端口** | `session_id` 中的端口（`ip:port`），用于实例间通信 | **高** — 每个实例必须使用唯一端口 |
| **RDMA QP（队列对）** | 底层传输的连接资源 | 因 HCOM/native RDMA 实现而异 |
| **RDMA Swap 空间** | `MF_HYBM_RDMA_SWAP_SPACE_SIZE` 控制的中转内存 | 进程间独立，不共享 |
| **NPU 设备** | 通过 `ASCEND_RT_VISIBLE_DEVICES` 控制可见性 | 不同实例必须绑定不同设备 |

---

## Config Store 规划

### 共享 vs 隔离

Config Store 是 Prefill 和 Decode 相互发现的纽带。多个 TransferEngine 实例可以共享同一个 Config Store，也可以各自使用独立的。

| 模式 | 适用场景 | 说明 |
|------|---------|------|
| **共享 Config Store** | 同一组 PD 集群内的所有实例 | 所有 Prefill 和 Decode 使用同一 `ASCEND_MF_STORE_URL`。MemFabric 内部通过 `session_id` 区分不同实例。**推荐** |
| **独立 Config Store** | 多组 PD 集群物理隔离 | 每组集群使用不同的 `ASCEND_MF_STORE_URL` 端口，完全隔离 |

> **重要**：共享 Config Store 时，同一 Config Store 下的所有实例必须使用唯一 `session_id`。SGLang 通过 `ip:port` 作为 `session_id`，因此各实例的 RPC 端口必须唯一。

### Config Store 端口范围

MemFabric 通过环境变量控制 Config Store 可用端口范围：

```bash
# 默认范围 9000~65535
export MF_CONFIG_STORE_PORT_START=9000
export MF_CONFIG_STORE_PORT_END=65535

# 当 session_id 未指定端口（如 "ip" 或 "ip:0"）时，
# TransferEngine.initialize() 将自动在此范围内选择端口
```

### 典型配置示例

#### 单组 PD 集群（共享 Config Store）

```bash
# 所有节点统一配置
export ASCEND_MF_STORE_URL="tcp://192.168.1.1:24670"
```

#### 多组 PD 集群（独立 Config Store）

```bash
# 集群 A
export ASCEND_MF_STORE_URL="tcp://192.168.1.1:24670"

# 集群 B
export ASCEND_MF_STORE_URL="tcp://192.168.1.1:24671"
```

---

## Session ID 与端口管理

### Session ID 的结构

Session ID 是 TransferEngine 实例的唯一标识符，格式为 `ip:port`：

- SGLang 自动构造：`NetworkAddress(hostname, rpc_port).to_host_port_str()`
- 若 `session_id` 不指定端口或传 `0`，`initialize()` 将自动分配端口（可通过 `get_rpc_port()` 获取实际端口）

### 端口唯一性保证

多实例场景下的端口规划原则：

1. **每个 TransferEngine 实例的 Session ID 端口必须唯一**
2. 单机多实例时，可通过指定不同端口来区分：

```python
# 实例 A（自动分配端口）
engine_a.initialize(store_url, "192.168.1.1:0", "Prefill", 0, ...)
port_a = engine_a.get_rpc_port()

# 实例 B（显式指定端口）
engine_b.initialize(store_url, "192.168.1.1:50052", "Decode", 1, ...)
```

1. SGLang 中，不同进程（如 Prefill 和 Decode）通过 `--disaggregation-bootstrap-port` 参数区分各自的 bootstrap 端口。TransferEngine 的 RPC 端口由 `session_id` 控制，两者独立。

### 常见问题

**Q: 两个实例设置了相同的 `session_id`（相同 IP:Port）会怎样？**

A: 第二个初始化的实例会因为端口冲突而初始化失败，返回非零错误码。需要确保每个实例的端口唯一。

**Q: 如何查看实际绑定的端口？**

A: 在 Python 层面调用 `engine.get_rpc_port()` 获取实时端口。SGLang 日志中也会打印初始化成功的 `session_id`。

---

## 传输协议选择

### 协议类型

| 协议 | 环境变量值 | 适用硬件 | 说明 |
|------|-----------|---------|------|
| SDMA（默认） | `sdma` | A3 | Device SDMA 传输，适用于超节点内 |
| HOST_RDMA | `host_rdma` | A2, A3 | 通过 Host RDMA 网卡传输，跨机场景 |
| DEVICE_RDMA | `device_rdma` | A2 | 设备侧 RDMA，A2 跨机推荐 |

### 协议配置

```bash
# 通过环境变量设置
export ASCEND_MF_TRANSFER_PROTOCOL="host_rdma"

# 或
export ASCEND_MF_TRANSFER_PROTOCOL="device_rdma"
```

代码中协议选择逻辑（`python/sglang/srt/disaggregation/ascend/transfer_engine.py`）：

```python
transfer_protocol = os.getenv("ASCEND_MF_TRANSFER_PROTOCOL")

if transfer_protocol == "host_rdma":
    trans_op_type = TransferEngine.TransDataOpType.HOST_RDMA
elif transfer_protocol is None or transfer_protocol == "sdma":
    trans_op_type = TransferEngine.TransDataOpType.SDMA
else:
    trans_op_type = TransferEngine.TransDataOpType.DEVICE_RDMA
```

### 多实例下协议一致性要求

同一组 PD 集群内的所有 TransferEngine 实例必须使用 **相同** 的传输协议。如果协议不一致，Prefill 侧注册的内存分布与 Decode 侧不匹配，将导致传输失败。

---

## HCOM / RDMA 资源配置

多实例环境下的 RDMA 资源管理。

### 核心环境变量

| 环境变量 | 默认值 | 说明 | 多实例影响 |
|----------|--------|------|-----------|
| `MF_HYBM_RDMA_SWAP_SPACE_SIZE` | 1024 (MB) | RDMA 交换空间大小 | 进程级隔离，每个进程独立分配 |
| `MF_HYBM_RDMA_USE_HCOMM` | 0 | HCOMM 传输开关 | 各实例独立设置 |
| `MF_HYBM_RDMA_FORCE_UNREGISTERED` | 0 | 强制跳过内存注册路径 | 各实例独立设置 |
| `HCOM_MAX_SLICE_SIZE` | NPU: 1MB | HCOM 传输最大切片大小 | 各实例独立设置 |
| `HCOM_RECV_DATA_SIZE` | NPU: 1MB+1KB | HCOM 接收缓冲区大小 | 各实例独立设置 |

### RDMA 交换空间

每个进程独立分配 RDMA swap 空间。多实例时，swap 空间的总占用量 = `MF_HYBM_RDMA_SWAP_SPACE_SIZE × 实例数`。确保系统 DRAM 充足：

```bash
# 每个实例 2GB swap 空间
export MF_HYBM_RDMA_SWAP_SPACE_SIZE=2048

# 启动实例 A（进程 1）
python -m sglang.launch_server ... --disaggregation-mode prefill ...

# 启动实例 B（进程 2）
python -m sglang.launch_server ... --disaggregation-mode decode ...
```

> **注意**：swap 空间按进程分配，同一进程内的多个 TransferEngine 共享同一份 swap 空间。当前 SGLang 集成中每个 sglang 进程内只有一个 `AscendTransferEngine` 实例。

### HCOMM vs Native RDMA

| 参数 | `MF_HYBM_RDMA_USE_HCOMM=0`（默认） | `MF_HYBM_RDMA_USE_HCOMM=1` |
|------|------|------|
| 传输实现 | Native RDMA（`RdmaTransportManager`） | HCOMM（`DeviceRdmaHcommTransportManager`） |
| 动态内存注册 | ✅ 天然支持 | ❌ A2 下需中转 |
| A2（910B）推荐 | ✅ | ❌（需 `MF_HYBM_RDMA_FORCE_UNREGISTERED=1`） |
| A5（950）URMA | ✅（自动使用 URMA 通道） | N/A |

多实例下推荐所有实例统一使用 native RDMA（默认）以避免 HCOMM 通道 MR 快照问题。

---

## NPU 设备管理与隔离

### `ASCEND_RT_VISIBLE_DEVICES`

控制每个进程可见的 NPU 设备列表。多实例必须正确设置以确保不同实例绑定到不同 NPU：

```bash
# 实例 1：使用 NPU 0-3
export ASCEND_RT_VISIBLE_DEVICES=0,1,2,3

# 实例 2：使用 NPU 4-7
export ASCEND_RT_VISIBLE_DEVICES=4,5,6,7
```

### `ASCEND_NPU_PHY_ID`

容器环境中，SGLang 需要通过 `ASCEND_NPU_PHY_ID` 映射物理 NPU ID。多容器多实例时：

```bash
# 容器 1
export ASCEND_NPU_PHY_ID=0
export ASCEND_RT_VISIBLE_DEVICES=0

# 容器 2
export ASCEND_NPU_PHY_ID=1
export ASCEND_RT_VISIBLE_DEVICES=1
```

### SGLang `--base-gpu-id`

SGLang 的 `--base-gpu-id` 参数指定当前进程的起始 GPU ID。在单机多实例场景中用于分配不同的 TP 组：

```bash
# Prefill：使用 NPU 0-7
python -m sglang.launch_server \
    --tp-size 8 --base-gpu-id 0 \
    --disaggregation-mode prefill ...

# Decode：使用 NPU 8-15
python -m sglang.launch_server \
    --tp-size 8 --base-gpu-id 8 \
    --disaggregation-mode decode ...
```

---

## 内存注册

### 注册机制

MemFabric 要求所有参与传输的内存区域通过 `register_memory` 注册。SGLang 的 `AscendTransferEngine.batch_register()` 对传入的指针列表进行批量注册：

```python
def batch_register(self, ptrs, lengths):
    try:
        ret_value = self.engine.batch_register_memory(ptrs, lengths)
    except Exception:
        ret_value = -1
    if ret_value != 0:
        logger.debug(f"Ascend memory registration for ptr {ptrs} failed.")
```

### 对齐要求

MemFabric 注册内存对齐到 2 MiB 边界。SGLang 在 `AscendKVManager.register_buffer_to_engine()` 中将所有 KV buffer 合并为一次 `batch_register` 调用，利用批量注册自动合并重叠对齐范围。

### 多实例注意事项

1. **内存区域隔离**：不同进程的 KV Cache 位于不同 NPU 的 HBM 上，天然隔离，无冲突风险
2. **注册失败诊断**：当前 SGLang 对注册失败仅打 debug 日志。如需排查，设置日志级别：

   ```bash
   export MF_LOG_LEVEL=0  # DEBUG 级别
   ```

3. **内存容量**：多实例共享同一物理机的 DRAM/HBM 时，确保 `--mem-fraction-static` 参数总和不超过 1.0

---

## 部署示例

### 示例 1：单机 16 卡 PD 分离（A3 超节点）

**场景**：1 台 A3 超节点（16 NPU），8 Prefill + 8 Decode，SDMA 传输。

```bash
# ===== 通用环境 =====
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export ASCEND_MF_STORE_URL="tcp://127.0.0.1:24670"
export SGLANG_DISAGGREGATION_BOOTSTRAP_TIMEOUT=3600
export SGLANG_DISAGGREGATION_WAITING_TIMEOUT=3600

# ===== Prefill（NPU 0-7）=====
export ASCEND_RT_VISIBLE_DEVICES=0,1,2,3,4,5,6,7
python -m sglang.launch_server \
    --model-path /path/to/model \
    --attention-backend ascend --device npu \
    --tp-size 8 --nnodes 1 --node-rank 0 \
    --port 30000 --host 127.0.0.1 \
    --disaggregation-mode prefill \
    --disaggregation-transfer-backend ascend \
    --disaggregation-bootstrap-port 8996 \
    --base-gpu-id 0 \
    --mem-fraction-static 0.4 &

# ===== Decode（NPU 8-15）=====
export ASCEND_RT_VISIBLE_DEVICES=8,9,10,11,12,13,14,15
python -m sglang.launch_server \
    --model-path /path/to/model \
    --attention-backend ascend --device npu \
    --tp-size 8 --nnodes 1 --node-rank 0 \
    --port 30001 --host 127.0.0.1 \
    --disaggregation-mode decode \
    --disaggregation-transfer-backend ascend \
    --disaggregation-bootstrap-port 8997 \
    --base-gpu-id 8 \
    --mem-fraction-static 0.4 &

# ===== Router =====
python -m sglang_router.launch_router \
    --pd-disaggregation \
    --prefill http://127.0.0.1:30000 \
    --decode http://127.0.0.1:30001 \
    --host 0.0.0.0 --port 8000
```

### 示例 2：双机 PD 分离（A2，HOST_RDMA 传输）

**场景**：2 台 A2 服务器，每台 8 NPU。节点 1 运行 Prefill，节点 2 运行 Decode，通过 HOST_RDMA 跨机传输。

```bash
# ===== 节点 1（Prefill，IP: 192.168.1.1）=====
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export ASCEND_MF_STORE_URL="tcp://192.168.1.1:24670"
export ASCEND_MF_TRANSFER_PROTOCOL="host_rdma"
export ASCEND_RT_VISIBLE_DEVICES=0,1,2,3,4,5,6,7
export SGLANG_DISAGGREGATION_BOOTSTRAP_TIMEOUT=3600

python -m sglang.launch_server \
    --model-path /path/to/model \
    --attention-backend ascend --device npu \
    --tp-size 8 --nnodes 1 --node-rank 0 \
    --port 30000 --host 192.168.1.1 \
    --dist-init-addr 192.168.1.1:5000 --nnodes 2 --node-rank 0 \
    --disaggregation-mode prefill \
    --disaggregation-transfer-backend ascend \
    --disaggregation-bootstrap-port 8996 \
    --base-gpu-id 0 \
    --mem-fraction-static 0.7

# ===== 节点 2（Decode，IP: 192.168.1.2）=====
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export ASCEND_MF_STORE_URL="tcp://192.168.1.1:24670"
export ASCEND_MF_TRANSFER_PROTOCOL="host_rdma"
export ASCEND_RT_VISIBLE_DEVICES=0,1,2,3,4,5,6,7

python -m sglang.launch_server \
    --model-path /path/to/model \
    --attention-backend ascend --device npu \
    --tp-size 8 --nnodes 1 --node-rank 0 \
    --port 30001 --host 192.168.1.2 \
    --dist-init-addr 192.168.1.1:5000 --nnodes 2 --node-rank 1 \
    --disaggregation-mode decode \
    --disaggregation-transfer-backend ascend \
    --disaggregation-bootstrap-port 8997 \
    --base-gpu-id 0 \
    --mem-fraction-static 0.7
```

### 示例 3：多组 PD 集群共享同一台机器

**场景**：1 台 A3 超节点（32 NPU），部署两组 PD 集群（集群 A: 8 Prefill + 8 Decode，集群 B: 8 Prefill + 8 Decode），使用不同 Config Store 端口隔离。

```bash
# ===== 集群 A：使用 Config Store 端口 24670 =====

# Prefill A（NPU 0-7）
export ASCEND_RT_VISIBLE_DEVICES=0-7
export ASCEND_MF_STORE_URL="tcp://127.0.0.1:24670"
python -m sglang.launch_server \
    ... \
    --port 30000 --disaggregation-mode prefill \
    --disaggregation-bootstrap-port 8996 \
    --base-gpu-id 0 &

# Decode A（NPU 8-15）
export ASCEND_RT_VISIBLE_DEVICES=8-15
export ASCEND_MF_STORE_URL="tcp://127.0.0.1:24670"
python -m sglang.launch_server \
    ... \
    --port 30001 --disaggregation-mode decode \
    --disaggregation-bootstrap-port 8997 \
    --base-gpu-id 8 &

# ===== 集群 B：使用 Config Store 端口 24671 =====

# Prefill B（NPU 16-23）
export ASCEND_RT_VISIBLE_DEVICES=16-23
export ASCEND_MF_STORE_URL="tcp://127.0.0.1:24671"
python -m sglang.launch_server \
    ... \
    --port 30002 --disaggregation-mode prefill \
    --disaggregation-bootstrap-port 8998 \
    --base-gpu-id 16 &

# Decode B（NPU 24-31）
export ASCEND_RT_VISIBLE_DEVICES=24-31
export ASCEND_MF_STORE_URL="tcp://127.0.0.1:24671"
python -m sglang.launch_server \
    ... \
    --port 30003 --disaggregation-mode decode \
    --disaggregation-bootstrap-port 8999 \
    --base-gpu-id 24 &
```

> **注意**：两组 PD 集群使用不同的 `ASCEND_MF_STORE_URL`（不同端口）实现 Config Store 隔离。同时需确保 SGLang 的 HTTP port、bootstrap port、dist-init-addr 全都不冲突。

---

## 与 Mooncake 后端混合使用

MemFabric 也支持作为 Mooncake 传输后端的底层加速器。当设置以下环境变量时，SGLang 使用 Mooncake 的上层协议，但底层传输走 Ascend NPU 的加速路径：

```bash
export ENABLE_ASCEND_TRANSFER_WITH_MOONCAKE=true
```

此模式下需要额外设置 NPU 物理 ID：

```bash
export ASCEND_NPU_PHY_ID=0
```

Mooncake 模式下 `ASCEND_MF_STORE_URL` 仍然需要设置，但主要用于 SGLang 内部路由判断；Mooncake 自身通过 `mooncake-transfer-engine` 建立连接。

---

## 故障排查

### 初始化失败

| 错误现象 | 排查步骤 |
|---------|---------|
| `TransferEngine initialize failed` | 1. 检查 `ASCEND_MF_STORE_URL` 是否正确且可路由<br>2. 确认端口未被占用（使用 `netstat -tlnp \| grep PORT`）<br>3. 验证 `session_id` 是否唯一（所有实例端口不冲突） |
| `initialize` 返回非 0 | 1. 设置 `MF_LOG_LEVEL=0` 查看 MemFabric debug 日志<br>2. 检查 CANN 环境和 NPU 驱动是否正确加载 |
| Config Store 连接超时 | 1. 使用 `telnet <ip> <port>` 验证连通性<br>2. 防火墙是否拦截了 Config Store 端口 |

### 数据传输失败

| 错误现象 | 排查步骤 |
|---------|---------|
| `transfer_sync_write` 返回非 0 | 1. 确认目标实例的 `session_id` 正确且可达<br>2. 检查两端传输协议是否一致<br>3. 验证内存注册是否成功 |
| KV 传输卡住 | 1. 增加 `SGLANG_DISAGGREGATION_BOOTSTRAP_TIMEOUT`<br>2. 检查所有 NPU 卡是否在线 |
| 数据校验错误 | 1. 确认两端模型配置一致（TP size、layer 数等）<br>2. 检查是否使用了相同的 `--mem-fraction-static` |

### 多实例资源冲突

| 错误现象 | 排查步骤 |
|---------|---------|
| 端口冲突（Address already in use） | 1. 确保每个实例的 `session_id` 端口唯一<br>2. 确保 `--disaggregation-bootstrap-port` 在各实例间唯一<br>3. 检查 SGLang HTTP port (`--port`) 和 dist-init-addr 端口不冲突 |
| NPU 设备冲突 | 1. 检查 `ASCEND_RT_VISIBLE_DEVICES` 是否正确设置<br>2. 验证各进程的 `--base-gpu-id` 不重叠 |
| RDMA 传输性能下降 | 1. 检查 `HCOM_MAX_SLICE_SIZE` 设置是否合理<br>2. 确认 swap 空间充足（`free -h`） |
| 内存不足 | 1. 调整 `--mem-fraction-static` 参数<br>2. 减少 `MF_HYBM_RDMA_SWAP_SPACE_SIZE` |

### 日志获取

```bash
# MemFabric 日志级别（仅 Python 接口生效）
export MF_LOG_LEVEL=0   # DEBUG
export MF_LOG_LEVEL=1   # INFO
export MF_LOG_LEVEL=2   # WARN
export MF_LOG_LEVEL=3   # ERROR（默认）
export MF_LOG_LEVEL=4   # OFF

# SGLang 日志
export SGLANG_LOG=debug
```

---

## 环境变量参考

### MemFabric 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `MF_CONFIG_STORE_PORT_START` | 9000 | Config Store 可用端口范围起始值 |
| `MF_CONFIG_STORE_PORT_END` | 65535 | Config Store 可用端口范围结束值 |
| `MF_HYBM_RDMA_SWAP_SPACE_SIZE` | 1024 (MB) | RDMA 交换空间大小 |
| `MF_HYBM_RDMA_USE_HCOMM` | 0 | HCOMM 传输开关（0=Native RDMA, 1=HCOMM） |
| `MF_HYBM_RDMA_FORCE_UNREGISTERED` | 0 | 强制跳过内存注册路径 |
| `MF_LOG_LEVEL` | 3 | 日志级别（0=DEBUG, 1=INFO, 2=WARN, 3=ERROR, 4=OFF） |
| `HCOM_MAX_SLICE_SIZE` | NPU: 1MB | HCOM 传输最大切片大小 |
| `HCOM_RECV_DATA_SIZE` | NPU: 1MB+1KB | HCOM 接收缓冲区大小 |
| `ASCEND_HOME_PATH` | —（必填） | CANN 安装路径 |
| `ASCEND_RT_VISIBLE_DEVICES` | — | 设备可见性控制，格式 `0,1,2,3` |

### SGLang PD 分离环境变量

| 变量 | 说明 |
|------|------|
| `ASCEND_MF_STORE_URL` | MemFabric Config Store 地址，格式 `tcp://ip:port` |
| `ASCEND_MF_TRANSFER_PROTOCOL` | 传输协议，可选 `sdma` / `host_rdma` / `device_rdma` |
| `ASCEND_MF_HCOM_URL` | HCOM URL（device_rdma 协议使用） |
| `ASCEND_NPU_PHY_ID` | 容器内 NPU 物理 ID |
| `ENABLE_ASCEND_TRANSFER_WITH_MOONCAKE` | 是否使用 Mooncake + MemFabric 混合模式 |

---

## 相关文档

- [MemFabric 安装指南](../../installation.md)
- [MemFabric Python 接口文档](../../pythonAPI.md)
- [MemFabric 环境变量说明](../../environment_variables.md)
- [SGLang PD Disaggregation 文档](https://github.com/sgl-project/sglang/blob/main/docs/docs/advanced_features/pd_disaggregation.mdx)
- [SGLang Ascend NPU 环境变量](https://github.com/sgl-project/sglang/blob/main/docs/docs/hardware-platforms/ascend-npus/reference/environment_variables.mdx)
