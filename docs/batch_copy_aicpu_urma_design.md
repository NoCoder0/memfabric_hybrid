# 鲲鹏 DDR 到昇腾 950 HBM 的 Batch_Copy AICPU 算子技术方案

**Authors:** MemFabric Hybrid URMA Maintainers

**Created:** 2026-07-15

**Updated:** 2026-07-21

**Status:** Draft

**Related Issue/PR:** 无

**Implementation Plan:** [batch_copy_aicpu_urma_staged_implementation_plan.md](batch_copy_aicpu_urma_staged_implementation_plan.md)

---

# 1. 概述

## 1.1 简介

本方案面向鲲鹏服务器与昇腾 950 NPU 组成的超节点，为卡侧发起的 URMA 读提供
`Batch_Copy` AICPU 算子。算子逻辑输入为鲲鹏 DDR 源地址列表、昇腾 HBM 目的地址列表、
长度列表和列表元素个数，按源地址解析对应的 HCOMM `channel/thread`，将远端鲲鹏 DDR
数据批量读取到本地 NPU HBM。

方案在每张 NPU 上维护一份只由 MemFabric 控制面写入的卡级 HBM 路由表。路由表位于
`HYBM_DEVICE_META_ADDR` 前方固定的 2 MiB 卡级元数据区域。URMA
建链和远端内存导入成功后，Device URMA manager 把 CPU peer、地址区间及 `channel/thread` 写入
固定地址。AICPU 算子不接收内部通信句柄，直接读取这张只读表，按 CPU peer 对 batch 分组，然后
复用现有 `HybmBatchRead` 完成 HCOMM batch 提交、单条回退、fence 和完成标记读取。


## 1.2 目标与限制

### 目标

- 新增 `Batch_Copy` AICPU 算子，仅暴露三组列表和列表元素个数。
- 路由表规格支持每张 NPU 最多 64 个鲲鹏 CPU peer，每个 peer 最多 16 个 DDR 地址区间。
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
| 单 CPU 单地址 | 从一个鲲鹏 DDR 区间读取到一个 HBM 区间 | GVA 路由命中、单条 HCOMM 读、完成语义 |
| 单 CPU 多地址 | 同一 peer 的多个离散 DDR 地址批量读取 | 优先使用 HCOMM batch，一次 fence |
| 多 CPU 混合 batch | 一个列表中包含不同 CPU peer 的地址 | 正确查表分组，每个 peer 使用自身句柄 |
| 最大拓扑 | 每张 NPU 连接 64 个 CPU peer，每 peer 16 个地址区间 | 固定表容量和初始化失败回滚 |
| 异常输入 | 越界、溢出、空指针、无路由地址 | 传输前失败，不提交任何 HCOMM 请求 |


## 2.2 安全、可靠性与兼容性要求

- 源 GVA 的完整 `[src, src + len)` 必须落在一个已发布路由区间内。
- 目的地址必须位于本地 HBM 有效范围，且不得覆盖 Batch_Copy 卡级元数据、HYBM meta 或任一
  entity extra context。
- 地址加法必须检查 `uint64_t` 溢出，长度为 0 的条目按 no-op 跳过。
- 路由表和 entity extra context 使用相互独立的地址区间；entity extra context 保持用户上下文语义。
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
        B["NPU Device URMA manager 构建并发布 route"]
    end

    subgraph NPU["昇腾 950 NPU"]
        T["固定 2 MiB Batch_Copy 卡级元数据区"]
        R["固定地址路由表"]
        O["Batch_Copy AICPU 算子"]
        G["按 peer 分组"]
        H["复用 HybmBatchRead"]
        D["目的 HBM"]
    end

    K["鲲鹏 DDR"]

    P --> C --> I --> B --> T
    O --> R
    T --> R
    O --> G --> H
    K --> H --> D
```

控制面负责资源创建、远端 MR 导入和路由发布；数据面只读路由表并使用已发布资源。
每张 NPU 的路由表最多记录 64 个 CPU peer，每个 peer 最多 16 个区间，总计最多 1024 个地址
区间。目标部署规模为 16 NPU × 16 CPU；每张 NPU 持有自己的本地
channel/thread 句柄，卡间不共享路由表。

### 3.1.2 地址语义

`src_ddr_ptr_list` 中的元素定义为 MemFabric GVA，而不是鲲鹏进程本地 VA。
GVA 必须在超节点内唯一，且与 `RemoteRegistration::addr/size` 使用同一地址空间。

鲲鹏 DDR 源端采用 GVA 固定地址注册：

1. `HybmConnBasedSegment::ReserveMemorySpace()` 在 `enable56BitsGva=false` 时预留完整 GVA 窗口，
   `localVirtualBase_` 等于本 rank 的 GVA 起点；`AllocMemory()` 通过 `MAP_FIXED` 把 DDR 映射到
   `sliceAddr`，并只接受 `mapped == sliceAddr` 的结果。`XPU_TYPE=NONE` 的 `MapSlice()` 不调用
   `HalHostRegister()`，加入 `HybmVaManager` 的 DVA 为 0，HVA/GVA 均为 `mapped`。
2. `HostUrmaTransportManager::RegisterMemoryRegion()` 对 HOST_DRAM 直接调用
   `HcommMemReg(mr.addr)`。注册前通过 `HybmVaManager::FindAllocByVa(mr.addr, HVM_GVA)` 校验完整
   区间属于本地 rank 的 `HYBM_MEM_TYPE_HOST` 分配记录；其中 `mr.addr` 等于固定映射后的 GVA。
   昇腾本机 HOST_DRAM 由 `DeviceUrmaTransportManager` 执行 HVA→DVA 注册。
3. `QueryMemoryKey()` 导出的 `key.keys[1]` 与 HCOMM 实际注册地址使用同一个 GVA。
4. NPU 侧 `ImportRemoteMemKeysLocked()` 从 key 恢复 GVA，调用 `HcommMemImport()` 后校验
   `exportDesc.addr == remoteAddr == view.addr`、`exportDesc.size == remoteSize` 且
   `view.size >= remoteSize`。不满足时返回 `BM_NOT_SUPPORTED`，不发布路由。
P0 配置 `enable56BitsGva=false`，鲲鹏 DDR 必须由上述固定 GVA 内存池分配。通过
`hybm_register_local_memory()` 注册的任意 Host 指针不保证 HVA 等于 GVA，不进入 Batch_Copy 路由表。
GVA 超出窗口、地址已被占用或固定映射失败时，初始化失败。

`MemEntityDefault::ImportSliceExchangeInfo()` 保持 `ImportForSegment()` 在前、`ImportForTransport()` 在后的
调用顺序。
固定 GVA 资格由鲲鹏本地注册阶段完成校验，NPU 路由发布不依赖随后执行的
`HybmConnBasedSegment::Mmap()`。

目标 UBC_TP/UBC_CTP 后端保持 HCOMM 注册地址，地址传递流程如下：

1. `LocalUbRmaBuffer::GetExchangeDto()` 把 `buf->GetAddr()` 原样写入 `ExchangeUbBufferDto::addr`；
2. `RemoteUbRmaBuffer(rdmaHandle, dto)` 把 `dto.addr` 原样保存到 `addr`，底层
   `HrtRaUbRemoteMemImport()` 返回的 `targetSegVa` 另存为 `segVa`，不会覆盖 `addr`；
3. `UbRegedMemMgr::MemoryImport()` 最终执行
   `outMem->addr = reinterpret_cast<void *>(remoteUbRmaBuffer->GetAddr())`。

因此该后端满足：

```text
HcommMemImport.outMem.addr == 对端传给 HcommMemReg 的地址
```

鲲鹏端固定 `mmap` 到 GVA，并以该地址调用 `HcommMemReg()`，因此导入结果等于 GVA。NPU 侧执行
`exportDesc.addr == remoteAddr == view.addr` 和 `view.size >= remoteSize` 校验，用于防止 CANN 后端变化、
描述符损坏或注册路径误用。路由区间只保存
调用者可见的 `srcGvaBegin/srcGvaEnd`，算子直接使用输入 GVA：

```text
hcommSrc = srcGva
```

`BatchCopyRangeEntry` 只保存 `srcGvaBegin/srcGvaEnd/peerIndex`，不保存地址转换基址。

UBC 的 `UbRegedMemMgr::MemoryImport()` 只回填 `outMem.addr/size`，没有写 `outMem.type`。
`HcommTransportManager::HcommMemImport()` 使用 MemFabric 外层 `UrmaExportDesc.memoryType` 设置
`view.type`，并校验 `outMem.addr/size`。路由构建器只收录远端 endpoint 为
`ENDPOINT_LOC_TYPE_HOST` 且 `view.type == HOST_DRAM` 的区间；`DEVICE_HBM` 使用设备到设备数据路径，
不进入 Batch_Copy DDR 路由表。

同一导出 payload 尾部的 transfer flag 描述符也由 UBC 导入。普通 MR 与该 flag 共用现有
`UrmaExportDesc.memoryType`，不新增 `flagMemoryType` 字段。`ImportRemoteMemKeysLocked()` 对
`flagOutMem` 只校验 `addr != nullptr` 和 `size == sizeof(uint64_t)`，不读取 `flagOutMem.type`；校验通过后
把导入地址和大小保存到 `RemoteRankState::remoteFlagAddr/remoteFlagSize`。

不同 CPU peer 的源地址范围发生重叠时，NPU 侧 `BuildBatchCopyRouteTableLocked()` 返回失败，不发布 magic。

### 3.1.3 独立卡级元数据区

路由表位于 `HYBM_DEVICE_META_ADDR` 前方的固定卡级地址，生命周期归属逻辑 NPU。该区域与 entity
extra context 独立，`hybm_set_extra_context()` 只访问用户上下文。`HYBM_DEVICE_META_ADDR`、entity meta
和 user context 的地址保持不变。昇腾 950 使用 `MEM_HUGE_PAGE_TYPE`，因此卡级路由区按一个 2 MiB
大页映射；每张 NPU 占用 2 MiB HBM，16 张 NPU 共占用 32 MiB。NPU 初始化必须携带
`HYBM_FLAG_INIT_SHMEM_META`，使 `hybm_init_hbm_gva()` 建立该固定控制区映射。

地址常量如下：

```cpp
constexpr uint64_t HYBM_BATCH_COPY_META_SIZE = HYBM_LARGE_PAGE_SIZE; // 2 MiB
constexpr uint64_t HYBM_BATCH_COPY_META_ADDR =
    HYBM_DEVICE_META_ADDR - HYBM_BATCH_COPY_META_SIZE;
constexpr uint64_t HYBM_DEVICE_CONTROL_ADDR = HYBM_BATCH_COPY_META_ADDR;
constexpr uint64_t HYBM_DEVICE_CONTROL_SIZE =
    HYBM_BATCH_COPY_META_SIZE + HYBM_DEVICE_INFO_SIZE; // 34 MiB
```

其中 `HYBM_DEVICE_INFO_SIZE` 表示 32 MiB HYBM 元数据区。完整地址布局如下，地址从低到高：

```text
SVM_END_ADDR - 1 GiB
┌──────────────────────────────────────────────────────────────────┐
│ 1 GiB 固定 VA 预留区中的未映射部分：990 MiB                     │
├──────────────────────────────────────────────────────────────────┤
│ HYBM_BATCH_COPY_META_ADDR = SVM_END_ADDR - 34 MiB                │
│ Batch_Copy 卡级元数据映射：2 MiB                                 │
├──────────────────────────────────────────────────────────────────┤
│ HYBM_DEVICE_META_ADDR = SVM_END_ADDR - 32 MiB                    │
│ global meta + 511 个 entity meta：64 KiB                         │
├──────────────────────────────────────────────────────────────────┤
│ HYBM_DEVICE_USER_CONTEXT_ADDR                                    │
│ 511 个 entity extra context：511 × 64 KiB                        │
├──────────────────────────────────────────────────────────────────┤
│ SVM_END_ADDR                                                     │
└──────────────────────────────────────────────────────────────────┘

Batch_Copy 元数据区 + HYBM 元数据区 = 2 MiB + 32 MiB = 34 MiB
```

Modern 路径预留 1 GiB VA，`HalMemCreate()` 申请 34 MiB，并从 `HYBM_DEVICE_CONTROL_ADDR` 开始一次性
`HalMemMap()`。Legacy 路径从同一地址执行 `HalGvaAlloc()`。释放路径使用
`HYBM_DEVICE_CONTROL_ADDR/HYBM_DEVICE_CONTROL_SIZE` 解除映射并释放物理资源。

每个逻辑 NPU 维护一份表。`DeviceUrmaTransportManager` 通过按 `userDeviceId_` 索引的
`BatchCopyRouteOwnerRegistry` 获取唯一发布权；同一卡的第二个发布者返回 `BM_BUSY`。

### 3.1.4 路由表布局与规格

#### 固定规格

| 项目 | 规格 |
| --- | ---: |
| 每张 NPU 最大 peer 数 | 64 |
| 每个 peer 最大地址区间数 | 16 |
| 每张 NPU 最大地址区间总数 | 1024 |
| 每个 peer 的 channel/thread 数 | 各 1 |
| Completion cell 数 | 64，每项 8 B |
| `BatchCopyPeerEntry` 大小 | 32 B（`0x20`） |
| `BatchCopyRangeEntry` 大小 | 32 B（`0x20`） |
| 静态路由表大小 | 34880 B（`0x8840`） |
| 运行期 completion workspace | 512 B（`0x0200`） |
| Batch_Copy 控制区实际使用大小 | 35392 B（`0x8A40`） |
| 卡级元数据物理映射开销 | 2 MiB/NPU |
| 16 张 NPU 的新增 HBM 开销 | 32 MiB |

路由表使用固定容量数组。这样 AICPU 可以使用编译期 offset 定位 peer、range 和 completion，无需根据
数量计算变长区域地址。有效 peer 被压缩放在前 `peerCount` 项；有效 range 被全局按
`srcGvaBegin` 排序后放在前 `rangeCount` 项。

#### 结构体设计

```cpp
constexpr uint32_t BATCH_COPY_ROUTE_MAGIC = 0x42435059U; // "BCPY"
constexpr uint16_t BATCH_COPY_MAX_PEER_COUNT = 64;
constexpr uint16_t BATCH_COPY_MAX_RANGE_PER_PEER = 16;
constexpr uint16_t BATCH_COPY_MAX_RANGE_COUNT =
    BATCH_COPY_MAX_PEER_COUNT * BATCH_COPY_MAX_RANGE_PER_PEER;

struct alignas(64) BatchCopyRouteHeader {
    uint32_t magic;
    uint16_t peerCount;
    uint16_t rangeCount;
};

struct alignas(32) BatchCopyPeerEntry {
    uint64_t thread;
    uint64_t channel;
    uint64_t remoteFlagAddr;
};

struct alignas(32) BatchCopyRangeEntry {
    uint64_t srcGvaBegin;
    uint64_t srcGvaEnd;
    uint16_t peerIndex;
};

struct alignas(64) BatchCopyRouteTable {
    BatchCopyRouteHeader header;
    BatchCopyPeerEntry peers[BATCH_COPY_MAX_PEER_COUNT];
    BatchCopyRangeEntry ranges[BATCH_COPY_MAX_RANGE_COUNT];
};

struct alignas(64) BatchCopyCompletionArea {
    uint64_t cells[BATCH_COPY_MAX_PEER_COUNT];
};

static_assert(sizeof(BatchCopyRouteHeader) == 0x40);
static_assert(sizeof(BatchCopyPeerEntry) == 0x20);
static_assert(sizeof(BatchCopyRangeEntry) == 0x20);
static_assert(offsetof(BatchCopyRouteTable, peers) == 0x40);
static_assert(offsetof(BatchCopyRouteTable, ranges) == 0x840);
static_assert(sizeof(BatchCopyRouteTable) == 0x8840);
static_assert(sizeof(BatchCopyCompletionArea) == 0x200);

constexpr uint64_t BATCH_COPY_COMPLETION_OFFSET = sizeof(BatchCopyRouteTable);
constexpr uint64_t BATCH_COPY_CONTROL_USED_SIZE =
    sizeof(BatchCopyRouteTable) + sizeof(BatchCopyCompletionArea);
static_assert(BATCH_COPY_COMPLETION_OFFSET == 0x8840);
static_assert(BATCH_COPY_CONTROL_USED_SIZE == 0x8A40);
```

Header、PeerEntry 和 RangeEntry 中的尾部 padding 只用于 64/32 字节对齐，不定义为扩展字段。
`BatchCopyPeerEntry` 的 3 个有效字段占 24 B，尾部对齐填充 8 B；`BatchCopyRangeEntry` 的有效字段
占 18 B，尾部对齐填充 14 B。`remoteFlagSize` 固定为 `sizeof(uint64_t)`，发布前校验，不写入表。
`BatchCopyRangeEntry` 使用左闭右开区间 `[srcGvaBegin, srcGvaEnd)`，查表要求
`srcGvaBegin <= srcGva` 且 `srcGva + len <= srcGvaEnd`。
`peerRank` 只用于 Host 侧建链和日志，AICPU 使用
`peerIndex` 定位通信资源，因此也不写入表。`BatchCopyRouteTable` 在发布后只读；单独定义的
`BatchCopyCompletionArea` 是算子和 HCOMM 在运行期读写的工作区，不属于静态路由表。

#### 2 MiB 区域内部布局

```text
HYBM_BATCH_COPY_META_ADDR
  + 0x000000  ┌──────────────────────────────────────┐
              │ BatchCopyRouteHeader                 │ 0x0040 B
  + 0x000040  ├──────────────────────────────────────┤
              │ PeerEntry[64]                        │ 0x0800 B
  + 0x000840  ├──────────────────────────────────────┤
              │ RangeEntry[1024]                     │ 0x8000 B
  + 0x008840  ├──────────────────────────────────────┤
              │ BatchCopyCompletionArea              │ 0x0200 B
              │   cells[64]                          │
  + 0x008A40  ├──────────────────────────────────────┤
              │ 大页映射粒度产生的未使用空间         │
  + 0x200000  └──────────────────────────────────────┘
HYBM_DEVICE_META_ADDR
```

`0x8A40～0x200000` 为 HBM 大页映射粒度产生的未使用空间，不承载协议字段。
普通 HBM 分配和 Batch_Copy 目的地址校验都必须把
`[HYBM_BATCH_COPY_META_ADDR, SVM_END_ADDR)` 视为控制区，禁止业务读写。

发布前必须满足：

- `1 <= peerCount <= 64`；
- `1 <= rangeCount <= 1024`，且 `rangeCount <= peerCount * 16`；
- 每个 `peerIndex` 对应的 range 不超过 16 个；
- 每个 peer 的远端 endpoint 为 `ENDPOINT_LOC_TYPE_HOST`，每个 range 的内存类型为 `HOST_DRAM`；
- 每个区间非空、不溢出，所有有效区间按 GVA 升序且互不重叠；
- 每个有效 peer 的 `thread/channel/remoteFlagAddr` 非 0。

`TryPublishBatchCopyRouteLocked()` 仅在初始 Host peer 集合中的每个 peer 都已建立 channel/thread、
导入至少一个合法 DDR MR 且导入 8 B transfer flag 后发布路由。发布时获取每卡 owner，并固定注册从
`BATCH_COPY_COMPLETION_OFFSET` 开始的 512 B HBM 区域。构建 route image 时先清零整个
`BatchCopyRouteTable`，header 的 `magic` 保持 0，并单独清零 `BatchCopyCompletionArea`；完成 H2D
拷贝和 completion 注册后，最后单独写入 `BATCH_COPY_ROUTE_MAGIC`。AICPU 只在 magic 有效时读取
静态表。Close 时先清零 magic，再释放 channel/thread 和 completion 注册句柄。

### 3.1.5 建链与路由表发布

#### Endpoint 描述

`UrmaEndpointDesc` 使用 HCOMM 原生 `EndpointLoc` 表示 endpoint 位置：

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
- `EndpointLoc.host.id` 使用全局唯一的 `rankId`，用于 MemFabric endpoint 描述符的稳定标识。
  CANN Host UB plugin 内部的 `hostResourceId` 固定为 0，与该字段不是同一概念。

#### Host/Device URMA manager 设计

`transport::urma` 公共模块提供 `HcommTransportManager`、endpoint/MR 描述符、序列化和范围查找工具。
`DeviceUrmaTransportManager` 管理 ACL、AICPU thread/channel、远端 MR 导入和设备数据面；
`HostUrmaTransportManager` 管理 Host endpoint、CPU channel、本地 DDR/flag 导出以及 Host CPU 主动
Read/Write/Fence 数据面。两个 manager 独立
实现 `TransportManager` 接口并组合使用公共 HCOMM 模块。

函数职责划分如下：

| 函数或函数组 | Device manager | Host manager |
| --- | --- | --- |
| `OpenDevice()`、`CloseDevice()` 及回滚函数 | 管理 Ascend 950、ACL 和 device kernel 生命周期 | 管理 Host 内存及 HCOMM endpoint/channel/MR 生命周期 |
| 本地信息和 endpoint 构建函数 | 查询逻辑/物理卡、SDID、serverId、superPodId 和设备 EID | `InitLocalHostInfoLocked()/BuildLocalHostEndpointDescLocked()` 读取 Host NIC 并填写 `loc.host.id` |
| transfer flag 初始化 | 使用 `AclrtMalloc/AclrtMemcpy` 创建 device flag | `InitHostTransferFlagLocked()` 分配 8 B Host flag，写入 `uint64_t{1}`，并以 `COMM_MEM_TYPE_HOST` 注册 |
| TLS/completion context 函数 | 管理 ACL stream、device notify 和 TLS completion context | 不创建 ACL/TLS context；按 peer 保存 CPU channel 的 pending/fence 状态 |
| 本地 MR 注册、查询和注销函数 | 本机 HOST_DRAM 执行 HVA→DVA，HBM 使用 device address | 固定 GVA/HVA 直接注册；共用范围查找、描述符序列化和 refCount helper |
| 远端 MR 导入函数 | 导入鲲鹏 DDR MR/flag 并保存 HCOMM view | Host peer 验证场景导入对端 DDR MR/flag；NPU peer 场景只导出本地 DDR/flag |
| `Prepare()` 及 channel 清理函数 | 首次调用固定 Host peer 集合并创建 `COMM_ENGINE_AICPU_TS` thread、`COMM_ENGINE_AICPU` channel；同一 endpoint 的第二次调用复用资源并导入初始 DDR key | 首次创建 `COMM_ENGINE_CPU` channel；第二次调用校验 peer/endpoint 未变化并复用 channel，按远端 endpoint 类型决定是否导入 DDR key |
| `UpdateRankOptions()` | Batch_Copy 路由发布后返回 `BM_NOT_SUPPORTED`；不存在 Host Batch_Copy peer 时保持现有 DEVICE_URMA 行为 | Batch_Copy 初始化完成后返回 `BM_NOT_SUPPORTED` |
| `RemoveRanks()` | 存在 Host Batch_Copy peer 时返回 `BM_NOT_SUPPORTED`；否则保持现有 DEVICE_URMA 行为 | 存在 Batch_Copy peer 时返回 `BM_NOT_SUPPORTED` |
| 连接和 private-data 函数 | 序列化 Device `EndpointLoc` | 序列化 Host `EndpointLoc` |
| Remote I/O 函数 | 通过 AICPU kernel 发起 device 数据面 | 使用 `HcommReadOnThread/HcommWriteOnThread`，thread 传 0，并用 `HcommChannelFenceOnThread` 完成同步 |
| H2D staging、kernel launch 和 stream 同步函数 | 管理设备侧执行资源 | 复用初始化阶段已注册的 Host staging MR；不加载或启动 device kernel |

新增 `HYBM_DOP_TYPE_HOST_DEVICE_URMA`（公共 Python 名为 `BmDataOpType.HOST_DEVICE_URMA`）。鲲鹏和
昇腾两端使用同一 `HOST_DEVICE_URMA` 协议位进入 URMA 建链、peer 过滤和
`TransportMemoryKey.keys[0, 6 * KEY_SIZE)` device key 区域。
`ComposeTransportManager::OpenDeviceTransport()` 调用 `CreateHostDeviceUrmaTransportManager()`：
`ASCEND_NPU` 构建
创建 `DeviceUrmaTransportManager`，`XPU_TYPE=NONE` 构建创建 `HostUrmaTransportManager`，
`NVIDIA_GPU` 构建返回 `BM_NOT_SUPPORTED`。既有 `HOST_URMA` 继续进入 HCOM，既有 `DEVICE_URMA` 继续
表示 Device↔Device HCOMM，`DEVICE_UBOE` 仍由 Device manager 处理。

鲲鹏部署加载 `libhcomm_cpu_ub_plugin.so`。该 plugin 注册 `COMM_PROTOCOL_UBC_TP` 和
`COMM_PROTOCOL_UBC_CTP`，只接管 `ENDPOINT_LOC_TYPE_HOST` endpoint，并要求 channel engine 为
`COMM_ENGINE_CPU`。`ASCEND_HOME_PATH` 非空时，插件安装在 `${ASCEND_HOME_PATH}/hcomm_plugin/`；
仅在 `ASCEND_HOME_PATH` 为空的独立调试环境中通过 `HCOMM_NIC_PLUGIN_SO` 显式指定路径。鲲鹏节点的
runtime device 数量必须为 0；CANN loader 检测到 device 时会跳过 Host NIC plugin 加载。

两个 manager 在 private data 中填写 `UrmaEndpointDesc.loc`。`URMA_PRIVATE_DATA_VERSION` 为 2，
`SerializePrivateData()/ParsePrivateDataToEndpointDesc()` 校验 magic、version、payloadLen 和容量；版本或
负载不匹配时返回 `BM_NOT_SUPPORTED`。

两端均通过 `HcommChannelDescInit()` 初始化 channel 描述符，填写远端 endpoint，设置
`exchangeAllMems = true`，并按 `(localRank > peerRank) ? CLIENT : SERVER` 选择互补的 socket role。
NPU 侧使用 `COMM_ENGINE_AICPU`，鲲鹏侧使用 `COMM_ENGINE_CPU`。

初始化行为如下：

- `ASCEND_NPU` 构建加载 `DlRtApi` 和 `DlHcommApi`，并初始化 `DataOpDeviceURMA`。
- `XPU_TYPE=NONE` 构建加载 `DlHcommApi`，`HostComposeDataOp` 为 `HOST_DEVICE_URMA` 复用现有
  `HostDataOpRDMA`；单条和批量接口通过 `HostUrmaTransportManager` 发起 CPU 数据面，未注册 tensor 经
  建链前已注册的 Host staging MR 搬运。
- `DlApi::CleanupLibrary()` 统一清理已加载的 wrapper。

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
    participant M as 固定 Batch_Copy 元数据区

    CE->>CE: LoadExtendLibrary(): DlHcommApi only
    CE->>CC: InitTransManager() / OpenDeviceTransport(HOST_DEVICE_URMA)
    CC->>CC: CreateHostDeviceUrmaTransportManager() [XPU_TYPE=NONE]
    CC->>HM: OpenDevice(options)
    HM->>HM: InitLocalHostInfoLocked()
    HM->>HM: BuildLocalHostEndpointDescLocked(rankId, nic)
    HM->>H: HcommEndpointCreate(HOST, UBC_CTP)
    HM->>HM: InitHostTransferFlagLocked() / flag = 1
    HM->>H: HcommMemReg(flag, HOST)
    CE->>CE: InitDataOperator(): HostDataOpRDMA for HOST_DEVICE_URMA
    CE->>HM: RegisterMemoryRegion(HOST_DRAM)
    HM->>H: HcommMemReg(GVA, HOST)
    HM->>H: HcommMemExport(DDR + Host flag)

    NE->>NE: LoadExtendLibrary(): DlRtApi + DlHcommApi
    NE->>NC: InitTransManager() / OpenDeviceTransport(HOST_DEVICE_URMA)
    NC->>NC: CreateHostDeviceUrmaTransportManager() [ASCEND_NPU]
    NC->>DM: OpenDevice(options)
    DM->>DM: InitLocalDeviceInfoLocked()
    DM->>DM: BuildLocalEndpointDescLocked(UBC_CTP)
    DM->>H: HcommEndpointCreate(DEVICE, UBC_CTP)
    NE->>NE: InitDataOperator(): create DataOpDeviceURMA

    CE->>CE: ImportEntityExchangeInfo(完整 NPU peer 集合)
    CE->>CC: ImportForTransportManager() / Prepare(no memKeys) / Connect()
    CC->>HM: Prepare(NPU peer endpoints, no memKeys)
    loop 每个 NPU peer
        HM->>HM: ParsePrivateDataToEndpointDesc(v2)
        HM->>HM: PreparePeerLocked(peerRank)
        HM->>H: HcommChannelCreate(COMM_ENGINE_CPU)
    end
    NE->>NE: ImportEntityExchangeInfo(完整 Host peer 集合)
    NE->>NC: ImportForTransportManager() / Prepare(no memKeys) / Connect()
    NC->>DM: Prepare(Host peer endpoints, no memKeys)
    loop 每个 Host peer
        DM->>DM: ParsePrivateDataToEndpointDesc(v2)
        DM->>DM: PreparePeerLocked(peerRank)
        DM->>H: HcommThreadAlloc(COMM_ENGINE_AICPU_TS)
        DM->>H: HcommChannelCreate(COMM_ENGINE_AICPU)
    end

    CE->>CE: ImportSliceExchangeInfo(初始全量 slice)
    CE->>CE: ImportForSegment() / ImportForTransportPrecheck()
    CE->>CC: ImportForTransport() / ConnectWithOptions()
    CC->>HM: Prepare(相同 peer endpoints + memKeys)
    HM->>HM: ValidateInitialPeerSetLocked() / reuse CPU channels

    NE->>NE: ImportSliceExchangeInfo(初始全量 DDR slice)
    NE->>NE: ImportForSegment() / ImportForTransportPrecheck()
    NE->>NC: ImportForTransport() / ConnectWithOptions()
    NC->>DM: Prepare(相同 Host endpoints + 全量 DDR keys)
    loop 每个 Host peer
        DM->>DM: ValidateInitialPeerSetLocked() / reuse channel/thread
        DM->>H: ImportRemoteMemKeysLocked() / HcommMemImport()
        H-->>DM: outMem.addr / size
        DM->>DM: ValidateImportedGvaLocked(exportDesc, remoteAddr, remoteSize, view)
        DM->>DM: ValidateImportedFlagLocked(flagOutMem, 8 B)
    end
    DM->>DM: TryPublishBatchCopyRouteLocked()
    DM->>DM: AcquireBatchCopyRouteOwnerLocked(userDeviceId_)
    DM->>M: ClearBatchCopyRouteMagicLocked()
    DM->>M: ClearBatchCopyCompletionAreaLocked()
    DM->>H: RegisterBatchCopyCompletionRegionLocked(BATCH_COPY_COMPLETION_OFFSET, 512 B)
    DM->>DM: BuildBatchCopyRouteTableLocked()
    DM->>M: WriteBatchCopyRouteTableLocked(magic = 0)
    DM->>M: PublishBatchCopyRouteMagicLocked()
    DM-->>NC: 第二阶段 Prepare() 成功
    NC-->>NE: ConnectWithOptions() 成功
    NE-->>NE: ImportSliceExchangeInfo() 成功
    NE->>NE: MemEntityDefault::Mmap() / dramSegment_->Mmap()
```

当前 entity 流程分为两阶段。`ImportEntityExchangeInfo()` 通过 `ImportForTransportManager()` 首次调用
`ComposeTransportManager::Prepare()/Connect()`，此时只有完整 peer endpoint，没有 memory key；
`ImportSliceExchangeInfo()` 随后执行 `ImportForSegment()`、`ImportForTransportPrecheck()` 和
`ImportForTransport()`，再由 `TransportManager::ConnectWithOptions()` 第二次调用
`ComposeTransportManager::Prepare()`，携带初始全量 memory key。Host/Device manager 的 `Prepare()`
必须支持相同 endpoint 的幂等复用，Device manager 只在第二阶段导入信息完整后发布路由。具体修改点和
原因如下：

| 文件/位置 | 修改内容 | 原因 |
| --- | --- | --- |
| `smem_bm_def.h`、`hybm_def.h`、SMEM/TRANS 转换和 Python wrapper | 追加 `HOST_DEVICE_URMA` 公共/内部 bit，不重排既有值；同步合法值、冲突和无卡构建掩码 | 明确区分 HCOMM Host↔Device 与既有 `HOST_URMA` HCOM、`DEVICE_URMA` Device↔Device 路径，并让异构两端协商同一 bit |
| `hybm_entity_tag_info.cpp`、`hybm_entity_default.cpp` | 增加新协议的字符串双向映射、compatible info、能力和加载掩码 | DataOpType 会参与 tag 和 rank-to-rank 协商，不能只在 Compose 本地解释 |
| `device/urma/hcomm_transport_manager.{h,cpp}`、`device_urma_transport_manager.cpp` 顶部序列化 helper | 迁移到 `transport/urma/` 公共目录；共享 endpoint/MR 描述符、private-data v2 编解码和 HCOMM 封装 | Host/Device manager 使用同一套 wire format 和资源封装 |
| `transport/urma/urma_transport_common.{h,cpp}` | `UrmaEndpointDesc` 保存完整 `EndpointLoc`；`SerializePrivateData()/ParsePrivateDataToEndpointDesc()` 严格校验 magic、v2、payloadLen 和容量 | HCOMM 根据 endpoint 实际位置创建 Host/Device 资源 |
| `compose_transport_manager.cpp`，`OpenDeviceTransport()/CreateHostDeviceUrmaTransportManager()` | 新增 `HOST_DEVICE_URMA` 分支并按本端平台创建 Host/Device manager；既有 `DEVICE_URMA` 分支不改语义 | manager 实现与本地平台一致，同时不改变现有 Device↔Device 行为 |
| `under_api/dl_api.cpp`，`LoadExtendLibrary(DL_EXT_LIB_DEVICE_URMA)` | `ASCEND_NPU` 加载 RT + HCOMM；`XPU_TYPE=NONE` 加载 HCOMM | 无卡鲲鹏只依赖 Host HCOMM 能力 |
| `data_operation/host/hybm_data_op_factory.*`、`hybm_compose_data_op.cpp` | 为 `HOST_DEVICE_URMA` 按平台创建现有 `HostDataOpRDMA` 或 `DataOpDeviceURMA`，不增加 `HostDataOpRDMA` 字段 | 支持两鲲鹏主动拷贝及最终异构流程，并避免无必要的类布局修改 |
| `host/urma/host_urma_transport_manager.{h,cpp}` | 实现 Host endpoint、DDR/flag 注册导出、Host peer MR 导入、CPU Read/Write/Fence、channel 和清理 | 承载无卡鲲鹏的 URMA 控制面与主动数据面 |
| `hybm_conn_based_segment.cpp`，`MapSlice()` | `ASCEND_NPU` 构建按现有条件执行 `HalHostRegister()`；无卡构建直接把固定 mmap 结果加入 VA manager，DVA 置 0 | 鲲鹏 DDR 不经过本地 device 映射，无卡节点不能依赖 HAL device 注册 |
| `HostUrmaTransportManager::RegisterMemoryRegion()/QueryMemoryKey()` | 使用 `FindAllocByVa(mr.addr, HVM_GVA)` 校验完整区间属于本地 rank 的 Host GVA 分配记录，再直接 `HcommMemReg(mr.addr)` 并导出同一 GVA | 鲲鹏 DDR 使用 Host VA/GVA，不经过 DVA；任意 Host 指针不会进入 `HOST_DEVICE_URMA` 导出集合 |
| `HcommTransportManager::HcommMemImport()` | 使用 `UrmaExportDesc.memoryType` 设置 view type，并校验 UBC 返回的 addr/size | CANN UBC import 只回填 addr/size，内存类型由 MemFabric 描述符确定 |
| `DeviceUrmaTransportManager::ImportRemoteMemKeysLocked()` | 调用 `ValidateImportedGvaLocked(exportDesc, remoteAddr, remoteSize, view)`，校验注册地址、导出描述符、导入 view 和 HOST_DRAM 类型；导入 transfer flag 后校验 addr 和 8 B 大小 | RangeEntry 只在导入 view 与发布 GVA 一致时省略地址转换字段；UBC 不回填普通 MR 和 flag 的 `outMem.type` |
| `src/hybm/csrc/common/hybm_define.h`、`hybm_batch_copy_route.h` | 增加 2 MiB route 区、34 MiB control 映射常量及 Host/AICPU 共享路由 ABI | 固定地址、结构大小和 offset 使用同一组编译期定义 |
| `hybm_gva.cpp`，`hybm_init_hbm_gva()/HybmModernInitMetaGva()/HybmLegacyInitMetaGva()` | meta 物理申请和映射使用 34 MiB，起点为 `HYBM_DEVICE_CONTROL_ADDR` | 2 MiB route 区与 32 MiB HYBM 元数据区一次性映射 |
| `src/hybm/csrc/hybm_entry.cpp`，`hybm_uninit()` | Modern 使用 `HYBM_DEVICE_CONTROL_ADDR` unmap，保留 `g_baseAddr` 释放 1 GiB VA；Legacy 按 control 起点和 34 MiB 大小 free | 映射地址、物理 handle 和 1 GiB VA reservation 是不同资源，初始化/销毁边界必须成对 |
| `DeviceUrmaTransportManager::Prepare()/PreparePeerLocked()` | 首次无 key 调用创建 channel/thread 并固定 Host peer 集合；第二次全量 key 调用校验 endpoint 未变化、复用资源、导入初始 DDR MR/flag，最后调用 `TryPublishBatchCopyRouteLocked()` | 对齐 entity 先交换 endpoint、后导入 slice 的两阶段调用顺序，避免重复创建通信资源或提前发布不完整路由 |
| `HostUrmaTransportManager::Prepare()/ValidateInitialPeerSetLocked()` | 首次创建 CPU channel 并固定 peer 集合；第二次校验 peer/endpoint 未变化并复用 channel；Host peer 导入 DDR key，NPU peer 不导入 HBM key | 同时支持两鲲鹏主动拷贝验证和最终鲲鹏对 NPU 的 DDR 导出，并接受 entity 的第二阶段幂等 `Prepare()` |
| `DeviceUrmaTransportManager::RollbackInitialImportsLocked()` | 第二阶段任一 peer 导入或 route 发布失败时，按逆序释放本次调用新增的全部 import；首次阶段创建的 channel/thread 由 manager 清理流程持有 | 防止跨 peer 的部分导入残留，同时不误删前一阶段已经建好的资源 |
| `DeviceUrmaTransportManager::TryPublishBatchCopyRouteLocked()/PublishBatchCopyRouteLocked()` | 确认固定 peer 集合中的所有 channel/thread、DDR range 和 flag 完整后，按 `userDeviceId_` 获取发布权，清 magic/completion，注册 512 B completion 区并构建、写入、发布 route | 仅含 Host peer 且初始信息完整的 manager 占用卡级路由资源，其他 Device URMA entity 不受影响 |
| `DeviceUrmaTransportManager::UpdateRankOptions()/RemoveRanks()` | 已固定 Host Batch_Copy peer 集合时返回 `BM_NOT_SUPPORTED`；没有 Host Batch_Copy peer 时保留现有动态 rank/key 行为 | P0 不接受后续新增 MR、peer 删除或断链，同时保持普通 DEVICE_URMA 场景兼容 |
| `HostUrmaTransportManager::UpdateRankOptions()/RemoveRanks()` | Batch_Copy 初始化完成后的变更请求返回 `BM_NOT_SUPPORTED` | P0 的 Host peer 集合和 DDR 导出集合在初始化后保持不变 |
| `DeviceUrmaTransportManager::CloseDevice()` | 先执行 `ClearBatchCopyRouteMagicLocked()`，再注销 completion 和通信资源，最后释放 owner | 防止 AICPU 读取已经释放的 HCOMM 句柄 |
| HBM 地址范围校验 | 控制区起点使用 `HYBM_BATCH_COPY_META_ADDR` | 业务目的 HBM 排除完整 34 MiB 控制区 |
| `src/smem/python/memfabric_hybrid/setup.py` | 把共享路由 ABI 头加入 `_AICPU_WLIST` | wheel 首次 import 会从随包源码交叉编译 AICPU kernel，构建输入必须包含新头文件 |

初始化约束为：第一次 `ImportEntityExchangeInfo()` 一次性携带完整 peer 集合，第一次
`ImportSliceExchangeInfo()` 一次性携带 Batch_Copy 使用的全部 DDR 区间；后续不增加 MR，不删除 peer，
也不处理断链。发布事务边界为：第一阶段完成全部 Host peer 建链 → 第二阶段 NPU 导入并校验所有 DDR
MR/flag → 获取每卡唯一 owner → 清零 completion area 并注册固定 completion 区 → 构建固定 route image →
以 magic=0 同步写入卡级元数据区 → 最后写 magic → 初始化成功。任一步失败都不得留下有效 magic，
第二阶段失败时释放该阶段新增的全部 import。Close 前调用方保证没有在途算子，manager 先清 magic，
再按逆序释放 HCOMM 资源。

新增和拆分函数均不超过 50 行非空非注释代码，嵌套深度不超过 4 层。

### 3.1.6 Batch_Copy 执行流程

详细时序如下：

```mermaid
sequenceDiagram
    participant U as 图执行器/调用方
    participant O as HybmBatchCopy AICPU
    participant T as 固定 BatchCopyRouteTable
    participant B as 现有 HybmBatchRead
    participant H as HCOMM
    participant P as 鲲鹏 DDR peer
    participant D as 本地 HBM

    U->>O: srcList, dstList, lenList, size
    O->>O: 校验四输入并获取单实例执行权
    alt 参数非法或已有算子在途
        O-->>U: BM_INVALID_PARAM / BM_BUSY
    else 进入执行
        O->>T: 读取 HYBM_BATCH_COPY_META_ADDR header
        T-->>O: magic, peerCount, rangeCount
        O->>O: 校验 magic 和 64/1024 固定规格
        O->>T: 读取 header、peer entries、range entries
        O->>O: 校验固定 offset、数量和区间有序非重叠
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
                O->>D: completionCell = 0
                O->>B: HybmOneSideOpParam(group, thread, channel, flags)
                B->>H: BatchTransfer(每片最多 1000 条)
                opt batch 接口不支持
                    B->>H: 逐条 HcommReadOnThread
                end
                B->>H: HcommChannelFenceOnThread
                B->>H: Read remoteFlag -> completionCell
                B-->>O: 提交结果
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

1. 校验参数结构、三组列表地址和 `size != 0`，检查按 `size` 计算列表/描述符字节数时无整数溢出，
   并以原子方式取得 P0 单实例执行权。
2. 从 `HYBM_BATCH_COPY_META_ADDR` 读取 header，校验 magic、`peerCount <= 64` 和
   `rangeCount <= 1024`。
3. 按固定 offset 读取 peer/range 表，校验 `rangeCount <= peerCount * 16`、每 peer 区间数不超过 16，
   并检查区间升序且不重叠。
4. 扫描所有输入但暂不提交传输：
   - 0 长度条目直接跳过。
   - 检查源/目的地址加长度不溢出。
   - 二分查找包含完整源区间的 range entry。
   - 校验 `peerIndex` 有效、thread/channel 非 0。
   - 直接使用 `srcGva` 作为 `hcommSrc`，并检查目的地址不落入
     `[HYBM_BATCH_COPY_META_ADDR, SVM_END_ADDR)` 控制区。
5. 按 `peerIndex` 分组；组内保持输入顺序，peer 组按索引升序处理。
   所有条目长度均为 0 时直接返回 `BM_OK`，不调用 HCOMM。
6. 对每个已使用 peer，先清零对应 completion cell，再构造 `HybmOneSideOpParam`：组内源/目的/长度
   列表分别写入 `src_buf_addr_list/dst_buf_addr_list/len_list`，路由表中的句柄和 flag 地址写入
   `thread/channel/remote_flag_addr`，本 peer 的 cell 写入 `local_flag_addr`，`flag_size` 固定为 8 B。
7. 调用现有 `HybmBatchRead()`。该函数负责每 1000 条分片调用 `HcommBatchTransferOnThread`、
   不支持时逐条回退 `HcommReadOnThread`、执行一次 `HcommChannelFenceOnThread`，最后把 remote flag
   读入 completion cell；其他错误立即停止后续 peer 提交。
8. 轮询所有已使用 peer 的 completion cell，全部完成或达到 60 秒超时后返回。
9. 释放单实例执行权。

混合 peer 使用不同 HCOMM thread，线程之间没有完成顺序保证。每 peer 独立完成后由 AICPU 汇聚，
保证算子成功返回时所有目的 HBM 数据均可见。completion cell 的清零、DMA 可见性和轮询屏障使用
昇腾 950/HCOMM 支持的设备侧
内存屏障，不能仅依赖 Host C++ 的 `std::atomic_thread_fence`。

所有输入在提交前完成校验，因此参数错误不会导致部分拷贝。HCOMM 提交开始后的链路错误可能造成
部分完成，算子返回失败但不回滚已写入的 HBM；调用方不得在失败后使用整个 batch 的输出。

### 3.1.7 并发和顺序语义

- P0 每张 NPU 同时只允许一个 Batch_Copy；并发调用返回 `BM_BUSY`。
- 同一 peer 内保持输入顺序，并在该 peer 最后执行 fence。
- 不同 peer 之间不承诺写入先后顺序，但成功返回时全部完成。
- 调用方不得提供相互重叠的目的 HBM 区间；P0 不做 `O(n²)` 的重叠检测。
- 路由在初始化完成前发布一次，运行期不可变；P0 不支持动态 MR、peer 删除或断链。
- 最终 Close 前由调用方保证没有在途 Batch_Copy，然后先清除 route magic，再释放通信资源。

## 3.2 技术选型

- 路由键使用 MemFabric GVA，算子从固定 HBM 路由表解析 peer 和 HCOMM 句柄。
- 路由表位于 `HYBM_DEVICE_META_ADDR` 前方独立的 2 MiB 卡级元数据区。
- 路由在初始化期一次性发布，运行期只读。
- 鲲鹏 DDR 固定映射并以同一 GVA 调用 `HcommMemReg()`。
- 地址区间全局排序，算子使用二分查找并按固定 64 个 peer 桶分组。
- 每个 peer 复用 `HybmBatchRead` 按 1000 条分片提交，完成状态通过独立 completion cell 汇聚。

## 3.3 功能与性能设计

### 3.3.1 代码影响范围

| 模块 | 修改内容 |
| --- | --- |
| `src/hybm/csrc/common/` | 增加卡级 route/control 地址常量和 Host/AICPU 共享固定表定义 |
| `src/hybm/include/hybm_def.h`、`src/smem/include/host/smem_bm_def.h` | 定义错误码 `BM_BUSY (-11)`；追加内部/公共 `HOST_DEVICE_URMA` bit，既有值不变 |
| `src/hybm/csrc/driver/`、`hybm_entry.cpp` | 元数据物理映射、回滚和释放使用 34 MiB 控制区边界 |
| `src/hybm/csrc/mm/hybm_conn_based_segment.cpp` | 无卡构建的固定 GVA mmap 不执行 `HalHostRegister()` |
| `src/hybm/csrc/under_api/` | 无卡构建的 `HOST_DEVICE_URMA` 初始化加载 HCOMM |
| `src/hybm/csrc/data_operation/host/` | `HOST_DEVICE_URMA` 按本端平台复用 Host staging/主动 CPU 数据面或 Device URMA 数据面 |
| `src/hybm/csrc/transport/` | 抽取公共 URMA/HCOMM 封装，在 `UrmaEndpointDesc` 中携带 endpoint 位置，并增加 route 和 completion 接口 |
| `src/hybm/csrc/transport/host/urma/` | 新增无卡 Host endpoint、固定 GVA MR/flag 导出、Host peer MR 导入和 CPU Read/Write/Fence channel |
| `src/hybm/csrc/transport/device/urma/` | 增加 import 地址校验、路由构建和 completion 注册 |
| `src/hybm/ops/hybm_kernel/hybm_batch_copy.{h,cc}` | 新增 `HybmBatchCopy` 参数校验、查表、分组，并调用现有 `HybmBatchRead` 完成提交和完成汇聚 |
| `src/hybm/ops/hybm_kernel/libcann_hybm_kernel.json` | 注册 `HybmBatchCopy` 函数；该 JSON 是当前 CMake 和 kernel loader 使用的配置 |
| `src/hybm/ops/CMakeLists.txt`、`src/hybm/ops/hybm_kernel/CMakeLists.txt` | 编译 `hybm_batch_copy.cc`，把共享路由 ABI 头加入编译依赖，并继续把生成的 `.so` 封装为 `cann-hybm-compat.tar.gz` |
| `src/smem/python/memfabric_hybrid/setup.py` | 把共享路由 ABI 头加入 AICPU 源码白名单，随 NPU wheel 打包 |
| `test/ut/testcase/hybm/` | 路由发布、查表、混合 peer、回滚和异常路径 UT |
| `doc/installation_aicpu_kernel.md` | 增加算子名称、版本兼容和验证方法 |

### 3.3.2 性能策略

- 路由表只在初始化建链完成后写入一次，不在数据热路径写入。
- AICPU 只读取实际 `peerCount/rangeCount` 对应的内容。
- 地址区间排序后使用二分查找，1024 个区间最多比较 10 次；分组使用固定 64 桶，避免哈希表。
- HCOMM batch 每组最多 1000 条，沿用现有内核限制和 fallback 行为。
- 每个 peer 只执行一次 fence 和一次 completion read。
- `size` 没有固定总条数上限。临时数组按 `size` 受检分配，所有长度乘法先做溢出检查；分配失败记录
  batch size 并返回 `BM_MALLOC_FAILED`。每次 HCOMM 提交最多包含 1000 条。
- completion 默认超时为 60 秒，与 Device URMA 数据面超时保持一致。

### 3.3.3 返回语义

| 场景 | 返回值 | 是否可能已写入部分 HBM |
| --- | --- | --- |
| 空指针、`size == 0`、长度计算或地址溢出 | `BM_INVALID_PARAM` | 否 |
| 路由表未初始化或布局非法 | `BM_NOT_INITIALIZED` / `BM_INVALID_PARAM` | 否 |
| 地址未命中或 peer 句柄无效 | `BM_NOT_CONNECTED` | 否 |
| 并发调用 | `BM_BUSY` | 否 |
| 临时内存不足 | `BM_MALLOC_FAILED` | 否 |
| HCOMM 提交/fence 失败 | `BM_ERROR` | 是 |
| completion 超时 | `BM_TIMEOUT` | 是 |
| 全部成功 | `BM_OK` | 是，且全部可见 |

`BM_BUSY` 的值为 `-11`，表示可重试的单实例并发冲突。该值位于 `BM_NOT_SUPPORT_FUNC (-10)` 之后，
与 `BM_NOT_SUPPORTED (-100)`、`BM_NOT_CONNECTED (-101)` 不冲突。

## 3.4 安全隐私与 DFX 设计

发布前校验 64 peer、每 peer 16 range、总计 1024 range 的边界、GVA
区间和 HCOMM 句柄；算子提交前校验输入、目的 HBM 范围和路由命中。错误日志记录
`userDeviceId/phyDeviceId/rankId/peerIndex`、batch index、地址/长度和 HCOMM 返回码，但不读取或打印
业务数据。
Host 与 AICPU 对共享结构执行 `sizeof/offsetof` 断言，UT 覆盖固定 offset、最大规格、route 构建、查找、
发布失败和每卡 owner 冲突。

## 3.5 编程与调用设计

### 3.5.1 编程模型基本设计

#### 开发环境

- 昇腾 950 NPU 与支持 URMA/HCOMM 的鲲鹏服务器。
- CANN/Ascend 环境由 `ASCEND_HOME_PATH` 指定。
- 昇腾节点以 `XPU_TYPE=NPU` 构建；鲲鹏无卡节点以 `XPU_TYPE=NONE` 构建。两端均包含目标 HCOMM/URMA
  依赖，只有昇腾节点构建和安装 AICPU 算子。
- 鲲鹏节点把 `libhcomm_cpu_ub_plugin.so` 安装到 `${ASCEND_HOME_PATH}/hcomm_plugin/`。独立调试且
  `ASCEND_HOME_PATH` 为空时，通过 `HCOMM_NIC_PLUGIN_SO` 指向该文件。
- 鲲鹏节点不安装 NPU，HCOMM runtime device 探测结果必须为 0。
- AICPU 算子沿用两种交付路径：`script/kernel/build_ops_run.sh` 生成独立 run 包；NPU wheel 在首次
  `import memfabric_hybrid` 时由 `_provision.py` 使用随包源码构建并安装。两条路径使用相同的源文件、
  共享路由 ABI 头和 `libcann_hybm_kernel.json`。

#### 使用约束

- NPU 侧通过 `HYBM_FLAG_INIT_SHMEM_META` 完成 HYBM 初始化，并完成 URMA 建链、鲲鹏 DDR
  注册/导出和 NPU 侧导入。
- `src_ddr_ptr_list` 的元素必须是 MemFabric GVA。
- 三组列表本身必须位于 AICPU 可访问的设备内存。
- 目的地址必须是本地 NPU HBM，并已满足 HCOMM 本地内存访问要求。
- P0 单张 NPU 只允许一个在途 Batch_Copy。

#### 验收设计

构建和基础测试命令：

```bash
bash script/build_and_pack_run.sh --build_hcom ON --build_hcom_rdma ON
bash script/build_and_pack_run.sh --xpu_type NONE --build_hcom ON --build_hcom_rdma OFF
bash script/kernel/build_ops_run.sh
bash script/run_ut.sh --fast UrmaTransportManager
```

前两条分别是昇腾节点和鲲鹏节点构建命令；第三条验证独立 run 包路径，只在昇腾/CANN 环境执行。
NPU wheel 还需覆盖首次 `import memfabric_hybrid` 的自动制备路径，并确认安装后的 JSON 包含
`HybmBatchCopy`。

硬件验收矩阵至少包括：

| 维度 | 取值 |
| --- | --- |
| 拓扑 | 1 NPU × 1 CPU、1 × 16、1 × 64（规格验证）、16 × 16（目标部署） |
| batch size | 1、16、128、999、1000、1001，以及按硬件可用内存选取的较大 batch |
| 单条长度 | 1 B、4 KiB、64 KiB、1 MiB、4 MiB |
| 地址分布 | 单 MR、每 peer 16 MR、1024 MR 满表、跨 peer 混合、区间边界 |
| HCOMM 能力 | batch 可用、batch 不支持回退单条 |
| 异常 | URMA private-data v1/v2 不匹配、导入失败、表损坏、HCOMM 提交失败、完成超时 |

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
| `size` | 输入 | `uint32_t` | 三个列表的元素个数，不是字节数 | 大于 0；列表可访问且临时空间可分配；无额外固定上限 |

**返回值：**

| 返回值 | 说明 |
| --- | --- |
| `BM_OK` | 全部非零长度条目已完成，目的 HBM 数据可见 |
| `BM_INVALID_PARAM` | 参数、地址、长度或表项非法 |
| `BM_NOT_INITIALIZED` | 固定路由表 header magic 尚未发布 |
| `BM_NOT_CONNECTED` | 源地址无路由或 peer 句柄无效 |
| `BM_BUSY` | 已有算子在途 |
| `BM_MALLOC_FAILED` | AICPU 临时空间分配失败 |
| `BM_TIMEOUT` | 一个或多个 peer 未在 60 秒内完成 |
| `BM_ERROR` | HCOMM 或其他内部错误 |

其中 `BM_BUSY` 是新增的 HYBM 公共错误码，数值为 `-11`。

**异常处理：** 参数和路由错误在任何 HCOMM 提交前返回。提交后的错误可能部分写入，调用方应丢弃
本次 batch 的全部输出或按上层重试协议重新读取。

**约束：** 不支持重叠目的区间、多实例并发和 raw remote VA。

**变更说明：** `HybmBatchCopy` 使用独立符号，`HybmBatchRead/HybmBatchWrite` 接口语义保持不变。

**参考代码：**

```cpp
HybmBatchCopyParam param{};
param.srcDdrPtrList = deviceSrcGvaList;
param.dstHbmPtrList = deviceDstHbmList;
param.ptrLenList = deviceLengthList;
param.size = batchSize;

// 图执行器按 libcann_hybm_kernel.json 中的 HybmBatchCopy 注册名在业务 stream 上拉起。
// 返回成功后，deviceDstHbmList 指向的非零长度区间均已完成写入。
```

#### 3.5.2.2 内部路由发布接口

内部接口不对 SMEM/HYBM 公共 API 暴露：

```cpp
Result DeviceUrmaTransportManager::PreparePeerLocked(uint32_t peerRank,
                                                     const TransportRankPrepareInfo &info);
Result DeviceUrmaTransportManager::RollbackPreparedPeersLocked(const std::vector<uint32_t> &peerRanks);
Result DeviceUrmaTransportManager::FreezeInitialPeerSetLocked(const HybmTransPrepareOptions &options);
Result DeviceUrmaTransportManager::ValidateInitialPeerSetLocked(const HybmTransPrepareOptions &options) const;
Result DeviceUrmaTransportManager::RollbackInitialImportsLocked(const InitialImportSnapshot &snapshot);
Result DeviceUrmaTransportManager::ValidateImportedGvaLocked(const UrmaExportDesc &exportDesc,
                                                             uint64_t remoteAddr, uint64_t remoteSize,
                                                             const UrmaCommMem &view) const;
Result DeviceUrmaTransportManager::ValidateImportedFlagLocked(const HcommCommMem &flagOutMem) const;
Result DeviceUrmaTransportManager::TryPublishBatchCopyRouteLocked();
Result DeviceUrmaTransportManager::PublishBatchCopyRouteLocked();
Result DeviceUrmaTransportManager::AcquireBatchCopyRouteOwnerLocked();
Result DeviceUrmaTransportManager::ClearBatchCopyCompletionAreaLocked();
Result DeviceUrmaTransportManager::RegisterBatchCopyCompletionRegionLocked();
Result DeviceUrmaTransportManager::BuildBatchCopyRouteTableLocked(BatchCopyRouteTable &table) const;
Result DeviceUrmaTransportManager::WriteBatchCopyRouteTableLocked(const BatchCopyRouteTable &table);
Result DeviceUrmaTransportManager::PublishBatchCopyRouteMagicLocked();
Result DeviceUrmaTransportManager::ClearBatchCopyRouteMagicLocked();
```

这些函数是 Device manager 私有实现，不对 SMEM/HYBM 公共 API 或 entity 暴露。第一次 `Prepare()`
固定 Host peer 集合并创建通信资源；第二次 `Prepare()` 校验同一集合、导入初始全量 DDR key，并调用
`TryPublishBatchCopyRouteLocked()`。只有全部固定 peer 的 MR/flag 完整时，该函数才调用
`PublishBatchCopyRouteLocked()` 获取每卡发布权、清除 magic、注册固定 completion 区并构建发布路由表。
P0 的 `UpdateRankOptions()`、`RemoveRanks()` 不更新 Batch_Copy 路由并返回 `BM_NOT_SUPPORTED`；没有
Host Batch_Copy peer 的 Device URMA manager 保持现有行为。最终销毁前清除 magic。下层失败处负责记录
ERROR 日志，上层只透传已记录的根错误时不重复打印。

### 3.5.3 编程手册设计

在现有 AICPU 安装文档和 API 文档中增加：

1. 支持平台、HCOMM/RDMA 构建开关和 run 包安装。
2. `HybmBatchCopy` 四输入语义，特别说明 `size` 为元素个数。
3. MemFabric GVA 与 raw remote VA 的区别。
4. 建链、内存注册/导入、算子拉起和最终资源释放的完整示例。
5. 每卡 64 peer、每 peer 16 range、总计 1024 range 的容量和单实例并发限制。
6. 错误码、部分完成语义、超时和重试建议。
7. user/physical device owner、peer/channel/thread 和 route 数量日志的排障方法。

---

# 4. 缺点和风险

| 风险 | 影响 | 缓解措施 |
| --- | --- | --- |
| 源地址不是全局唯一 GVA | 可能路由到错误 CPU peer | 发布时拒绝跨 peer 重叠地址区间 |
| 固定 GVA mmap 失败 | 鲲鹏源 DDR 无法按 GVA 注册 | 限制并预留可映射 GVA 窗口；初始化失败且不发布路由 |
| HCOMM import 重定位地址 | 算子直接使用 GVA 会读错地址 | 建链时强制校验 `view.addr == remoteAddr`，不相等则拒绝启用 |
| 目标 CANN 未提供无卡 Host UB plugin | 鲲鹏端创建 endpoint 失败 | 部署包含 Host UB plugin 的 CANN 版本，并在启动时检查能力 |
| 元数据映射从 32 MiB 增至 34 MiB | 每张 NPU 额外占用 2 MiB HBM | 保持 2 MiB 大页对齐；同时覆盖 modern/legacy 初始化、回滚和释放 UT |
| 同一 NPU 多 manager 发布 | 固定路由表被相互覆盖 | `BatchCopyRouteOwnerRegistry` 按 `userDeviceId_` 保证每卡单 owner |
| Host/NPU 的 URMA private-data 版本混用 | endpoint 位置被错误解释或建链失败 | 解析端严格校验 v2，并在创建 channel 前拒绝其他版本 |
| 新旧 Host/AICPU 包混用 | 旧 Host 只映射 32 MiB，新算子访问未映射 route 地址 | Host 包和 AICPU 包版本绑定；启动时清零并校验固定 magic |
| 最终销毁时仍有在途算子 | use-after-free、传输失败或设备异常 | 调用方保证 quiescent；先清 route magic，再释放 HCOMM 资源 |
| 多 peer 完成语义不明确 | 算子提前返回，目的数据未完成 | P0 使用每 peer completion cell 汇聚；硬件上验证内存屏障和超时路径 |
| AICPU 轮询占用核 | 长尾或断链时占用 AICPU 资源 | 60 秒超时并返回 `BM_TIMEOUT` |
| 大 batch 的临时描述占用 AICPU 堆 | 内存不足或抖动 | 字节数溢出检查、每 1000 条分片提交、分配失败可诊断 |
| batch 中途失败可能部分完成 | 上层误用部分数据 | 明确失败语义；成功前不对上层声明可用；提供整体重试建议 |
| peer/range 超过固定规格 | 初始化无法发布路由 | 超过 64 peer、每 peer 16 range 或总计 1024 range 时返回资源不足 |
| P0 单实例限制影响多 stream 并发 | 并发调用返回 `BM_BUSY` | 调用方按卡串行提交 Batch_Copy |

`HybmBatchRead/HybmBatchWrite` 调用者无需修改。`HybmBatchCopy` 调用者通过独立 run 包或 NPU wheel
首次 import 自动制备，安装包含该符号的 AICPU kernel，并使用路由表 ABI 一致的 Host
`libmf_hybm_core.so` 与设备包。

---

# 5. 现有技术

本方案复用以下仓库能力：

- `HybmBatchRead()` 提供 HCOMM batch、单条 fallback、fence 和 remote flag 完成通知封装。
- `device_urma_transport_manager.cpp` 提供按 peer 保存的 `RemoteRankState`、远端 MR 导入和 HCOMM view。
- `hybm_define.h/hybm_gva.cpp` 提供 `SVM_END_ADDR` 前的固定 VA 预留和 HBM 大页映射能力。
- `hybm_conn_based_segment.cpp` 提供固定地址 `mmap()` 能力。
- `DeviceUrmaTransportManager::RegisterMemoryRegion()` 处理昇腾本机 HOST_DRAM 的 HVA→DVA 注册；
  `HostUrmaTransportManager` 处理鲲鹏 DDR 的固定 GVA 注册。
- `src/smem/python/memfabric_hybrid/memfabric_hybrid/_provision.py` 提供 NPU wheel 首次 import 时的
  AICPU kernel 构建和安装能力。
- `app/zbal` 提供 Host/Device 共用定长结构和固定 offset 的 AICPU ABI 实践。

---

# 6. 未解决问题

以下硬件和部署条件需在 RFC 批准前确认：

- [ ] 确认目标 CANN 安装包包含并启用 `libhcomm_cpu_ub_plugin.so`，启动日志显示 Host UB plugin
      已加载并注册 UBC_TP/UBC_CTP。
- [ ] 确认鲲鹏进程可固定 mmap 的 GVA 窗口及预留策略；P0 配置 `enable56BitsGva=false`。
- [ ] HCOMM/CANN 团队确认昇腾 950 上 completion cell 清零、remote flag 写入和 AICPU 轮询所需的
      设备内存屏障；若不支持，改用每 peer STARS notify/event 聚合。
- [ ] 确认 `HcommChannelFenceOnThread` 的完成边界，明确 fence 后 remote flag read 是否覆盖该
      thread 上此前所有 batch/single read。
- [ ] 确认昇腾 950 modern 和 legacy 路径均支持从 `HYBM_DEVICE_META_ADDR - 2 MiB` 开始一次性映射
      34 MiB，并验证初始化失败回滚和 uninit 释放边界。

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
- `src/hybm/ops/hybm_kernel/libcann_hybm_kernel.json`
- `src/hybm/ops/CMakeLists.txt`
- `src/hybm/ops/hybm_kernel/CMakeLists.txt`
- `src/smem/python/memfabric_hybrid/setup.py`
- `src/smem/python/memfabric_hybrid/memfabric_hybrid/_provision.py`
- `src/hybm/csrc/common/hybm_define.h`
- `src/hybm/csrc/driver/hybm_gva.cpp`
- `doc/installation_aicpu_kernel.md`

CANN/HCOMM 源码参考（以下路径相对于 HCOMM 源码根目录）：

- `include/hcomm_res_defs.h`
- `src/base_comm/primitives/api_c_adpt/hcomm_c_adpt.cc`
- `src/base_comm/primitives/api_c_adpt/nic_plugin/nic_plugin_dispatcher.cc`
- `src/base_comm/resources/reged_mems/urma_mem.cc`
- `src/legacy/ascend950/unified_platform/resource/buffer/local_ub_rma_buffer.cc`
- `src/legacy/ascend950/unified_platform/resource/buffer/remote_rma_buffer.cc`
- `experimental/base_comm/nic_plugin/README.md`
- `experimental/base_comm/nic_plugin/host_ub_plugin.cc`
- `experimental/base_comm/endpoint/cpu_urma_endpoint.cc`
- `experimental/base_comm/channel/host_cpu_urma_channel.cc`
- `experimental/base_comm/comm_mem/urma_mem.cc`

## B. 术语

| 术语 | 含义 |
| --- | --- |
| GVA | MemFabric 全局虚拟地址，在本方案中必须可唯一定位 CPU peer 和远端 MR |
| HCOMM view | `HcommMemImport()` 返回、可供本地 HCOMM 操作使用的远端内存视图 |
| peer | 一条本地 NPU 到远端鲲鹏 CPU endpoint 的通信关系 |
| completion cell | 每个 peer 的本地 HBM 完成标记，由 remote flag read 写入 |
