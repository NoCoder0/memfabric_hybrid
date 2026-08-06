# 01_enable_unified_address_space

## 场景

开启内存池 unified_address_space 特性，对比开启前后的地址访问行为与使用方式。

## 目标

给出一个"特性开关类"最小样例模板，后续可复用到其他特性。

## 使用能力

- `BmConfig.unified_address_space`（开关）
- `bm.initialize` / `create2` / `join`
- `peer_rank_ptr` 与 `gva_to_va`
- 同步 `copy_data`

## 规模建议

- world_size=2
- 每 rank local_dram_size=1GB
- 开关前后执行同一组访问用例

## 必要条件

- 当前版本强制 `unified_address_space=true`（`SmemBmConfigCheck` 在 `src/smem/csrc/smem_bm/smem_bm.cpp:51` 校验，设 `False` 会被拒绝）。README 中"开关前后对比"为设计目标，当前仅演示 ON 路径。
- 所有 rank 使用一致配置，且运行环境支持 unified_address_space。
- 需要 NPU（样例使用 DEVICE_RDMA 协议）。

## 验收标准

- `unified_address_space=True` 下 round-trip 数据正确。
- `gva_to_va` 返回的 VA 可访问，地址语义可解释、可复现。
- 文档明确给出启用场景与限制（当前版本强制 ON）。
