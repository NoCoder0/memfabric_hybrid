# 鉴别诊断 — 结论证据门槛与高歧义现象判别

关键词命中只能生成**假设**，不能作为结论。对每个高歧义现象，必须建立候选假设集，
为每个假设写出：**支持证据**、**反证**、**下一条判别证据**。判别证据执行后仍无法区分时，降级为"证据不足"。

## 一、结论证据门槛

| 结论类型 | 最低证据要求 | 禁止 |
|---|---|---|
| 代码 BUG | 指出 `文件` + `函数` + 代码逻辑矛盾点；同类实现横向对比佐证；能用日志时间轴串起触发路径 | 仅凭"日志行号对不上"或"看起来不对"断言 BUG |
| 已修复 BUG | 给出修复 commit/PR 或至少说明修复所在的版本范围；标注对齐方式（精确 ref / date-approximation） | 承诺具体升级版本（日期近似下） |
| 环境问题 | 环境检查命令的实际输出直接偏离期望值（如大页不足、URL 错误、端口不通） | 由超时倒推网络问题 |
| 配置问题 | 引用变量名 + 日志/代码中读取该变量的位置 + 实际值 | 仅凭"变量是默认值"断言 |
| 性能问题 | ptracer 数据（P50/P99/AVG/MAX）或 benchmark 基线对比 | 用单次日志时间差断言瓶颈 |
| 证据不足 | — | 下任何根因结论；只输出待收集项 |

置信度标注：**已确认**（日志+代码+环境三重支撑）/ **高概率假设**（两重支撑，给出验证动作）/ **待收集**（仅症状）。

## 二、高歧义现象与判别证据

### 症状 A：`smem_bm_init` / TransferEngine 初始化失败或卡住

| 候选假设 | 支持证据 | 反证 | 下一条判别证据 |
|---|---|---|---|
| `MF_CONFIG_STORE_URL` 未设置/格式错 | 日志含 store URL 解析或空 URL 报错 | URL 已打印且格式正确 | `echo $MF_CONFIG_STORE_URL` |
| config store server 未启动/不可达 | client 日志反复 `ReConnectAfterBroken` | server 日志有 `ServerStart` | server 侧 `ss -lntp`；client 侧 `nc -zv` |
| 大页不足（A2 DRAM 池化） | init 日志含 hugepage/mmap 分配失败 | `HugePages_Total` 充足 | `grep Huge /proc/meminfo` vs 池化大小 |
| 驱动/设备不可用 | 报错来自 acl/rt/drv 层初始化 | 报错在 store 连接之前即出现 | plog 中 DRV/RUNTIME 首错 |
| AICPU 算子缺失（A2/A5 device 路径） | 日志含 custom op/kernel 加载失败 | 非 device RDMA/HCOMM 路径 | `mfcli kernel info`；`mfcli kernel version` |

### 症状 B：join 超时 / rank 分配失败

| 候选假设 | 支持证据 | 反证 | 下一条判别证据 |
|---|---|---|---|
| 部分 rank 未到达 join | server 日志 `alive_rank_list` 少于 `world_size` | 所有 rank 日志均已进入 join | 对齐双端 join 时间戳 |
| worldSize 配满 | server 日志 `worldSize exceeded` | alive 数 < world size | `etcdctl get /memfabric_hybrid/config_store/data/alive_rank_list` |
| config store 心跳掉线误清 rank | 心跳超时日志（默认 60s） | 掉线后 rank 重新上线记录 | `MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S` 实际值与网络 RTT |
| 传输层建链阻塞（非 store） | store 状态正常、QP 就绪检查超时日志 | store 连接即失败 | QP 就绪日志、`MF_QP_READY_CHECK_TIMEOUT_BASE` |
| etcd 模式 Leader 降级/重选窗口 | `StopServer` / 选举日志落在 join 窗口内 | 无降级记录 | 选举时序对照 `config_store_cluster_ha.md`（Lease 5s、健康检查 4s、恢复等待 5s） |

### 症状 C：传输建链失败 / peer down

| 候选假设 | 支持证据 | 反证 | 下一条判别证据 |
|---|---|---|---|
| HCOMM 路径 A2 MR 快照限制 | 实现类为 `DeviceRdmaHcommTransportManager` + 失败对象为 join 后新注册内存（如 KV cache） | 实现类为 `RdmaTransportManager`（native 实时查 MR） | 日志确认实现类；检查 `MF_HYBM_RDMA_FORCE_UNREGISTERED`/`MF_HYBM_RDMA_SWAP_SPACE_SIZE` |
| 未注册内存且无 swap | 报错含未注册/swap 未分配语义 | swap 分配成功日志 | `MF_HYBM_RDMA_SWAP_SPACE_SIZE`（device_rdma 默认 0）/ `MF_HYBM_URMA_SWAP_SPACE_SIZE`（默认 0） |
| 4K 页被拦截 | 报错含页大小/地址对齐语义 | 页大小 ≥ 阈值 | `MF_HYBM_ENABLE_4K_PAGE` |
| 对端进程退出/网络断 | 双端日志均有 `LinkBroken`/peer down 记录，且时间戳吻合 | 单端记录、时间差大 | 双端日志时间轴归并（注意时钟偏移） |
| QoS/队列参数非法 | 参数校验回退日志（如 QoS 非法回退 0） | 无该日志 | `MF_DEVICE_UB_QOS`、CQ/WR 深度取值范围 |

### 症状 D：数据传输慢 / 周期性卡顿

| 候选假设 | 支持证据 | 反证 | 下一条判别证据 |
|---|---|---|---|
| 未走预期路径（如该走 RDMA 实际走 TCP/swap） | ptracer 对应 TP 耗时异常、日志路径打印与预期不符 | 路径打印即预期 | 日志确认 DOP_TYPE 与实现类 |
| 切片过小（HCOM） | 大量小片传输记录 | 切片即默认/预期 | `HCOM_MAX_SLICE_SIZE`、`HCOM_RECV_DATA_SIZE` |
| swap 中转放大拷贝 | FORCE_UNREGISTERED=1 且 ptracer 显示 swap 路径耗时占比高 | 直传路径 | 关闭强制未注册对比（native 路径支持动态注册时） |
| 主机侧 NUMA 不亲和 | host_rdma/host_shm 场景跨 NUMA 访问特征 | 本地节点内存充足 | `mfcli mem scan` 对照中转内存分布 |
| 底层网络/驱动问题 | plog DRV 层重传/丢弃记录 | 应用层日志无对应特征 | `dmesg` RDMA 驱动日志、ibv 状态 |

### 症状 E：Python 侧异常（Import/接口行为）

| 候选假设 | 支持证据 | 反证 | 下一条判别证据 |
|---|---|---|---|
| ABI 不兼容（`undefined symbol`/`_GLIBCXX_USE_CXX11_ABI`） | 加载器报错符号名 | 报错为 Python 层逻辑异常 | 构建参数 `--build_open_abi` 与发行版 GCC 版本 |
| offload 扩展库未安装 | offload 调用报 lib 不存在 | bm/trans 正常 | `mfcli kernel info` |
| `MF_LOG_LEVEL` 不生效（误解） | 用户改级别无变化但功能正常 | — | 文档明确：仅 Python 接口生效，bm/shm 不生效 |
| acc_offload 源码编译失败 | `mfcli kernel install` 日志 | 环境不支持（910B 自动跳过属正常） | 安装日志提示的缺失依赖（CANN/torch_npu） |

## 三、时间轴归并规则

1. 以**日志时间戳**（非文件 mtime、非文件名顺序）排序锁定首错。
2. 多 rank/双端必须显式标注来源（`node0/rank0: 文件:行号`），逐一列出。
3. 双端时钟偏差无法确认时（无 NTP 证据），跨端先后顺序只能作参考，不能作为唯一判别依据；此时置信度降级。
4. 上层症状（`transfer failed`、`stream timeout`）与底层首错分开记录，结论以底层首错为准。

## 四、代码交叉验证方法

```bash
# 定位报错文案来源（含历史版本）
git grep -n "<报错文案>" -- src
git grep -n "<报错文案>" <sha> -- src

# 追关键变量来源后，横向比较同类实现
# 例：native RDMA（实时 MR 表）vs HCOMM（A2 MR 快照）vs URMA（支持 UpdateMemInfo）
```

- 同类实现不一致（一个支持运行时更新、一个快照冻结）时，先核对文档是否已声明该限制
  （如 A2 HCOMM 限制已写在 `environment_variables.md`）：**已声明的限制不是 BUG**，是配置/用法问题。
- 逻辑确实经不起推敲且文档无声明 → 代码 BUG 候选，进入 BUG 证据门槛流程。
