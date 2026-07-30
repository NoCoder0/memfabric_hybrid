# group_pack_copy_offload

## 场景

单机多卡 MoE 专家分发实例：每 rank 在本地 DRAM 上创建 1GB offload 池，准备 N 个专家槽位的输入/输出张量（inputs/outputs/lens/groupList 均长度 N）；通过 `group_list` 标记活跃专家（非 0）后，调用 `group_pack_copy` 把活跃专家的 inputs **紧凑**拷贝到 outputs 前 M 个槽位（outputs[0..M)），并把 `group_list` 的非 0 元素按原序紧凑写入 `packed_group_list` 前 M 个，完成一次写入与读回校验。

## 目标

验证 ACC OFFLOAD 的 `group_pack_copy` 接口在多卡并发下：

- `group_list` 非 0 位置跳过 0，第 j 个非 0（原下标 i）紧凑写入 outputs[j] 与 packed_group_list[j]
- outputs 前 M 个有数据（host 零值覆盖 device ones），outputs[M..N) 未被触动
- `packed_group_list` 前 M 个为 `group_list` 非 0 元素且原序不变，第 M 个之后保持 0

## 使用能力

- `mf.set_log_level(level)`
- `offload.initialize(config: OffloadConfig)` / `offload.uninitialize()`
- `OffloadConfig`（`device_id`、`reserve_size`、`alloc_size`；LOCAL 场景下 `reserve_size` 与 `alloc_size` 须相等）
- `offload.empty(sizes, dtype, pin_memory)`（在 host DRAM 池分配专家输入 tensor）
- `offload.group_pack_copy(srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtr, groupList, packedGroupList, deviceId)`

## 接口语义

inputs / outputs / lens / groupList 都是长度 N 的数组（N = *numLocalExpertPtr，公共长度）：

- `srcPtrs`：长度 N 的 int64 地址数组，指向 host 池中的专家输入
- `dstPtrs`：长度 N 的 int64 地址数组，指向 NPU 侧专家输出；前 M 个被紧凑写入，其余不动
- `lenPtrs`：长度 N 的 int32 **字节计数**（kernel 实例化为 uint8_t 字节拷贝，调用方传 `tensor.numel() * tensor.element_size()`）
- `numLocalExpertPtr`：int32 标量，值为 N（公共数组长度）
- `groupList`：长度 N 的 int64 数组，元素表示专家要接收的 token 数，0 表示该专家空闲
- `packedGroupList`：长度 ≥ M 的 int64 输出数组，前 M 个按原序写入 `groupList` 的非 0 元素，其余不动（建议按 N 超额分配）
- `deviceId`：`torch.device`，取其 `.index` 作为 NPU 设备号

## 规模建议

- world_size=4（4 进程 4 卡，spawn 启动）
- 每 rank offload 池 reserve_size=alloc_size=1GB
- N=NUM_LOCAL_EXPERTS=8，EXPERT_HIDDEN=512，dtype=bfloat16
- group_list：偶 rank 激活偶数下标专家，奇 rank 激活奇数下标专家（每 rank 4 个活跃专家，M=4）

## 必要条件

- 单机至少 4 张可用 NPU（device id 0/1/2/3）
- 已安装 **`memfabric_hybrid`**、`torch`、`torch_npu`、`numpy`
- 已通过 `script/run_pkg_maker/install.sh` 安装 accoffload 扩展库（`libmf_hybm_accoffload.so` + `libmf_hybm_accoffload_kernel.so`），并设置 `MEMFABRIC_HYBRID_EXTEND_LIB_PATH` 指向其目录

## 验收标准

- 各 rank 的 `offload.initialize` 与 `offload.group_pack_copy` 均返回 0
- outputs[0..M) 之和为 0（host 零值紧凑覆盖 device ones）
- outputs[M..N) 之和保持 EXPERT_HIDDEN（ones 未被触动）
- `packed_group_list` 前 M 个元素等于 `group_list` 的非 0 元素且顺序不变，第 M 个之后保持 0
- 4 个子进程全部正常退出（exitcode=0），打印 `group_pack_copy_offload: all ranks OK`

## 运行（Python）

```bash
cd examples/kv_offload/group_pack_copy_offload
python3 group_pack_copy_offload.py
```

成功时打印 `group_pack_copy_offload: all ranks OK`。

## Q&A
