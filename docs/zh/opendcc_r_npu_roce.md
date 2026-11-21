# openDCC R_npu_roce 接入与验收

本分支基于 MemFabric master `3b45c951c8efc380629353d6ac6ef0047126c2b1`，
导入 `host_raw_xdma` 的 openDCC 基础代码 `04c0605c7db4c8a68d0a44209b2c44ea6d435f28`。
MemCache 接口基线为 master `eab3f0d42e5cce8084337755ceea8f90998170fd`，不修改其公共接口。
交付分支为 `feat/kunpeng-dram-v4`，远端为 `NoCoder0/memfabric_hybrid`。

目标是 A3 业务 KV HBM 与鲲鹏 CX7 DRAM 缓存池之间的直接 RDMA WRITE/READ。
Host DRAM 是远端缓存的最终存放位置；A3 不分配本机 DRAM swap，不复制 KV 到本机临时区。
本次只验收 128K、256K 两个序列档位；代码与模拟测试通过不代表硬件、业务正确性或性能已经通过。

## 1. 接口和职责

```text
MemCache（原有 register_buffer / batch set/get）
  -> SMEM BM / MemEntityDefault / HostComposeDataOp（复用）
  -> ComposeTransportManager（原有 DEVICE_RDMA 协议槽）
  -> OpenDccTransportManager（新增公共适配）
       DeviceRdmaOpenDccTransportManager（NPU 构建）
       HostRdmaOpenDccTransportManager（NONE 构建）
  -> DlOpenDccApi -> libmf_opendcc.so 的 hcc_* C 接口
  -> HCC communicator / MR / tunnel / request
  -> Genetty Device RDMA + HCCP / TS 或 Host verbs
```

MF 不直接依赖 Genetty 的 context/QP/MR 实现，只调用 HCC 导出的接口。
两端均使用原有 `device_rdma` 逻辑协议；本地 provider 分别是 `DEVICE_RDMA_GENETTY` 与 `HOST_RDMA_GENETTY`。
不新增 MF 协议枚举，不把鲲鹏侧改成另一套逻辑协议。

| MF 接口 | HCC 接口及约束 |
| --- | --- |
| `OpenDevice` / `CloseDevice` | `hcc_init/create` 与 `hcc_destroy/uninit`；按 MF 逻辑设备 ID 获取选定的 Device Context，不逐卡打开所有 NPU |
| `RegisterMemoryRegion` / `UnregisterMemoryRegion` | `hcc_memory_register/unregister`；支持建链后注册、同区间引用计数，存在请求引用时不能注销最后一个 MR |
| `QueryMemoryKey` / `UpdateMemoryKey` | HCC MR 编解码；MF 外层保存匹配用 GVA，内层保存实际网卡可访问地址和 key；更新 GVA 不改网络地址 |
| `Prepare` / `UpdateRankOptions` | 更新对应 rank 的地址、provider 和 MR 集合；实体信息更新不携带 key 时保留已有 MR |
| `ConnectRank` | `hcc_tunnel_create/export/import`；复用 acc_links TCP/TLS 控制面交换不透明链接信息 |
| `WriteRemote[BatchAsync]` / `ReadRemote[BatchAsync]` | 构造带真实地址、offset、size、lkey/rkey 的描述符，`hcc_tunnel_submit` → `hcc_request_wait` → `hcc_request_release` |
| `Synchronize` | 首版适配在每次调用内完成提交和等待；此处检查连接状态 |
| `RemoveRanks` | 销毁所选 peer 的 tunnel 并释放控制连接；重新加入必须重新交换 QP 和 key |

控制面由小 rank 发起，经历 EXPORT、IMPORT、READY。Device 端先 import 后导出最终属性，
Host 端导入对端最终属性后才进入 READY。双方完成后才允许业务访问。
控制消息使用非阻塞连接；无链接数据的消息带一个版本字节，满足 acc_links 的非空消息体约束。
RDMA TC 默认 132，可通过原有 `MF_DEVICE_RDMA_TC` 配置；保持与现有 A3 RoCE 配置一致。
控制端点复用 MF 原有 `transUrl`，即 MC 的 `hcom_url`：从基础端口 + rank 至 65535 的范围内，
复用 `AccFindAvailableTcpPort` 选择可用端口，`GetNic()` 通过原有实体元数据发布实际端口。
同一进程不同 BM ID 可复用基础 URL，各自监听、握手和销毁，部署时需开放实际控制端口范围。

HCC 请求保存描述符和 MR 引用，每个 tunnel 接受一个活动 batch；同一 batch 逐 WR 推进，检查真正 RDMA CQE 的
状态、操作与 `usr_ctx` 后才推进下一条。超过 32 位长度的区间拆成多个 WR。
为保持首版最小实现，进度由 `hcc_request_wait` 调用线程驱动，不新增后台线程或每 block 线程，
原 `HccCqeHarvester` 占位类没有进入本路径。`submit` 接受请求不表示数据已完成，也不会独立在后台推进。

MF 中名称带 `Async` 的新适配接口仍等本 rank 的请求完成后才返回，保持旧数据算子的失败出口安全。
投递失败、CQ 错误或超时必须先停止 QP 的 DMA 访问，再允许业务复用缓冲区；不能确认停止时终止该测试实例。
错误码在适配层转换，不把 HCC 错误码直接当成 MF 错误码；失败后不自动换回旧后端或重放 WRITE。
控制连接断开或数据请求失败后，SMEM 链接查询返回 IDLE；显式重新连接会销毁旧 QP、重新握手。
注销 MR 失败会保留 SMEM 注册记录、HYBM slice 与 VA 映射，调用方重试成功后才释放这些记录。

HCC 原先未实现的 P2P `destroy/unregister/tunnel_destroy/tunnel_import` 改为返回状态，options 增加 provider 等字段。
旧二进制消费者需重新编译；MF/SMEM/MemCache 公共 ABI 不变。此次实现只覆盖 P2P，不宣称原有 HCC server/client 占位接口可用。

## 2. 修改边界

新增代码集中在 openDCC、两个派生 manager、公共 manager、动态加载器、测试与独立启动/验证工具。
原有代码的必要接点：构建开关、Compose 后端选择、Entity 加载与逻辑设备传入、NONE 的 SMEM 普通 BM 入口、
Host 数据算子选择，以及 Host DRAM 映射跳过 NPU HAL 注册。

`BUILD_OPENDCC` 默认 OFF；运行时 `MF_HYBM_RDMA_USE_OPENDCC` 默认 0，开关未启用时原后端选择保持原行为。
启用后仅普通 BM、纯 DEVICE_RDMA、Host 发起、DEFAULT 角色的实体选择 openDCC。
同一进程内 TRANS、provider/代理、mixed protocol 和 AI Core 发起实体仍使用原有后端。
已选择 openDCC 的实体遇到缺少构建或加载失败时明确报错，不回退旧后端。环境选择按进程启动配置生效，不支持热切换。

## 3. 构建与测试

分别在目标平台构建，保留旧产物，使用独立安装前缀或容器：

```bash
# A3
bash script/build_and_pack_run.sh --xpu_type NPU --build_opendcc ON
# 鲲鹏：使用 libibverbs 和 CX7 驱动，不依赖 ACL 设备初始化
bash script/build_and_pack_run.sh --xpu_type NONE --build_opendcc ON
```

新增库为 `output/openDCC/lib64/libmf_opendcc.so`，公共头文件在 `output/openDCC/include`，
样例在 `output/openDCC/bin/hcc_pingpong`。ON 构建的 run 包与 Python wheel 均携带此库。
新 manager 用 dlopen 加载 HCC；从源码目录手工部署时，把 MF、SMEM、openDCC 库放入同一独立运行目录并设置库搜索路径。
Bazel 暂不支持该可选后端，wrapper 对 ON+Bazel 报错，默认 Bazel 流程保持原行为。

```bash
git submodule update --init --recursive
bash script/run_opendcc_ut.sh
MF_HYBM_RDMA_USE_OPENDCC=1 bash script/run_opendcc_ut.sh 'OpenDccSelectionTest.*:HccP2pTest.*'
python3 examples/opendcc/test_verify.py
```

测试覆盖 Genetty 原有用例、HCC 建链后注册/偏移读写/保护区/多 peer/部分失败/超时/MR 引用，
以及三个 MF manager 通过实际 TCP 控制连接握手、交换变长 key 和偏移 WRITE/READ。
这些 UT 的 RDMA/ACL/HCCP 是模拟接口，不能替代真机数据验证。

先在两台机器运行只经公开 HCC 通信接口的样例：

```bash
# 鲲鹏：分别指定控制网 IP 和 CX7 RoCE IP
LD_LIBRARY_PATH=/opt/opendcc/lib:$LD_LIBRARY_PATH hcc_pingpong host <control-ip> 21000 <cx7-roce-ip>
# A3：最后一个参数是 ACL UserDeviceId，程序转换为 LogicDeviceId
LD_LIBRARY_PATH=/opt/opendcc/lib:$LD_LIBRARY_PATH hcc_pingpong device <host-control-ip> 21000 0
# 可追加第二个 Host 服务端，复用同一 communicator，验证不同数据图案
hcc_pingpong device <host1> 21000 0 <host2> 21000
```

样例在建立 tunnel 后才注册 HBM/DRAM，执行两段 WRITE、两段 READ，验证数据、偏移及两侧保护区。
样例中的 HBM 初始化和结果校验使用 ACL 拷贝；实际网络传输的 SGE 始终指向 HBM。

## 4. MemCache/vLLM 新组启动

MC 保持现有普通 BM 接入，两端协议使用 `device_rdma`；不要使用历史 provider/代理配置。
复用既有模型、KV 池、数据和请求发生器，独立生成启动命令，不能先运行旧 `roce_v3.start` 再覆盖冻结库。

vLLM connector 原 `_register_buffers_if_needed()` 忽略注册返回值，新路径必须先补检查：

```bash
# 在实际运行镜像对应的 vllm-ascend 源码根目录执行
git apply --check /path/to/memfabric_hybrid/examples/opendcc/vllm_ascend_register_buffer.patch
git apply /path/to/memfabric_hybrid/examples/opendcc/vllm_ascend_register_buffer.patch
```

补丁只在 openDCC 开关为 1 时启用严格注册：失败会清理本批已注册区间并抛错，成功记录 worker PID、device 和 KV 区间。
补丁按当前本地 vllm-ascend 接口制作，部署前必须在实际 GLM5.2 0.30 镜像代码上通过 apply 检查并打包部署；
仅放补丁到仓库不会修改已有镜像。默认模式保持原有注册处理行为。

```bash
# <command> 是该节点独立生成的 MC/vLLM 启动命令；wrapper 不覆盖模型或协议参数
bash examples/opendcc/start.sh /opt/opendcc/lib device <command> <args...>
MF_OPENDCC_RDMA_IP=<cx7-ip> bash examples/opendcc/start.sh /opt/opendcc/lib host <command> <args...>
```

wrapper 强制设置 `MF_HYBM_RDMA_USE_OPENDCC=1`、`MF_HYBM_RDMA_USE_HCOMM=0`、
`MF_HYBM_RDMA_FORCE_UNREGISTERED=0`、`MF_HYBM_RDMA_SWAP_SPACE_SIZE=0`。
检查真实进程加载映射，防止 Python 包的绝对路径加载覆盖库搜索路径。
保留模型集合通信/Mooncake 所需依赖；出现 HCCL 等库不等于业务 RDMA 后端选择错误。

## 5. 路径证据与性能判定

功能取证时打开 MF DEBUG 日志，执行真实 KV 远端写入和回温，然后在服务运行期间执行 `verify.py`。
manifest 必须列出全部 P worker 和 Host 服务，PID 使用执行 verifier 所在 PID namespace 的值，日志必须包含原生 PID 前缀。
每个进程指定 rank、role、日志，以及期望的 MC/MF/openDCC 文件 SHA-256，例如：

```json
{
  "hosts": [
    {"pid": 2345, "rank": 16, "log": "/logs/host-16.log"},
    {"pid": 2346, "rank": 17, "log": "/logs/host-17.log"}
  ],
  "processes": [
    {
      "pid": 1234, "rank": 0, "role": "device", "peers": [16, 17], "log": "/logs/p-worker.log",
      "libraries": {
        "libmf_smem.so": "实际SHA256", "libmf_hybm_core.so": "实际SHA256",
        "libmf_opendcc.so": "实际SHA256", "libmemcache.so": "实际库名及SHA256"
      }
    },
    {
      "pid": 2345, "rank": 16, "role": "host", "log": "/logs/host.log",
      "libraries": {
        "libmf_smem.so": "实际SHA256", "libmf_hybm_core.so": "实际SHA256",
        "libmf_opendcc.so": "实际SHA256", "libmemcache.so": "实际库名及SHA256"
      }
    }
  ]
}
```

```bash
python3 examples/opendcc/verify.py runtime.json --output runtime-evidence.json
```

跨节点分别在对应节点运行，`processes` 包含该节点全部相关进程，汇总所有节点报告。
在 Device 节点汇集目标 Host 日志，`hosts` 记录日志中的原始 Host PID、rank 和本地归档路径；
Host 节点自己的 `processes` 报告仍负责验证其实际加载库和运行环境。
脚本读取 `/proc/PID/maps` 的实际库并校验哈希，检查 provider/env，关联 connector KV 区间和 HBM MR。
每个 `peers` 目标 Host 都必须有完成的 READ 和 WRITE，远端 DMA 地址须落在该 Host 的 DRAM MR 内；
访问另一个 NPU 的 READ 不能作为 Host 回温证据。
它验证记录到的 DMA 地址路径，不替代业务输出正确性、双侧网口计数或完整进程清单核对。
取证结束后正式性能测试关闭逐描述符 DEBUG 日志，避免日志 I/O 影响结果；保存取证和正式测试各自的完整配置。

128K 使用 131072 输入、256 输出；P max-num-seqs=1、DP2 TP8 DCP1，按冻结 manifest 跑一次 C8 预热、
C16 对照及 C2/C4/C8/C12/C16 正式阶段。256K 使用 262144 输入、256 输出、DP2 TP8 DCP8，
原 C4 对照和 headroom 扫描分别按各自 manifest 的上下文上限、并发、水位运行，不混用配置或输入。
不增加其他序列长度档位。

原后端与 openDCC 使用相同硬件、模型、MC/MF 基线、冻结输入与测量窗口；保留后端所需配置差异。
将既有采样结果汇总为两个 JSON，顶层包含相同 `comparison_config`、各自 `artifacts` 哈希、`evidence` 路径和 `stages`。
每个 stage 至少包含：

```json
{
  "input_tokens": 131072, "stage": "formal_c16_control", "round": 1,
  "dataset_sha256": "实际SHA256", "requests": 160, "succeeded": 160,
  "output_tokens": 256, "concurrency": 16, "warmup": "一次C8/80条", "max_model_len": 262144,
  "data_correct": true, "hbm_direct": true, "swap_bytes": 0,
  "requests_per_second": 1.0, "output_tokens_per_second": 256.0,
  "ttft_mean_ms": 1000.0, "ttft_p50_ms": 900.0, "ttft_p95_ms": 1500.0,
  "tpot_ms": 10.0, "duration_seconds": 160.0
}
```

以上数字仅说明字段，不是实测结果。必须从原始日志/请求结果填入真实数据，归档命中率、实际计算 token、失败数、
CPU、队列等待和 A3/CX7 双侧计数；完整阶段清单按冻结计划核对。

```bash
python3 examples/opendcc/compare.py baseline.json opendcc.json --output comparison.json
```

脚本要求同时提供 128K、256K，逐阶段/逐轮配对，不设回退容忍百分比；吞吐不降低，TTFT/TPOT/窗口耗时不增加。
有波动时保留所有成对测量，不能取最好一轮。单 WR 窗口或逐 rank 完成若形成瓶颈，需依据真机结果优化后重测；
本次不预先承诺该功能版本的吞吐达到旧后端。

回滚时停止新组，恢复旧产物、配置及运行开关后冷启动。不要热切换已创建的 QP/MR。

## 6. 本地验证记录（2026-11-21）

环境为 WSL Ubuntu 22.04、x86_64、GCC 11.4。通过 CMake 直接构建，关闭 Python/HCOM；
NPU 配置仅验证该条件编译分支，不代表已在 AArch64、CANN 或 A3 上运行。

| 检查 | 结果 |
| --- | --- |
| `XPU_TYPE=NONE, BUILD_OPENDCC=ON` DEBUG / RELEASE | 构建通过，包含 HCC 样例；RELEASE 启用仓库的 Werror |
| `XPU_TYPE=NPU, BUILD_OPENDCC=ON` DEBUG | 构建通过 |
| `XPU_TYPE=NONE, BUILD_OPENDCC=OFF` DEBUG | 构建通过；hybm 动态库无新增 manager 符号 |
| openDCC UT | 149 项通过，含真实 TCP 控制连接与模拟 RDMA 数据路径 |
| 独立进程设置 `MF_HYBM_RDMA_USE_OPENDCC=1` | 后端选择用例通过 |
| 原 MF 相关回归（开关关闭） | 432 项中 427 项通过、5 项失败；对应 master 原版实现也复现同样 5 项失败，见下文 |
| clang-format 18.1.8 / Ruff 0.14.14 | 新增/修改代码检查通过，原 MF 文件仅格式化修改区间 |
| 启动及验收工具 | Shell 语法、环境覆盖、伪 `/proc` 取证、配对指标及回退拒绝用例通过 |
| vLLM connector 补丁 | 本地 vllm-ascend 源码 `git apply --check` 通过；注册成功/失败回滚分支模拟通过 |

原 MF 回归包含 ComposeTransportManager、HybmComposeDataOp、Entity、ConnBasedSegment、SMEM BM。
失败项及本地原因：

- `HybmEntityDefaultTest.SetExtraContext_Success_ReturnOk`：NONE 后端与测试中的 Ascend Memcpy mock 不匹配。
- `HybmConnBasedSegmentTest.AllocLocalMemory_MapsOneSliceAndTracksVa`、`Export_AfterRereserveUsesCurrentSlice`、
  `FreeMemory_ReleasesSlicesAndReservedWindow`：WSL 环境大页分配/映射失败。
- `HybmConnBasedSegmentTest.ReleaseRegisteredHostMemory_UnregistersDeviceForProviderAndProxy`：
  NONE 构建不执行测试所期望的 Ascend HAL 注册。

对照时从 master `3b45c951` 提取原版 `hybm_entity_default.cpp`、`hybm_conn_based_segment.cpp`，
使用相同编译配置重新编译、替换对应对象后单独运行这五项，均复现失败；未修改原有测试或业务代码来绕过它们。
这不是完整主干 UT 通过证明，也未执行全仓 ASAN/覆盖率验收。

尚未执行目标 A3/CX7 双机样例、GLM 0.30 镜像部署、128K/256K 业务及性能对照。
run 包和 wheel 的复制路径已接入，完整打包安装尚未在目标环境验证。
connector 补丁必须先对实际部署镜像内源码校验再应用。

## 7. 审阅修复与 CANN 共存限制

HCCP 结构按本地 hcomm `dd21371a3` 的 `hccp_common.h` 对齐：`QpAttr` 为 112 字节，
`vendorPrivInfo` 偏移 40，64 字节 `feature` 偏移 48；`TypicalQp` 为 184 字节。
代码增加大小/偏移静态断言，mock 按实际 CANN 写入完整尾部并检查相邻内存。
Host 显式 IP 选择匹配的 RoCE v2 GID，QP 导出和 RTR 使用同一个 GID/index；
同一网口不同 IP/VLAN 的 context 不修改彼此已选定的 GID。

**HCCL 先初始化同卡资源的共存路径尚未完成，不能据此宣称 GLM Worker 已可用。**
当前 Device Context 仍自行初始化 RA/TSD/Rdev。检查目标 CANN 源码发现：

- `RaRdevGetHandle` 可查询已存在 Rdev，但没有公开资源引用计数语义。
- `RaQpCreateWithAttrs` 会清零 `dataPlaneFlag`，仅设置 `cq_cstm` 不能让当前 OP_EXT QP 脱离后台 CQ 轮询。
- HCCL 的 lite 后台线程与 `RaPollCq` 会消费同一个 CQ；直接借用已有 Rdev 不能保证本库收到每个 CQE。
- 在借用 RA 后另建 Rdev 会改写设备级 notify 地址/cache，也不能作为不影响 HCCL 的替代实现。

完成该项需要目标 CANN 提供受支持的、按 QP 排除后台轮询的接口，以及清晰的资源借用生命周期。
2026-11-21 已确认本轮保留此限制，作为后续待解决问题记录，不增加 CANN/HCCP 接口或补丁。
本分支未使用私有结构强转或关闭 HCCL 全局线程来绕过。
独立 openDCC 模拟测试不证明 HCCL 共存，也不证明 128K/256K 业务与性能通过。

后续解决条件：明确借用 RA/TSD/Rdev 的所有权与释放规则，保证每个 QP 的 CQE 只有一个消费方；
分别验证独立初始化和 HCCL 先初始化两种顺序，并确认 openDCC 销毁后 HCCL 集合通信仍正常。
该限制解除并完成真机验证前，GLM Worker 的 HCCL 共存路径保持未验收状态。

本轮修复验证（同一 WSL/NONE 环境）：

- openDCC 开关开启：156 项全部通过；关闭：153 项通过、3 项按条件跳过。
- 原 MF 与 acc_links 回归：490 项中 485 项通过，失败仍为第 6 节列出的同 5 项。
- 开关开启的 Compose/数据算子/TRANS 元数据及回调单测：154 项全部通过。
- 验收脚本：5 项通过，包含 NPU READ 不能替代目标 Host READ、逐 Host 双向操作和远端 MR 范围检查。
- NONE RELEASE、NPU DEBUG、NONE 关闭 BUILD_OPENDCC 构建通过；关闭构建的 hybm 动态库无 OpenDccTransportManager 符号。
- 完整 TRANS 测试中的真实 HOST_TCP 收发因本地未构建/安装 `libhcom.so` 无法完成；
  开关关闭后单独运行同样复现缺库，不记为通过，也不修改原测试绕过依赖。
