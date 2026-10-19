# <span style="color:#008a00;">MemFabric Roadmap 2026 H1</span>

MemFabric在2026年H1阶段的目标是增强在多硬件下的兼容能力、增强接口在多场景下的适用性、持续提升集群可靠性和易用性，具体的研发Roadmap如下：

---

## 关键特性

### <span style="color:#0066cc;">硬件能力</span>

- [ ] [**830**] A3 1T/2T硬件支持内存池化
- [x] [**730**] MemFabric支持A5硬件
- [x] [**330**] MemFabric支持KunPeng 950硬件，基于host ub池化

### <span style="color:#0066cc;">异构能力</span>

- [ ] [**630**] 支持A3跨超节点池化，提供百TB级容量内存池
- [ ] [**630**] 支持A3/A2 + GPU异构集群，基于host rdma池化
- [x] [**330**] MemFabric支持A2+A3异构集群，基于device rdma池化

### <span style="color:#0066cc;">集群可靠性</span>

- [ ] [**830**] 集群加入方案2.0，提升可靠性，稳定性
- [ ] [**330**] 支持中心化配置分发模块ConfigStore Server内聚时支持etcd高可用
- [x] [**2025H4**] 支持中心化配置分发模块ConfigStore Server由系统中其他的中心化进程拉起

---
