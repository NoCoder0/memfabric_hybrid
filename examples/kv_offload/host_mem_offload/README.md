# host_mem_offload

## 场景

新 host-mem entry（`OFFLOAD_FLAG_HOST_MEM`）整池生命周期实例，覆盖 LOCAL 与 SHARED 两种模式：`initialize` 即把 GB 对齐的整个 `reserve_size` 作为**一个** slice 整池背衬（SHARED 在 init 内一次集合完成整池导出/导入/映射），之后 `malloc`/`free` 全部是池内 16B 对齐切分，不再触碰驱动。用例钉住：**紧凑 bump**（相邻 malloc 增量恰为前一块 16B 对齐尺寸，无 1G/2M 逐块取整）、**注册前 DVA 即可用**且池内线性、**register 幂等**、**free 后洞复用**、**相邻空闲合并复用**、**大请求不跨洞缝合**、**精确容量**（700M+300M 装进 1G 池，随后超容量 malloc 被拒并恢复）；块内容全程写入/读回校验；SHARED 额外全组 allgather 校验对称槽位寻址（任意块 `dva − 自身池内偏移 == pool_base + stride × rank`）。

## 目标

验证整池预背衬模型下 malloc/free/get_device_pointer/register 的池语义——紧凑分配、复用与合并、容量精确性——以及非等大分配与对称 GVA 布局是否工作正常。

## 使用能力

- 实例 API（master 多实例）：`pool = offload.Offload()` 后 `pool.initialize(config: OffloadConfig)` / `pool.uninitialize()`
- 多实例：`--instances N`（每 rank N 个池；同一 `store_url` 下按动态 entityId 前缀 `(<entityId>)_` 天然隔离，SHARED 中 instance i 与各 rank 的 instance i 集合对齐——与旧 entry 多实例需逐实例 +1 端口不同）
- `OffloadConfig`（`flags=OFFLOAD_FLAG_HOST_MEM`、`scene`、`reserve_size`、`world_size`、`rank_id`、`store_url`）
- `pool.malloc(size)` / `pool.free(ptr)`（host-mem entry 语义为池内 16B 对齐切分/归还，相邻空闲自动合并；与旧 entry 同名接口按 init 模式自动分发）
- `pool.host_register(ptr)`（幂等池内检查：池在 initialize 已整体注册；为调用侧逐块序列保留）
- `pool.host_get_device_pointer(ptr)`（任意**池内**地址均有效；vmm 形态 dva==ptr，A5 conn 形态为独立 dva，须始终使用返回值）

## 规模建议

- **内存按整池预背衬**：每实例实际占用 `align_up(reserve_size, 1G)`，多实例按 `instances × 单实例` 同比例放大（`--reserve_mib` 为单实例值）
- `--reserve_mib` 最小 1024（精确容量阶段需要 700M+300M 装满一个 1G 池）
- SHARED 各 rank 基线块非等大（64+2r / 32+2r MB）

## 必要条件

- 单机至少 world_size 张可用 NPU（device id 0..world_size-1）
- 已安装 **`memfabric_hybrid`**、`torch`、`torch_npu`

## 验收标准

- 各 rank `initialize`/`malloc`/`host_register`/`host_get_device_pointer` 返回值全部符合契约
- 紧凑 bump：相邻 malloc 地址增量恰为前一块 16B 对齐尺寸（无逐块 1G/2M 取整）；每次 malloc 返回值 16B 对齐
- 注册前 `host_get_device_pointer` 即成功且池内地址线性（sub_dva == dva + 偏移）；重复 `host_register` 幂等
- free 复用：刚释放的洞被同尺寸请求复用；相邻空闲合并后被从洞起点整体复用；大于洞的请求越过洞分配、绝不跨块缝合
- 精确容量：700M+300M 恰好装满 1G 池；超容量 malloc 返回 0；释放 300M 洞后同位恢复
- vmm 形态下 dva == ptr
- SHARED：全组 allgather 后，任意块 `dva − 池内偏移` 的 rank 间差 == stride × rank（对称槽位）；多实例时**逐实例**独立成立
- 多实例（`--instances > 1`）：跨实例隔离成立——A 池块经 B 池 `host_get_device_pointer` 返回 0、`free` 被拒，且失败的跨池调用不影响 A 池自身继续解析该块
- 子进程全部正常退出（exitcode=0），打印 `host_mem_offload: all ranks OK, scene: <scene>`（多实例时每 rank 每实例各打一行 `instance:i/n ok`）

## 运行（Python）

```bash
cd examples/kv_offload/host_mem_offload
# 默认 reserve 1G（每实例整池背衬 1G）
python3 host_mem_offload.py --scene local
python3 host_mem_offload.py --scene shared --world_size 8 --store_port 8531
# A5 giant_page（conn 形态）
python3 host_mem_offload.py --scene local --giant_page
# 多实例（每 rank 2 个池，同一 store url；内存 = 2 × reserve）
python3 host_mem_offload.py --scene shared --instances 2 --world_size 8 --store_port 8531
# 共享机上指定起始卡号：rank i 跑在 device offset+i（如只用后四卡）
python3 host_mem_offload.py --scene local --device_offset 4
```

成功时打印 `host_mem_offload: all ranks OK, scene: <scene>`，且每 rank 每实例各打印一行
`host_mem_offload rank_id:<r> scene:<s> instance:<i>/<n> ok, blocks:<n>, ptr0:0x..., dva0:0x...`。

## Q&A

- **`--giant_page` 在 A5 上走什么路径？** A5 路由到 conn-based segment（内核大页 + `HalHostRegister`，dva 与 ptr 不同）。整池是**一个** GB 对齐 slice，驱动"巨页句柄拒绝子 GB 映射"的限制不再触发，malloc/free 仍是 16B 池内切分。依赖内核大页配置，失败时按日志提示检查 `/proc/sys/vm/nr_hugepages`。**A3 上该 flag 无任何效果**：默认即 vmm + 1G 大页背衬 + NUMA（内部 OOM 兜底 2M）。
- **SHARED 各 rank 能否申请不同大小？** 可以。`reserve_size` 是全组一致的槽位步长，每 rank 的池内分配互不影响；本例基线块即按 64+2r / 32+2r MB 非等大申请。
- **SHARED 的 register 为什么不再是集合调用？** 整池导出/导入/映射在 `initialize` 内一次集合完成（fingerprint 校验之后）；运行期 `host_register` 只做幂等池内检查，各 rank 调用次数、时机都不再需要对齐。
