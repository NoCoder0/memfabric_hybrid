# MemFabric Hybrid

MemFabric 是一款开源内存池化软件，面向昇腾超节点、昇腾服务器、鲲鹏超节点、鲲鹏服务器等硬件，
其设计目的与核心思想是：

- **异构设备的统一池化**：将多节点的异构设备内存（DRAM、HBM 等）池化，
  基于 GVA（Global Virtual Address）提供高性能的全局内存直接访问能力。
- **简单的北向接口**：提供内存语义访问接口，即 xcopy with GVA。接口贴近传统 `memcpy` 概念，
  通过 xcopy 完成数据交换，支持 D2RH、RH2D、RH2H、D2D 等多向访问。
- **南向高可扩展**：支持插件化对接多种 DMA 引擎、LD/ST 及网络/灵衢互联，
  包括 Device SDMA、Device RDMA、Device URMA、Device UBoE、Host RDMA、Host TCP、
  Host URMA、Host SHM 等协议或引擎。

![MemFabric Hybrid 架构](source/architecture.png)
