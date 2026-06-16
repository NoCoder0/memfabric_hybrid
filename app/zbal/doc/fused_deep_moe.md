# FusedDeepMoe 算子设计说明

> 本文档描述 `app/zbal/src/csrc/operators/npu/device/fused_deep_moe/` 中 fused_deep_moe 融合算子的架构设计与实现细节。

---

## 1. 概述

fused_deep_moe 是面向 Ascend NPU 的**融合 MoE（Mixture of Experts）设备侧算子**，将 LLM 推理/训练中 MoE 层的多个操作融合为单次 kernel 发射，以减少 kernel 启动开销和数据搬移。

### 1.1 融合算子链

```
Token Quantization → Dispatch (AllToAll) → GMM1 (Grouped MatMul + SwiGLU + Dynamic Quant) → GMM2 (Grouped MatMul + Dequant + Combine)
```

### 1.2 设计目标

- **低延迟**：通过 Deep Fuse 模式将 dispatch、双 GEMM、combine 全在 Device 侧完成，消除 Host-Device 往返
- **高吞吐**：利用 Grouped MatMul 批量处理多 Expert 的 token，最大化 NPU 计算单元利用率
- **显存节约**：GMM1 输出即时量化为 int8，减少 GMM2 输入显存占用
- **跨 Rank 通信**：基于 ZBAL GVA 的对称内存实现跨 NPU 的 dispatch/combine，无需 Host 参与

---

## 2. 文件结构

```
fused_deep_moe/
├── zbal_kernel_fused_deep_moe.cpp          # 入口 kernel + Host 侧 Launch 函数
├── zbal_kernel_fused_deep_moe.h            # 主编排类 FusedDeepMoe<...>
├── zbal_kernel_fused_deep_moe_base.h        # 类型别名、模板宏、HCCL 上下文结构体
├── zbal_kernel_fused_deep_moe_comm.h        # ZBAL 通信上下文与 GVA 地址计算
├── gemm/
│   ├── dispatch_policy.h                    # GMM2 Resident-A dispatch policy
│   ├── block/
│   │   ├── block_mmad.h                     # BlockMmad 聚合头文件
│   │   └── block_mmad_preload_async_with_callback_resident_a.h  # GMM2 驻留 A 矩阵优化
│   └── kernel/
│       ├── grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h  # GMM1 kernel
│       └── grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h                # GMM2 kernel
├── epilogue/
│   ├── dispatch_policy.h                    # Epilogue dispatch policy（SwiGLU / Combine）
│   ├── block/
│   │   ├── block_epilogue.h                 # BlockEpilogue 聚合头文件
│   │   └── block_epilogue_per_token_dequant_swiglu.h  # SwiGLU 激活 + Per-Token Dequant
│   └── tile/
│       ├── tile_stride_muls.h               # Tile 级 stride 乘法
│       └── tile_stride_binary.h             # Tile 级 stride 二元运算
├── raw_distributed/
│   ├── zbal_moe_distribute_dispatch.h       # Dispatch: Token → Expert (AllToAll Write)
│   └── zbal_moe_distribute_combine.h        # Combine: Expert → Token (AllToAll Read + Reduce)
└── (host) zbal_kernel_fused_deep_moe_tiling.h  # Tiling 数据结构与编译期常量
```

---

## 3. 架构总览

### 3.1 执行流水线

```
Input Tokens (bf16/fp16)
    │
    ├──[SHARED_EXPERT]──→ Share Expert Token Quantization (int8) ──┐
    │                                                               │
    ▼                                                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                        DISPATCH 阶段                                │
│                                                                     │
│  Shallow 模式: CamMoeDistributeDispatch (AIV 上运行)                 │
│    - Token → Expert 映射 + 动态量化 + AllToAll Write to Remote Rank  │
│                                                                     │
│  Deep Fuse 模式: Pull-Mode                                          │
│    - 每个 Rank 将量化 Token 写入本地 quant workspace                  │
│    - GMM1 kernel 内通过 zbal_ptr 跨 rank 拉取 token                  │
└─────────────────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         GMM1 阶段                                    │
│                                                                     │
│  GroupedMatmulSliceMPerTokenDequantSwigluQuantMultiStageWorkspace    │
│    - int8 × int8 Grouped MatMul (per Expert)                        │
│    - SwiGLU 激活 (左半: x·σ(x), 右半: x)                             │
│    - 动态量化 → int8 output + per-token dequant scale                │
│                                                                     │
│  输入: 量化 Token (int8) × Expert Weights (int8)                     │
│  输出: 量化中间结果 (int8) + dequant scales (float)                  │
└─────────────────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         GMM2 阶段                                    │
│                                                                     │
│  GroupedMatmulSliceMPerTokenDequantMultiStageWorkspace               │
│    - int8 × int8 Grouped MatMul (per Expert)                        │
│    - Per-Token Dequantization (使用 GMM1 输出的 scale)               │
│    - Combine (AllToAll Read + ReduceSum)                             │
│                                                                     │
│  输入: GMM1 量化输出 (int8) × Expert Weights (int8/bf16)             │
│  输出: 最终 Token (bf16/fp16)                                        │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 AIC/AIV 核间分工

```
┌────────────────────────────────────────────────────┐
│                    AIC (AI Core)                    │
│  - GEMM 计算 (GMM1 + GMM2)                         │
│  - BlockMmad 矩阵分块乘加                           │
│  - 流水线: GM→L1→L0A/L0B→Cube→L0C→GM              │
│  - 多层缓冲: L1 Buffer (stages=2-4)                 │
│                 + L0 Buffer (A/B/C stages)          │
│                 + Workspace (UB stages=4)            │
└────────────────────────────────────────────────────┘
                        ↕ CrossCoreFlag + Soft Sync
┌────────────────────────────────────────────────────┐
│                    AIV (AI Vector)                  │
│  - Dispatch / Combine 通信逻辑                      │
│  - GMM1 Send/Recv: Token 量化与跨 Rank 搬移          │
│  - GMM2 Epilogue: Dequant + SwiGLU + Combine Reduce │
│  - GMM2 Send/Recv: AllToAll 通信                    │
│  - 每个 AIC 核对应 2 个 AIV 子核 (subBlock)          │
└────────────────────────────────────────────────────┘
```

---

## 4. 关键组件详解

### 4.1 入口函数

**[zbal_kernel_fused_deep_moe.cpp](app/zbal/src/csrc/operators/npu/device/fused_deep_moe/zbal_kernel_fused_deep_moe.cpp)**

- `fused_deep_moe()` — AscendC `__global__ __aicore__` kernel，根据 `exec_flag` + `src_data_type` dispatch 到正确的模板实例化
- `ZBALOpFusedDeepMoeLaunch()` — Host 侧启动函数，调用 `aclrtLaunchKernel` 发射 kernel
- 支持的数据类型组合：`(bf16, float, bf16)` 和 `(fp16, float, bf16)`
- Execution flag 支持的值：0 (基础), 1 (DEEP_FUSE), 9 (DEEP_FUSE+SHARED_EXPERT), 25 (DEEP_FUSE+SHARED_EXPERT+SMOOTH_QUANT), 27, 29, 31

### 4.2 主编排类

**[zbal_kernel_fused_deep_moe.h](app/zbal/src/csrc/operators/npu/device/fused_deep_moe/zbal_kernel_fused_deep_moe.h)**

`FusedDeepMoe<XType, W1SType, W2SType, ExpandIdxType, IsNeedReduceScatter, EXEC_FLAG>`

**Init() 职责：**
1. 初始化 EP/TP 两个 `ZbalCommContext`，设置 data window 和 state window 的分割比例
2. 从 `FusedDeepMoeTilingData` 读取 tiling 参数（hidden size、bs、topK、expert 数量等）
3. Deep Fuse 模式下 data window 全分配给 state window（因为 dispatch 使用 quant workspace，combine 使用独立 workspace）

**Process() 职责：**
1. **Shallow 模式**：AIV 运行 `CamMoeDistributeDispatch` → CrossCoreFlag 同步 → AIC 运行 GEMM
2. **GMM1**：调用 `GmmDeqSwigluQuant<...>()`，执行 grouped matmul + SwiGLU + dynamic quantization
3. **GMM2**：调用 `GmmDeq<...>()`，执行 grouped matmul + per-token dequant + combine
4. **Workspace 布局**（按偏移递增）：
   - `gmShareX1 / gmX1`：量化后 token（int8），share expert 在前
   - `gmShareX1Scale / gmX1Scale`：per-token scale（float）
   - `gmWorkspace`：GEMM workspace（int32）
   - `gmSwigluOut / gmGmm2DepOut`：SwiGLU/GMM2 中间输出（float/ExpandXType）
   - `gmGroupList`：per-group token 累计数（int64）
   - `gmExpandIdx`：token→expert 映射索引（int32）
   - `gmEpSendCount`：per-rank per-expert 发送计数（int32）
   - `gmResvered`：预留空间（256KB）
   - `gmQuantWorkspace`：Deep Fuse pull-mode dispatch 量化临时空间
   - `gmCombineWs`：Deep Fuse combine workspace（替代 data window）

### 4.3 通信上下文

**[zbal_kernel_fused_deep_moe_comm.h](app/zbal/src/csrc/operators/npu/device/fused_deep_moe/zbal_kernel_fused_deep_moe_comm.h)**

```cpp
struct ZbalCommContext {
    __gm__ CommGroupInfo *comm;          // GVA 通信组信息
    __gm__ uint8_t *dataWindowBase;      // 数据窗口基地址
    __gm__ uint8_t *stateWindowBase;     // 状态窗口基地址
    uint64_t dataWindowSize;             // 数据窗口大小
    uint64_t stateWindowSize;            // 状态窗口大小
    uint32_t myRankId;                   // 当前 Rank ID
    uint32_t worldSize;                  // 通信组大小
    uint64_t localDeviceMemSize;         // 本地 Device 内存大小
};
```

- `zbal_ptr()`：利用 GVA 对称特性计算远程 Rank 地址：`remotePtr = localPtr + (worldDstPe - worldCurPe) * localDeviceMemSize`
- `GetZbalDataAddr()` / `GetZbalStateAddr()`：封装本地/远程地址计算，本地直接偏移，远程通过 zbal_ptr

### 4.4 Tiling 配置

**[zbal_kernel_fused_deep_moe_tiling.h](app/zbal/src/csrc/operators/npu/host/fused_deep_moe/zbal_kernel_fused_deep_moe_tiling.h)**

| 参数 | GMM1 | GMM2 |
|------|------|------|
| L1 Tile (M×N×K) | 256×128×512 | 128×256×512 |
| L0 K Tile | 128 | 128 |
| Epilogue Tile M | 64 | 32 |
| L1A/L1B Stages | 2/2 | 4/2 |
| L0A/L0B Stages | 2/2 | 4/2 |
| Workspace Stages | 4 | 4 |
| Swizzle Offset/Direction | 3/0 | 3/0 |

**Execution Flags**（位掩码）：

| Bit | 宏定义 | 含义 |
|-----|--------|------|
| 0 | `EXEC_FLAG_DEEP_FUSE` | Deep Fuse 模式（pull-mode dispatch） |
| 1 | `EXEC_FLAG_TENSOR_LIST` | 权重使用 TensorList 格式 |
| 2 | `EXEC_FLAG_X_ACTIVE_MASK` | 输入有 active mask |
| 3 | `EXEC_FLAG_SHARED_EXPERT` | 包含 Shared Expert |
| 4 | `EXEC_FLAG_SMOOTH_QUANT` | Smooth Quantization |

### 4.5 GMM1 Kernel（带 SwiGLU + 量化）

**[grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h](app/zbal/src/csrc/operators/npu/device/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h)**

**AIC 侧职责（Compute Core）：**

1. **Grouped MatMul**：
   - 按 Expert 分组，每组的 M = 该 expert 收到的 token 数
   - 使用 workspace 存储 int32 累加结果（stages=4，支持双缓冲流水线）
   - 通过 `BlockScheduler` 将每组 GEMM 任务分配到各 AIC 核
   - 可选 `EXEC_FLAG_TENSOR_LIST`：权重以 ListTensor 格式提供

2. **Share Expert**（可选）：
   - 先计算 share expert 的 GEMM，使用 share weight
   - 等待 AIV 完成 share token 量化

**AIV 侧职责（Vector Core）：**

分为三类功能核：
- **Send Core**：运行 `SendCoreFunc()`，完成 token 量化 + 发送 token count → 跨 rank 写入 token 数据
- **Recv Core**：运行 `RecvCoreFunc()`，等待远程 token count → 读取远程 token 数据
- **Comp Core**：运行 `CompCoreFunc()`，执行 SwiGLU epilogue + 动态量化

**深融合模式（DEEP_FUSE）核心流程：**

1. `SendCoreFunc()`：对每个 token 做动态量化（Abs→Max→Scale Muls→Round→Cast→int8），写入本地 quant workspace；设置数据就绪 flag
2. AIC 等待每个 Expert 的 token 就绪后，直接从其他 Rank 的 quant workspace 读取量化 token（通过 zbal_ptr 跨 rank 读取）
3. GEMM 计算完成后，`CompCoreFunc()` 执行 SwiGLU epilogue + 量化输出
4. 量化输出直接写入 GMM2 的输入位置，避免中间 GM 往返

**SwiGLU Epilogue 实现**（BlockEpilogue）：

```
C (int32) → Cast→float
  ├─ Left Half:  Mul(PerTokenScale) → Neg → Exp → Add(1) → Div → D
  └─ Right Half: Mul(PerTokenScale) → D
```

- Left half: `x / (1 + exp(-x * dequant_scale))` 即 `x·σ(x)`
- Right half: `x * dequant_scale` 直接输出

**动态量化 BlockQuant**：

```
Input (float) → Abs → ReduceMax → Muls(127/max) → Round → Cast→int8
                                                          └→ dequant_scale = max/127
```

### 4.6 GMM2 Kernel（带 Dequant + Combine）

**[grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h](app/zbal/src/csrc/operators/npu/device/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h)**

**AIC 侧：**
- 与 GMM1 类似的 Grouped MatMul 流水线
- 使用 `MmadAtlasA2PreloadAsyncWithCallbackResidentA` dispatch policy（A 矩阵驻留优化）
- 与 AIV 通过 Soft Sync Flag 同步 GEMM workspace 的读写

**AIV 侧：**
- **Epilogue**：Per-Token Dequant（使用 GMM1 输出的 dequant scale）
- **Combine**：通过 `CamMoeDistributeCombine` 完成：
  1. `PublishCombineGva()`：向其他 Rank 的 state window 发布本地 combine workspace GVA
  2. `AllToAllSend()`：AllToAll 发送 token
  3. `ReducePermute()`：接收 + ReduceSum + Permute 到最终输出
  4. `LoadRemoteGva()`：按需加载远程 Rank 的 combine workspace GVA

**SubBlock 分工**（DEEP_FUSE 模式）：
- SubBlock 0：Epilogue + `AllToAllSend()`
- SubBlock 1：Epilogue + `ReducePermute()`

### 4.7 Dispatch 模块

**[zbal_moe_distribute_dispatch.h](app/zbal/src/csrc/operators/npu/device/fused_deep_moe/raw_distributed/zbal_moe_distribute_dispatch.h)**

`CamMoeDistributeDispatch<XType, ExpandXOutType, StaticQuant, DynamicQuant, IsSmoothScaleExist, IsNeedAllgater, EXEC_FLAG>`

**核心流程：**
1. **AlltoAllDispatch()**：读取 expert_ids → 统计 per-expert token count → 发送 token 到对应 Rank
2. **SetStatus()**：将 per-expert token count 写入各远程 Rank 的 state window
3. **WaitDispatch()**：轮询等待所有远程 Rank 的 token count 就绪
4. **LocalWindowCopy()**：从 data window 读取远程写入的 token → 拷贝到本地 output buffer
5. **(可选) TP AllGather**：通过 TP domain 进行 AllGather
6. **UpdataTokenNumsOut()**：汇总 per-expert token 数量

**优化特性：**
- **AIV Loop 优化**（`enableAivOpt_`）：当 bs ≤ 64 且 expert 数 ≤ 256 时，使用查表法加速 expert 计数
- **双缓冲**（`BUFFER_NUM=2`）：量化流水线使用双缓冲，重叠计算与数据传输
- **动态量化**：Abs → ReduceMax → Muls(127/max) → Round → Cast(int8)
- **Smooth Quant**：量化前乘 smooth scale

### 4.8 Combine 模块

**[zbal_moe_distribute_combine.h](app/zbal/src/csrc/operators/npu/device/fused_deep_moe/raw_distributed/zbal_moe_distribute_combine.h)**

`CamMoeDistributeCombine<ExpandXType, W1ScaleType, W2ScaleType, ExpandIdxType, IsNeedReduceScatter, EXEC_FLAG>`

**核心流程：**
1. **PublishCombineGva()**：发布本地 combine workspace GVA 到其他 Rank 的 state window
2. **SetWaitTpStatusAndDisPatch()**：TP 同步 + ExpertAlltoAllDispatchCopyAdd（按 expert 分发 + ReduceSum）
3. **SetStatus()**：设置完成标志
4. **WaitDispatch()**：等待所有远程 Rank 完成
5. **LocalWindowCopy()**：Token 级 ReduceSum（`scale · x_remote`） + 写入最终输出

**Combine Workspace 机制**（Deep Fuse 模式）：
- 替代传统的 data window
- 每个 remote rank 为所有 expert 写入 token 到本地 combine workspace
- 通过 `LoadRemoteGva()` 按需加载远程 workspace 地址
- 布局：`epRankSize × groupCount × maxBs × tokenHiddenSize × sizeof(ExpandXType)`

---

## 5. 执行模式对比

| 特性 | Shallow 模式 | Deep Fuse 模式 |
|------|-------------|----------------|
| Dispatch | AIV 单独阶段（CrossCoreFlag 同步） | Pull-mode：GEMM 内部拉取 token |
| 通信方式 | Data Window（DataCopy via zbal_ptr） | Quant Workspace + Combine Workspace |
| 同步机制 | PipeBarrier + CrossCoreFlag | Soft Sync Flag + Spin Wait |
| SubBlock 利用 | AIC 单 SubBlock | AIC 2 SubBlocks 分工（Epilogue/Send/Recv） |
| 适用场景 | 标准 MoE 推理/训练 | DeepSeek-V3 等大模型高吞吐场景 |

---

## 6. 数据类型流

```
Host Input           GMM1                    GMM1 Output           GMM2                    GMM2 Output
┌──────────┐      ┌──────────┐            ┌──────────┐         ┌──────────┐            ┌──────────┐
│ Token    │──→──│ Quantize │──→────────│ int8     │──→─────│ Dequant  │──→────────│ bf16/    │
│ bf16/fp16│      │ int8     │            │ Token    │         │ int8×int8│            │ fp16     │
└──────────┘      └──────────┘            │ + Scale  │         │ + Scale  │            └──────────┘
                                          └──────────┘         └──────────┘
                   Weight1: int8 (zN)                          Weight2: int8 (zN)
                   Scale1:  W1ScaleType                        Scale2:  W2ScaleType
```

---

## 7. 同步机制

### 7.1 核间同步

| 机制 | 用途 | 开销 |
|------|------|------|
| `PipeBarrier<PIPE_ALL>` | 同类型核全同步 | 低 |
| `SyncAll<true>()` | 同类型核全同步（含 MTE） | 低 |
| `CrossCoreFlag` | AIC ↔ AIV 同步 | 中 |
| `CrossCoreBarrier` | AIC ↔ AIV 栅栏同步 | 高 |
| Soft Sync Flag（GM 标记轮询） | AIC ↔ AIV 流水线同步 | 中 |

### 7.2 Soft Sync Flag

通过 `stateWindow` 中的 GM 标记实现 AIC/AIV 间的非阻塞同步：
- `EncreaseSyncFlag(addr, idx)`：标记自增（生产者通知消费者）
- `CheckSyncFlag(addr, idx, target)`：轮询等待标记达到目标值

用于 GMM1/GMM2 的 multi-stage workspace 流水线同步。

---

## 8. 关键设计决策

### 8.1 为何 GMM1 输出要量化为 int8？

- GMM1 的 hidden dim 通常较大（如 DeepSeek-V3 中为 18432），中间结果以 int8 存储可将 GMM2 输入显存减少 4×
- 动态量化（per-token）保留了精度，dequant scale 作为 GMM2 epilogue 的输入

### 8.2 为何 GMM2 使用 Resident-A 优化？

- GMM2 的 K 维度通常很小（如 2048），而 N 维度很大（如 7168）
- 当 `kTileCount == L1A_STAGES` 时，A 矩阵可在 L1 中驻留，避免重复加载
- `MmadAtlasA2PreloadAsyncWithCallbackResidentA` 在 A 矩阵地址未变时跳过 L1A 加载

### 8.3 为何 Deep Fuse 模式需要独立 Workspace？

- **Quant Workspace**：Pull-mode dispatch 中，每个 Rank 将量化 token 写入本地 workspace，其他 Rank 通过 zbal_ptr 直接读取。避免了 DataCopy 搬移到 data window 的开销
- **Combine Workspace**：替换 data window，通过 state window 中的 GVA exchange 实现地址共享，`LoadRemoteGva()` 延迟加载减少首次同步开销

### 8.4 为何使用 Template 参数控制 EXEC_FLAG？

- 编译期分支消除：`if constexpr (EXEC_FLAG & EXEC_FLAG_DEEP_FUSE)` 等条件在编译期求值
- 不同 flag 组合编译出独立的 kernel 变体，避免运行时分支开销
- `.cpp` 中通过 switch-case 显式实例化常用 flag 组合（0, 1, 9, 25, 27, 29, 31）

---

## 9. 依赖关系

```
FusedDeepMoe
├── Catlass (csrc/deepep/catlass/)
│   ├── catlass.hpp, arch/arch.hpp, layout/layout.hpp
│   ├── gemm/gemm_type.hpp, block/block_swizzle.hpp
│   └── epilogue/tile/tile_*.hpp
├── ZBAL Communication
│   ├── zbal_kernel_utils.h (CommGroupInfo)
│   └── zbal_kernel_fused_deep_moe_comm.h (ZbalCommContext, zbal_ptr)
├── Distributed Ops
│   ├── raw_distributed/zbal_moe_distribute_dispatch.h
│   └── raw_distributed/zbal_moe_distribute_combine.h
├── Tiling
│   └── fused_deep_moe/zbal_kernel_fused_deep_moe_tiling.h
└── AscendC Runtime
    └── kernel_operator.h, lib/matmul_intf.h
```

---

## 10. 适配说明

本算子从 umdk（统一内存开发套件）的 fused_deep_moe 算子适配而来，主要变更：

1. **通信栈替换**：HCCL → ZBAL GVA
   - `HcclOpResParam` → `ZbalCommContext` + `CommGroupInfo`
   - `GetWindAddrByRankId()` → `GetZbalDataAddr()` / `GetZbalStateAddr()`
   - HCCL 回调 → Soft Sync Flag + CrossCoreFlag

2. **命名空间**：`Cam` → `ZbalCam`

3. **Include 路径**：`umdk/...` → `fused_deep_moe/...`

4. **Catlass 来源**：zbal/third_party → csrc/deepep/catlass/

5. **TensorList 可选化**：原 CANN opbuild 中 `ptrB`/`ptrScale` 固定为 ListTensorDesc，适配后仅在 `EXEC_FLAG_TENSOR_LIST` 时使用
