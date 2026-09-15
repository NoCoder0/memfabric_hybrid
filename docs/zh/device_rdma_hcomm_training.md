| **WR** | Work Request | 工作请求，描述一次 RDMA 操作 |
| **SGL** | Scatter/Gather List | 散列/聚集列表，描述多段内存 |
| **SGE** | Scatter/Gather Element | 散列/聚集元素 |
| **MR** | Memory Region | 内存区域，RDMA 注册的内存 |
| **DVA** | Device Virtual Address | 设备虚拟地址 |
| **AICPU** | AI CPU | 昇腾 AI 处理器 |
| **HCOMM** | Huawei Communication | 华为设备侧通信库 |
| **HCOM** | Host Communication | 华为 Host 侧通信库 |
| **One-Side** | One-Side RDMA | 单边操作，远端 CPU 不互与 |

---

## 八、RDMA 接口在 HCOMM 中的映射

### 8.1 完整的接口封装层次

```
MF 层（最外层）
  │
  ▼
HCOMM API（Hcomm* 开头）
  │
  ▼
HCOMM 内部（Ra* / Rs* 开头）
  │
  ▼
ibverbs（ibv_* 开头）← 最终 RDMA 接口
```

### 8.2 每个 RDMA 接口的完整映射

> **注意**：`RaRdevInit` 只在 legacy 目录中被调用。base_comm 中通过 `Endpoint::Init()` 直接初始化 RDMA 设备，不经过 `RaRdevInit`。

| RDMA 接口 | HCOMM 封装（Rs*） | MF 调用的 Hcomm* 接口 | 调用流程 |
|-----------|------------------|----------------------|---------|
| **ibv_alloc_pd** | `RsIbvAllocPd()` | `HcommEndpointCreate()` | 创建 Endpoint → `RaRdevInit()` → `RsDrvQueryNotifyAndAllocPd()` |
| **ibv_create_cq** | `RsIbvCreateCq()` | `HcommEndpointCreate()` | 创建 Endpoint → `RaRdevInit()` → `RsDrvCreateCq()` |
| **ibv_create_qp** | `RsIbvCreateQp()` | `HcommChannelCreate()` | 创建 Channel → `RsDrvQpCreate()` |
| **ibv_reg_mr** | `RsIbvRegMr()` | `HcommMemReg()` | 注册内存 → `RsDrvMrReg()` |
| **ibv_post_send** | `RsIbvPostSend()` | `HcommBatchTransferOnThread()` | 数据传输 → `RsDrvSendExp()` |
| **ibv_post_recv** | `RsIbvPostRecv()` | `HcommReadOnThread()` | 数据接收 → `RsDrvPostRecv()` |

### 8.3 MF 调用 HCOMM 接口的完整流程

| 流程 | MF 接口 | HCOMM 接口 | HCOMM 内部 | 底层 RDMA |
|------|---------|-----------|-----------|-----------|
| **创建 Endpoint** | `HcommApiWrapper::CreateEndpoint()` | `HcommEndpointCreate()` | `RsDrvQueryNotifyAndAllocPd()` | `ibv_alloc_pd()` + `ibv_create_cq()` |
| **注册 MR** | `HcommApiWrapper::RegisterMemory()` | `HcommMemReg()` | `LocalRdmaRmaBuffer()` → `RaRegisterMr()` → `RsRegisterMr()` → `RsDrvMrReg()` | `ibv_reg_mr()` |
| **导出 MR** | `HcommApiWrapper::ExportMemory()` | `HcommMemExport()` | - | - |
| **导入 MR** | `HcommApiWrapper::ImportMemory()` | `HcommMemImport()` | - | - |
| **创建 Channel** | `HcommApiWrapper::CreateChannel()` | `HcommChannelCreate()` | `RsDrvQpCreate()` | `ibv_create_qp()` |
| **分配 Thread** | `HcommApiWrapper::AllocThread()` | `HcommThreadAlloc()` | - | - |
| **数据传输** | `hybm_batch_transfer.cc` | `HcommBatchTransferOnThread()` | `RsDrvSendExp()` | `ibv_post_send()` |
| **数据读取** | `hybm_batch_transfer.cc` | `HcommReadOnThread()` | `RsDrvPostRecv()` | `ibv_post_recv()` |

### 8.4 关键调用链

**注册 MR：**
```
MF: HcommApiWrapper::RegisterMemory()
  → DlHcommApi::HcommMemReg()
    → HCOMM: RoceRegedMemMgr::RegisterMemory()
      → LocalRdmaRmaBuffer() → RaRegisterMr()
        → RsRegisterMr() → RsDrvMrReg()
          → RsIbvRegMr() → ibv_reg_mr()
```

**创建 Channel（含 QP）：**
```
MF: HcommApiWrapper::CreateChannel()
  → DlHcommApi::HcommChannelCreate()
    → HCOMM: HcomChannelCreate()
      → RsDrvQpCreate() → RsIbvCreateQp()
        → ibv_create_qp()
```

**数据传输：**
```
MF: hybm_batch_transfer.cc
  → HcommBatchTransferOnThread()
    → HCOMM: RsDrvSendExp()
      → RsIbvPostSend()
        → ibv_post_send()
```

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `device_rdma_hcomm_init_flow.excalidraw` | 提前准备流程图（create2 + join） |
| `device_rdma_hcomm_overview.excalidraw` | 拷贝流程图（层间调用 + 每层函数） |
