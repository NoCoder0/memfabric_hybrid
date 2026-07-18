# 鲲鹏 DDR 到昇腾 950 HBM 的 Batch_Copy AICPU 算子技术方案

**Authors:** MemFabric Hybrid URMA Maintainers

**Created:** 2026-07-15

**Updated:** 2026-07-17

**Status:** Draft

**Related Issue/PR:** 无

---

# 1. 概述

## 1.1 简介

本方案面向鲲鹏服务器与昇腾 950 NPU 组成的超节点，为卡侧发起的 URMA 读提供
`Batch_Copy` AICPU 算子。算子逻辑输入为鲲鹏 DDR 源地址列表、昇腾 HBM 目的地址列表、
长度列表和列表元素个数，按源地址解析对应的 HCOMM `channel/thread`，将远端鲲鹏 DDR
数据批量读取到本地 NPU HBM。

方案在每张 NPU 上维护一份只由 MemFabric 控制面写入的 HBM 路由表。URMA 建链和远端
内存导入成功后，Host 侧把 CPU peer、地址区间、地址转换关系及 `channel/thread` 写入
Batch_Copy 所属 entity 的 64 KiB extra context，并最后发布一个固定位置的路由根描述。
AICPU 算子不接收内部通信句柄，而是通过路由根定位只读快照，按 CPU peer 对 batch 分组，
然后调用 `HcommBatchTransferOnThread`；接口不支持时回退到 `HcommReadOnThread`。


## 1.2 目标与限制

### 目标

- 新增 `Batch_Copy` AICPU 算子，仅暴露三组列表和列表元素个数。
- 支持一张 NPU 与最多 16 个鲲鹏 CPU peer 建立 URMA 通信。
- 支持 16 张 NPU、8 台双路鲲鹏服务器，即 16 个 CPU peer 的超节点规模。
- 建链成功后自动把地址范围与 `channel/thread` 的对应关系发布到 HBM。
- 单次 batch 可包含来自不同 CPU peer 的源地址，由算子完成查表、分组和传输。
- 保持现有 `HybmBatchRead`、`HybmBatchWrite` 及 Host 侧数据面接口兼容。
- 对参数错误、路由缺失、句柄失效、HCOMM 失败和完成超时提供可定位的 ERROR 日志。

### 限制

- 不在算子中动态建链、注册内存或导入远端内存。
- 不支持建链完成后动态增加内存区间，也不支持 peer 移除、主动断链或路由热更新。
- 初始化建链和路由发布串行执行，不支持初始化流程并发。
- 不支持同一张 NPU 上多个 Batch_Copy 实例并发执行。

---

# 2. 用例分析

## 2.1 核心用例

| 用例 | 功能要求 | 验收重点 |
| --- | --- | --- |
| 单 CPU 单地址 | 从一个鲲鹏 DDR 区间读取到一个 HBM 区间 | 地址转换、单条 HCOMM 读、完成语义 |
| 单 CPU 多地址 | 同一 peer 的多个离散 DDR 地址批量读取 | 优先使用 HCOMM batch，一次 fence |
| 多 CPU 混合 batch | 一个列表中包含不同 CPU peer 的地址 | 正确查表分组，每个 peer 使用自身句柄 |
| 最大拓扑 | 每张 NPU 连接 16 个 CPU peer | 表容量和初始化失败回滚 |
| 异常输入 | 越界、溢出、空指针、无路由地址 | 传输前失败，不提交任何 HCOMM 请求 |


## 2.2 安全、可靠性与兼容性要求

- 源 GVA 的完整 `[src, src + len)` 必须落在一个已发布路由区间内。
- 目的地址必须位于本地 HBM 有效范围，且不得覆盖 HYBM meta 或任一 entity extra context。
- 地址加法必须检查 `uint64_t` 溢出，长度为 0 的条目按 no-op 跳过。
- Batch_Copy 所属 entity 的 extra context 由内部路由表独占，公共 `hybm_set_extra_context()`
  必须拒绝覆盖该 entity 的内容。
- 根错误必须记录 peer rank、地址、长度、channel、thread、batch index 和返回码中的相关字段。

---

# 3. 方案设计

## 3.1 总体方案

### 3.1.1 架构

```mermaid
flowchart LR
    subgraph Host["Host 控制面"]
        P["Host/Device UrmaTransportManager::Prepare"]
        C["两端创建 HCOMM channel，NPU 分配 thread"]
        I["NPU 导入鲲鹏 DDR MR"]
        B["NPU 构建 route，entity 发布"]
    end

    subgraph NPU["昇腾 950 NPU"]
        T["entity 64 KiB extra context 路由表"]
        R["固定 HYBM meta 中的路由根"]
        O["Batch_Copy AICPU 算子"]
        G["按 peer 分组"]
        H["HCOMM Batch Read / ReadOnThread"]
        D["目的 HBM"]
    end

    K["鲲鹏 DDR"]

    P --> C --> I --> B --> T --> R
    O --> R --> T
    O --> G --> H
    K --> H --> D
```

控制面负责资源创建、远端 MR 导入和路由发布；数据面只读路由表并使用已发布资源。
每张 NPU 的路由表最多记录 16 个 CPU peer。16 张 NPU 各自持有本地句柄，因此系统最多
存在 16 × 16 组 NPU 到 CPU 的 channel/thread 关系，但单张表仍只有 16 个 peer。

### 3.1.2 地址语义

`src_ddr_ptr_list` 中的元素定义为 MemFabric GVA，而不是鲲鹏进程本地 VA。
GVA 必须在超节点内唯一，且与 `RemoteRegistration::addr/size` 使用同一地址空间。

目标场景不复用“昇腾本机 Host 内存供卡侧 URMA 访问”的 DVA 注册路径。当前
`MapSlice()` 中昇腾 950 使用 `HalMemAlloc()`，以及 `RegisterMemoryRegion()` 将 HOST_DRAM 的 HVA
转换为 DVA，都是为本机 NPU 访问 Host 内存服务；鲲鹏 DDR 作为远端源时不需要这层转换。

P0 对鲲鹏 DDR 源端采用 GVA 固定地址注册：

1. 去除目标分支中昇腾 950 强制 `HalMemAlloc()` 的行为，鲲鹏侧使用 `mmap()` 把 DDR 映射到
   `sliceAddr`；源端要求 `sliceAddr == GVA`，映射返回其他地址或失败时终止初始化。
2. 该源端路径不调用 `HalHostRegister()`，也不生成 DVA。Batch_Copy 源 DDR 注册时增加内部
   `REG_MR_FLAG_GVA_DIRECT` 标志，`HcommMemReg()` 据此直接注册 `mr.addr`，并要求 `mr.addr == GVA`；
   未设置该标志的既有 HOST_DRAM 注册继续执行 HVA→DVA 转换。
3. `QueryMemoryKey()` 导出的 `key.keys[1]` 与 HCOMM 实际注册地址使用同一个 GVA。
4. NPU 侧 `ImportRemoteMemKeysLocked()` 从 key 恢复 GVA，调用 `HcommMemImport()` 后校验
   `view.addr == remoteAddr` 且 `view.size >= remoteSize`。不相等时返回 `BM_NOT_SUPPORTED`，不发布路由。

固定地址 mmap 只适用于 GVA 落在鲲鹏进程可映射 VA 范围且地址未被占用的配置。当前
`enable56BitsGva` 路径把 GVA 与本地可访问地址分离，不能直接满足该约束；应禁用该模式，或在
后续实现前提供一段经平台验证、可由鲲鹏进程固定映射的 GVA 地址窗口。

对目标使用的 UBC_TP/UBC_CTP 后端，CANN/HCOMM 主线和 Host plugin 源码都可以确认
`HcommMemImport()` 不会生成另一段远端 VA：

1. `LocalUbRmaBuffer::GetExchangeDto()` 把 `buf->GetAddr()` 原样写入 `ExchangeUbBufferDto::addr`；
2. `RemoteUbRmaBuffer(rdmaHandle, dto)` 把 `dto.addr` 原样保存到 `addr`，底层
   `HrtRaUbRemoteMemImport()` 返回的 `targetSegVa` 另存为 `segVa`，不会覆盖 `addr`；
3. `UbRegedMemMgr::MemoryImport()` 最终执行
   `outMem->addr = reinterpret_cast<void *>(remoteUbRmaBuffer->GetAddr())`。

因此该后端满足：

```text
HcommMemImport.outMem.addr == 对端传给 HcommMemReg 的地址
```

它并不无条件等于 MemFabric GVA；只有鲲鹏端固定 `mmap` 到 GVA，并以该地址调用
`HcommMemReg()` 时，导入结果才等于 GVA。P0 同时保留 `view.addr == remoteAddr` 和
`view.size >= remoteSize` 校验，用于防止 CANN 后端变化、描述符损坏或注册路径误用。路由区间只保存
调用者可见的 `srcGvaBegin/srcGvaEnd`，算子直接使用输入 GVA：

```text
hcommSrc = srcGva
```

因此 `BatchCopyRangeEntry` 删除 `hcommVaBegin`。现有通用 `RemoteIo()`/`RemoteIoBatch()` 仍可保留
`RemoteRegistration::view` 和基址加偏移逻辑，避免影响非 Batch_Copy 或非固定地址注册场景。

当前 UBC 的 `UbRegedMemMgr::MemoryImport()` 只回填 `outMem.addr/size`，没有回填 `outMem.type`；零
初始化会使 type 看起来像 `COMM_MEM_TYPE_DEVICE`。因此不能用 `outMem.type` 判断鲲鹏 DDR。
`HcommTransportManager::HcommMemImport()` 应以 MemFabric 外层 `UrmaExportDesc.memoryType` 设置
`view.type`，并校验其为 `HOST_DRAM`；快照构建器只收录这类区间。`DEVICE_HBM` 继续使用现有设备到
设备数据路径，不进入 Batch_Copy DDR 路由表。

若不同 CPU peer 的源地址范围发生重叠，Host 发布前直接失败。仅凭四个输入无法区分地址
重叠的 peer，不能通过“选择第一个命中项”规避该问题。

### 3.1.3 复用 entity extra context

复用现有每 entity 64 KiB extra context：

```text
routeBase = HYBM_DEVICE_USER_CONTEXT_ADDR
          + ownerEntityId * HYBM_DEVICE_USER_CONTEXT_PRE_SIZE
```

`MemEntityDefault::SetExtraContext()` 已完成 H2D 拷贝，并通过 `UpdateHybmDeviceInfo()` 更新
`HybmDeviceMeta::extraContextSize`。Batch_Copy 提取并复用其中的拷贝 helper，但 route owner 的 extra
context 改为 MemFabric 内部独占；公共 `smem_shm_set_extra_context()`/`hybm_set_extra_context()` 对该
entity 返回 `BM_NOT_SUPPORTED`。P0 同一逻辑 NPU 只允许一个 route owner。

AICPU 从 `HybmDeviceGlobalMeta::reserved` 起始处读取最小路由根，不改变 128 B global meta 总大小：

```cpp
struct alignas(8) BatchCopyRouteRoot {
    uint32_t magic;
    uint16_t ownerEntityId;
};

static_assert(sizeof(BatchCopyRouteRoot) == 8);
```

结构中没有版本、状态、selector、generation、CRC、地址和扩缩容预留字段。`routeBase` 由
`ownerEntityId` 按固定公式计算，route 大小由 header 中的两个数量计算。初始化时先写完整 route，
再写 `ownerEntityId`，最后写 `magic`；`magic` 等于 `BATCH_COPY_ROUTE_MAGIC` 即表示可用。关闭时先把
`magic` 清零，再释放 HCOMM 资源。当前初始化无并发、运行期路由不可变，不需要 READY 状态机。

### 3.1.4 路由表布局

路由表使用紧凑的变长布局，不固定分配 512 个 range，也不保留后续扩缩容空间：

```mermaid
flowchart TB
    GM["HYBM global meta<br/>reserved 起始 8 B"] --> ROOT["BatchCopyRouteRoot<br/>magic + ownerEntityId"]
    ROOT -->|"计算 routeBase"| CTX["owner entity 的 64 KiB extra context"]
    CTX --> H["RouteHeader：8 B"]
    H --> P["PeerEntry：peerCount × 24 B"]
    P --> R["RangeEntry：rangeCount × 24 B"]
    R --> C["CompletionCell：peerCount × 8 B"]
```

具体地址关系如下，所有边界天然按 8 字节对齐：

```text
routeBase
  + 0                                      BatchCopyRouteHeader
  + 8                                      peerBase
  + 8 + peerCount * 24                     rangeBase
  + 8 + peerCount * 24 + rangeCount * 24   completionBase
  + 8 + peerCount * 32 + rangeCount * 24   routeEnd

约束：routeEnd - routeBase <= HYBM_DEVICE_USER_CONTEXT_PRE_SIZE (64 KiB)
```

只保留执行所需字段：

```cpp
struct alignas(8) BatchCopyRouteHeader {
    uint16_t peerCount;
    uint16_t rangeCount;
};

struct alignas(8) BatchCopyPeerEntry {
    uint64_t thread;
    uint64_t channel;
    uint64_t remoteFlagAddr;
};

struct alignas(8) BatchCopyRangeEntry {
    uint64_t srcGvaBegin;
    uint64_t srcGvaEnd;
    uint16_t peerIndex;
};

static_assert(sizeof(BatchCopyRouteHeader) == 8);
static_assert(sizeof(BatchCopyPeerEntry) == 24);
static_assert(sizeof(BatchCopyRangeEntry) == 24);
```

编译器产生的尾部 padding 只用于 8 字节对齐，不是可扩展字段。`remoteFlagSize` 固定为
`sizeof(int64_t)`，在发布前校验，不进入表；completion cell 同样是一个 `int64_t`。Peer 数量由目标
约束限制为 16。Range 数量不再使用缺少需求依据的“512”常量，而由 64 KiB 实际可用空间限制；
当 `peerCount == 16` 时最多可容纳 2709 个 range。`peerRank` 只用于 Host 建链和日志，AICPU 执行
不需要它，因此不写入 PeerEntry；算子错误日志使用 `peerIndex` 定位。

Range entries 按 `srcGvaBegin` 升序排列，发布前检查区间非空、不溢出、不重叠，并检查
`peerIndex < peerCount`。多个 MR 可以指向同一 peer entry。route 写入 HBM 后，仅将实际
`peerCount * sizeof(int64_t)` 大小的 completion 区域注册到本地 HCOMM endpoint；注册句柄只保留在
Host 控制对象中，不写入路由表。

### 3.1.5 建链与路由表发布

#### Host endpoint 填写规则

当前 `UrmaEndpointDesc` 只有 device 位置字段，`ToHcommEndpointDesc()` 还硬编码
`ENDPOINT_LOC_TYPE_DEVICE`。应把位置改成 HCOMM 原生 `EndpointLoc`：

```cpp
struct UrmaEndpointDesc {
    UrmaProtocol protocol;
    CommAddrType type;
    uint8_t raws[URMA_ENDPOINT_RAW_LEN];
    EndpointLoc loc;
};
```

- 昇腾侧设置 `loc.locType = ENDPOINT_LOC_TYPE_DEVICE`，并填写 `loc.device.devPhyId`、
  `superDevId`、`serverIdx` 和 `superPodIdx`；
- 鲲鹏侧设置 `loc.locType = ENDPOINT_LOC_TYPE_HOST`，只填写 `loc.host.id = rankId`；初始化结构时
  先清零，不能把 `superPodId` 等 device 字段填成伪造值；
- `EndpointLoc.host.id` 是 Host endpoint 的普通拓扑标识，可以使用全局唯一的 `rankId`。它与
  extra context 槽位的 `entityId` 不是同一概念。

CANN 的 endpoint factory 会为 HOST + UBC_TP/UBC_CTP 选择 `CpuUrmaEndpoint`，CPU channel 使用
`COMM_ENGINE_CPU`。但当前主线 `CpuUrmaEndpoint::Init()` 仍调用 `hrtGetDevice()` 和
`hrtGetDevicePhyIdByIndex()`，不能直接运行在无卡鲲鹏上；`experimental/base_comm/nic_plugin` 中的
Host UB plugin 已改用 `kHostResourceId` 和 Host NIC，不依赖 device。P0 部署必须使用该 Host plugin
路径，或使用已经合入等价无卡实现的 CANN 版本，否则这是建链阻塞项。

#### DeviceUrmaTransportManager 全函数审计与拆分结论

`DeviceUrmaTransportManager` 的生命周期、线程上下文和数据面大部分依赖 ACL/NPU。若在现有类中
增加 HOST 分支，会使几乎每个阶段出现平台判断。结论是新增独立
`HostUrmaTransportManager`，与 Device manager 组合复用 `HcommTransportManager`；不让 Host 类继承
Device 类。现有 `host::HcomTransportManager` 使用旧 HCOM Host 数据面，不能替代基于
`HcommEndpoint/HcommMemExport` 的新 Host endpoint。当前 `HcommTransportManager` 和
`UrmaEndpointDesc` 位于 `transport::device` 命名空间，也应迁移到中立的 `transport::urma` 公共目录，
避免 Host manager 反向依赖 device 模块。

全部函数按职责审计如下：

| 函数或函数组 | Device 特有内容 | HostUrma 处理 |
| --- | --- | --- |
| `~DeviceUrmaTransportManager()`、`OpenDevice()`、`CloseDevice()`、`RollbackOpenDeviceLocked()`、`CloseDeviceCleanupResourcesLocked()` | Ascend 950 检查、ACL 和 device kernel 生命周期 | Host 类实现独立生命周期，只清理 Host 内存、HCOMM endpoint/channel/MR |
| `InitLocalDeviceInfoLocked()`、`BuildLocalEndpointDescLocked()`、`OpenEndpointResourcesLocked()`、`CreateEndpointAndInitResourcesLocked()` | 查询逻辑/物理卡、SDID、serverId、superPodId 和设备 EID | 新增 `InitLocalHostInfoLocked()`、`BuildLocalHostEndpointDescLocked()`，读取 Host NIC 地址并填写 `loc.host.id` |
| `InitDeviceTransferFlagLocked()`、`EnsureDeviceKernelLoadedLocked()` | `AclrtMalloc/AclrtMemcpy` 和 AICPU kernel handle | 新增 `InitHostTransferFlagLocked()`，分配 8 B Host flag，以 `COMM_MEM_TYPE_HOST` 注册；不加载 kernel |
| `GetTlsBindings()`、`LookupOrCreateContextLocked()`、`CreateAndPublishContextLocked()`、`EnsureContextInitLocked()`、`RollbackContextInitLocked()`、`CleanupContextLocked()`、`FindCurrentContextLocked()`、`ReleasePendingTransfersLocked()`、`SynchronizeContextLocked()`、`IsAnyRegistryContextPendingForRank()` | ACL stream、device notify、TLS completion context | 鲲鹏源端不发起数据搬运，Host 类不实现这些上下文 |
| `FindLocalRegistrationLocked()`、`RegisterMemoryRegion()`、`UnregisterMemoryRegion()`、`QueryHasRegistered()`、`QueryMemoryKey()`、`UpdateMemoryKey()`、`CleanupLocalRegistrationsLocked()` | Device 类对本机 HOST_DRAM 可能执行 HVA→DVA | Host 类直接注册固定 mmap 后的 GVA/HVA；抽取范围查找、描述符序列化和 refCount helper 复用 |
| `FindRemoteRegistrationLocked()`、`ImportRemoteMemKeysLocked()`、`UnimportPeerImportsAndFlag()` | NPU 导入鲲鹏 MR/flag，并保存 AICPU 使用的 view | 保留在 Device 类；Host P0 不需要导入 NPU HBM，只导出本地 DDR/flag |
| `Prepare()`、`DestroyRankChannelsAndThread()`、`CleanupPeerRankState()`、`UnregisterPeerHandlesAndDestroyEndpoint()` | 分配 `COMM_ENGINE_AICPU_TS` thread，创建 `COMM_ENGINE_AICPU` channel | Host 类的 `Prepare()` 创建 `COMM_ENGINE_CPU` channel，不分配或发布 AICPU thread |
| `RemoveRankLocked()`、`RemoveRanks()`、`UpdateRankOptions()` | 动态 peer/MR 管理及在途检查 | P0 明确不支持动态变化，Host 类返回 `BM_NOT_SUPPORTED`；Device 既有行为不用于路由热更新 |
| `Connect()`、`AsyncConnect()`、`WaitForConnected()`、`GetNic()`、`GetPrivateData()` | 当前多数是状态或 endpoint 描述封装 | 两个 manager 分别实现；私有数据序列化必须携带正确的 `EndpointLoc` |
| `RemoteIo()`、`ReadRemote()`、`WriteRemote()`、`ReadRemoteAsync()`、`WriteRemoteAsync()`、`ResolveBatchIoAddressesLocked()`、`RemoteIoBatch()`、`ReadRemoteBatchAsync()`、`WriteRemoteBatchAsync()` | 通过 AICPU kernel 发起 device 数据面 | Host P0 不承担数据面，接口返回 `BM_NOT_SUPPORTED` |
| `StageAndLaunchTransfer()`、`GetDeviceKernelFunc()`、`ReleaseDeviceTransferBuffers()`、`PrepareKernelLaunchBuffers()`、`LaunchDeviceKernelBatch()`、`Synchronize()` | H2D 参数 staging、kernel launch、ACL stream 同步 | 仅保留在 Device 类，Host 类不复制这些函数 |

新增 `TransportOptions::endpointLocType`，由本地平台初始化层明确设置为 HOST 或 DEVICE，不能根据
`rankId`、`entityId` 或远端类型猜测。`ComposeTransportManager::OpenDeviceTransport()` 在
DEVICE_URMA/UBC_CTP 场景根据该字段选择 `DeviceUrmaTransportManager` 或
`HostUrmaTransportManager`。现有 `HcommTransportManager` 继续作为两者共用的 HCOMM API 和描述符
封装层。

#### 建链和发布时序

```mermaid
sequenceDiagram
    participant CE as 鲲鹏 MemEntityDefault
    participant CC as 鲲鹏 ComposeTransportManager
    participant HM as HostUrmaTransportManager
    participant H as HCOMM Host plugin
    participant NE as NPU MemEntityDefault
    participant NC as NPU ComposeTransportManager
    participant DM as DeviceUrmaTransportManager
    participant X as entity extra context
    participant G as HYBM global meta

    CE->>CC: InitTransManager() / OpenDeviceTransport(HOST)
    CC->>HM: OpenDevice(options)
    HM->>HM: InitLocalHostInfoLocked()
    HM->>H: CreateEndpoint(BuildLocalHostEndpointDescLocked())
    CE->>HM: RegisterMemoryRegion(GVA_DIRECT)
    HM->>H: HcommMemReg(GVA, HOST)
    HM->>H: HcommMemExport(DDR + Host flag)

    NE->>NC: InitTransManager() / OpenDeviceTransport(DEVICE)
    NC->>DM: OpenDevice(options)
    DM->>DM: InitLocalDeviceInfoLocked()
    DM->>H: CreateEndpoint(BuildLocalEndpointDescLocked())

    CE->>CC: ImportForTransport() / ConnectWithOptions()
    CC->>HM: Prepare(peer endpoint)
    HM->>H: HcommChannelCreate(COMM_ENGINE_CPU)
    NE->>NC: ImportForTransport() / ConnectWithOptions()
    NC->>DM: Prepare(peer endpoint + DDR keys)
    DM->>H: HcommThreadAlloc(COMM_ENGINE_AICPU_TS)
    DM->>H: HcommChannelCreate(COMM_ENGINE_AICPU)
    DM->>H: ImportRemoteMemKeysLocked() / HcommMemImport()
    H-->>DM: outMem.addr / size
    DM->>DM: ValidateImportedGvaLocked()
    DM->>DM: BuildBatchCopyRouteContextLocked()

    NE->>NC: GetBatchCopyRouteContext()
    NC->>DM: GetBatchCopyRouteContext()
    DM-->>NE: route image
    NE->>X: WriteBatchCopyRouteContext() / H2D
    NE->>DM: RegisterBatchCopyCompletionRegion(completionBase)
    DM->>H: HcommMemReg(completion cells, DEVICE)
    NE->>NE: UpdateHybmDeviceInfo(routeSize)
    NE->>G: PublishBatchCopyRouteRoot(id_)，最后写 magic
    NE-->>NE: ImportForTransport() 成功
```

当前主调用链仍是 `MemEntityDefault::ImportForTransport()` →
`TransportManager::ConnectWithOptions()` → `ComposeTransportManager::Prepare()`，但 CPU/NPU 两端分别
进入 Host/Device manager。具体修改点和原因如下：

| 当前文件/位置 | 修改内容 | 原因 |
| --- | --- | --- |
| `hybm_transport_common.h` | 增加 `endpointLocType` 和 `REG_MR_FLAG_GVA_DIRECT`，不增加 `entityId` | endpoint 平台类型必须显式；entity owner 已由 `MemEntityDefault::id_` 持有 |
| `device/urma/hcomm_transport_manager.{h,cpp}` | 迁移到公共 `transport/urma` 目录和命名空间 | Host/Device 都要复用 HCOMM endpoint、MR 和描述符封装，Host 不应依赖 device 模块 |
| 同文件，`UrmaEndpointDesc/ToHcommEndpointDesc()` | 保存并转换完整 `EndpointLoc`，删除 locType 硬编码 | CPU 必须走 HCOMM HOST endpoint，不能伪造 device 拓扑字段 |
| `hybm_entity_default.cpp`，`InitTransManager()` | 从本地平台角色设置 `endpointLocType` | 使 Compose 能在无卡鲲鹏上选择 HostUrma manager |
| `compose_transport_manager.{h,cpp}`，`OpenDeviceTransport()` | 按 `endpointLocType` 创建 Host/Device URMA manager；转发 route 获取和 completion 注册 | 当前函数对 DEVICE_URMA 无条件创建 Device manager，会在 CPU 上调用 ACL 失败 |
| 新增 `host/urma/host_urma_transport_manager.{h,cpp}` | 实现 Host endpoint、DDR/flag 注册导出、CPU channel 和清理 | Host 无卡路径与 Device manager 的 ACL/kernel 生命周期差异过大 |
| `hybm_conn_based_segment.cpp`，`AllocMemory()/MapSlice()` | 鲲鹏源 DDR 固定 `mmap` 到 GVA，跳过 HAL 分配和 DVA 注册 | HCOMM 导入地址等于注册地址；要让算子直接使用 GVA，源端注册地址必须就是 GVA |
| `hybm_entity_default.cpp`，源 DDR 注册处 | 仅鲲鹏 Batch_Copy 源 DDR 设置 `REG_MR_FLAG_GVA_DIRECT` | 不改变既有 NPU 本机 Host 内存的 HVA→DVA 路径 |
| `HostUrmaTransportManager::RegisterMemoryRegion()/QueryMemoryKey()` | 直接 `HcommMemReg(mr.addr)`，校验并导出同一 GVA | 鲲鹏没有 DVA，也不应调用 `HybmVaManager::TransformVa(HVA, DVA)` |
| `HcommTransportManager::HcommMemImport()` | 用 `UrmaExportDesc.memoryType` 设置 view type，不使用未回填的 `outMem.type` | 当前 UBC import 只回填 addr/size，零初始化的 type 会把 Host DDR 误判为 DEVICE |
| `DeviceUrmaTransportManager::ImportRemoteMemKeysLocked()` | 增加 `ValidateImportedGvaLocked()`，校验 addr/size 和导出类型 | 只有验证导入 view 等于已发布 GVA 后，RangeEntry 才能省略地址转换字段 |
| `DeviceUrmaTransportManager::Prepare()` | 拆出 `PreparePeerResourcesLocked()` 和 `BuildBatchCopyRouteContextLocked()` | 现有函数已经很长；路由只能在全部 peer 资源和 MR/flag 导入成功后生成 |
| `device_urma_transport_manager.{h,cpp}` | 新增 `GetBatchCopyRouteContext()`、`RegisterBatchCopyCompletionRegion()` 和对应清理 | route image 来自 Device manager；completion cells 必须作为本地 HCOMM 目的内存注册 |
| `hybm_transport_manager`、`compose_transport_manager` | 增加上述两个内部虚接口和转发 | entity 不应访问具体 Device manager 类型，公共 SMEM/HYBM API 不变 |
| `hybm_entity_default.cpp`，`ImportForTransport()` | 新增 `WriteBatchCopyRouteContext()`、`PublishBatchCopyRouteRoot()` | entity 知道 `id_` 和固定 HBM 槽位，是 route 的正确发布者 |
| `hybm_entity_default.cpp`，`SetExtraContext()` | 提取 H2D helper，并拒绝覆盖 route owner context | 防止用户 extra context 覆盖 thread/channel 等控制数据 |
| `src/hybm/csrc/common/hybm_define.h` | 增加 Host/AICPU 共用的最小 root/header/peer/range ABI | Host 构建的数据必须能被 AICPU 按完全相同的 offset 读取 |

发布事务边界为：两端全部 peer 建链成功 → NPU 导入并校验所有 DDR MR/flag → 构建 route image →
同步写入 extra context → 注册 completion cells → 更新 entity meta → 最后写 root magic → 初始化成功。
任一步失败都不得写有效 magic，并释放本次创建的资源。Close 前调用方保证没有在途算子；
`ClearBatchCopyRouteRoot()` 先清 magic，再按现有逆序释放 HCOMM，不增加 drain/RCU 状态机。

新增和拆分函数均不超过 50 行非空非注释代码，嵌套深度不超过 4 层。

### 3.1.6 Batch_Copy 执行流程

详细时序如下：

```mermaid
sequenceDiagram
    participant U as 图执行器/调用方
    participant O as HybmBatchCopy AICPU
    participant G as RouteRoot
    participant T as RouteContext
    participant H as HCOMM
    participant P as 鲲鹏 DDR peer
    participant D as 本地 HBM

    U->>O: srcList, dstList, lenList, size
    O->>O: 校验四输入并获取单实例执行权
    alt 参数非法或已有算子在途
        O-->>U: BM_INVALID_PARAM / BM_BUSY
    else 进入执行
        O->>G: 读取固定 global meta reserved
        G-->>O: magic, ownerEntityId
        O->>O: 校验 magic 和 ownerEntityId，计算 routeBase
        O->>T: 读取 header、peer entries、range entries
        O->>O: 校验数量、总大小和区间有序非重叠
        loop 每个非零长度 item，提交前预校验
            O->>O: 检查地址溢出和目的 HBM 范围
            O->>T: 二分查找完整覆盖源 GVA 的 range
            T-->>O: peerIndex, srcGvaBegin, srcGvaEnd
            O->>O: 直接以 srcGva 作为 hcommSrc 并加入 peer 分组
        end
        alt 任一 item 预校验失败
            O->>O: 释放单实例执行权
            O-->>U: BM_INVALID_PARAM / BM_NOT_CONNECTED
        else 全部 item 合法
            loop 每个已使用 peer
                loop 每个不超过 1000 条的分片
                    O->>H: HcommBatchTransferOnThread(READ)
                    alt batch 接口不支持
                        loop 分片内每个 item
                            O->>H: HcommReadOnThread(hcommSrc, dst, len)
                        end
                    end
                end
                O->>H: HcommChannelFenceOnThread
                O->>D: completionCell = 0
                O->>H: Read remoteFlag -> completionCell
                H->>P: URMA 读取源 DDR/flag
                P-->>D: DMA 写入目的 HBM/completionCell
            end
            loop 直到所有 peer 完成或 60 秒超时
                O->>D: 带设备内存屏障读取 completion cells
            end
            O->>O: 释放单实例执行权
            alt 全部 peer 完成
                O-->>U: BM_OK
            else HCOMM 失败或超时
                O-->>U: BM_ERROR / BM_TIMEOUT
            end
        end
    end
```

算子使用以下顺序处理一次调用：

1. 校验参数结构、三组列表地址和 `size`，并以原子方式取得 P0 单实例执行权。
2. 读取固定路由根，校验 magic 和 `ownerEntityId`，按固定公式计算 `routeBase`。
3. 读取 header，校验 `peerCount <= 16`，并计算各数组地址和 `routeEnd` 不超过 64 KiB。
4. 扫描所有输入但暂不提交传输：
   - 0 长度条目直接跳过。
   - 检查源/目的地址加长度不溢出。
   - 二分查找包含完整源区间的 range entry。
   - 校验 `peerIndex` 有效、thread/channel 非 0。
   - 直接使用 `srcGva` 作为 `hcommSrc`，并检查目的地址不落入 HYBM 控制区。
5. 按 `peerIndex` 分组；组内保持输入顺序，peer 组按索引升序处理。
6. 每组优先调用 `HcommBatchTransferOnThread`，每次最多提交 1000 条 READ 描述。
7. HCOMM batch 不支持时，逐条调用 `HcommReadOnThread`；其他错误立即停止后续提交。
8. 每个已使用 peer 调用一次 `HcommChannelFenceOnThread`。
9. 将该 peer 的本地 completion cell 清零，再把已注册的 remote flag 读入 completion cell。
10. 轮询所有已使用 peer 的 completion cell，全部完成或达到 60 秒超时后返回。
11. 释放单实例执行权。

P0 使用每 peer completion cell 的原因是：现有实现只有一个 notify，混合 peer 时不同 HCOMM thread
之间不能假定完成顺序。每 peer 独立完成后再由 AICPU 汇聚，才能保证算子成功返回时所有目的 HBM
数据均可见。completion cell 的清零、DMA 可见性和轮询屏障必须使用昇腾 950/HCOMM 支持的设备侧
内存屏障，不能仅依赖 Host C++ 的 `std::atomic_thread_fence`。

所有输入在提交前完成校验，因此参数错误不会导致部分拷贝。HCOMM 提交开始后的链路错误可能造成
部分完成，算子返回失败但不回滚已写入的 HBM；调用方不得在失败后使用整个 batch 的输出。

### 3.1.7 并发和顺序语义

- P0 每张 NPU 同时只允许一个 Batch_Copy；并发调用返回 `BM_BUSY`。
- 同一 peer 内保持输入顺序，并在该 peer 最后执行 fence。
- 不同 peer 之间不承诺写入先后顺序，但成功返回时全部完成。
- 调用方不得提供相互重叠的目的 HBM 区间；P0 不做 `O(n²)` 的重叠检测。
- 路由在初始化完成前发布一次，运行期不可变；P0 不处理动态 MR、peer 删除或断链并发。
- 最终 Close 前由调用方保证没有在途 Batch_Copy，然后先清除 root magic，再释放通信资源。

## 3.2 技术选型

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| Host 每次传入 thread/channel | 已有实现，改动小 | Host 参与热路径；一次只能选择一个 peer；暴露内部句柄 | 不满足目标 |
| 算子增加 `src_rank_list` | 路由明确，可支持重叠远端 VA | 改变四输入要求；上游必须额外生成列表 | 作为 GVA 不唯一时的备选 |
| 从源 GVA 查询 HBM 路由表 | 四输入不变；支持混合 peer；无 Host 热路径 | 需要保存通信资源元数据 | 采用 |
| 复用 entity extra context | 已有 64 KiB 区域和 H2D 写入流程；不改固定映射 | owner entity 不能再存用户 context | P0 采用并设为内部独占 |
| 新增专用固定 64 KiB 控制区 | 与用户 context 隔离 | 需要扩展 VMM/legacy 映射和兼容测试 | P0 不采用 |
| 单表、初始化期发布一次 | 状态机简单；符合当前无并发初始化和无动态更新约束 | 不支持运行期扩容 | P0 采用 |
| 鲲鹏源 DDR 固定映射并以 GVA 注册 | 算子直接使用 GVA；range 少一个基址 | 限制 GVA 窗口；import 后必须校验地址未重定位 | P0 采用 |
| 线性查找地址表 | 实现简单 | `rangeCount × batchSize` 最坏开销高 | 拒绝 |
| 排序地址表 + 二分查找 | 查找上界稳定，适合只读快照 | 发布时需要排序和重叠检查 | 采用 |

## 3.3 功能与性能设计

### 3.3.1 代码影响范围

| 模块 | 预期修改 |
| --- | --- |
| `src/hybm/csrc/common/` | 增加 Host/AICPU 共享的路由根和路由表 ABI 定义 |
| `src/hybm/include/hybm_def.h` | 新增非破坏性错误码 `BM_BUSY (-11)` |
| `src/hybm/csrc/entity/` | 复用 extra context 拷贝、发布 route root，并阻止 owner context 被公共 API 覆盖 |
| `src/hybm/csrc/mm/` | 鲲鹏源 DDR 使用固定 GVA mmap，并标记 GVA direct 注册模式 |
| `src/hybm/csrc/transport/` | 抽取公共 URMA/HCOMM 封装，透传 endpoint 位置、route 和 completion 接口 |
| `src/hybm/csrc/transport/host/urma/` | 新增无卡 Host endpoint、固定 GVA MR/flag 导出和 CPU channel |
| `src/hybm/csrc/transport/device/urma/` | 增加 import 地址校验、路由构建和 completion 注册 |
| `src/hybm/ops/hybm_kernel/` | 新增 `HybmBatchCopy` 参数校验、查表、分组、HCOMM 读和完成汇聚 |
| `src/hybm/ops/hybm_kernel/libcann_hybm_kernel.ini` | 注册 `HybmBatchCopy` 函数 |
| AICPU CMake/打包 | 将新源文件和共享 ABI 头加入 `cann-hybm-compat.tar.gz` |
| `test/ut/testcase/hybm/` | 路由发布、查表、混合 peer、回滚和异常路径 UT |
| `doc/installation_aicpu_kernel.md` | 增加算子名称、版本兼容和验证方法 |

### 3.3.2 性能策略

- 路由表只在初始化建链完成后写入一次，不在数据热路径写入。
- AICPU 只读取实际 `peerCount/rangeCount` 对应的内容。
- 地址区间排序后使用二分查找；分组使用固定 16 桶，避免哈希表。
- HCOMM batch 每组最多 1000 条，沿用现有内核限制和 fallback 行为。
- 每个 peer 只执行一次 fence 和一次 completion read。
- 所有临时数组受 `size <= 4096` 限制；分配失败记录 batch size 并返回 `BM_MALLOC_FAILED`。

### 3.3.3 返回语义

| 场景 | 返回值 | 是否可能已写入部分 HBM |
| --- | --- | --- |
| 空指针、size 越界、地址溢出 | `BM_INVALID_PARAM` | 否 |
| 路由表未初始化或布局非法 | `BM_NOT_INITIALIZED` / `BM_INVALID_PARAM` | 否 |
| 地址未命中或 peer 句柄无效 | `BM_NOT_CONNECTED` | 否 |
| 并发调用 | `BM_BUSY` | 否 |
| 临时内存不足 | `BM_MALLOC_FAILED` | 否 |
| HCOMM 提交/fence 失败 | `BM_ERROR` 或下层错误码 | 是 |
| completion 超时 | `BM_TIMEOUT` | 是 |
| 全部成功 | `BM_OK` | 是，且全部可见 |

`BM_BUSY (-11)` 为本方案新增错误码，用于区分可重试的并发冲突与不可恢复的内部错误；它只新增
返回值，不改变现有错误码数值。

## 3.4 安全隐私与 DFX 设计

P0 只保留必要防护和诊断：发布前校验数量、总大小、GVA 区间和 HCOMM 句柄；算子提交前校验输入、
目的 HBM 范围和路由命中。route owner 的 extra context 禁止被公共 API 覆盖。错误日志记录
`rankId/entityId/peerIndex`、batch index、地址/长度和 HCOMM 返回码，但不读取或打印业务数据。Host 与
AICPU 对共享结构执行 `sizeof/offsetof` 断言，UT 覆盖 route 构建、查找、发布失败、混合 peer 和
extra context 覆盖拒绝。

## 3.5 编程与调用设计

### 3.5.1 编程模型基本设计

#### 开发环境

- 昇腾 950 NPU 与支持 URMA/HCOMM 的鲲鹏服务器。
- CANN/Ascend 环境由 `ASCEND_HOME_PATH` 指定。
- MemFabric 以 NPU 模式构建并启用 HCOM/RDMA。
- AICPU 算子使用现有 `script/kernel/build_ops_run.sh` 构建和打包。

#### 使用约束

- 调用前必须完成 HYBM 初始化、URMA 建链、鲲鹏 DDR 注册/导出和 NPU 侧导入。
- `src_ddr_ptr_list` 的元素必须是 MemFabric GVA。
- 三组列表本身必须位于 AICPU 可访问的设备内存。
- 目的地址必须是本地 NPU HBM，并已满足 HCOMM 本地内存访问要求。
- P0 单张 NPU 只允许一个在途 Batch_Copy。

#### 验收设计

构建和基础测试命令：

```bash
bash script/build_and_pack_run.sh --build_hcom ON --build_hcom_rdma ON
bash script/kernel/build_ops_run.sh
bash script/run_ut.sh --fast UrmaTransportManager
```

硬件验收矩阵至少包括：

| 维度 | 取值 |
| --- | --- |
| 拓扑 | 1 NPU × 1 CPU、1 × 16、16 × 16 |
| batch size | 1、16、128、1000、4096 |
| 单条长度 | 1 B、4 KiB、64 KiB、1 MiB、4 MiB |
| 地址分布 | 单 MR、每 peer 多 MR、跨 peer 混合、区间边界 |
| HCOMM 能力 | batch 可用、batch 不支持回退单条 |
| 异常 | 导入失败、表损坏、HCOMM 提交失败、完成超时 |

### 3.5.2 接口定义与设计

#### 3.5.2.1 `HybmBatchCopy`

**描述：** 从一个或多个鲲鹏 CPU peer 的 DDR GVA 批量读取到本地昇腾 950 HBM。

**原型：**

```cpp
struct HybmBatchCopyParam {
    const void *const *srcDdrPtrList;
    void *const *dstHbmPtrList;
    const uint64_t *ptrLenList;
    uint32_t size;
};

extern "C" uint32_t HybmBatchCopy(HybmBatchCopyParam *param);
```

结构只包含用户要求的四个输入；末尾如有编译器 padding，不定义为可写字段。

**输入参数：**

| 参数 | 输入/输出 | 类型 | 说明 | 范围 |
| --- | --- | --- | --- | --- |
| `srcDdrPtrList` | 输入 | `const void *const *` | 鲲鹏 DDR MemFabric GVA 列表 | 非空，设备可访问 |
| `dstHbmPtrList` | 输入 | `void *const *` | 本地昇腾 HBM 地址列表 | 非空，不能指向控制区 |
| `ptrLenList` | 输入 | `const uint64_t *` | 每组源/目的区间的字节长度 | 元素可为 0 |
| `size` | 输入 | `uint32_t` | 三个列表的元素个数，不是字节数 | 1～4096 |

**返回值：**

| 返回值 | 说明 |
| --- | --- |
| `BM_OK` | 全部非零长度条目已完成，目的 HBM 数据可见 |
| `BM_INVALID_PARAM` | 参数、地址、长度或表项非法 |
| `BM_NOT_INITIALIZED` | 路由根 magic 尚未发布 |
| `BM_NOT_CONNECTED` | 源地址无路由或 peer 句柄无效 |
| `BM_BUSY` | 已有算子在途 |
| `BM_NOT_SUPPORTED` | ABI/HCOMM 能力不兼容且无 fallback |
| `BM_MALLOC_FAILED` | AICPU 临时空间分配失败 |
| `BM_TIMEOUT` | 一个或多个 peer 未在 60 秒内完成 |
| `BM_ERROR` | HCOMM 或其他内部错误 |

其中 `BM_BUSY` 是新增的 HYBM 公共错误码，数值为 `-11`。

**异常处理：** 参数和路由错误在任何 HCOMM 提交前返回。提交后的错误可能部分写入，调用方应丢弃
本次 batch 的全部输出或按上层重试协议重新读取。

**约束：** 不支持重叠目的区间、多实例并发和 raw remote VA。

**变更说明：** 新增独立符号，不改变 `HybmBatchRead/HybmBatchWrite`。

**参考代码：**

```cpp
HybmBatchCopyParam param{};
param.srcDdrPtrList = deviceSrcGvaList;
param.dstHbmPtrList = deviceDstHbmList;
param.ptrLenList = deviceLengthList;
param.size = batchSize;

// 通过现有 AICPU kernel loader 获取 HybmBatchCopy 并在业务 stream 上拉起。
// 返回成功后，deviceDstHbmList 指向的非零长度区间均已完成写入。
```

#### 3.5.2.2 内部路由发布接口

内部接口不对 SMEM/HYBM 公共 API 暴露：

```cpp
Result GetBatchCopyRouteContext(BatchCopyRouteContext &context) const;
Result RegisterBatchCopyCompletionRegion(uint64_t addr, uint64_t size);
Result MemEntityDefault::WriteBatchCopyRouteContext(const BatchCopyRouteContext &context) noexcept;
Result MemEntityDefault::PublishBatchCopyRouteRoot() noexcept;
Result MemEntityDefault::ClearBatchCopyRouteRoot() noexcept;
```

`DeviceUrmaTransportManager::Prepare()` 生成 context，`MemEntityDefault::ImportForTransport()` 在
`ConnectWithOptions()` 成功后写入并注册 completion 区，最后发布 magic。P0 的
`UpdateRankOptions()`、`RemoveRanks()` 不更新已发布表，对 Batch_Copy route 返回
`BM_NOT_SUPPORTED`。最终销毁前只清除 route root，不引入 drain 接口。下层失败处负责记录 ERROR
日志，上层只透传已记录的根错误时不重复打印。

### 3.5.3 编程手册设计

在现有 AICPU 安装文档和 API 文档中增加：

1. 支持平台、HCOMM/RDMA 构建开关和 run 包安装。
2. `HybmBatchCopy` 四输入语义，特别说明 `size` 为元素个数。
3. MemFabric GVA 与 raw remote VA 的区别。
4. 建链、内存注册/导入、算子拉起和最终资源释放的完整示例。
5. 16 NPU × 16 CPU 容量限制、64 KiB route 空间限制和单实例并发限制。
6. 错误码、部分完成语义、超时和重试建议。
7. owner entity、peer/channel/thread 和 route 数量日志的排障方法。

---

# 4. 缺点和风险

| 风险 | 影响 | 缓解措施 |
| --- | --- | --- |
| 源地址不是全局唯一 GVA | 可能路由到错误 CPU peer | 发布时拒绝重叠；确认上游地址语义；必要时改为 rank list 接口 |
| 固定 GVA mmap 失败 | 鲲鹏源 DDR 无法按 GVA 注册 | 限制并预留可映射 GVA 窗口；初始化失败且不发布路由 |
| HCOMM import 重定位地址 | 算子直接使用 GVA 会读错地址 | 建链时强制校验 `view.addr == remoteAddr`，不相等则拒绝启用 |
| 目标 CANN 未提供无卡 Host UB plugin | 鲲鹏端创建 endpoint 时调用 device runtime 失败 | 安装 Host plugin 或使用已合入等价实现的 CANN 版本，并在启动时检查能力 |
| extra context 与用户数据冲突 | 路由或用户上下文被覆盖 | owner entity 独占；公共 set-extra-context 返回不支持 |
| 同一 NPU 多 owner 冲突 | 固定 route root 无法选择 entity | P0 只允许一个 owner，第二个初始化失败并记录 entityId |
| Host/AICPU 结构布局不匹配 | 算子误解析句柄或地址 | `sizeof/offsetof` 断言；Host 包和 AICPU 包版本绑定 |
| 最终销毁时仍有在途算子 | use-after-free、传输失败或设备异常 | 调用方保证 quiescent；先清 root magic，再释放 HCOMM 资源 |
| 多 peer 完成语义不明确 | 算子提前返回，目的数据未完成 | P0 使用每 peer completion cell 汇聚；硬件上验证内存屏障和超时路径 |
| AICPU 轮询占用核 | 长尾或断链时占用 AICPU 资源 | 60 秒超时；后续评估 STARS 多 notify/event 聚合 |
| 4096 条临时描述占用 AICPU 堆 | 内存不足或抖动 | 固定上限、分片提交、分配失败可诊断 |
| batch 中途失败可能部分完成 | 上层误用部分数据 | 明确失败语义；成功前不对上层声明可用；提供整体重试建议 |
| route 超过 64 KiB | 初始化无法发布路由 | 按实际 `peerCount/rangeCount` 计算大小，超限时返回资源不足 |
| P0 单实例限制影响多 stream 并发 | 吞吐受限或返回 `BM_BUSY` | 先保证正确性；后续使用 per-invocation completion workspace 扩展并发 |

迁移方面，旧调用者无需修改。只有使用新 `HybmBatchCopy` 的调用者需要安装包含该符号的新 AICPU
run 包，并确保 Host `libmf_hybm_core.so` 与设备包的路由表 ABI 一致。

---

# 5. 现有技术

本方案主要复用仓库内已有实现：

- `src/hybm/ops/hybm_kernel/hybm_batch_transfer.cc` 已实现 HCOMM batch、单条 fallback、fence 和
  remote flag 完成通知，新算子复用其 HCOMM 封装与错误处理模式。
- `src/hybm/csrc/transport/device/urma/device_urma_transport_manager.cpp` 已按 peer 保存
  `RemoteRankState`，并在导入远端 MR 后保存原始区间与 HCOMM view，可直接生成路由表。
- `src/hybm/csrc/common/hybm_define.h` 和 `MemEntityDefault::SetExtraContext()` 已定义每 entity
  64 KiB 固定 user context 和 H2D 写入流程，本方案复用该空间而不扩展 driver 固定映射。
- `src/hybm/csrc/mm/hybm_conn_based_segment.cpp` 的非 56-bit 路径已经使用固定地址 `mmap()`；目标
  分支去除强制 HAL 分配后可复用该模式，使鲲鹏源端 HVA 与 GVA 相等。
- `DeviceUrmaTransportManager::RegisterMemoryRegion()` 的 HVA→DVA 分支继续服务本机 NPU 访问 Host
  内存；鲲鹏源 DDR 改由 `HostUrmaTransportManager` 直接以固定 GVA 注册。
- `app/zbal` 的 AICPU workspace 固定布局表明 Host/Device 共用定长结构和 offset 的模式在仓库中
  已有实践，但其 workspace 属于 ZBAL，不作为 HYBM 路由表的存储位置。

与现有 `HybmBatchRead` 的主要差异是：当前实现由 Host 完成 peer 选择和地址转换，并把单个
`thread/channel` 放入 `HybmOneSideOpParam`；本方案将选择、转换和跨 peer 分组下沉到 AICPU。

---

# 6. 未解决问题

以下问题必须在 RFC 批准前给出结论。`HcommMemImport()` 的地址语义已由当前 CANN 源码确认，不再
作为待确认问题；目标硬件验收仍需覆盖相等校验。

- [ ] 确认目标 CANN 安装包包含并启用无卡 Host UB plugin，或其 `CpuUrmaEndpoint` 已不再调用
      `hrtGetDevice()`/`hrtGetDevicePhyIdByIndex()`。
- [ ] 确认鲲鹏进程可固定 mmap 的 GVA 窗口及预留策略；P0 不启用当前会分离 GVA/HVA 的
      `enable56BitsGva` 模式。
- [ ] HCOMM/CANN 团队确认昇腾 950 上 completion cell 清零、remote flag 写入和 AICPU 轮询所需的
      设备内存屏障；若不支持，改用每 peer STARS notify/event 聚合。
- [ ] 确认 `HcommChannelFenceOnThread` 的完成边界，明确 fence 后 remote flag read 是否覆盖该
      thread 上此前所有 batch/single read。
- [ ] 确认同一进程、同一逻辑 NPU 上是否允许多个 AI-core-initiate entity 同时拥有 URMA manager；
      P0 默认第二个 Batch_Copy owner 初始化失败；若业务必须多 owner，算子接口需增加 entity 选择信息。
- [ ] 确认 Batch_Copy owner entity 的 extra context 可由 MemFabric 独占，业务不会再调用公共
      `smem_shm_set_extra_context()` 写入该 entity。
- [ ] 用目标硬件数据确认 P0 上限：4096 个 batch 元素、64 KiB route 空间和 60 秒超时。
- [ ] 确认外部算子名使用需求中的 `Batch_Copy`，还是按现有命名规则发布为 `HybmBatchCopy`；本文
      建议配置/符号统一使用 `HybmBatchCopy`，文档中保留 Batch_Copy 作为功能名。

---

# 附录

## A. 参考文件

- `src/hybm/csrc/transport/device/urma/device_urma_transport_manager.h`
- `src/hybm/csrc/transport/device/urma/device_urma_transport_manager.cpp`
- `src/hybm/csrc/transport/device/urma/hcomm_transport_manager.cpp`
- `src/hybm/csrc/transport/hybm_transport_common.h`
- `src/hybm/csrc/transport/hybm_transport_manager.cpp`
- `src/hybm/csrc/transport/compose/compose_transport_manager.cpp`
- `src/hybm/csrc/entity/hybm_entity_default.cpp`
- `src/hybm/csrc/mm/hybm_conn_based_segment.cpp`
- `src/hybm/ops/hybm_kernel/hybm_batch_transfer.h`
- `src/hybm/ops/hybm_kernel/hybm_batch_transfer.cc`
- `src/hybm/csrc/common/hybm_define.h`
- `src/hybm/csrc/driver/hybm_gva.cpp`
- `doc/installation_aicpu_kernel.md`

CANN/HCOMM 源码参考（以下路径相对于 HCOMM 源码根目录）：

- `include/hcomm_res_defs.h`
- `src/base_comm/resources/endpoints/endpoint.cc`
- `src/base_comm/resources/endpoints/cpu_urma_endpoint.cc`
- `src/base_comm/resources/reged_mems/urma_mem.cc`
- `src/legacy/ascend950/unified_platform/resource/buffer/local_ub_rma_buffer.cc`
- `src/legacy/ascend950/unified_platform/resource/buffer/remote_rma_buffer.cc`
- `experimental/base_comm/nic_plugin/README.md`
- `experimental/base_comm/endpoint/cpu_urma_endpoint.cc`
- `experimental/base_comm/comm_mem/urma_mem.cc`

## B. 术语

| 术语 | 含义 |
| --- | --- |
| GVA | MemFabric 全局虚拟地址，在本方案中必须可唯一定位 CPU peer 和远端 MR |
| HCOMM view | `HcommMemImport()` 返回、可供本地 HCOMM 操作使用的远端内存视图 |
| peer | 一条本地 NPU 到远端鲲鹏 CPU endpoint 的通信关系 |
| completion cell | 每个 peer 的本地 HBM 完成标记，由 remote flag read 写入 |

## C. 文档更新计划

RFC 决策完成后同步更新：

- `doc/API.md`：新增算子 ABI、错误码和调用约束。
- `doc/installation_aicpu_kernel.md`：新增安装后符号检查与硬件验证步骤。
- `doc/environment_variables.md`：仅当超时或调试开关最终允许配置时新增对应变量。
