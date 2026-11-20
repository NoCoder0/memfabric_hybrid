# TRANS 双 rank 集成测试

本目录覆盖 `TransferEngine` 的 15 个公开方法，在 Linux Ascend A2、A3 和 A5 上使用两张健康且空闲的物理卡。
`device_rdma` 验证 HBM/HOST 同步传输，`sdma` 验证 HBM 同步和异步传输。
HOST_RDMA、跨节点、非零预留 flags 不在本套验证范围内。

## 目录职责

```text
trans/
├── trans_config.py       # 一次 pytest 运行的固定配置与静态校验
├── conftest.py           # 选项、延迟启动 fixture、失败状态及阶段结果
├── trans_runtime.py      # 双 worker、引擎生命周期、控制消息与原生日志审计
├── trans_helpers.py      # 对齐 buffer、注册、数据校验和 ACL stream 操作
├── batch_transfer/       # 批量同步/异步传输与批量注册，001–010
└── transfer/             # 非批量传输、单块注册及引擎生命周期，001–011
```

参照 `bm/copy_data_batch/`，每个文件一个 pytest class，采用三位编号，例如
`batch_transfer/test_batch_transfer_001.py` → `TestBatchTransfer001`。
class 实现 `setup(ctx)`、`run(ctx)`、`teardown(ctx)`，语义化测试方法调用 `trans_pool.run(type(self))`。
worker 执行这些方法；case 决定 API、方向和参数，运行时只管理通用生命周期。
配置在一次 pytest 运行内固定；共享池在首个支持的用例执行时启动，运行结束或首次失败时关闭。
首次失败后的用例标为未执行，诊断单个接口时应另起 pytest 指定文件，保留原分组失败结果。

## 运行

环境需要当前源码构建的 MemFabric、CANN、torch、torch_npu、pytest ≥ 8。
pytest 7 会把 `setup(ctx)` 识别为旧式 setup 钩子，与 case 生命周期冲突。
从仓库根目录执行：

```bash
python3 -m pytest test/integration/trans -v -s --log-cli-level=INFO \
  --trans-protocol device_rdma --trans-devices 2 3 --trans-log-dir /tmp/trans-rdma
python3 -m pytest test/integration/trans -k async -v -s --log-cli-level=INFO \
  --trans-protocol sdma --trans-devices 2 3 --trans-log-dir /tmp/trans-sdma
```

单独运行混合 HBM/HOST 批量同步写：

```bash
python3 -m pytest test/integration/trans/batch_transfer/test_batch_transfer_001.py \
  -v -s --trans-protocol device_rdma --trans-devices 2 3
```

`--trans-devices` 是物理卡号；上例子进程内分别使用逻辑设备 0、1。不指定时自动选择两张健康空闲卡；
显式指定也会检查 NPU 和 HCCP 占用。收集用例不初始化设备，父进程的设备可见性环境变量会还原。
`--trans-timeout` 是有限正秒数，默认 120；禁止 pytest-xdist 并行运行。
包含 HOST 的 case 仅支持 DEVICE_RDMA，异步 case 仅支持 SDMA，协议门控在启动设备前执行。
已支持场景的初始化、传输或清理失败会报错，不转为 SKIP/XFAIL。

## 用例编号与 API 覆盖

共 21 个 case。`APIS` 声明实际调用的方法，`PROTOCOLS` 声明适用协议。
单块注册、注销、初始化、RPC 端口、销毁和 unInitialize 均纳入 `transfer/`，不会因非传输接口而遗漏。

| 目录 | 编号 | 接口与验证场景 |
| --- | --- | --- |
| batch_transfer | [001](batch_transfer/test_batch_transfer_001.py) | batch_transfer_sync_write，混合 HBM/HOST 与不同长度 |
| batch_transfer | [002](batch_transfer/test_batch_transfer_002.py) | batch_transfer_sync_read，混合 HBM/HOST 与不同长度 |
| batch_transfer | [003](batch_transfer/test_batch_transfer_003.py) | 批量同步写向量长度不等拒绝，源和目标不变 |
| batch_transfer | [004](batch_transfer/test_batch_transfer_004.py) | 批量同步读向量长度不等拒绝，源和目标不变 |
| batch_transfer | [005](batch_transfer/test_batch_transfer_005.py) | batch_transfer_async_write_submit，SDMA HBM 与真实 stream |
| batch_transfer | [006](batch_transfer/test_batch_transfer_006.py) | batch_transfer_async_read_submit，SDMA HBM 与真实 stream |
| batch_transfer | [007](batch_transfer/test_batch_transfer_007.py) | batch_register_memory，混合内存批量注册后实际传输 |
| batch_transfer | [008](batch_transfer/test_batch_transfer_008.py) | 批量异步写向量长度不等拒绝，stream 完成同步及数据不变 |
| batch_transfer | [009](batch_transfer/test_batch_transfer_009.py) | 批量异步读向量长度不等拒绝，stream 完成同步及数据不变 |
| batch_transfer | [010](batch_transfer/test_batch_transfer_010.py) | 批量注册的地址/容量向量长度不等拒绝，buffer 不变 |
| transfer | [001](transfer/test_transfer_001.py) | transfer_sync_write，HBM→远端 HBM |
| transfer | [002](transfer/test_transfer_002.py) | transfer_sync_write，HBM→远端 HOST |
| transfer | [003](transfer/test_transfer_003.py) | transfer_sync_read，远端 HBM→本地 HBM |
| transfer | [004](transfer/test_transfer_004.py) | transfer_sync_read，远端 HBM→本地 HOST |
| transfer | [005](transfer/test_transfer_005.py) | transfer_async_write_submit，SDMA HBM 与真实 stream |
| transfer | [006](transfer/test_transfer_006.py) | transfer_async_read_submit，SDMA HBM 与真实 stream |
| transfer | [007](transfer/test_transfer_007.py) | register_memory / unregister_memory，注销、重注册后实际传输 |
| transfer | [008](transfer/test_transfer_008.py) | initialize / get_rpc_port，非法参数拒绝、初始化前后端口 |
| transfer | [009](transfer/test_transfer_009.py) | initialize 默认 SDMA 参数、真实 RPC 端口 |
| transfer | [010](transfer/test_transfer_010.py) | destroy / unInitialize 重复调用、destroy 后端口为 0、HBM 重建 |
| transfer | [011](transfer/test_transfer_011.py) | HOST 注册后销毁重建，再注册同一地址并传输 |

长度不等的拒绝路径在 Python wrapper 中检查，不将非法地址提交给 DMA。
接口存在用例不等于实机通过；报告须区分调用、数据断言、清理和未执行状态。

## 数据与资源语义

Prefill rank 0 发起读写，Decode rank 1 被动。双方先注册，再经 worker 专用 Pipe 交换真实
`127.0.0.1:get_rpc_port()` 与 payload 指针；不通过控制通道传递期望数据。
两端按 seed 和块名独立生成期望值，校验源未改变、目标数据及前后 guard。
NPU 准备完成后显式同步；异步使用真实 ACL stream，确认完成后才销毁。

每个 Block 持有完整的 2 MiB 对齐注册区域。`unregister_memory()` 当前为兼容实现，返回成功不证明物理注册释放；
注册对象持有到 engine destroy / unInitialize 完成。无法确认异步流完成时，不显式销毁相关 engine 和 buffer，
并报告清理失败；worker 退出不代表驱动资源正常释放。

日志位于 `--trans-log-dir/run-*/rank0.log` 和 `rank1.log`，包含 Python 与原生 stdout/stderr。
成功校验记录 `DATA_VERIFIED`；pytest 各阶段记录 `TRANS_RESULT`。
非零退出码、缺少 cleanup_done、强制终止及原生释放错误均使运行失败。
SQ/CQ 报错判为清理失败。此处不修复原生流生命周期。
数据通过、API 失败和清理失败应分别保留，不用独立运行成功覆盖共享引擎运行失败
