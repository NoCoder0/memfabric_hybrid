# CLAUDE.md

## AscendC API 参考

- 对 `src/csrc/operators/npu/` 下算子里无法理解的 AscendC API（如 `DataCopy`、`DataCopyPad`、`SetAtomicAdd`、`SyncAll`、`PipeBarrier`、`GlobalTensor`、`LocalTensor` 等），请上网搜索 `AscendC <API名称>` 获取用法说明。
- 对 `src/csrc/operators/npu/` 下算子的修改，尽量参考其他已有算子的实现，且要最小化修改范围。

## 自定义内存allocator

- `app/zbal/src/csrc/sma/` 下是一个符合Pytorch标准的自定义内存分配器，ZBAL启动时申请入参指定的内存，将基地址和大小委托给sma管理，后续Device侧的tensor都基于这个空间申请和释放。
- 委托给sma的内存是基于GVA的对称内存空间，每个rank申请相同大小内存，映射到连续的GVA地址，每个rank可以根据地址偏移直接读写其他rank内存地址。

## 典型硬件参数

- A3：对应宏`ZBAL_ASCEND_NPU_A3`，AIV核数有48、40等规格，最大不超过48；每个核的Unified Buffer是192KB。
- A5：对应宏`ZBAL_ASCEND_NPU_A5`，AIV核数有72、56等规格，最大不超过72；每个核的Unified Buffer是256KB。
