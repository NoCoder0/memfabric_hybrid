# 集成测试 (Integration Tests)

集成测试目录，用于验证各组件在真实环境下的功能和性能。

## 目录说明

```text
test/integration/
├── bm/                   # BM 双 rank 设备内存批量拷贝测试
└── trans/                # TRANS 双 rank TransferEngine 真机测试
```

## 组件列表

| **名称** | **介绍** | **路径** |
|---------|---------|----------|
| bm | BM 批量拷贝集成测试 | [bm](./bm/README.md) |
| trans | TransferEngine 双 rank 传输集成测试 | [trans](./trans/README.md) |

## TRANS

`trans/` 用 `spawn` 拉起 Prefill（逻辑卡 0）和 Decode（逻辑卡 1）。设备只在 fixture 执行时选择；不传
`--trans-devices` 时只选择没有 NPU 进程的两张卡，不会默认抢占忙卡。物理卡通过
`ASCEND_RT_VISIBLE_DEVICES` 映射到逻辑 0/1，pytest 收集阶段不会加载 ACL 或占用设备。

```bash
pytest -q test/integration/trans --trans-protocol device_rdma --trans-devices 0 1 \
  --trans-log-dir /tmp/mf-trans-rdma --trans-timeout 180
pytest -q test/integration/trans -k async --trans-protocol sdma --trans-devices 0 1 \
  --trans-log-dir /tmp/mf-trans-sdma --trans-timeout 180
```

请勿使用 `pytest-xdist`。每次运行的 rank 原生日志保留在 `--trans-log-dir/run-*/rank0.log` 和 `rank1.log`；子进程退出码、
清理异常以及 `free sq_cq failed` 等已知释放错误都会失败。覆盖同步 HBM/HOST write/read、混合 HBM/DRAM batch、
batch 向量长度不匹配的拒绝路径，以及 `destroy()` → `unInitialize()` → 重建并重新注册。两侧先注册内存，再通过
Pipe 交换 `ip:get_rpc_port` 与对端已注册裸指针；只有 Prefill rank 0 发起传输。四个异步接口使用 SDMA 和纯 HBM，且
不以 `unregister_memory()` 返回值作为回收证据：tensor 会持有到引擎 destroy/unInitialize 完成
