# Device-to-Device BatchCopy route probe

该目录只在 `BUILD_TEST=ON` 时构建，用两台 Ascend 950 服务器各一张卡验证：

- Device HBM import view 被发布到固定 route HBM；
- AICPU 只使用 `peerIndex/rangeIndex` 查表并完成卡间读取；
- 非零 offset、区间边界、非法索引和 `magic=0` 被正确处理；
- `CloseDevice()` 在释放 HCOMM 资源前清零 route magic。

## 构建与安装

两台机器使用同一提交构建：

```bash
bash script/build_and_pack_run.sh \
    --xpu_type NPU \
    --build_test ON \
    --build_hcom ON \
    --build_hcom_rdma ON

cd output/hybm/batch_copy_route_probe
sudo -E bash install_probe.sh
export MF_HYBM_AICPU_KERNEL_JSON="$ASCEND_HOME_PATH/opp/vendors/cust/mf_hybm_probe/op_impl/aicpu/config/libcann_hybm_probe_kernel.json"
```

probe 使用独立的 `cann-hybm-probe.tar.gz`、JSON 和 vendor 子目录，不覆盖生产
`libcann_hybm_kernel.json`。卸载命令：

```bash
sudo -E bash install_probe.sh uninstall
```

## 两机运行

先在 rank 0 启动监听，再在 rank 1 指定 rank 0 的控制面 IPv4 地址。`--device` 是各机器本地使用的
ACL device id。

rank 0：

```bash
export MF_HYBM_AICPU_KERNEL_JSON="$ASCEND_HOME_PATH/opp/vendors/cust/mf_hybm_probe/op_impl/aicpu/config/libcann_hybm_probe_kernel.json"
./batch_copy_route_probe --rank 0 --device 0 --port 29876
```

rank 1：

```bash
export MF_HYBM_AICPU_KERNEL_JSON="$ASCEND_HOME_PATH/opp/vendors/cust/mf_hybm_probe/op_impl/aicpu/config/libcann_hybm_probe_kernel.json"
./batch_copy_route_probe --rank 1 --device 0 --peer-ip <rank0-ip> --port 29876
```

程序内部会启用 `MF_HYBM_BATCH_COPY_ROUTE_PROBE=1`，通过 TCP 交换 private data 和 HBM key，依次执行
两阶段 `Prepare()`。两端均输出 `batch_copy_route_probe PASSED` 才表示通过。

程序还会额外导出一块保持为 0 的 Device HBM，并临时把测试 route 的 remote flag 指向该 range，
验证 AICPU 的 60 秒 completion 超时路径；该注入不增加生产或 probe 算子参数。
