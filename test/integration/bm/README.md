# BM 批量拷贝集成测试

## 目录职责

- `config.py` 保存一次 pytest 运行的固定配置，校验容量、设备和超时。
- `conftest.py` 解析命令行参数，只在 BM 用例执行时选择设备，并维护全运行失败状态。
- `bm_runtime.py` 管理一个延迟启动的双 worker BM 池。它在首个用例启动，在运行结束或首次失败时关闭。
- `helpers.py` 提供 buffer 分配、注册清理、数据生成、校验和设备发现。
- `copy_data_batch/` 每个文件一个 pytest class。class 实现 `setup(ctx)`、`run(ctx)` 和 `teardown(ctx)`。

## 运行

在 Linux Ascend 环境中，从仓库根目录运行。环境需要安装当前源码、CANN、torch、torch_npu 和 pytest。

```bash
pytest test/integration/bm -v -s
```

单独运行 GH2L 用例：

```bash
pytest test/integration/bm/copy_data_batch/test_copy_data_batch_001.py -v -s
```

池容量参数以字节为单位，超时参数以秒为单位，且必须为有限正数。以下参数值与默认配置相同：

```bash
pytest test/integration/bm -v -s --bm-protocol device_rdma \
  --bm-world-size 256 \
  --bm-local-dram-size $((1 << 30)) --bm-max-dram-size $((1 << 40)) \
  --bm-local-hbm-size 0 --bm-max-hbm-size 0 \
  --bm-setup-timeout 180 --bm-case-timeout 120 --bm-cleanup-timeout 60
```

## 添加用例

在 `copy_data_batch/` 中新增测试文件，并在普通 pytest `Test...` class 中实现 `setup(ctx)`、`run(ctx)` 和
`teardown(ctx)`。测试方法调用 `shared_pool.run(type(self))`。每个 class 的 setup、run 和 teardown 会在各自
worker 中执行。场景目录保留 `__init__.py`，以区分不同场景下的同名测试模块。单个用例不能更改池配置；
不同配置需要单独启动 pytest 调用。
