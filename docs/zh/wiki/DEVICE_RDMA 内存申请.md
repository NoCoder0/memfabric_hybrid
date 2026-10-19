# DEVICE_RDMA 内存申请

## 概述

使用device_rdma传输协议时，memcache按顺序分别尝试以下三种DRAM内存资源申请，顺利完成其中之一的申请即为成功，三种都无法申请则初始化失败：

1. 静态大页（通常为2M）
2. 透明大页（通常为2M）
3. 系统默认内存页（通常为4K）

## 静态大页

### 性能

最高

### 使用方法

> [!TIP] 注意
> 需要能操作宿主机且服务运行环境可以访问/dev/hugepages，MA、k8s或一些云化场景可能无法使用。

先在宿主机上通过如下命令查询大页配置，获取Hugepagesize。示例如下：

```shell
[root@node-106 memcache]# grep -i huge /proc/meminfo
AnonHugePages:   8689664 kB
ShmemHugePages:        0 kB
FileHugePages:         0 kB
HugePages_Total:   200000
HugePages_Free:    200000
HugePages_Rsvd:        0
HugePages_Surp:        0
Hugepagesize:       2048 kB
Hugetlb:        409600000 kB
```

这里 Hugepagesize：2048 kB 表示系统大页大小为2M。

然后计算需要配置的大页数量，方法如下：

```text
每台环境所需大页数量 = 单卡内存量 * 服务使用卡数 / 大页大小
```

例如，每卡配置100GB DRAM内存，使用8卡拉起服务，大页大小为2MB，则总共需要大页数量为 `100GB*8/2MB=409600`。

最后通过如下命令配置大页数量（假设配置409600个大页）

```shell
echo 409600 > /proc/sys/vm/nr_hugepages
```

如果是非root用户可使用如下命令

```shell
echo 409600 | sudo tee /proc/sys/vm/nr_hugepages
```

### 日志

申请成功时打印INFO级日志

```text
Successfully allocated <size> bytes DRAM hugepage via mmap ...
```

申请失败时打印WARN级日志

```text
Unable to alloc size <size> with hugepage via mmap ...
```

## 透明大页

### 性能

较高

### 使用方法

在大页内存无法申请时采用，需要在local配置文件中增加如下配置。（实际条件为启用二次映射）

```ini
ock.mmc.local_service.max.dram.size = 1024GB
```

### 日志

申请成功时打印INFO级日志

```text
Successfully allocated DRAM hugepage via halMemAlloc ...
```

申请失败时打印WARN级日志

```text
halMemAlloc not successful, ret: ...
```

## 系统默认内存页

### 性能

一般

### 概述

作为兜底手段，仅在上述两种方法都无法成功的情况下支持基本功能，且每卡申请容量无法超过30G，否则会导致出现[报错](https://gitcode.com/Ascend/memfabric_hybrid/wiki/FAQ.md#8-使用4k大小内存页时注册失败)。

### 日志

申请成功时打印INFO级日志

```text
Successfully allocated <size> bytes DRAM 4K page via mmap ...
```

申请失败时打印ERROR级日志

```text
Failed to alloc size: <size> ...
```

## 总结

三种内存申请方式对比如下：

| 申请方式             | 性能 | 用法                                                                                           | 限制                                                                              |
|----------------------|------|------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------|
| 静态大页（2M）       | 最高 | 宿主机配置大页数量（如 `echo 409600 > /proc/sys/vm/nr_hugepages`）                             | 需要能操作宿主机且服务运行环境可访问/dev/hugepages，MA、k8s或云化场景可能无法使用 |
| 透明大页（2M）       | 较高 | 在local配置文件中增加 `ock.mmc.local_service.max.dram.size = 1024GB`（实际条件为启用二次映射） | 需在静态大页无法申请时采用，依赖二次映射配置生效                                  |
| 系统默认内存页（4K） | 一般 | 无需额外配置，前两种方式均失败后自动采用                                                       | 仅支持基本功能，每卡申请容量无法超过30G，否则会导致注册失败                       |

内存申请遇到问题时，可在全量日志中grep alloc，确定使用了哪种内存申请方式。
