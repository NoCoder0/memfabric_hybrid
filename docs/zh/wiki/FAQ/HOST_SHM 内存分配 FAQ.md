
# MemFabric Hybrid 常见问题

---

## HOST_SHM 内存分配 FAQ

本文档面向两类读者：

- **应用开发者**：关注 API 调用、参数设置、错误排查
- **软件部署者**：关注系统配置、内核参数调优、环境准备

> [!NOTE] 环境说明
> 下文命令示例中的数值来自一台 20 GB 物理内存、未预留大页的测试环境，实际部署时请替换为你环境的真实值。

---

### 主题一：HOST_SHM 内存分配限制总览

---

#### Q1: HOST_SHM 能分配多大内存？有哪些限制？

HOST_SHM 的内存上限由多层因素共同决定，从外到内依次是：

| 层级 | 限制因素 | 示例（本机） |
|------|---------|-------------|
| 物理层 | 物理内存总量 `MemTotal` | 20 GB (`/proc/meminfo`) |
| 内核层 | 可用内存 `MemAvailable` | ~15.5 GB |
| 内核层 | `vm.overcommit_memory` / `vm.overcommit_ratio` | 0 / 50 |
| 内核层 | 大页预留（如启用 hugetlbfs） | 未配置（0 个大页） |
| 文件系统层 | `/dev/shm` tmpfs 容量 | 9.8 GB（物理内存的一半） |
| 内核层 | `RLIMIT_MEMLOCK` | 65536 KB (64 MB)，但当前代码路径不受此限制（见 Q15） |
| 代码层 | 分配大小必须 2 MB 对齐 | `HYBM_LARGE_PAGE_SIZE = 2 MB` |
| API 层 | `localDRAMSize` 参数 | 由调用者指定 |

**关键结论**：HOST_SHM 实际能分配的最大内存 = **`min(物理可用内存, 文件系统可用空间, 大页可用数 × 大页大小, MEMLOCK 限制)`**。在未配置大页的默认环境下，主要受限于 `/dev/shm` 的 tmpfs 容量（通常为物理内存的 50%）。

---

#### Q2: HOST_SHM 的两个内存后端是什么？代码如何选择？

HOST_SHM 使用 POSIX 共享内存实现，有两个可选文件系统后端：

| 后端 | 路径 | 文件系统 | 特点 |
|------|------|---------|------|
| 大页后端（优先） | `/dev/hugepages/memfabric_hybrid_<rankId>` | `hugetlbfs` | 使用大页，TLB 效率高，避免页表开销 |
| 普通后端（回退） | `/dev/shm/memfabric_hybrid_<rankId>` | `tmpfs` | 使用 4 KB 标准页，更易配置 |

代码选择逻辑（`src/hybm/csrc/mm/hybm_host_shm_segment.cpp:36-89`）：

1. 检查 `/dev/hugepages` 是否为 hugetlbfs 挂载点（通过 `statfs()` 比对 magic number `0x958458f6`）
2. 读取 `/proc/meminfo` 检查 `HugePages_Free > 0`
3. 两者都满足 → 使用 `/dev/hugepages`；任一不满足 → 回退到 `/dev/shm`

---

#### Q3: 为什么 HOST_SHM 只支持 DRAM 不支持 HBM？能否混用其他协议？

**为什么不支持 HBM**：HBM 是 NPU 设备侧高带宽内存，HOST_SHM 的设计目标是同一节点内多个进程间通过主机内存进行零拷贝共享。HBM 物理上位于 NPU 设备上，无法直接作为主机侧 POSIX 共享内存的后端。

**代码中明确的限制**（`src/smem/csrc/smem_bm/smem_bm.cpp:185-188`）：

```cpp
if (isHostShm && (option->localDRAMSize == 0 || option->localHBMSize != 0)) {
    SM_LOG_AND_SET_LAST_ERROR("HOST_SHM op type only supports DRAM shared memory without HBM");
    return nullptr;
}
```

**能否混用其他协议**：**不可以**。HOST_SHM 是独立的 `dataOpType` 位掩码值，不能与 SDMA、RDMA、TCP、URMA 等协议混合使用（`src/smem/csrc/smem_bm/smem_bm.cpp:189-194`）：

```cpp
constexpr uint32_t hostShmConflictMask = SMEMB_DATA_OP_SDMA | SMEMB_DATA_OP_HOST_RDMA |
    SMEMB_DATA_OP_HOST_URMA | SMEMB_DATA_OP_HOST_TCP | SMEMB_DATA_OP_DEVICE_RDMA;
if (isHostShm && (option->dataOpType & hostShmConflictMask) != 0) {
    SM_LOG_AND_SET_LAST_ERROR("HOST_SHM op type does not support mixing with other data op types");
    return nullptr;
}
```

---

### 主题二：`free -h` 深度解读

---

#### Q4: `free -h` 各列含义是什么？

以下为本机实测输出：

```text
               total        used        free      shared  buff/cache   available
Mem:            19Gi       4.7Gi        14Gi       3.2Mi        966Mi        14Gi
Swap:           10Gi          0B        10Gi
```

| 列名 | 含义 | 本机值 | 说明 |
|------|------|--------|------|
| `total` | 物理内存总量 | 19 Gi | 内核可见的总物理内存 |
| `used` | 已用内存 | 4.7 Gi | `total - free - buff/cache` |
| `free` | 完全未使用内存 | 14 Gi | 未被任何用途占用的内存，**不等于可申请内存** |
| `shared` | tmpfs 共享内存 | 3.2 Mi | 主要来自 `/dev/shm`、`/run/shm` 中的文件 |
| `buff/cache` | 内核缓冲和页缓存 | 966 Mi | 文件系统缓存，需要时可回收 |
| `available` | 可分配内存估值 | 14 Gi | 在不触发 swap 的前提下可用于新进程的内存 |

---

#### Q5: `free` 和 `available` 详细区别？为什么 `available > free`？

`free` 和 `available` 是两个不同层次的概念：

- **`free`**：当前**完全空闲**的物理内存页。这些内存页上没有存储任何数据。

- **`buff/cache`**：被内核用作文件系统缓存的内存。其中的**可回收部分**（`SReclaimable`）在内存紧张时可以释放。

- **`available`** = `free` + 可回收的 `buff/cache` - 内核最小预留。它是内核估算的**实际可用于新分配**的内存量。

本机示例：

```text
MemFree:        14832044 kB  ≈ 14.8 GB   ← 完全空闲
Buffers:           39924 kB
Cached:           764640 kB
SReclaimable:     185560 kB
---
MemAvailable:   15537020 kB  ≈ 15.5 GB   ← 实际可用（更多）
```

`available`（15.5 GB）> `free`（14.8 GB），差值约 700 MB 来自可回收的内核缓存。

> [!TIP] 注意
> `available` 是一个估算值，不代表可以一次性分配这么多。实际分配受 overcommit 策略、地址空间碎片等因素影响。

---

#### Q6: 哪个数字才是 HOST_SHM 能申请的内存大小？

**HOST_SHM 能申请的内存 ≠ `free`，也 ≠ `available`。**

实际能分配的内存大小由以下路径决定：

- **走 `/dev/shm`（默认回退路径）**：上限 = `/dev/shm` 的 tmpfs 实际可用空间。本机 `df -h /dev/shm` 显示 9.8 GB。

- **走 `/dev/hugepages`（大页路径）**：上限 = `HugePages_Free × Hugepagesize`。本机大页未配置，此项为 0。

**判断方法**：

```bash
# 查看 /dev/shm 可用空间
df -h /dev/shm

# 查看大页可用量
grep Huge /proc/meminfo

# 查看当前 MEMLOCK 限制
ulimit -l
```

**结论**：在未配置大页的默认环境中，HOST_SHM 单次最多可申请约 **/dev/shm 可用空间**（本机约 9.8 GB），同时需满足 2 MB 对齐约束。当前代码通过 hugetlbfs 文件系统路径分配大页，不经过 `RLIMIT_MEMLOCK` 限制（详见 Q15）。

---

### 主题三：/dev/shm 专题

---

#### Q7: /dev/shm 是什么？默认多大？

`/dev/shm` 是 Linux 内核提供的一种 **tmpfs 临时文件系统**，也称为 POSIX 共享内存文件系统。它使用物理内存（可能被 swap out）作为存储后端，允许多个进程通过文件路径访问同一块共享内存。

**默认大小**：通常为物理内存的 **50%**。

本机示例：

```bash
$ df -h /dev/shm
Filesystem      Size  Used Avail Use% Mounted on
none            9.8G     0  9.8G   0% /dev/shm
```

物理内存 20 GB → `/dev/shm` 大小 9.8 GB（约 50%）。

**查看方式**：

```bash
df -h /dev/shm
mount | grep shm
```

---

#### Q8: HOST_SHM 如何使用 /dev/shm？

当大页不可用时，HOST_SHM 通过以下步骤使用 `/dev/shm`：

1. **创建文件**：`open("/dev/shm/memfabric_hybrid_<rankId>", O_CREAT | O_EXCL | O_RDWR)`（`hybm_host_shm_segment.cpp:381-383`）
2. **文件锁**：`flock(fd, LOCK_EX | LOCK_NB)` 防止并发冲突（`:386`）
3. **设置大小**：`ftruncate(fd, size)` 分配所需空间（`:427`）
4. **内存映射**：`mmap(addr, size, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_SHARED|MAP_POPULATE, fd, 0)`（`:436-437`）
5. **跨进程映射**：其他进程通过 `open()` + `mmap()` 同路径文件即可访问同一块共享内存
6. **清理**：销毁时调用 `unlink()` 删除文件（`:293`）

`MAP_SHARED` 标志确保所有映射者看到相同的物理页，`MAP_POPULATE` 提前建立页表映射。

---

#### Q9: 如何扩大 /dev/shm？

**方式一：临时扩大**（重启后失效）

```bash
# 将 /dev/shm 扩大为 16 GB
sudo mount -o remount,size=16G /dev/shm
```

**方式二：永久扩大**（编辑 `/etc/fstab`）

```bash
# 在 /etc/fstab 中添加或修改：
none /dev/shm tmpfs defaults,size=16G 0 0
```

然后重新挂载：

```bash
sudo mount -o remount /dev/shm
```

**方式三：使用大页绕过 /dev/shm 限制**

配置大页后将自动使用 `/dev/hugepages` 而非 `/dev/shm`，完全绕开 tmpfs 的 50% 物理内存限制。详见主题四。

**建议**：将 `/dev/shm` 设置为物理内存的 80%~100% 以最大化 HOST_SHM 可用空间。

---

#### Q10: /dev/shm 满了会怎样？如何排查？

**现象**：

- `smem_bm_create()` 或 `smem_bm_create2()` 返回 `nullptr`
- 日志中出现 `"Failed to truncate local shm file"` 错误（`hybm_host_shm_segment.cpp:428-429`）
- `errno = ENOSPC`（No space left on device）

**排查步骤**：

```bash
# 1. 检查 /dev/shm 使用量
df -h /dev/shm

# 2. 查看哪些文件占用空间
ls -lh /dev/shm/

# 3. 查找 memfabric 残留文件
ls -lh /dev/shm/memfabric_hybrid_*

# 4. 清理残留文件（确保对应进程已停止）
sudo rm /dev/shm/memfabric_hybrid_*
```

**预防**：

- 确保程序正常退出时调用 `smem_bm_destroy()`，它会触发 `unlink()` 删除共享内存文件
- 如遇进程异常崩溃，手动清理 `/dev/shm/memfabric_hybrid_*` 残留文件

---

### 主题四：大页（Huge Pages）专题

---

#### Q11: 大页是什么？为什么 HOST_SHM 要用大页？

**大页**是 Linux 内核提供的一种大于标准 4 KB 的内存页。常见大小：

| 类型 | 大小 | 说明 |
|------|------|------|
| 标准页 | 4 KB | 默认页大小 |
| 大页（Huge Page） | 2 MB | 本机 `Hugepagesize: 2048 kB` |
| 巨页（Giant Page） | 1 GB | 需 CPU 支持 |

**为什么 HOST_SHM 要用大页**：

1. **TLB 命中率更高**：2 MB 大页覆盖的地址空间是 4 KB 页的 512 倍，大幅减少 TLB miss
2. **页表层级更少**：x86-64 上 4 KB 页需要 4 级页表，2 MB 页仅需 3 级
3. **减少缺页中断**：`mmap` 时使用 `MAP_POPULATE` 预填充，大页减少缺页次数
4. **不会被 swap 换出**：大页内存始终驻留在物理内存中，保证访问延迟稳定
5. **代码强制 2 MB 对齐**：`HYBM_LARGE_PAGE_SIZE = 2 MB`，所有 HOST_SHM 分配必须对齐到此值（`hybm_define.h:32`）

---

#### Q12: 如何检查大页配置？

```bash
# 方式一：查看所有大页相关信息
grep Huge /proc/meminfo
```

本机输出（未配置大页）：

```text
AnonHugePages:         0 kB
ShmemHugePages:        0 kB
FileHugePages:         0 kB
HugePages_Total:       0        ← 大页总数
HugePages_Free:        0        ← 可用大页数
HugePages_Rsvd:        0        ← 已预留大页数
HugePages_Surp:        0        ← 超额大页数
Hugepagesize:       2048 kB     ← 每个大页 2 MB
Hugetlb:               0 kB
```

| 字段 | 含义 |
|------|------|
| `HugePages_Total` | 系统预留的大页总数 |
| `HugePages_Free` | 当前未使用的大页数 |
| `HugePages_Rsvd` | 已通过 mmap 预留但尚未实际分配的大页数 |
| `Hugepagesize` | 每个大页的大小 |

```bash
# 方式二：查看 hugetlbfs 挂载状态
mount | grep huge
```

正常输出应包含 `hugetlbfs on /dev/hugepages`。

---

#### Q13: 如何启用和配置大页？

**步骤 1：计算所需大页数量**

```text
所需大页数 = ceil(需要的内存大小 / 大页大小)
```

例如，要为 HOST_SHM 预留 8 GB 内存（使用 2 MB 大页）：

```text
8 GB / 2 MB = 8192 MB / 2 MB = 4096 个大页
```

**步骤 2：配置大页**

```bash
# 临时配置（重启后失效）
echo 4096 > /proc/sys/vm/nr_hugepages

# 永久配置（编辑 /etc/sysctl.conf 或 /etc/sysctl.d/99-hugepages.conf）
echo "vm.nr_hugepages = 4096" | sudo tee /etc/sysctl.d/99-hugepages.conf
sudo sysctl -p /etc/sysctl.d/99-hugepages.conf
```

**步骤 3：挂载 hugetlbfs**

```bash
# 确保 /dev/hugepages 已挂载
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs none /dev/hugepages
```

永久挂载（添加至 `/etc/fstab`）：

```bash
none /dev/hugepages hugetlbfs defaults 0 0
```

**步骤 4：验证配置**

```bash
grep Huge /proc/meminfo
# 应显示 HugePages_Total 和 HugePages_Free 均为 4096
```

---

#### Q14: 大页分配失败怎么办？

**常见失败场景及解决方案**：

| 场景 | 日志/现象 | 解决方案 |
|------|---------|---------|
| 大页配置不足 | `HugePages_Free is 0, will fallback to /dev/shm` | 增大 `vm.nr_hugepages` |
| hugetlbfs 未挂载 | `Failed to access hugepage dir /dev/hugepages` | `mount -t hugetlbfs none /dev/hugepages` |
| hugetlbfs 挂载点类型不对 | `Hugepage dir /dev/hugepages is not hugetlbfs` | 确认挂载类型为 `hugetlbfs` |
| 内存碎片无法分配大页 | `echo 4096 > /proc/sys/vm/nr_hugepages` 后 `HugePages_Total < 4096` | 在系统启动早期配置大页（内核启动参数 `hugepages=4096`） |
| MEMLOCK 限制不足 | `mmap` 返回 `EPERM` 或 `ENOMEM` | 增大 `RLIMIT_MEMLOCK`（见 Q15） |
| 大页文件创建失败 | `Failed to open local shm file /dev/hugepages/...` | 检查 `/dev/hugepages` 权限（应为 `drwxr-xr-x`） |

**代码回退行为**：大页不可用时，HOST_SHM **自动回退**到 `/dev/shm`（`hybm_host_shm_segment.cpp:377`），不会导致创建失败，但性能会下降。

---

#### Q15: RLIMIT_MEMLOCK 对大页的限制？

`RLIMIT_MEMLOCK` 限制一个进程可以通过 `mlock()`/`mlockall()` 锁定的最大内存量，也限制 `mmap(MAP_HUGETLB)` 的分配量。

本机限制：

```bash
$ ulimit -l
65536    # 64 MB
```

> [!TIP] 注意
> HOST_SHM 代码中**没有直接调用 `mlock()` 或使用 `MAP_HUGETLB` 标志**，而是通过 **hugetlbfs 文件系统上的 `open()` + `mmap()`** 来分配大页内存。这种方式的 `mmap` 使用 `MAP_SHARED`（而非 `MAP_HUGETLB`），理论上不经由 `RLIMIT_MEMLOCK` 限制。

但如果未来版本添加了 `MAP_HUGETLB` 路径或 `mlock()` 调用，则需注意此限制。当前环境 `RLIMIT_MEMLOCK = 64 MB` 对 HOST_SHM 的 hugetlbfs 文件系统路径**不构成限制**。

**调试用**：

```bash
# 查看当前限制
ulimit -l

# 临时提升（软限制不能超过硬限制）
ulimit -l unlimited

# 永久配置（/etc/security/limits.conf）
* soft memlock unlimited
* hard memlock unlimited
```

---

### 主题五：突破内存分配限制的配置指南

---

#### Q16: 完整配置清单：如何最大化 HOST_SHM 可用内存？

按优先级从高到低：

```bash
# ============ 1. 大页配置（推荐，绕开 /dev/shm 限制） ============
# 计算：所需大页数 = 目标内存 / 2MB
# 例如为 HOST_SHM 预留 10 GB：
#   10 * 1024 MB / 2 MB = 5120 个大页
echo 5120 > /proc/sys/vm/nr_hugepages

# 验证
grep Huge /proc/meminfo
# HugePages_Total: 5120, HugePages_Free: 5120 → 可用 10 GB

# ============ 2. 扩大 /dev/shm（如果不想用大页） ============
# 将 /dev/shm 设置为物理内存的 80%~100%
sudo mount -o remount,size=16G /dev/shm

# ============ 3. 调整 overcommit 策略 ============
# （详见 Q17，通常保持默认值 0 即可）
echo 1 > /proc/sys/vm/overcommit_memory   # 允许 overcommit

# ============ 4. 提升 RLIMIT_MEMLOCK（备用） ============
ulimit -l unlimited

# ============ 5. 确保大页在启动早期配置（防止碎片） ============
# 编辑 /etc/default/grub:
#   GRUB_CMDLINE_LINUX="hugepages=5120"
# 然后:
sudo update-grub && sudo reboot
```

**推荐方案顺序**：

1. **首选**：配置大页（绕过 `/dev/shm` 大小限制，性能最佳）
2. **备选**：扩大 `/dev/shm`（简单但受物理内存 50% 默认限制）
3. **不推荐**：不配置大页也不扩大 `/dev/shm`（本机仅 9.8 GB 可用）

---

#### Q17: `vm.overcommit_memory` 对 HOST_SHM 的影响？

`vm.overcommit_memory` 控制内核对内存申请的 overcommit 策略：

| 值 | 含义 | 对 HOST_SHM 的影响 |
|----|------|-------------------|
| `0` | 启发式 overcommit（默认） | 允许合理的 overcommit，HOST_SHM 申请大于物理内存时可能被拒绝 |
| `1` | 始终允许 overcommit | HOST_SHM 可以申请超过物理内存，但可能导致 OOM killer |
| `2` | 禁止 overcommit | 最严格，可分配内存上限 = `Swap + RAM * (overcommit_ratio / 100)` |

本机配置：

```bash
vm.overcommit_memory = 0
vm.overcommit_ratio = 50
```

- **默认值 `0`** 对大多数场景已足够，不需要修改
- **`overcommit_ratio = 50`** 仅在 `overcommit_memory = 2` 时生效，表示允许分配物理内存的 50% + swap
- 如果遇到 `ENOMEM` 且确认物理内存充足，可尝试设为 `1`

> [!TIP] 注意
> HOST_SHM 是通过文件系统（`/dev/shm` 或 `/dev/hugepages`）分配内存的，文件系统层面的空间限制（tmpfs 大小、大页数量）比 overcommit 策略更重要。大多数分配失败的原因是文件系统空间不足而非 overcommit。

---

### 主题六：SMEM API 参数设置

---

#### Q18: 调 HOST_SHM 需要设置哪些 SMEM 参数？

**核心 API**：

- `smem_bm_create(id, memberSize, dataOpType, localDRAMSize, localHBMSize, flags)`
- `smem_bm_create2(id, &option)` — 推荐，支持完整配置

**HOST_SHM 必需参数**（`smem_bm_create2` 通过 `smem_bm_create_option_t` 结构体）：

| 参数 | 类型 | 必需值/范围 | 说明 |
|------|------|-----------|------|
| `dataOpType` | `smem_bm_data_op_type` | `SMEMB_DATA_OP_HOST_SHM` (值 `32`) | 必须单独使用，不可与其他类型混合 |
| `localDRAMSize` | `uint64_t` | `> 0` 且为 **2 MB 的整数倍** | 本地 DRAM 贡献给 Big Memory 的内存大小 |
| `localHBMSize` | `uint64_t` | **必须为 `0`** | HOST_SHM 不支持 HBM |
| `maxDramSize` | `uint64_t` | `>= localDRAMSize` | 全局 DRAM 池化内存上限 |
| `maxHbmSize` | `uint64_t` | **必须为 `0`** | HOST_SHM 不支持 HBM |
| `flags` | `uint32_t` | `0` | 可选标志（通常为 0） |
| `enable56BitsGva` | `bool` | 总地址空间 > 32 TB 时设为 `true` | 大多数场景不需要 |

**最小调用示例**（C 语言）：

```c
#include "smem_bm.h"

// 初始化
smem_bm_init("tcp://127.0.0.1:8573", 2, 0, NULL);

// 创建 HOST_SHM Big Memory 对象（每个 rank 分配 256 MB）
smem_bm_create_option_t opt;
smem_bm_config_init(&opt);
opt.dataOpType = SMEMB_DATA_OP_HOST_SHM;
opt.localDRAMSize = 256ULL * 1024 * 1024;  // 256 MB，必须 2 MB 对齐
opt.localHBMSize = 0;                       // 必须为 0
opt.maxDramSize = opt.localDRAMSize;        // 最大 DRAM 空间
opt.maxHbmSize = 0;

smem_bm_t bm = smem_bm_create2(0, &opt);
if (bm == NULL) {
    printf("创建失败\n");
    return -1;
}

// 加入全局空间
smem_bm_join(bm, 0);

// ... 使用 smem_bm_copy() 等进行数据传输 ...

// 退出并销毁
smem_bm_leave(bm, 0);
smem_bm_destroy(bm);
smem_bm_uninit(0);
```

**Python 示例**：

```python
import memfabric_hybrid as mf

# 初始化
mf.init("tcp://127.0.0.1:8573", 2, 0)

# 创建 HOST_SHM Big Memory 对象
bm = mf.BigMemory(
    id=1,
    memberSize=2,
    dataOpType=mf.SMEMB_DATA_OP_HOST_SHM,
    localDRAMSize=256 * 1024 * 1024,  # 256 MB
    localHBMSize=0,
    flags=0
)

# 加入全局空间
bm.join()

# ... 使用 bm.copy() 等进行数据传输 ...

# 退出并销毁
bm.leave()
bm.destroy()
mf.uninit()
```

---

#### Q19: 分配失败返回什么错误？如何诊断？

**返回值**：

| 返回值/现象 | 错误码 | 含义 |
|-----------|--------|------|
| `nullptr` | — | `smem_bm_create` / `smem_bm_create2` 分配失败 |
| `-2000` | `SMEM_INVALID_PARAM` | 参数无效（大小不对齐、类型冲突） |
| `-2001` | `SMEM_MALLOC_FAILED` | 系统内存不足 |
| `-2002` | `SMEM_NO_RESOURCES` | 资源不足（如 `/dev/shm` 空间不足） |
| `-2008` | `SMEM_NOT_INIT` | 未调用 `smem_bm_init` |
| `-1` | `SMEM_ERROR` | 通用错误 |

**代码层检查逻辑**（`smem_bm.cpp:185-194`）：

1. `localDRAMSize == 0` 或 `localHBMSize != 0` → `"HOST_SHM op type only supports DRAM shared memory without HBM"`
2. 混合其他 `dataOpType` → `"HOST_SHM op type does not support mixing with other data op types"`

**日志诊断**：

```bash
# 查看 memfabric 日志（默认路径取决于安装配置）
cat /var/log/memfabric_hybrid/*.log | grep -i "host_shm\|hugepage\|dev/shm\|error"

# 关键日志模式：
# "Failed to access hugepage dir"     → hugetlbfs 未挂载
# "HugePages_Free is 0"              → 大页不足
# "Failed to truncate local shm"      → /dev/shm 空间不足
# "Failed to mmap local shm"          → 内存映射失败（检查 MEMLOCK、地址空间）
```

**快速排查流程**：

```text
smem_bm_create2 返回 nullptr
    ↓
1. 检查 localDRAMSize > 0 && localHBMSize == 0？
   否 → 设置 localDRAMSize > 0，localHBMSize = 0
   是 ↓
2. 检查 dataOpType 是否混合了其他类型？
   是 → 仅使用 SMEMB_DATA_OP_HOST_SHM
   否 ↓
3. 检查 /dev/shm 空间：df -h /dev/shm
   不足 → 清理残留文件或扩大 /dev/shm
   充足 ↓
4. 检查大页配置：grep Huge /proc/meminfo
   需大页但未配置 → 配置大页或依赖 /dev/shm 回退
   充足 ↓
5. 检查日志：grep -i error /var/log/memfabric_hybrid/*.log
```

---

### 主题七：常见问题排查速查表

---

#### Q20: 问题速查表

| 现象 | 可能原因 | 排查命令 | 解决方案 |
|------|---------|---------|---------|
| `smem_bm_create` 返回 `nullptr` | 参数不符合 HOST_SHM 限制 | 检查 `localDRAMSize > 0`、`localHBMSize == 0`、`dataOpType` 不混合 | 修正参数（见 Q18） |
| 日志：`"only supports DRAM"` | 设置了 `localHBMSize != 0` | — | 将 `localHBMSize` 设为 0 |
| 日志：`"does not support mixing"` | `dataOpType` 混合了多种协议 | — | 仅使用 `SMEMB_DATA_OP_HOST_SHM` |
| 日志：`"Failed to truncate"` | `/dev/shm` 空间不足 | `df -h /dev/shm` | 清理残留文件或扩大 tmpfs（Q9） |
| 日志：`"Failed to mmap"` + errno 12 (ENOMEM) | 物理内存不足或 overcommit 拒绝 | `free -h`、`cat /proc/sys/vm/overcommit_memory` | 释放内存或调整 overcommit（Q17） |
| 日志：`"HugePages_Free is 0"` | 大页未配置或已耗尽 | `grep Huge /proc/meminfo` | 配置大页（Q13） |
| 日志：`"not hugetlbfs"` | `/dev/hugepages` 挂载点类型错误 | `mount \| grep huge` | 重新挂载为 hugetlbfs（Q13） |
| 大页配置后仍然回退到 `/dev/shm` | hugetlbfs 未挂载或大页未真正可用 | `grep Huge /proc/meminfo`、`mount \| grep huge` | 确认 HugePages_Free > 0 且 /dev/hugepages 为 hugetlbfs（Q13） |
| HugePages_Total 小于配置值 | 内存碎片导致无法分配连续大页 | `cat /proc/buddyinfo` | 在启动时配置大页（内核参数 `hugepages=N`） |
| 残留的 `/dev/shm/memfabric_hybrid_*` | 进程异常退出未调用 destroy | `ls -lh /dev/shm/` | 手动 `rm` 清理（Q10） |
| `smem_bm_join` 失败 | 对端进程未创建或 `/dev/shm` 文件不存在 | 检查 `imported shm file not ready` 日志 | 确保所有 rank 都已调用 create（Q18） |
| 性能不达预期 | 使用了 `/dev/shm` 而非大页 | 查看日志确认 `useHugetlbfs=false` | 配置大页提升性能（Q11-Q14） |
| 分配大小被拒绝 | 大小不是 2 MB 对齐 | 代码：`size % HYBM_LARGE_PAGE_SIZE != 0` | 将大小对齐到 2 MB 的整数倍 |

---

**相关代码文件索引**：

| 文件 | 路径 | 关键内容 |
|------|------|---------|
| SMEM 公共 API | `src/smem/include/host/smem_bm.h` | `smem_bm_create`、`smem_bm_create2`、`smem_bm_destroy` |
| SMEM 数据操作类型 | `src/smem/include/host/smem_bm_def.h` | `SMEMB_DATA_OP_HOST_SHM` 枚举定义 |
| SMEM 错误码 | `src/smem/include/host/smem.h` | `SMEM_OK`、`SMEM_INVALID_PARAM` 等 |
| HOST_SHM 段实现 | `src/hybm/csrc/mm/hybm_host_shm_segment.cpp` | 大页检测、`/dev/shm` 回退、`mmap` 分配逻辑 |
| HYBM 大页常量 | `src/hybm/csrc/common/hybm_define.h` | `HYBM_LARGE_PAGE_SIZE = 2MB` |
| SMEM 创建校验 | `src/smem/csrc/smem_bm/smem_bm.cpp` | HOST_SHM 参数校验（DRAM only、不可混合） |
| 安装指南 | `doc/installation.md` | 大页配置快速指引（第 75-84 行） |
