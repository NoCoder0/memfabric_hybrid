# 04_single_node_multi_device_batch_copy

## 场景

单机两张 Ascend 950 通过 `HOST_DEVICE_URMA` 建链。每个进程在本地 HBM 写入数据，再由对端进程通过
生产 `HybmBatchCopy` AICPU 算子主动读取到本地 HBM。

## 验证内容

- 两端均通过公开 `BmDataOpType.HOST_DEVICE_URMA` 选择 Device manager。
- 数据路径使用公开 `copy_data()` 和 `copy_data_batch()`，不直接调用 manager、publisher 或测试 probe。
- 覆盖 1、999、1000 和 1001 条，验证 HCOMM 1000 条分片边界。
- 两个方向均执行真实 HBM 数据校验。

## 运行条件

- 单机至少两张 Ascend 950。
- 已安装与当前 MemFabric 版本匹配的 AICPU run 包或 NPU wheel。
- CANN/HCOMM 支持 `HOST_DEVICE_URMA` 使用的 UBC/HCOMM 能力。

## 运行

```bash
python3 04_single_node_multi_device_batch_copy.py
```
