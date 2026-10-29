# Engram权重卸载Host内存

## 背景介绍

- **模型**：DeepSeek v4.1 Flash模型创新性的引入了engram权重，将静态模式存储与动态计算解耦，让模型在不增加token计算量的前提下扩充知识容量。
- **方案**：通过在模型特定层挂载大规模静态嵌入表，用哈希查表的方式取回向量，访问频率低，计算量极小，读取数据量小。
- **代价**：显存占用量极为庞大，DeepSeek v4.1 Flash模型仅2个engram层使用FP8精度部署，占用显存约200GB，整个模型部署也才600+GB。

## memfabric_hybrid的实现方案

![](./mf-engram-deploy.PNG)

- **方案简介**：根据engram权重占比大、访问量小的特点，将权重卸载到host内存，并映射到全局虚拟地址（GVA），支持算子在device侧读取，访问host内存同时不影响入图。
- **性能优化**：卸载的host内存使用1G大页申请，并绑定numa，AIV算子double-buffer拷贝，提升离散随机访问带宽。
- **内存映射关系**：如图所示，原始两个engram权重部署在HBM，新方案切换为部署在host内存上（初始化时拷贝到memfabric_hybrid申请的内存即可），并且host内存映射到连续GVA空间，相当于这个连续的GVA地址空间保存了完整engram权重，且device侧可读。
- **部署形态支持**：当前支持A3多机部署，A5单机部署。
- **Engram读取**：通过AIV小算子（memfabric_hybrid提供engtry_gather，也可以自定义开发）根据engram entry id线性寻址GVA空间即可读取。

## 应用场景1：单实例共享

- 单实例共享是典型的应用场景，例如P/D混部部署共享、单个P实例共享、单个D实例共享，即上一节图里的部署方式，初始化时随每rank主进程调用memfabric_hybrid接口，将engram权重拷贝到host内存即可，接口定义详情可参考[offload shared API](https://gitcode.com/Ascend/memfabric_hybrid/blob/master/src/acc_offload/csrc/python_wrapper/pymf_acc_offload.cpp)。
- 单实例共享支持A3跨机部署。A5跨机部署即将支持。

## 应用场景2：多实例共享

![](./multi-instance-unbalance-share-engram.PNG)

- **跨实例组建GVA**：在P/D分离部署场景，P和D实例可以共享一份host侧的engram权重，即多个实例的进程共同映射到同一个GVA地址空间即可。
- **方案1：平均贡献**：全局实例的所有进程平均共享一份engram权重的slice，和**单实例共享**的方式映射没有本质区别，唯一区别在于参与贡献的进程更多了，每个进程平均贡献的slice更小，但汇总后还是一份完整的权重。此时由于跨了实例，初始化时指定**world_size**需要改成全局总rank数。
- **方案2：非平均贡献**：部分实例贡献engram权限，全局实例的进程共享读。通常P/D分离部署时P实例多于D实例，且P实例根据业务压力动态扩缩容，此时P实例不再适合参与贡献engram slice，此场景可以让D实例贡献一份engram权重，共享给所有P+D实例读，即上图3P1D部署示例，D节点贡献一份完整engram权重映射到GVA，全局4个实例共享。
    - 原理：memfabric_hybrid在组建DRAM池时，支持每个进程贡献不一样大的内存，即初始化让D节点进程申请DRAM，让P节点申请0，经memfabric_hybrid内部初始化后可全局共享可读。
