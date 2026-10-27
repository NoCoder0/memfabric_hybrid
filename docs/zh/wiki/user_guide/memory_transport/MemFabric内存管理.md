# MemFabric内存管理

[TOC]

## 全局统一内存地址

MemFabric通过构建逻辑上的全局内存语义统一编址，对分布在不同层级、不同节点的内存单元进行统一管理与使用，使系统能够像管理单一物理资源一样，对跨CPU、NPU的内存资源进行统一寻址和透明访问，核心目的是实现内存资源的整合与统一调度，最大程度的释放硬件性能。
全局统一内存地址(Global Virtual Address, GVA)的特点:

  - 它是一个简单的uint64
  - 所有进程的gva的起始地址一致
  - 所有进程的gva按线性排布且一致

在MemFabric的单个实例中，HBM和DRAM的GVA是两段连续地址，对于每段地址，假设约定的单进程共享的最大内存为maxSize(具体为smem_bm_create_option_t中的maxDramSize和maxHbmSize)，集群进程总数为worldSize，该段地址空间的总大小为maxSize*worldSize，假设该段地址的起始地址为start，则第i个进程提供的内存对应的gva区间为 `start + i * maxSize` 到 `start + (i + 1) * maxSize`

以下图为例，总共有两个进程，Host1上存在进程1，并绑定了host1的rank0；Host2上存在进程2，绑定了host2的rank0，每个进程都提供了50GB的HBM和100GB的DRAM
HBM的GVA起始地址为0x280000000000,那么对所有进程来说，都能看到HBM的池化空间的GVA范围为0x280000000000~0x281900000000
其中，Host1的进程共享的HBM对应的GVA为0x280000000000~0x280c80000000，Host2的进程共享的HBM对应的GVA为0x280c80000000~0x281900000000
DRAM同理，一般起始地址是HBM池化空间的结尾地址，下图中为0x281900000000
![unified_global_address.png](https://raw.gitcode.com/user-images/assets/7672916/50319e89-afd1-4272-a51d-8f40804643cd/unified_global_address.png 'unified_global_address.png')
GVA本身是一个逻辑地址，在底层硬件做实际的数据访问时，需要依赖操作系统中页表上的va。在昇腾系统中，host和device是相互独立的os，有独立的页表，也就有独立的host_va和device_va，host侧的硬件如cpu、host_rdma需要依赖host_va；device侧的硬件如mte、sdma、device_rdma等需要依赖device_va
**host_va和device_va并不一定相同**

## va_manager

### 简介

为了集中化的管理gva，host_va，device_va，设计了va_manager模块，记录了他们三者直接的对应关系，并方便内部各个模块查询
va_manager中对这三种va定义了统一枚举
![image.png](https://raw.gitcode.com/user-images/assets/7672916/22e65db4-8942-483d-9fe6-b937ee5b402e/image.png 'image.png')
并定义了基础数据结构**BaseAllocatedGvaInfo**将三种地址信息一并记录下来
![image.png](https://raw.gitcode.com/user-images/assets/7672916/777c361c-2cce-4e76-8df8-79f947976a8e/image.png 'image.png')
在va_manager中，有两个map数组，**allocatedMap_和reservedMap_**
reservedMap_记录内存申请情况，避免多实例场景下的地址冲突，考虑实际使用场景，当前主要使用HVM_DVA项
allocatedMap_记录**本进程的**所有由MemFabric申请的和用户注册进来的内存；特别的，对于host_va和device_va的allocatedMap_，都只会记录本进程可直接访问的va信息

### 大容量场景支持

一般情况下，为了方便使用，DRAM池的gva和host_va是相等的，HBM池的gva和device_va是相等的
但是，OS的页表支持的地址空间一般为128TB(如ubuntu)或256TB，如果需要构建大容量的内存池，则无法维持gva和host_va/device_va相等，所以我们新增了**二次映射**的选项，支持超大容量的内存池构建(具体为smem_bm_create_option_t的**isSecondMapping**)
在默认情况下，我们将保证gva和host_va/device_va相等，如果开启isSecondMapping，则会为gva分配一个较大的虚拟值，以保证gva在大容量场景下的线性排布
在bm场景下，如果想要通过gva得到本进程内对应的host_va/device_va，可以通过调用**smem_bm_gva_to_va**接口查询

### 内部实现细节

![image.png](https://raw.gitcode.com/user-images/assets/7672916/63e94ba5-94bb-4ca5-bbba-c9002d908a7d/image.png 'image.png')
在hybm中，因为所有的内存都由segment产生，而用户注册的内存也会经由segment处理，所以va_manager的所有修改操作都是由segment发起的，其它模块只会进行查询操作
segment的主要调用顺序为ReserveMemorySpace-->AllocLocalMemory-->Export-->Import-->Mmap
各函数主要功能如下：

- **ReserveMemorySpace**:初始化本进程va地址空间。在函数中会从va_manager申请可用的gva和对应介质的本地device_va/host_va空间(一般dram需要申请host_va，hbm需要申请device_va)，并记录到reservedMap_中，然后调用底层接口对本地的device_va/host_va完成页表上的地址空间占用
- **AllocLocalMemory**:申请本进程的物理地址，并完成映射。该接口会从ReserveMemorySpace申请的device_va/host_va中二次分配一个空闲地址段，申请物理内存并将内存映射到该va上，然后记录到va_manager的allocatedMap_中。在一些场景中，如vmm_base_segment，底层会同时映射host的os页表和device的os页表，这样我们就会在allocatedMap_的三个map中都记录上该地址信息
- **Export**:将本进程的内存导出为字符串，方便共享给其它进程
- **Import**:导入其它进程导出的共享内容，解析并记录
- **Mmap**:把其它进程共享的内存映射到本进程va中，可能涉及host_va和device_va。一般的传输场景下，如RDMA场景，对于其它进程共享的内存，本地没有可以直接访问的va，我们只会记录gva信息到va_manager的allocatedMap_中；如果是SDMA场景，device侧的os会将共享的内存写入本地页表，就会生成本地可访问的device_va，我们就会同时记录gva和device_va到allocatedMap_中
- 另外还有**RegisterMemory**函数：处理用户注册的自己申请的内存。用户注册的内存不会有gva信息；如果用户注册的dram，并且需要device侧访问，则会调用底层接口生成device_va。在该函数中，我们也会将用户注册的内存信息记录到va_manager中

### 典型使用场景

- 我们支持用户在copy接口中smem_bm_copy_type填入SMEMB_COPY_BUTT，这样就需要在内部通过va来判断介质类型，就可以通过va_manager查询具体信息，简单的判断出来，并且能够判断入参地址的有效性
- 一般情况下，用户调用copy接口传入的都是gva，gva并不一定能用于data_operator中进行数据访问，就需要在data_operator内将gva转化成对应的host_va/device_va，具体需要哪种va则视data_operator情况而定
- 在transport中，特别是device_rdma场景下，在注册dram的mr时，一般都是传入的host_va，而device_rdma仅支持device_va，这时候就需要查询va_manger完成转换
