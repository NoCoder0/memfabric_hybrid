# NUMA 亲和配置

MemFabric 支持在创建 BM 时通过 `flags` 控制本地 DRAM 的 NUMA 分配策略，用于缩短 NPU 与 Host DRAM 之间的数据访问路径。

NUMA 策略在 `smem_bm_create()` 或 `smem_bm_create2()` 创建 BM 时生效，BM 创建后不能动态切换。该策略不改变 HBM 的分配位置，也不会迁移通过注册接口传入的外部内存。

## 配置方式

`flags` 中 bit 7 为 NUMA 性能模式开关，bit 0～6 为 NUMA 策略值：

| 模式 | 配置 | 含义 |
|---|---|---|
| OFF | bit 7 为 `0`，例如 `flags = 0` | 不启用显式 NUMA 亲和，使用对应平台的默认分配策略 |
| AUTO | bit 7 为 `1`，bit 0～6 为 `127` | 自动选择亲和位置，推荐作为常规性能配置 |
| MANUAL | bit 7 为 `1`，bit 0～6 为 NUMA node ID | A2/A3 指定目标 node；A5 以该 node 解析目标 CPU socket |

可使用公共头文件中的宏构造配置：

```c
/* OFF */
uint32_t flags = 0;

/* AUTO：0x80 | 0x7f，最终值为 0xff */
uint32_t flags = (1U << SMEM_BM_PERFORMANCE_MODE_FLAG_INDEX) |
                 SMEM_BM_BIND_NUMA_AUTO_AFFINITY_FLAG;

/* MANUAL：示例指定 Linux NUMA node 2 */
uint32_t numa_id = 2;
uint32_t flags = (1U << SMEM_BM_PERFORMANCE_MODE_FLAG_INDEX) |
                 (numa_id << SMEM_BM_BIND_NUMA_FLAG_INDEX);
```

MANUAL 模式可用的 NUMA node ID 范围为 `0～126`；`127` 保留给 AUTO 模式。使用前应通过
`numactl --hardware` 或 `/sys/devices/system/node/` 确认 node ID 存在。

如果还需要设置其他 BM 标志，应使用按位或保留它们：

```c
option.flags |= (1U << SMEM_BM_PERFORMANCE_MODE_FLAG_INDEX) |
                SMEM_BM_BIND_NUMA_AUTO_AFFINITY_FLAG;
```

## A2/A3 与 A5 的差异

| 项目 | A2/A3 | A5 |
|---|---|---|
| 主要生效路径 | VMM DRAM 分配路径 | connection-based DRAM 分配路径，典型场景为 `DEVICE_URMA` |
| OFF | HAL 以 `devid = -1` 使用默认交织分配 | 不修改线程 CPU 亲和性，按系统默认策略分配和触页 |
| AUTO | 由 HAL 按当前调用线程所在 NUMA 分配 | 根据当前逻辑 NPU device ID，通过 DCMI 动态查询亲和 CPU，再选择对应 CPU socket |
| MANUAL | 将 NUMA node ID 直接传给 HAL | 从 Linux sysfs 读取指定 NUMA node 的 CPU，并选择其所属 CPU socket |
| 实现机制 | 通过 `HalMemCreate()` 的内存属性选择 NUMA | 分配和触页期间临时设置线程 CPU affinity，利用 first-touch 完成亲和分配，随后恢复原 affinity |

A5 不使用硬编码的 device-to-NUMA 映射。AUTO 模式依赖`dcmiv2_get_affinity_cpu_info_by_dev_id()` 返回当前设备的亲和 CPU 信息；MANUAL 模式依赖`/sys/devices/system/node/node<N>/cpulist`。解析出的 CPU socket 还会与进程当前允许使用的 CPU 集合取交集，因此能够遵守容器 cpuset、cgroup 和 `taskset` 限制。

## 使用示例

以下示例使用 AUTO 模式创建 BM：

```c
smem_bm_create_option_t option = {0};
option.maxDramSize = 8ULL * 1024 * 1024 * 1024;
option.localDRAMSize = 8ULL * 1024 * 1024 * 1024;
option.maxHbmSize = 0;
option.localHBMSize = 0;
option.dataOpType = SMEMB_DATA_OP_DEVICE_URMA;
option.enable56BitsGva = true;
option.flags = (1U << SMEM_BM_PERFORMANCE_MODE_FLAG_INDEX) |
               SMEM_BM_BIND_NUMA_AUTO_AFFINITY_FLAG;
option.dramShmFd = -1;

smem_bm_t handle = smem_bm_create2(0, &option);
if (handle == NULL) {
    /* 获取并处理 MemFabric 错误信息。 */
}
```
