# memfabric_hybrid 数据传输示例

## 目录说明

本目录包含基于 memfabric_hybrid 传输引擎（mf_adapter / TransferEngine）的跨节点数据传输示例，适用于基于
昇腾 NPU 的分布式计算场景。示例通过内存注册与直接内存访问实现节点间数据的高效传输，避免了传统 IO 操作的
性能开销。

各脚本均以 **Prefill**（数据生产方）与 **Decode**（数据消费方）两类角色运行，每个实例内部会按自身会话 ID
（`src-unique-id`）创建配置存储服务（config store），对端按会话 ID 直连该服务完成发现与建链，支持大规模张量
数据（torch.Tensor）的传输。

| 脚本 | 传输模式 | 说明 |
|------|---------|------|
| test_transfer_engine.py | push | 基础跨节点张量传输：Prefill 生成数据并主动写入 Decode |
| test_transfer_engine_rd2h.py | pull | SFA RD2H 场景 PD KV Cache 拉取传输，支持 Decode 侧多 rank 与可选 d2d |
| test_transfer_engine_d2rh_shared.py | push | SFA D2RH 场景 PD KV Cache 推送传输，Decode 共享 DRAM 池 + 可选 d2d |

## test_transfer_engine.py：基础跨节点传输（push 模式）

### 功能说明

工具包含两种工作角色：

* **Prefill 角色**：负责生成数据并将数据传输到目标节点

* **Decode 角色**：负责接收并处理 Prefill 节点传输的数据

两者各自在本地创建配置存储服务（config store），Prefill 按 Decode 的会话 ID（`ip:port`）直连其 config store，
完成信息交换与建链。

### 使用方法

基本命令格式：

```bash
python test_transfer_engine.py [参数]
```

### 参数说明

| 参数名             | 必选 | 说明                                               |
|-----------------|----|--------------------------------------------------|
| --role          | 是  | 工作角色，可选值：`Prefill` 或 `Decode`                    |
| --src-unique-id | 是  | 当前节点的会话 ID，同时也是本实例内部 config store 的监听地址，格式：`ip:port` |
| --store-url     | 是  | 兼容参数（脚本仍要求传入，可传任意合法 `tcp://ip:port`）：当前版本config store协议固定为tcp且由各实例自行创建，该参数不再参与对端发现 |
| --npu-id        | 否  | NPU 设备 ID，默认值：0                                  |
| --dst-unique-id | 否  | 目标节点会话 ID（仅 Prefill 角色需要），格式：`ip:port`           |
| --log-level     | 否  | 日志级别：0 (debug)、1 (info)、2 (warn)、3 (error)，默认值：0 |

### 运行步骤

#### 1. 启动 Decode 节点

```bash
python test_transfer_engine.py \
    --role Decode \
    --src-unique-id "127.0.0.1:50051" \
    --store-url tcp://127.0.0.1:8000 \
    --npu-id 0 \
    --log-level 1
```

#### 2. 启动 Prefill 节点

```bash
python test_transfer_engine.py \
    --role Prefill \
    --src-unique-id "127.0.0.1:50052" \
    --store-url tcp://127.0.0.1:8000 \
    --npu-id 1 \
    --dst-unique-id "127.0.0.1:50051" \
    --log-level 1
```

### 工作流程说明

1. **初始化阶段**：

    * 两个节点分别启动并通过`engine.initialize()`初始化 TransferEngine，`src-unique-id`即本实例内部config store的监听地址

    * 对端发现不再依赖`store_url`：Prefill 按 Decode 的`src-unique-id`直连其config store，因此 Prefill 节点的`dst-unique-id`必须与
      Decode 节点的`src-unique-id`完全一致（包括端口号）

2. **数据传输阶段**：

    * Decode 节点创建接收缓冲区（大小为`(10, 50, 40, 20, 60)`的 float16 张量）并注册内存地址

    * Prefill 节点生成随机张量数据，注册内存后通过`transfer_sync_write`方法将数据传输到 Decode 节点

    * 数据通过直接内存访问方式传输，避免数据拷贝

### 故障排查

1. **初始化失败**：

    * 确认`src-unique-id`的地址/端口在所有节点可访问（对端会直连该地址的config store，可使用`telnet ip port`验证端口连通性），
      建议使用固定端口而非自动分配端口

    * 确认`src-unique-id`格式正确且未被占用

    * 验证 NPU 设备是否正常工作（可通过`npu-smi info`命令检查）

2. **数据传输失败**：

    * 检查 Prefill 节点的`dst-unique-id`是否与 Decode 节点的`src-unique-id`完全一致

    * 确认两个节点的内存注册成功（日志中包含`register success`）

    * 查看 debug 级日志（`--log-level 0`）获取详细错误信息

## test_transfer_engine_rd2h.py：SFA RD2H PD KV Cache 拉取传输（pull 模式）

### 简介

仿照 vllm-ascend `SfaRemoteD2HConnector` + `SparseKVOffloadManager` 的生产用法，模拟 SFA 场景下
Prefill/Decode 分离部署的 KV Cache 拉取（pull）传输：

* **Prefill 侧**：按 MLA/SFA main cache 布局（bf16）生成每层 KV，仅 `register_memory` 注册源内存，从不主动写数据
* **Decode 侧多 rank**：每 rank 一个进程模拟多卡，共享同一 DRAM offload 池（仅 rank0 分配、其余挂接），池基地址经
  **HCCL broadcast** 同步（等价生产 `tp_group.broadcast(gvas)`，不经 Prefill 中继）
* 各 rank 独立初始化 TransferEngine，按 `tp_block_range` 均分区间拉取各自分片；完成后 rank0 用同 seed 重放期望
  KV 全量校验，并演示 `sparse_copy` 将 decode 新增 KV 直写 CPU 池（D2H）

可选 **d2d 混合介质传输**（对齐生产 SFA indexer/scale 布局）：默认开启 `--enable-hbm` 时，每个 owned 块的
main K/V 自 P 的 HBM 拉取到共享 DRAM 池（rd2h：HBM→DRAM），同批块的 indexer 组件自 P 的 HBM 拉取到本 rank 的
HBM 热区 arena（d2d：HBM→HBM），两类描述符合入同一次 `batch_transfer_sync_read`，一次 batch 同时覆盖两条介质路径。
内存排布对齐生产 `_allocate_kv_cache_tensors`：P 侧 main K/V 每层独立分配、各自 2MiB 对齐，indexer C8 下
K+scale 同一 raw 前后排布（边界仅按 scale_dtype 对齐）；D 侧 arena 每层一段（段基址 2MiB 对齐）逐层独立注册。

设备内存共享名走 IPC 语义仅同机有效，跨机（`prefill-ip != local-ip`）时 HBM 热区可能导入失败；可用
`--no-enable-hbm` 去掉 HBM 热区（仅拉取共享 DRAM 池的 main K/V）做跨机验证，**两侧开关必须一致**。

与 test_transfer_engine.py 的主要差异：

| 维度 | test_transfer_engine.py | test_transfer_engine_rd2h.py |
|------|-------------------------|-------------------------------|
| 传输模式 | push（P 主动写 D） | pull（D 各 rank 主动读 P） |
| 数据布局 | 任意张量 | MLA/SFA main KV cache（k/v 分离 + block table）+ 可选 indexer/scale |
| Decode 并行 | 单进程 | 多 rank（共享池 + HCCL 广播 + 块区间切分） |
| 目的内存 | D 侧注册的接收缓冲 | D 侧共享 offload DRAM 池 + 可选本 rank HBM 热区 |
| 正确性校验 | 传输后比对 | seed 重放逐块全量校验 + D2H 演示 |
| 性能统计 | 无 | warmup/iterations 多轮纯传输带宽（均值/最好） |

默认参数（32 层 × 128 块）下单层 main K+V 为 18 MiB、每轮约 576 MiB；开启 d2d 时 indexer 部分 32 层合计
约 128 MiB/轮；默认 1 轮 warmup + 3 轮测量。

### 主要使用示例

单机双卡快速验证（最常见）：Prefill 占卡 0，Decode 占卡 1，两侧均启用默认的 d2d 混合介质传输。

```bash
# 1) Prefill 侧（store server，承担 config store）
python test_transfer_engine_rd2h.py --role Prefill --decode-tp-size 1 --npu-id 0

# 2) Decode 侧（一条命令拉起全部 rank，rank r 用 --npu-id + r）
python test_transfer_engine_rd2h.py --role Decode --tp-size 1 --npu-id 1
```

多卡 Decode（如 2 rank，卡 1/2）：

```bash
python test_transfer_engine_rd2h.py --role Decode --tp-size 2 --npu-id 1
```

跨机部署或排除 HBM 热区（两侧必须一致加 `--no-enable-hbm`，仅做 rd2h main K/V 到 DRAM 池的拉取）：

```bash
# Prefill 侧
python test_transfer_engine_rd2h.py --role Prefill --decode-tp-size 1 \
    --local-ip <本机可达 IP> --npu-id 0 --no-enable-hbm
# Decode 侧
python test_transfer_engine_rd2h.py --role Decode --tp-size 1 --npu-id 1 \
    --prefill-ip <Prefill 真实 IP> --no-enable-hbm
```

C8 量化 indexer 路径（对齐生产 LIC8，K int8 + scale int8 同 storage）：

```bash
python test_transfer_engine_rd2h.py --role Prefill --decode-tp-size 1 --npu-id 0 --indexer-c8
python test_transfer_engine_rd2h.py --role Decode --tp-size 1 --npu-id 1 --indexer-c8
```

手动单独启动某个 rank 或调整测量轮数：

```bash
python test_transfer_engine_rd2h.py --role Decode --npu-id 1 --tp-rank 1 --tp-size 2                 # 指定 rank
python test_transfer_engine_rd2h.py --role Decode --npu-id 1 --tp-size 2 --warmup 2 --iterations 10   # 调轮数
```

### 参数说明（常用）

| 参数名 | 必选 | 说明 |
|-------|----|------|
| --role | 是 | 工作角色：`Prefill` 或 `Decode` |
| --local-ip | 否 | 本机 IPv4，默认 `127.0.0.1`；跨机须传对端可达的本机网卡 IP |
| --prefill-ip | 否 | Prefill 机器 IPv4，默认 `127.0.0.1`；跨机须传真实 Prefill IP |
| --npu-id | 否 | launcher 模式：Decode 起始卡号（rank r 用 `起始+r`，默认 0）；手动 `--tp-rank` 模式：可选，默认等于 rank |
| --tp-size | 否 | Decode TP rank 数，默认 1 |
| --tp-rank | 否 | Decode TP rank；缺省时一条命令拉起全部 rank 子进程 |
| --decode-tp-size | 否 | 期望的 Decode rank 数（仅 Prefill 侧），默认 1 |
| --num-layers / --num-blocks / --block-size | 否 | KV cache 几何，默认 32 / 128 / 128 |
| --kv-lora-rank / --qk-rope-head-dim | 否 | MLA 维度，默认 512 / 64 |
| --indexer-head-dim | 否 | SFA indexer K 头维度，默认 128 |
| --indexer-c8 | 否 | 开启 int8 K + int8 scale（scale_dim=1）的 LIC8 路径；默认是非量化 bf16 |
| --enable-hbm / --no-enable-hbm | 否 | 是否包含 d2d 拉取 HBM 热区（indexer/scale），默认开启；关闭后退化为纯 rd2h，跨机友好 |
| --warmup / --iterations | 否 | 预热/测量轮数，默认 1 / 3 |
| --seed | 否 | mock 数据随机种子，默认 2026 |
| --timeout | 否 | 控制通道超时（秒），默认 120 |
| --log-level | 否 | 日志级别：0 (debug)、1 (info)、2 (warn)、3 (error)，默认 2 |

约束：`num_layers * 3` 不得超过 256（底层 HCCL 注册 region 上限，每层 main K / main V / indexer 各一段）；
Decode 多 rank 必须在同一台机器上（共享 DRAM 池要求）；`--enable-hbm` 与 `--indexer-c8` 两侧必须一致。

### 运行步骤

单机快速验证两侧均可省略 `--local-ip` 与 `--prefill-ip`；跨机部署两侧都须传对端可达的 `--local-ip`（P 侧地址会
进入 session id 供 D 回连），Decode 侧还须传真实 `--prefill-ip`。

1. **启动 Prefill 侧（store server）**：参考“主要使用示例”
2. **启动 Decode 侧**：参考“主要使用示例”
3. **观察结果**：两侧均输出 `PASS` 即成功，例如：

```text
[P] PASS: layers=32, decode_ranks=1, bytes=0.69 GiB/round, iterations=3,
    transfer_bw=23.81 GiB/s (best 24.35), enable_hbm=True, indexer_c8=False, d2h_demo_ok=True
[D0] PASS: pulled 0.69 GiB/round, total 2.07 GiB in 3 rounds, blocks=[0,128),
    merge ratio 1.00 (12288 pieces), bw=23.81 GiB/s (best 24.35),
    latency build=5.33ms + transfer=27.54ms/round, d2h_demo_ok=True
```

* `bytes/round`：单轮拉取总字节数（各 rank 分片之和，含 d2d 的 indexer/scale 部分）
* `merge ratio`：块表描述符合并比（randperm 下通常为 1.00，即无合并）
* `transfer_bw`：纯传输带宽（仅计时 `batch_transfer_sync_read`，不含描述符构建），多轮取均值，括号内为最好一轮
* 校验失败以 `verification failed` 报错退出，不会输出 PASS

### 工作流程说明

控制协议（P 与每个 D rank 之间的 TCP JSON 通道）：

```text
hello → init → pool_ready → meta → pull_done(各轮) → verify → final
```

1. **握手**：各 D rank 连接 P 并上报 tp_rank；P 校验 rank 集合完整
2. **共享池建立**：P 下发 `init`；各 rank 初始化 offload 共享池（内部 GroupBarrier 会合），rank0 分配 CPU 池张量，
   基地址经 HCCL broadcast 同步到所有 rank；`--enable-hbm` 时各 rank 同时分配本 rank HBM 热区 arena（pull 路径本地
   目的内存，无需上报 P）；随后 rank0 向 P 上报 `pool_ready`
3. **元数据下发**：P 将每层 K/V/indexer/scale 基地址、块长、非连续 block table（seed 确定的 randperm）、
   `indexer_c8`/`enable_hbm` 等打包为 `meta` 广播下发
4. **分片拉取**：各 rank 按 `tp_block_range` 划分的块区间构造读描述符（main K/V 落共享 DRAM 池的 `dst_idx`，
   indexer/scale 落本 rank HBM 热区的区内偏移 `dst_idx-start`，连续块自动合并），调用
   `batch_transfer_sync_read` 从 P 的 HBM 拉入对应目的内存；warmup 轮静默执行，测量轮分段计时
5. **全量校验**：所有轮完成后各 rank 先 `torch.npu.synchronize()` 再用相同 seed 重放的期望 indexer/scale 对 HBM 热区
   逐层 `torch.equal` 比对（`verify_hbm_slice`，`--no-enable-hbm` 时跳过）；随后 rank0 对整个共享池 main K/V 逐层全量校验，
   并运行 `sparse_copy` D2H 演示（失败仅告警）
6. **收尾**：P 汇总各 rank 各轮纯传输用时（按轮取最慢 rank），计算均值/最好带宽并下发 `final`；D 侧反注册 CPU/HBM
   目的区后释放 arena 与共享池，上报 `cleanup_done`

## test_transfer_engine_d2rh_shared.py：SFA D2RH PD KV Cache 推送传输（push 模式）

### 简介

对齐 sglang AscendTransferEngine 的 push 用法（`batch_transfer_sync_write`）：Prefill 侧生成每层 KV cache
（HBM bf16）并 `register_memory` 注册源内存，D 侧各 rank 建 HCCL 组后以 SHARED 场景初始化共享 DRAM offload 池
（仅 rank0 分配物理 DRAM，store url 与池基地址经 HCCL broadcast 同步，与 rd2h 一致），SHARED 池全组同 VA，各 rank
将广播基址的目的区 `register_memory` 到本 rank 引擎后上报 session；P 按 `tp_block_range` 均分的块区间逐 rank 推送
分片写入共享池，rank0 全池校验并演示 `sparse_copy` D2H。

支持与 rd2h 对称的 **d2d 混合介质传输**：`--enable-hbm`（默认开启）时，每个 owned 块的 main K/V 推送至共享 DRAM
池（HBM→DRAM），同批块的 indexer 组件推送至本 rank 的 HBM 热区 arena（HBM→HBM），两类描述符合入同一次
`batch_transfer_sync_write`；`--no-enable-hbm` 时仅推送 main K/V 到共享池，跨机友好。indexer 同样支持非量化
（默认）与 C8（`--indexer-c8`）两条路径，两侧开关必须一致。

### 主要使用示例

单机双卡快速验证（与 rd2h 对称，最常见）：

```bash
# 1) Prefill 侧（按 D 的 session 主动 push）
python test_transfer_engine_d2rh_shared.py --role Prefill --decode-tp-size 1 --npu-id 0

# 2) Decode 侧（一条命令拉起全部 rank，rank r 用 --npu-id + r）
python test_transfer_engine_d2rh_shared.py --role Decode --tp-size 1 --npu-id 1
```

多卡 Decode、C8 量化 indexer、跨机关闭 d2d 等用法与 rd2h 完全对称，把脚本名换成
`test_transfer_engine_d2rh_shared.py` 即可，例如：

```bash
# 跨机或排除 HBM 热区（两侧都加 --no-enable-hbm）
python test_transfer_engine_d2rh_shared.py --role Prefill --decode-tp-size 1 \
    --local-ip <本机可达 IP> --npu-id 0 --no-enable-hbm
python test_transfer_engine_d2rh_shared.py --role Decode --tp-size 1 --npu-id 1 \
    --prefill-ip <Prefill 真实 IP> --no-enable-hbm
```

### 参数说明

参数集与 `test_transfer_engine_rd2h.py` 完全一致（`--role / --local-ip / --prefill-ip / --npu-id /
--tp-size / --tp-rank / --decode-tp-size / --num-layers / --num-blocks / --block-size /
--kv-lora-rank / --qk-rope-head-dim / --indexer-head-dim / --indexer-c8 / --enable-hbm /
--warmup / --iterations / --seed / --dram-pool-gb / --timeout / --log-level`），语义相同，详见上一节。
约束：`num_layers * 3` 不得超过 256；Decode 多 rank 必须同机；`--enable-hbm` 与 `--indexer-c8` 两侧必须一致。

### 工作流程说明

控制协议与 rd2h 对称（P 与每个 D rank 之间的 TCP JSON 通道）：

```text
hello → init → pool_ready → meta → push_done(各轮) → verify → final
```

1. **握手与共享池建立**：与 rd2h 一致；`--enable-hbm` 时各 rank 同时分配本 rank HBM 热区 arena 并将基地址
   随 `pool_ready` 一并上报 P（push 路径：P 需知 D 的目的内存地址才能写入）
2. **元数据下发**：P 将每层 K/V/indexer/scale 基地址、块长、block table、`indexer_c8`/`enable_hbm` 以及 D 侧上报的
   `cpu_bases`/`hbm_bases` 打包为 `meta`（自产自用，不下发 D）
3. **分片推送**：P 按 `tp_block_range` 划分的块区间为每个 D rank 构造写描述符（main K/V 写 D 共享池 `dst_idx`，
   indexer/scale 写 D HBM 热区区内偏移 `dst_idx-start`，连续块自动合并），调用
   `batch_transfer_sync_write` 一次 batch 同时覆盖 HBM→远端 HBM 与 HBM→DRAM
4. **全量校验**：P 推送完所有 rank 后通知 D 校验；各 rank `torch.npu.synchronize()` 后对 HBM 热区 indexer/scale
   逐层 `torch.equal` 比对（`--no-enable-hbm` 时跳过），rank0 对共享池 main K/V 全量校验并运行 D2H 演示
5. **收尾**：P 汇总各 rank 各轮纯传输用时并下发 `final`；D 侧反注册目的区后释放 arena 与共享池

### rd2h 与 d2rh_shared 的选择

| 维度 | test_transfer_engine_rd2h.py（pull） | test_transfer_engine_d2rh_shared.py（push） |
|------|--------------------------------------|---------------------------------------------|
| 触发方 | D 各 rank 主动从 P 读 | P 主动向各 D rank 写 |
| store server | P 侧承担 config store | D 各 rank 承担 config store |
| D 侧目的地址同步 | 本 rank 本地已知，无需上报 P | `cpu_bases`/`hbm_bases` 随 `pool_ready` 上报 P |
| 控制消息 | `pull_done` | `push_done` |
| 适用场景 | 对齐 vllm-ascend `SfaRemoteD2HConnector` | 对齐 sglang AscendTransferEngine |

两者 KV 几何、indexer 路径、d2d 开关、校验流程完全对称，便于对照验证 pull/push 两种实现的一致性。
