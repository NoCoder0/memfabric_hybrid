# CLAUDE.md

## AscendC API 参考

- 对 `src/csrc/operators/npu/` 下算子里无法理解的 AscendC API（如 `DataCopy`、`DataCopyPad`、`SetAtomicAdd`、`SyncAll`、`PipeBarrier`、`GlobalTensor`、`LocalTensor` 等），请上网搜索 `AscendC <API名称>` 获取用法说明。
- 对 `src/csrc/operators/npu/` 下算子的修改，尽量参考其他已有算子的实现，且要最小化修改范围。