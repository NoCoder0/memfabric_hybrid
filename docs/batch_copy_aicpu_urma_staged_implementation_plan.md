# sparse_copy_urma URMA 分阶段编码与验证计划

> 状态：待评审
>
> 日期：2026-08-03
>
> 目标方案：[batch_copy_aicpu_urma_design.md](batch_copy_aicpu_urma_design.md)
>
> 本文用途：描述 T0～T3 的整体实现、依赖、测试和硬件验收计划；当前重新实施重点为 T2。

## 1. 总体结论与阶段划分

### 1.1 整体阶段

| 阶段 | 内容 | 当前状态 |
| --- | --- | --- |
| T0 | `HOST_DEVICE_URMA` 协议、公共 URMA/HCOMM 层和 private-data v2 | T0.1 已完成；T0.2 已回退 |
| T1 | Host manager、固定 GVA、Host CPU 数据面和两鲲鹏验证 | 已完成并通过测试 |
| T2 | route/control、publisher、统一 builder、AICPU、acc_offload API 和两卡验证 | 当前重新实现 |
| T3 | 鲲鹏 DDR→昇腾 HBM 的 Host-DDR 硬件验收 | 不新增独立生产代码 |

T0 和 T1 虽然已经完成，仍是本计划的组成部分。其接口、资源生命周期、测试门禁和完成定义继续保留，
用于后续评审和回归，不因当前工作聚焦 T2 而删除。

### 1.2 T2/T3 调整

T2 一次交付 route ABI、publisher、Device-HBM/Host-DDR 统一 builder、`HybmBatchCopy`、acc_offload
`sparse_copy_urma`、正常包和两个 Python example。T3 不增加第二套生产代码，只用 Host-DDR example
完成鲲鹏 DDR→昇腾 HBM 的硬件验收。

因此 T3 不再包含独立 launcher、transport、operator 或 route builder 任务。T3 能取消独立编码的前提是：
T2 已经实现 Host-DDR builder，并在生产代码中强制
`exported GVA == descriptor addr == import view addr`。该门禁不能只在 Python example 中模拟。

### 1.3 调用链调整

最终调用链固定为：

```text
MemFabric HYBM entity 初始化、URMA 建链和 route 发布
    ↓
业务获得远端 MemFabric GVA 和本地真实 HBM 地址
    ↓
mf_acc_offload.sparse_copy_urma(...)
    ↓
pymf_acc_offload.cpp
    ↓
offload_sparse_copy_urma(...)
    ↓
AccOffloadLaunchApi / AccOffloadSparseCopyUrma
    ↓
HybmBatchCopy AICPU
    ↓
固定 HBM route → HybmBatchRead → HCOMM/URMA
```

`copy_data/copy_data_batch`、`DataOpDeviceURMA`、`TransportManager::ReadRemoteBatchCopy` 和
`ComposeTransportManager` 数据操作转发不进入这条调用链。

## 2. 必须遵守的固定决策

### 2.1 T2 重新实现的非目标

- T1 已由另一位同事完成。T2 不得擅自重构、覆盖或复制 `HostUrmaTransportManager` 的实现。
- 不修复 `ComposeTransportManager::Connect()` 未设置 `connected_` 的既有问题。
- 不修改任何现有 `copy_data/copy_data_batch` API、DataOperator 选择或远端拷贝流程。
- 不创建 T2 专用 transport、route probe manager、probe operator、测试 launcher API 或平行 kernel。
- 不为 `sparse_copy_urma` 创建第二个 HYBM entity，不让 acc_offload 接管 route owner、MR、
  channel、thread 或 flag 生命周期。

### 2.2 T0.2 保持回退状态

当前代码继续检查 HCOMM 返回的 `outMem.type` 和 `flagOutMem.type`。整体计划不得：

- 从外层 descriptor 合成或覆盖 HCOMM 返回 type；
- 跳过普通 MR 或 transfer flag 的 type 校验；
- 增加替代 `flagMemoryType` 字段；
- 为旧 T0.2 预期新增 invalid-type UT。

只有硬件验证明确证明当前 HCOMM 返回 type 不可靠，或用户后续明确要求时，才重新设计 T0.2。

### 2.3 route 与生命周期

- modern 路径使用 2 MiB route + 原 32 MiB metadata，总 control 区为 34 MiB。
- legacy 路径保持原 32 MiB，不映射前置 route，也不支持 `sparse_copy_urma`。
- publisher 使用单 owner、completion 注册和 magic-last。
- Publish 成功后重复调用只返回 `BM_OK`，不得刷新 route。
- route 发布后不增加 BatchCopy route MR，不支持热更新、容错补路由或 peer 替换。
- route 发布后 `RemoveRanks()` 必须拒绝，防止 route 引用已释放资源。
- Close 前调用方保证没有在途算子；Close 先清 magic，再释放 completion、channel、thread 和 MR。
- `TryPublishBatchCopyRouteLocked(options)` 暂时同时位于 `Prepare()` 和
  `UpdateRankOptions()` 末尾。
- 将来独立修复 `connected_` 后，删除 `Prepare()` 末尾的发布调用，只保留
  `UpdateRankOptions()`。

### 2.4 AICPU 固定约束

`HybmBatchCopyParam` 只能有四个业务字段：

```cpp
struct HybmBatchCopyParam {
    uint32_t list_num;
    void **dst_buf_addr_list;
    void **src_buf_addr_list;
    uint64_t *len_list;
};
```

thread、channel、remote/local flag、completion、timeout 和 route mode 必须从固定 HBM route/control
区读取，不得扩展为算子参数。

其他固定要求：

- `FindCoveringRange` 先使用顺序查找，不实现二分查找。
- 不实现 `ScopedBatchCopyGuard`、`AcquirePerDeviceExecutionGuard` 或等价防护；调用方保证每卡只有一个
  在途算子。
- `InvalidateDeviceCache`、`FlushDeviceCache` 和 `DeviceMemoryBarrier` 只在 AArch64 AICPU 构建启用，
  实现参考 `app/zbal`。
- `ValidatePublishedRoute` 在硬件联调阶段必须无条件打印 ERROR 日志，输出完整有效 route；
  稳定后删除或降级。
- 目的地址只检查加法溢出和固定 control 区重叠，不使用 `HYBM_DEVICE_VA_START` 作为下界。
  PyTorch/框架分配的真实 HBM 地址可能不在 MemFabric SVM 业务窗口。

## 3. 总体依赖与提交策略

```mermaid
flowchart LR
    A["T0.1 协议与公共 URMA"] --> B["T1.1～T1.4 Host manager 与接入"]
    B --> C["T1.5 两鲲鹏验收"]
    C --> D["T2.1 control 区与 route ABI"]
    D --> E["T2.2 publisher 与 completion"]
    E --> F["T2.3 统一 route source builder"]
    F --> G["T2.4 生产 AICPU 算子"]
    G --> H["T2.5 acc_offload API 与 launcher"]
    H --> I["T2.6 CMake/run/wheel 交付"]
    I --> J["T2.7 Device-HBM 两卡验收"]
    J --> K["T3 Host-DDR 硬件验收"]
```

T0.2 不在依赖图中：其旧实现已经回退，当前计划保持 HCOMM 返回 type 校验，不把它作为 T1/T2 的前置
编码任务。

每项尽量独立提交。提交前必须：

1. 检查 `git status --short`，保护用户已有修改。
2. 检查与本任务相关的调用链和当前主干实现。
3. 执行 `git diff --check`。
4. 只在命令实际成功时记录构建或 UT 通过。

## 4. T0：公共 HCOMM/URMA 层

### 4.1 T0.1 协议、协商和公共描述符

新增并保留以下端到端协议值，既有枚举值不重排：

```cpp
// src/smem/include/host/smem_bm_def.h
SMEMB_DATA_OP_HOST_DEVICE_URMA = 1U << 8,

// src/hybm/include/hybm_def.h
HYBM_DOP_TYPE_HOST_DEVICE_URMA = 1U << 10,
```

Python 暴露为 `BmDataOpType.HOST_DEVICE_URMA`。该 bit 参与 SMEM→HYBM 转换、合法值和冲突校验、
entity tag、rank-to-rank compatible info、动态库加载、Compose transport 选择和 Python 枚举映射。

协议语义：

- `HOST_DEVICE_URMA` 表示 HCOMM Host↔Host 或 Host↔Device；
- `HOST_URMA` 保持原 HCOM 语义；
- `DEVICE_URMA` 保持 Device↔Device HCOMM 语义；
- P0 禁止 `HOST_DEVICE_URMA` 与 `DEVICE_URMA/DEVICE_UBOE` 同时配置。

公共 endpoint 描述符使用完整 `EndpointLoc`：

```cpp
struct UrmaEndpointDesc {
    UrmaProtocol protocol{UrmaProtocol::RESERVED};
    CommAddrType type{COMM_ADDR_TYPE_RESERVED};
    uint8_t raws[URMA_ENDPOINT_RAW_LEN]{};
    EndpointLoc loc{};
};
```

- Host 设置 `loc.locType = ENDPOINT_LOC_TYPE_HOST` 和 `loc.host.id = rankId`。
- Device 设置 `loc.locType = ENDPOINT_LOC_TYPE_DEVICE`，保留物理卡、super device、server 和 pod 信息。
- private-data 版本固定为 v2，序列化前清零，解析时校验 magic、version、payloadLen 和容量。
- Host/Device 对共享结构执行 `sizeof` 和 `std::is_trivially_copyable` 断言。

### 4.2 公共 HCOMM 封装

公共模块位于：

```text
src/hybm/csrc/transport/urma/
├── urma_transport_common.h/.cpp
└── hcomm_transport_manager.h/.cpp
```

统一封装 endpoint、MR reg/unreg、export/import/unimport 和原始 flag descriptor 的导入清理。要求：

| 接口 | 职责 |
| --- | --- |
| `CreateEndpoint` | 把公共 `UrmaEndpointDesc` 转成 HCOMM endpoint |
| `HcommMemReg/HcommMemUnreg` | 注册和注销本地 MR |
| `HcommMemExport` | 导出 HCOMM descriptor，并保持缓存生命周期 |
| `HcommMemImport/HcommMemUnimport` | 导入或释放完整 MemFabric payload |
| `HcommRawMemImport/HcommRawMemUnimport` | 导入或释放 payload 尾部 transfer flag |

- `UrmaExportDesc` 保持 v1 和 48 B 布局，不增加 `flagMemoryType` 或未使用字段。
- 普通 MR 与 payload 尾部 transfer flag 继续使用当前 descriptor 格式。
- descriptor 长度、地址、大小、type 和整数溢出在公共层校验。
- import 后 MemFabric 自身校验失败时，立即 unimport 回滚。
- 根错误日志包含 API、memTag、addr、size、descLen 和 HCOMM 返回码中适用字段。

### 4.3 T0.2 回退后的正式结论

T0.2 原计划曾尝试忽略 HCOMM 返回 type，并从外层 descriptor 恢复 type；该实现已经回退，不属于当前
整体方案。正式行为为：

- 普通 MR 继续检查 HCOMM 返回的 `outMem.type`；
- transfer flag 继续检查 `flagOutMem.type`；
- 同时校验 addr、size、descriptor type 和目标内存类型；
- 不增加 invalid-type 成功路径或 type 合成 helper。

若硬件验证失败，必须保存 CANN/HCOMM 版本、原始返回结构和日志，再单独评审 T0.2，不能在 T2/T3
实现中临时绕过。

### 4.4 动态库与构建

- Ascend NPU 构建为 `HOST_DEVICE_URMA` 加载 RT 和 HCOMM；失败时按逆序清理。
- `XPU_TYPE=NONE` 只加载 HCOMM，不调用 RT/ACL/HAL。
- Host UB plugin 从目标 CANN 安装路径加载；独立调试时才使用显式环境变量指定。
- 公共 URMA 源加入 HYBM/SMEM 正常 CMake 和 UT 构建，不保留 Device 目录下的重复实现。

### 4.5 T0 测试和完成门禁

- 新枚举的 SMEM/HYBM/Python 映射、合法值和冲突组合。
- Host/Device private-data v2 序列化、反序列化、错误版本和短 payload。
- `EndpointLoc` Host/Device 字段原样保留。
- 正确/错误 HCOMM type、空地址、0 长度、短 view 和 unimport 回滚。
- 48 B export header 和 `6 * KEY_SIZE` 容量。
- `XPU_TYPE=NONE` loader 不调用 RT/ACL/HAL。
- 迁移后既有 Device URMA UT 无回归。

T0 完成定义：公共 wire ABI、协议协商和 HCOMM 封装统一；T0.2 保持回退后的当前校验行为。

## 5. T1：HostUrmaTransportManager 与两鲲鹏验证

### 5.1 Host manager 职责

`HostUrmaTransportManager` 位于 `src/hybm/csrc/transport/host/urma/`，负责：

- 从 `TransportOptions.nic` 构建 Host endpoint；
- 创建和注册值为 1 的 8 B Host transfer flag；
- 固定 GVA Host DDR 的注册、导出和 key 查询；
- 两阶段 `Prepare()`、CPU channel 创建和幂等复用；
- 远端 Host MR/flag 导入和严格 GVA equality 校验；
- Host CPU 主动 Read/Write、batch fallback 和 fence；
- 失败回滚及 Close 的逆序清理。

它不依赖本地 NPU，也不加载或启动 AICPU kernel。

公共方法范围保持：

| 方法组 | 行为 |
| --- | --- |
| `OpenDevice/CloseDevice` | 初始化或释放 Host endpoint、flag、MR、channel 和 import |
| `RegisterMemoryRegion/UnregisterMemoryRegion` | 管理本地 Host MR 和引用计数 |
| `QueryHasRegistered/QueryMemoryKey/UpdateMemoryKey` | 查找固定 GVA 区间并导出交换 key |
| `Prepare/Connect/WaitForConnected` | 两阶段创建 channel、导入 key 并完成同步连接 |
| `UpdateRankOptions/RemoveRanks` | 幂等更新；P0 拒绝动态 peer/MR 和删除 |
| `ReadRemote/WriteRemote` | Host CPU 同步单条数据面 |
| `ReadRemoteAsync/WriteRemoteAsync` | Host CPU 异步提交并标记 pending |
| `ReadRemoteBatchAsync/WriteRemoteBatchAsync` | 全量预校验后逐条 fallback |
| `Synchronize` | 每 peer 执行一次 fence |

### 5.2 本地和远端内存

本地注册区分两类：

| 类型 | HCOMM 注册 | 是否可导出 | 地址要求 |
| --- | --- | --- | --- |
| 固定 GVA DDR 池 | 直接注册 `mr.addr` | 是 | 完整落在本 rank Host GVA 分配记录 |
| Host staging MR | 直接注册 HVA | 否 | 只供本地 CPU 数据面 |

相同区间增加引用计数；重叠但不相同的注册失败。`QueryMemoryKey()` 只允许固定 GVA 池区间，导出普通
MR 和 Host flag，并要求 key 地址、descriptor addr 与注册 GVA 一致。

远端状态保存 endpoint、CPU channel、导入 MR、remote flag 和 pending 状态。Host MR 导入要求：

```text
key.keys[1] == export descriptor addr == HCOMM import view addr
```

view size 必须覆盖完整导出区间。失败立即回滚该 import。

关键状态模型：

```cpp
struct LocalRegistration {
    TransportMemoryRegion mr{};
    HcommMemHandle handle{nullptr};
    UrmaMemTag memTag{0};
    uint64_t exportedGva{0};
    uint32_t refCount{0};
};

struct RemoteRegistration {
    uint64_t exportedAddr{0};
    uint64_t size{0};
    UrmaMemTag memTag{0};
    std::vector<uint8_t> descBytes{};
    UrmaCommMem view{};
};

struct RemoteRankState {
    std::mutex mutex{};
    UrmaEndpointDesc endpointDesc{};
    HcommChannelHandle channel{0};
    std::vector<RemoteRegistration> imports{};
    uint64_t remoteFlagAddr{0};
    uint64_t remoteFlagSize{0};
    bool pending{false};
};
```

manager 级 mutex 保护生命周期和 map；同一 peer 的 submit/fence 串行。阻塞 fence 不应长期持有 manager
全局锁。

### 5.3 Prepare、连接和清理

- 第一次 `Prepare()` 只有完整 endpoint 集合，为每个 peer 创建 `COMM_ENGINE_CPU` channel。
- 第二次 `Prepare()` 携带初始全量 memory key，复用 channel 并导入 MR/flag。
- 相同 endpoint/key 的重复调用幂等成功；新增 peer、endpoint 变化或初始化后新增导出区间失败。
- client/server 角色继续按 rank 大小选择，`exchangeAllMems = true`。
- `Connect/AsyncConnect` 在全部初始 peer 已准备后设置连接状态。
- `RemoveRanks()` 在 P0 返回不支持。
- Close 前调用方保证无在途 I/O；manager fence pending peer，再销毁 channel、unimport 远端资源、
  unreg 本地 MR/flag 并销毁 endpoint。

### 5.4 Host CPU 数据面

- `ReadRemoteAsync/WriteRemoteAsync` 校验本地注册范围和远端导入范围。
- Host thread 参数传 0，调用 `HcommReadOnThread/HcommWriteOnThread`。
- 同一 peer 成功提交后设置 pending。
- CPU HCOMM batch 当前不支持，batch API 在完成全量预校验后按输入顺序逐条提交。
- `Synchronize(peer)` 对 pending peer 执行一次 `HcommChannelFenceOnThread`；成功后清 pending。
- 同步 Read/Write 由 Async + Synchronize 组成。
- 提交中途失败可能已有请求在途，pending 不得被错误清除。

### 5.5 Compose、DataOperator 和无卡映射

`HOST_DEVICE_URMA` 的本地角色只解析一次：

- Ascend 950 创建 `DeviceUrmaTransportManager`；
- `XPU_TYPE=NONE` 或 runtime 明确无 device 时创建 `HostUrmaTransportManager`；
- 有 device 但非 Ascend 950，或 runtime/SOC 探测失败时返回错误，不静默回退 Host。

T1 的 Host 数据操作层复用 `HostDataOpRDMA`；Device role 保持主干既有 DataOperator。这里描述的是 T1
已有通用数据面，不表示 T2 的 `sparse_copy_urma` 要接入 DataOperator。

无卡 `MapSlice()` 使用固定 `mmap` 结果，记录 HVA=GVA、DVA=0，不调用 `HalHostRegister()`；回滚也不调用
Host unregister。NPU 路径继续执行当前 HVA→DVA 处理。

### 5.6 两鲲鹏集成验收

保留 T1 example：

```text
examples/memory_pool/02_scale_out/03_multi_node_host_urma_dram/
├── 03_multi_node_host_urma_dram.py
└── README.md
```

两端配置 `BmDataOpType.HOST_DEVICE_URMA`，设置各自 rank、store 和 Host URMA NIC。无卡构建基线：

```bash
bash script/build_and_pack_run.sh \
    --xpu_type NONE \
    --build_hcom ON \
    --build_hcom_rdma OFF
```

覆盖：

1. 两端 Host endpoint 和 CPU channel 建链。
2. rank 0/1 双向单条 Read 和 Write。
3. 四个离散区间的 batch fallback 和单 fence。
4. 1 B、4 KiB、4 MiB、非零 offset 和 MR 尾部恰好结束。
5. 跨 MR、未知 rank、未注册本地地址和错误 vector 长度。
6. 初始化失败回滚、正常 Close 和资源无残留。

### 5.7 T1 完成门禁

- 两端均显示 Host endpoint、`COMM_ENGINE_CPU` 和 thread=0 数据面。
- 每个导出池 MR 满足 GVA、descriptor addr 和 import view addr 三者相等。
- 单条 Read/Write、batch fallback、fence 和双向数据校验通过。
- 无卡构建和相关 UT 通过。
- MR、flag、channel 和 endpoint 在失败与 Close 时按逆序释放。

T1 当前已经完成。后续 T2 只消费其稳定接口；除非 Host 硬件验收暴露根因或用户明确要求，不在 T2
重构或覆盖该实现。

## 6. T2：route、AICPU 与 acc_offload 重新实现

T2 当前在 `feat/aicpu-urma-design-t3` 上重新实现。旧分支
`feat/aicpu-urma-design-t2-new` 以及提交 `b2536f20`、`f33f188b` 只作为已验证行为参考，不整体合并。
实现前比较当前主干，保留 T0/T1 和 acc_offload 已有代码。

旧 T2 的以下内容不再移植：

- `DataOpDeviceURMA::ShouldUseBatchCopyRoute()`、`CopyByBatchCopyRoute()` 和 `BatchCopyByRoute()`；
- `TransportManager::SupportsBatchCopyRoute()`、`ReadRemoteBatchCopy()` 及 Compose 转发；
- manager 内 BatchCopy 输入列表分配、kernel launch 和 stream synchronize；
- probe 环境变量、probe operator 和专用 integration executable。

### 6.1 T2.1：modern control 区与共享 route ABI

#### 6.1.1 control 区

新增或确认以下常量：

```cpp
constexpr uint64_t HYBM_BATCH_COPY_META_SIZE = HYBM_LARGE_PAGE_SIZE;
constexpr uint64_t HYBM_BATCH_COPY_META_ADDR =
    HYBM_DEVICE_META_ADDR - HYBM_BATCH_COPY_META_SIZE;
constexpr uint64_t HYBM_DEVICE_CONTROL_ADDR = HYBM_BATCH_COPY_META_ADDR;
constexpr uint64_t HYBM_DEVICE_CONTROL_SIZE =
    HYBM_BATCH_COPY_META_SIZE + HYBM_DEVICE_INFO_SIZE;
```

实现要求：

- modern 初始化从 `HYBM_DEVICE_CONTROL_ADDR` 映射 34 MiB。
- modern 失败回滚和 uninit 使用相同 34 MiB 边界。
- legacy 初始化、失败回滚和 uninit 继续使用 `HYBM_DEVICE_META_ADDR` 和 32 MiB。
- legacy 请求 `sparse_copy_urma` 能力时明确返回不支持，不能访问未映射 route。
- 现有 entity meta 和 extra context 地址、大小、语义不变。

#### 6.1.2 route ABI

固定容量：

| 项目 | 值 |
| --- | ---: |
| 最大 peer | 64 |
| 每 peer 最大 range | 16 |
| 最大 range 总数 | 1024 |
| route header | 64 B |
| peer entry | 32 B |
| range entry | 32 B |
| route table | `0x8840` B |
| completion area | `0x0200` B |
| control 实际使用 | `0x8A40` B |

`BatchCopyRangeEntry` 同时表达 GVA 和 HCOMM view：

```cpp
struct alignas(32) BatchCopyRangeEntry {
    uint64_t srcGvaBegin;
    uint64_t srcGvaEnd;
    uint64_t hcommVaBegin;
    uint16_t peerIndex;
};
```

Host 与 AICPU 编译均执行 `sizeof/offsetof`、offset 和容量 `static_assert`。

#### 6.1.3 测试门禁

- modern 34 MiB 初始化、失败回滚和 uninit。
- legacy 32 MiB 边界保持不变。
- route/peer/range/completion 的固定 offset 和容量。
- 区间非空、溢出、跨 peer 重叠和容量超限。
- 本阶段不增加 operator 或 probe。

### 6.2 T2.2：BatchCopyRoutePublisher

#### 6.2.1 职责边界

`BatchCopyRoutePublisher` 只负责：

- 校验 `BatchCopyRouteSource`；
- 按 `userDeviceId` 获取单 owner；
- 清 route magic；
- 清零并注册固定 completion 区；
- 构建 magic=0 的完整 image；
- H2D 写入 image；
- 最后单独写 magic；
- Close/失败时清 magic、注销 completion 并释放 owner。

publisher 不负责：

- 创建 channel/thread；
- 导入远端 MR 或 flag；
- 判断 Host/Device route mode；
- 启动 AICPU；
- 发布后的 route 刷新。

#### 6.2.2 关键行为

- 第一次成功 Publish 后设置 published 状态。
- 同一 publisher 再次 Publish 直接返回 `BM_OK`，不比较或覆盖新 sources。
- 同一卡第二个 owner 申请失败，错误只表示 route owner 冲突，不用于算子并发防护。
- 任一步失败后 magic 必须为 0，已创建资源按逆序回滚。
- route table 发布后只读，completion area 是运行期工作区。

#### 6.2.3 测试门禁

- image 字段、排序、range 容量和 peer 索引。
- magic-last，写 image 失败时 magic 为 0。
- completion 清零、注册、注销和失败回滚。
- 单 owner 冲突和 Close 后 owner 可重新获取。
- 重复 Publish 返回 `BM_OK` 且不刷新 route。

### 6.3 T2.3：统一 route source builder 与发布触发

#### 6.3.1 builder 输入和模式

`DeviceUrmaTransportManager` 从完整 `HybmTransPrepareOptions` 和已导入的 remote state 构建 sources。
一次 route 只能是一种 endpoint location：

| 模式 | endpoint | MR 类型 | 地址约束 |
| --- | --- | --- | --- |
| Device-HBM 验证 | `ENDPOINT_LOC_TYPE_DEVICE` | `DEVICE_HBM` | `srcGvaBegin` 为导出 GVA，`hcommVaBegin` 为 import view |
| Host-DDR 生产 | `ENDPOINT_LOC_TYPE_HOST` | `HOST_DRAM` | `key.keys[1] == exportDesc.addr == view.addr` |

共同校验：

- 每个 peer 已有非零 thread、channel 和 remote flag address。
- transfer flag 长度为 8 B，类型继续使用当前 HCOMM 返回 type 校验。
- 每个 peer 有 1～16 个合法 range。
- route 总 peer/range 不超过 64/1024。
- GVA 区间全局排序后不重叠。
- GVA 与 HCOMM view 的长度和 offset 计算不溢出。
- 不允许 Host/Device peer 混合发布。

Host equality 是硬门禁。`hcommVaBegin` 不能用来掩盖 Host import 地址不相等。

#### 6.3.2 发布触发

统一入口：

```cpp
Result DeviceUrmaTransportManager::TryPublishBatchCopyRouteLocked(
    const HybmTransPrepareOptions &options);
```

临时触发规则：

- `Prepare(options)` 末尾尝试发布；
- `UpdateRankOptions(options)` 末尾尝试发布；
- publisher 已成功时重复调用只返回 `BM_OK`。

T2 不修复 `connected_`。未来修复后删除 `Prepare()` 末尾调用。

发布后：

- 新增 route MR 请求返回 `BM_NOT_SUPPORTED`；
- `RemoveRanks()` 返回 `BM_NOT_SUPPORTED`；
- 不刷新、合并或局部修补 route；
- 普通未发布 BatchCopy route 的 Device URMA 行为保持不变。

#### 6.3.3 测试门禁

- Device-HBM GVA 与 import view 相同和不同两种情况。
- Host 三地址完全相等成功，任一不相等失败。
- Host/Device 混合 peer 拒绝。
- flag type/size/address、thread/channel 缺失。
- 两个发布入口幂等，成功后 route 内容不变化。
- 发布后新增 MR 和 RemoveRanks 拒绝。
- 当前 HCOMM `outMem.type/flagOutMem.type` 校验保持不变。

### 6.4 T2.4：生产 HybmBatchCopy AICPU

#### 6.4.1 文件归属

新建：

```text
src/acc_offload/csrc/operators/aicpu/
├── hybm_batch_copy.h
└── hybm_batch_copy.cc
```

`hybm_batch_transfer.{h,cc}` 和共享 route ABI 仍由 HYBM 提供。不得在 HYBM 与 acc_offload 下保留两份
`hybm_batch_copy.cc`。

#### 6.4.2 执行步骤

1. 校验四字段 ABI、三个列表指针、`list_num != 0` 和临时空间乘法溢出。
2. 从固定地址读取 route header，校验 magic、peer/range 数量和固定布局。
3. `ValidatePublishedRoute()` 必打印 ERROR 日志：
   - header magic、peerCount、rangeCount；
   - 每个有效 peer 的 index、thread、channel、remoteFlagAddr；
   - 每个有效 range 的 index、GVA begin/end、HCOMM begin、peerIndex。
4. 全 batch 预校验后才提交第一条 HCOMM：
   - 0 长度条目跳过；
   - 检查源/目的地址加法溢出；
   - 顺序查找完整覆盖源区间的 range；
   - 校验 peer index 和句柄；
   - 计算 `hcommSrc = hcommVaBegin + (srcGva - srcGvaBegin)`；
   - 目的地址不得与 `[HYBM_BATCH_COPY_META_ADDR, SVM_END_ADDR)` 重叠。
5. 按 peer 分组，组内保持输入顺序。
6. 每 peer 清 completion cell，构造内部 `HybmOneSideOpParam`。
7. 复用 `HybmBatchRead()`，每 1000 条分片，batch 不支持时使用既有逐条 fallback。
8. 每 peer fence 并读取 remote flag，最后汇聚全部 completion，超时返回 `BM_TIMEOUT`。

目的地址校验不得要求：

```cpp
destination >= HYBM_DEVICE_VA_START
```

该判断会误拒绝 example 中由框架分配的真实 HBM 地址。

#### 6.4.3 并发和 cache

- 不增加算子 guard 或 `BM_BUSY` 并发返回。
- 调用方保证同一 NPU 只有一个在途 `sparse_copy_urma`。
- AArch64 使用参考 zbal 的 cache invalidate/flush 和 device memory barrier。
- 非 AArch64 构建不执行这些 AArch64 指令，也不加入无验证的替代实现。

#### 6.4.4 测试门禁

- ABI `sizeof/offsetof`。
- 空指针、0 list、长度和地址溢出。
- 顺序查找首项、中间项、末项、未命中和跨 range。
- 真实目的 HBM 地址低于 `HYBM_DEVICE_VA_START` 时不因固定窗口下界失败。
- 目的地址与 control 区重叠时拒绝。
- peer 分组、输入顺序、999/1000/1001 条分片。
- 全零长度 batch。
- completion 成功、提交失败、fence 失败和超时。
- 不创建只供 T2 使用的 operator probe。

### 6.5 T2.5：acc_offload API 与 launcher

#### 6.5.1 公共接口

在 `src/acc_offload/include/host/acc_offload.h` 新增：

```cpp
int32_t offload_sparse_copy_urma(uint64_t srcPtrs,
                                 uint64_t dstPtrs,
                                 uint64_t lenPtrs,
                                 uint32_t listNum,
                                 uint16_t deviceId);
```

外部参数顺序与现有 acc_offload `sparse_copy` 一致，为 `src, dst, len`；内部构造 AICPU 参数时使用
`list_num, dst, src, len` 固定顺序。`listNum` 是 Host scalar，不是 device scalar 指针。

Python 接口：

```python
mf_acc_offload.sparse_copy_urma(
    src_ptrs,
    dst_ptrs,
    len_ptrs,
    list_num,
    device,
)
```

三个 tensor 均位于 NPU：

- `src_ptrs`：`torch.int64`，元素是远端 MemFabric GVA；
- `dst_ptrs`：`torch.int64`，元素是本地 tensor 的真实 `data_ptr()`；
- `len_ptrs`：`torch.int64`，按 `uint64_t` 字节数解释。

#### 6.5.2 launcher

修改：

- `acc_offload.cpp`：直接调用 `AccOffloadLaunchApi`，不经过 `AccOffloadEntryManager`。
- `pymf_acc_offload.cpp`：增加 pybind 定义。
- `mf_acc_offload.py`：把 tensor `data_ptr()`、`list_num` 和 `device.index` 传给 C API。
- `acc_offload_launch.{h,cpp}`：加载可选 `AccOffloadSparseCopyUrma` 符号。
- `acc_offload_operators_launch.cpp`：使用 `NPUGuard` 绑定 device，取得当前 NPU stream，按 device
  缓存 AICPU binary/function handle，启动并同步 `HybmBatchCopy`。

接口不要求 `offload.initialize()`。它只按需加载 launcher；HYBM entity 必须已经由业务完成初始化和
建链，并在调用结束前保持存活。

#### 6.5.3 错误和生命周期

- lazy-load、符号、device、stream、kernel handle 和公共指针错误在提交前返回。
- kernel launch 和 stream synchronize 失败记录 device、stream、kernel 和返回码。
- `BM_OK` 表示同步完成，目的 HBM 数据可见。
- HCOMM 已提交后的失败可能部分写入，调用方丢弃整个 batch 输出。
- launcher cache 按进程生命周期持有，不持有 route/channel/thread/MR。
- 清理前调用方保证没有在途调用。

#### 6.5.4 测试门禁

- C API 空指针、0 list、非法 device 和 loader 未安装。
- pybind 参数顺序与 Python wrapper 的 tensor 地址转换。
- 不调用 `offload.initialize()` 时可独立进入 lazy launcher。
- 现有 `sparse_copy`、`group_pack_copy` 行为不变。
- 当前 stream 提交和同步错误透传。

### 6.6 T2.6：正常 CMake、run 包和 wheel

#### 6.6.1 AICPU 产物

移动后的 `hybm_batch_copy.cc` 继续编入现有：

- `libcann_hybm_kernel.so`；
- `libcann_hybm_kernel.json`；
- `cann-hybm-compat.tar.gz`。

不新增第二个 AICPU so、JSON、run 包或安装目录。

#### 6.6.2 构建和收集规则

需要检查并修改：

| 文件 | 任务 |
| --- | --- |
| `src/hybm/ops/hybm_kernel/CMakeLists.txt` | 从 acc_offload 新目录编译唯一 `hybm_batch_copy.cc` |
| `src/hybm/ops/hybm_kernel/libcann_hybm_kernel.json` | 注册内部符号 `HybmBatchCopy` |
| `src/smem/python/memfabric_hybrid/setup.py` | 将新 AICPU 源和共享 route ABI 加入 wheel 制备白名单 |
| `script/run_pkg_maker/make_run.sh` | 修正 `operators/*` 非递归收集，显式包含 `operators/aicpu/` |
| `script/run_pkg_maker/install.sh` | 安装/编译阶段使用同一份新 AICPU 源 |
| `src/acc_offload/csrc/CMakeLists.txt` | host 库包含新增 API/loader，但继续排除 AICPU 源的 host 编译 |
| pybind CMake/Bazel 文件 | 新公共符号和 wrapper 能进入 Python 扩展 |

#### 6.6.3 交付门禁

- 默认 CMake 构建可找到移动后的唯一源码。
- `libcann_hybm_kernel.json` 中只有一个 `HybmBatchCopy` 注册。
- run 包解包后包含构建所需源码和共享 ABI 头。
- wheel 首次 import 能制备并安装同一 kernel。
- acc_offload 公共头、共享库和 Python wrapper 都包含 `sparse_copy_urma`。
- 不把测试 probe 或测试源码打入产物。

### 6.7 T2.7：两个 Python example

#### 6.7.1 文件布局

```text
examples/kv_offload/sparse_copy_urma/
├── 01_single_node_multi_device_urma.py
├── 02_host_device_urma.py
└── README.md
```

两个 example 共用同一 `sparse_copy_urma` C++/AICPU 实现，不通过不同代码开关区分 T2/T3。

#### 6.7.2 两卡 Device-HBM example

`01_single_node_multi_device_urma.py` 参考
`examples/memory_pool/02_scale_out/01_single_node_multi_device_dram/01_single_node_multi_device_dram.py` 的
进程、barrier、建链和双向 pattern 校验结构。

流程：

1. 两个进程分别绑定一张 Ascend 950。
2. 使用现有 MemFabric API 创建 entity、交换 peer 和建立 Device URMA channel。
3. 在源卡 HBM 写 pattern，并取得远端 MemFabric GVA。
4. 在目的卡通过框架分配输出 tensor，取得真实 `data_ptr()`。
5. 在目的卡构造 NPU 上的 src/dst/len 地址 tensor。
6. 直接调用 `mf_acc_offload.sparse_copy_urma`。
7. 校验数据，随后反向执行。

被测数据传输不得调用 `copy_data/copy_data_batch`。这些 API 只可用于与被测传输无关的现有初始化辅助，
且不能触发 BatchCopy route launcher。

覆盖：

- 单条、多条、非零 offset、range 尾部恰好结束；
- 999、1000、1001 条；
- Device GVA 与 import view 不同；
- 真实目的 HBM 地址不在 MemFabric SVM 业务窗口；
- 未知 GVA、跨 range、magic=0、completion 超时。

#### 6.7.3 Host-DDR→NPU example

`02_host_device_urma.py` 参考 `examples/kv_offload` 的参数组织和 acc_offload 调用风格。

流程：

1. 鲲鹏进程以固定 GVA 分配并注册 Host DDR，写入 pattern。
2. 昇腾进程完成 `HOST_DEVICE_URMA` 建链和 Host MR/flag 导入。
3. 日志确认 `key.keys[1] == exportDesc.addr == view.addr`。
4. 昇腾侧通过框架分配目的 HBM tensor，取得真实地址。
5. 构造 NPU src/dst/len tensor，直接调用 `sparse_copy_urma`。
6. 校验单条、batch、边界、分片和 completion。
7. entity 保持到调用完成，最后先销毁 route owner 所属 entity。

首轮验收顺序：

1. 1 CPU × 1 NPU、1 MR、单条 4 KiB。
2. 1 CPU × 1 NPU、多 MR 和 1000/1001 条。
3. 多 Host peer 分组和 completion 汇聚。
4. 多 NPU 各自独立 route owner。
5. 目标拓扑只在前述门禁稳定后执行。

#### 6.7.4 example 通过标准

- 建链和 route 发布由真实 manager 完成，无环境变量 probe。
- 数据拷贝入口只有 `sparse_copy_urma`。
- 两卡场景验证 GVA→import view 受检转换。
- Host 场景验证三地址严格相等。
- 目的 tensor 使用真实框架 HBM 地址。
- Close 后 magic 清零，通信资源无残留。
- 日志记录 commit、CANN 版本、设备/NIC、route 内容和数据校验结果。

## 7. T3：只保留 Host 硬件验收

T3 不再建立独立编码任务。`02_host_device_urma.py` 是 T3 的验收入口，但 T3 不是“只增加一个 Python
文件”：Host route builder、equality 门禁、operator 和 acc_offload API 必须已经在 T2 生产代码中完成。

T3 发现问题时按以下规则处理：

- Host equality 失败：修复 Host 注册/import 的根因，不使用 `hcommVaBegin` 掩盖。
- route 构建错误：修复 T2 统一 builder，不复制 Host 专用 publisher。
- operator 错误：修复唯一 `HybmBatchCopy`，不创建 Host 变体。
- launcher/打包错误：修复 acc_offload 正常交付链，不在 example 中直接调用私有 ACL API。
- HCOMM type 硬件行为与当前假设冲突：记录证据并单独评审 T0.2，不在 T3 临时跳过校验。

## 8. 文件级修改清单

| 文件或目录 | 计划修改 | 明确不做 |
| --- | --- | --- |
| `src/smem/include/host/smem_bm_def.h`、`src/hybm/include/hybm_def.h` | T0 保留 `HOST_DEVICE_URMA` 枚举 | 不重排既有 bit |
| SMEM/HYBM tag、转换和 Python 枚举文件 | T0 保留新协议的端到端映射 | 不复用 `HOST_URMA` |
| `src/hybm/csrc/transport/urma/` | T0 公共 endpoint/private-data/HCOMM 封装 | 不恢复 Device/Host 双份实现 |
| `src/hybm/csrc/under_api/dl_api.cpp` | T0/T1 保留无卡 HCOMM loader | 不在无卡构建调用 RT/ACL/HAL |
| `src/hybm/csrc/transport/host/urma/` | T1 Host manager、固定 GVA 和 CPU 数据面 | T2 不覆盖同事实现 |
| `src/hybm/csrc/transport/compose/compose_transport_manager.*` | T1 保留 Host/Device 角色选择 | T2 不增加 BatchCopy 数据转发 |
| `src/hybm/csrc/data_operation/host/` | T1 保留既有 Host/Device DataOperator | T2 不接入 BatchCopy launcher |
| `src/hybm/csrc/mm/hybm_conn_based_segment.cpp` | T1 保留无卡固定 GVA 映射 | 不在无卡路径调用 HAL 注册 |
| `examples/memory_pool/02_scale_out/03_multi_node_host_urma_dram/` | T1 两鲲鹏验收 | 不替代 T2/T3 example |
| `src/hybm/csrc/common/hybm_define.h` | modern control 常量 | 不改变 entity meta/extra context ABI |
| `src/hybm/csrc/common/hybm_batch_copy_route.h` | 固定 route ABI 和双基址 range | 不增加动态字段 |
| `src/hybm/csrc/driver/hybm_gva.cpp`、`hybm_entry.cpp` | modern 34 MiB，legacy 32 MiB | 不把 legacy 扩为 34 MiB |
| `src/hybm/csrc/transport/device/urma/batch_copy_route_publisher.*` | owner、completion、magic-last | 不负责建链或 launch |
| `src/hybm/csrc/transport/device/urma/device_urma_transport_manager.*` | 统一 builder 和发布触发 | 不增加 `ReadRemoteBatchCopy` |
| `src/hybm/csrc/transport/host/urma/` 的 T2 增量 | 原则上无；只允许 route 必需且经审查的最小适配 | 不重构 T1 |
| `src/acc_offload/csrc/operators/aicpu/` | 新增唯一生产算子 | 不创建 probe operator |
| `src/acc_offload/include/host/acc_offload.h` | 公共 C API | 不改变现有 API |
| `src/acc_offload/csrc/acc_offload.cpp` | 直接 lazy launcher | 不依赖 EntryManager 初始化 |
| `src/acc_offload/csrc/launch/` | 新 symbol、stream、per-device handle | 不持有 HYBM route 资源 |
| `pymf_acc_offload.cpp`、`mf_acc_offload.py` | Python API | 不改变现有 `sparse_copy` |
| HYBM AICPU CMake/JSON | 编译并注册移动后的源码 | 不新增第二套产物 |
| wheel/run maker | 递归收集新目录 | 不打包测试 probe |
| `examples/kv_offload/sparse_copy_urma/` | 两个验收 example | 不实现私有传输旁路 |
| API/Python/AICPU 文档 | 接口、生命周期和安装说明 | 不把未执行测试写成通过 |

## 9. 编码任务和完成门禁

| 任务 | 内容 | 依赖 | 完成门禁 |
| --- | --- | --- | --- |
| T0.1（已完成） | 枚举/协商、公共 URMA/HCOMM、private-data v2 | 无 | wire ABI 和协议映射 UT |
| T0.2（已回退） | 保持 HCOMM 返回 MR/flag type 校验 | T0.1 | 不保留 type 合成代码和 invalid-type 成功 UT |
| T1.1（已完成） | Host endpoint、flag、MR 注册/导出/清理 | T0.1 | Mock HCOMM 生命周期 UT |
| T1.2（已完成） | Host Prepare、CPU channel、幂等和 rollback | T1.1 | 两阶段 Prepare UT |
| T1.3（已完成） | Host Read/Write/Fence 和 batch fallback | T1.2 | 单条/批量/pending UT |
| T1.4（已完成） | Compose、DataOperator、MapSlice、loader 无卡接入 | T1.3 | 无卡构建和 UT |
| T1.5（已完成） | 两鲲鹏集成 example 和硬件验收 | T1.4 | T1 完成标准 |
| T2.1 | modern 34 MiB、legacy 32 MiB、route ABI | T1.5 | 映射/回滚和 ABI UT |
| T2.2 | publisher、owner、completion、magic-last | T2.1 | image/幂等/失败注入 UT |
| T2.3 | Device-HBM/Host-DDR 统一 builder、临时双触发 | T2.2 | 两种模式、equality、发布后拒绝 UT |
| T2.4 | acc_offload 目录下的生产 `HybmBatchCopy` | T2.3 | 四 ABI、顺序查找、分片、completion UT |
| T2.5 | C/Python `sparse_copy_urma` 和独立 launcher | T2.4 | 不初始化 EntryManager 的接口 UT |
| T2.6 | CMake、JSON、run、wheel 和文档 | T2.5 | 实际构建/安装成功后才能标记完成 |
| T2.7 | 两个 Python example；执行两卡 Device-HBM 验收 | T2.6 | 两卡数据和异常矩阵 |
| T3-A | 执行 Host-DDR→NPU example 硬件验收 | T2.7 | equality、单/多 peer、分片和清理 |

每个任务建议独立提交。禁止用 T3 硬件成功替代 T2 软件门禁，也禁止因本机环境无法构建而把测试状态
写为成功。

## 10. 测试与质量要求

### 10.1 每次修改

阶段回归范围：

| 阶段 | 必须回归的测试域 |
| --- | --- |
| T0 | 协议枚举、private-data v2、公共 HCOMM import/unimport、Device URMA 原功能 |
| T1 | Host manager 生命周期、两阶段 Prepare、CPU I/O、无卡 MapSlice/loader |
| T2 | route ABI/publisher/builder、AICPU、acc_offload API、打包和两个 example |
| T3 | Host equality、跨节点数据、completion 和清理 |

```bash
git status --short
git diff --check
```

根据实际变更选择：

```bash
bash script/run_ut.sh --fast BatchCopyRoute
bash script/run_ut.sh --fast BatchCopy
bash script/run_ut.sh --fast AccOffload
bash script/ci-pre-commit-pr.sh
```

若过滤器名称尚不存在，先通过 `rg` 查找真实测试名，不得把计划名称直接当成已执行命令。

### 10.2 构建

默认：

```bash
bash script/build_and_pack_run.sh
```

AICPU run 包和 wheel 需要分别验证。现有 build 目录曾缺少 `CMakeFiles/rules.ninja`；遇到该情况时：

1. 记录命令、退出码和缺失文件。
2. 不声称编译、UT 或安装通过。
3. 只在用户允许和环境具备依赖时重新生成 build。

### 10.3 失败注入

至少覆盖：

- private-data magic/version/payload 错误和协议冲突；
- Host endpoint、flag、MR 注册/导出/导入和 CPU channel 创建失败；
- Host Read/Write/Fence 失败及 pending 状态；
- route owner 冲突；
- completion 注册/注销失败；
- HBM image 写入和 magic 写入失败；
- 第二个 peer 或 MR 导入失败；
- Host equality 失败；
- HCOMM submit、fence、remote flag read 和 timeout；
- AICPU binary/symbol/stream/launch/synchronize 失败；
- run/wheel 缺少移动后的源码或 ABI 头。

每个根错误记录 ERROR 日志和真实关键参数，上层只透传已记录错误时不重复打印。

### 10.4 代码规范

- 新增函数尽量不超过 50 行非空非注释代码。
- 嵌套深度不超过 4 层。
- 行宽不超过 120 字符。
- 建链、builder、publisher、launcher 和 operator helper 保持单一职责。
- 不修改用户无关文件，不覆盖脏工作区内容。
- 不新增只为测试暴露的生产 API。

## 11. 风险与处理

| 风险 | 影响 | 处理 |
| --- | --- | --- |
| Host/Device private-data 版本混用 | endpoint location 解析错误 | 严格校验 v2，创建 channel 前拒绝 |
| Host UB plugin 缺失或未加载 | T1/T3 Host endpoint 创建失败 | 启动前检查 CANN 安装和 HCOMM 日志 |
| 无卡路径误调用 RT/ACL/HAL | 鲲鹏初始化失败 | `XPU_TYPE=NONE` 只加载 HCOMM，MapSlice 不注册 HAL |
| Host 固定 GVA mmap 失败 | Host MR 无法满足地址契约 | 初始化失败并拒绝导出，不回退普通 HVA |
| 旧 T2 整体移植覆盖主干 T1/acc_offload | 回归或重复实现 | 逐任务参考提交，禁止整体合并 |
| Host equality 失败 | Host route 访问错误地址 | 硬门禁失败，不用地址转换兼容 |
| Device GVA 与 import view 不同 | 两卡 route 命中后访问错误 | range 保存双基址并做受检 offset |
| framework HBM 地址不在 SVM 窗口 | 合法目的 tensor 被误拒绝 | 取消 `HYBM_DEVICE_VA_START` 下界，只保护 control 区 |
| legacy 未映射 route | AICPU 访问未映射地址 | legacy 保持 32 MiB并明确不支持 |
| 同卡多 publisher | route 被覆盖 | 单 owner；第二发布者失败 |
| caller 并发调用 | completion 工作区竞争 | P0 调用方串行，不增加算子 guard |
| 临时 ERROR route 日志过多 | 热路径日志膨胀 | 硬件稳定后删除或降级 |
| acc_offload lazy launcher 与 entity 生命周期分离 | route 未就绪或提前释放 | magic 校验；example 保持 entity 存活 |
| 新 AICPU 目录未被 run/wheel 收集 | 产物缺符号 | 三条交付链分别做安装验证 |
| 当前 HCOMM type 行为变化 | import 被拒绝 | 保留 T0.2；有硬件证据后单独评审 |
| `connected_` 既有问题 | 发布入口时序不稳定 | 临时双触发，不在本任务修复 |

## 12. 阶段完成定义

- **T0 完成：** `HOST_DEVICE_URMA` 完成端到端协商，Host/Device 共用 private-data v2 和公共 HCOMM
  封装；当前 MR/flag type 校验保持不变，旧 T0.2 代码不残留。
- **T1 完成：** 两台鲲鹏能够双向 Read/Write、batch fallback 和 fence，并证明固定 DDR
  `GVA == descriptor addr == import view addr`；无卡资源可完整回滚和清理。
- **T2 代码完成：** route control、publisher、两种 builder、唯一 AICPU 算子、acc_offload API 和交付链
  均进入生产目录；`copy_data` 调用链无 BatchCopy 增量。
- **T2 软件完成：** ABI、builder、publisher、operator、launcher 和失败注入 UT 实际通过，
  `git diff --check` 和适用预提交检查通过。
- **T2 硬件完成：** `01_single_node_multi_device_urma.py` 在两张 Ascend 950 上直接通过
  `sparse_copy_urma` 完成数据校验，无 probe。
- **T3 验收完成：** `02_host_device_urma.py` 证明 Host 三地址相等，并完成鲲鹏 DDR→NPU 的单/多 peer、
  1000/1001 条、异常和清理验证。

任何阶段未达到完成定义时，后续阶段成功不能替代前一阶段门禁。

## 13. 实现时参考

MemFabric：

- `AGENTS.md`
- `docs/batch_copy_aicpu_urma_design.md`
- `src/smem/include/host/smem_bm_def.h`
- `src/hybm/include/hybm_def.h`
- `src/hybm/csrc/entity/hybm_entity_tag_info.cpp`
- `src/hybm/csrc/transport/urma/`
- `src/hybm/csrc/transport/host/urma/host_urma_transport_manager.{h,cpp}`
- `src/hybm/csrc/transport/compose/compose_transport_manager.cpp`
- `src/hybm/csrc/data_operation/host/hybm_data_op_host_rdma.cpp`
- `src/hybm/csrc/mm/hybm_conn_based_segment.cpp`
- `src/acc_offload/csrc/python_wrapper/pymf_acc_offload.cpp`
- `src/acc_offload/csrc/acc_offload.cpp`
- `src/acc_offload/csrc/launch/acc_offload_launch.{h,cpp}`
- `src/acc_offload/csrc/launch/acc_offload_operators_launch.cpp`
- `src/acc_offload/include/host/acc_offload.h`
- `src/smem/python/memfabric_hybrid/memfabric_hybrid/mf_acc_offload.py`
- `src/hybm/csrc/transport/device/urma/device_urma_transport_manager.{h,cpp}`
- `src/hybm/csrc/transport/device/urma/batch_copy_route_publisher.{h,cpp}`
- `src/hybm/ops/hybm_kernel/hybm_batch_transfer.{h,cc}`
- `src/hybm/ops/hybm_kernel/libcann_hybm_kernel.json`
- `examples/memory_pool/02_scale_out/03_multi_node_host_urma_dram/`
- `examples/memory_pool/02_scale_out/01_single_node_multi_device_dram/`
- `examples/kv_offload/`
- 提交 `b2536f20`、`f33f188b` 和旧 T2 分支的硬件验证记录。

CANN/HCOMM：

- `include/hcomm_res_defs.h`
- Host UB plugin、CPU URMA endpoint/channel 和 comm_mem 实现。
- Ascend 950 AICPU kernel binary load、function handle 和 stream launch 接口。
- `app/zbal` 中 AArch64 cache 和 device memory barrier 的参考实现。
