# ConfigStore集群管理原理解析及基于etcd的高可用方案

---

[TOC]

---

## 0. 概述

Memfabric 实现了一个基于 **中心化 KV 存储（Key-Value Store）**
的分布式协调系统。整个系统的核心在于一个中心服务节点（Server），它维护着全局的内存状态数据；所有的客户端节点（Client）通过 TCP
连接到 Server，利用 KV 存储的原子操作（如 Set, Get, Add, Cas）和发布订阅机制（Watch）来实现集群成员管理、状态同步和集合通信（Barrier,
AllGather）。

## 1. 核心架构与分层设计

系统采用双层架构设计：

1. **底层通信与存储层 (`src/smem/csrc/config_store`)**：
    * 负责 TCP 连接生命周期管理、消息序列化（`SmemMessagePacker`）、以及核心的内存 KV 存储逻辑。
    * **Server 端**：由 `AccStoreServer` 实现，负责数据存储和事件分发。
    * **Client 端**：由 `TcpConfigStore` 实现，提供同步阻塞式的 RPC 接口。

2. **上层群组逻辑层 (`src/smem/csrc/net`)**：
    * 由 `SmemNetGroupEngine` 实现。
    * 它不关心网络细节，而是将底层的 KV 原语封装成具体的分布式语义（如 Join/Leave、Barrier、AllGather）。

---

## 2. 核心组件源码解析

### 2.1 服务端核心：`smem_tcp_config_store_server.cpp`

`AccStoreServer` 是整个集群的“大脑”，其实现并非简单的单线程模型，而是采用了 **多线程 + 细粒度锁 + 条件变量** 的设计。

#### A. 核心数据结构与并发控制

所有状态均由互斥锁 `storeMutex_` 保护，保证了操作的强原子性：

* **`kvStore_`**: `unordered_map<string, vector<uint8_t>>`，存储所有配置和状态数据。
* **`aliveRankSet_`**: 记录当前 TCP 连接正常的 Rank ID 集合，用于快速判断节点存活。
* **`waitCtx_` & `keyWaiters_`**: 实现“阻塞读（Long Polling）”的关键结构。
    * `waitCtx_`: 存储挂起的请求上下文（`StoreWaitContext`）。
    * `keyWaiters_`: 建立 `Key -> Set<ContextID>` 的索引，当 Key 被修改时快速唤醒等待者。

#### B. 阻塞读与唤醒机制 (The Blocking Get)

这是实现低延迟 Barrier 的核心机制：

1. **挂起 (GetHandler)**: 当 Client 发起 `GET` 请求且 Key 不存在，若请求携带 `timeoutMs > 0`，Server **不会立即返回**
   `NOT_EXIST`，而是构建 `StoreWaitContext` 放入 `waitCtx_`，暂时挂起该 TCP 请求。
2. **唤醒 (Set/AddHandler)**: 当有 Client 对 Key 执行写操作时，Server 更新 `kvStore_` 后，立即检查 `keyWaiters_`，调用
   `WakeupWaiters` 唤醒所有等待该 Key 的 Context，发送回包并移除上下文。

#### C. 故障广播机制 (Rank State Watch)

不同于普通 Key 的长轮询，这是一个真正的发布订阅模型：

* **触发**: `LinkBrokenHandler` 检测到 TCP 断开，将断开的 Rank ID 推入 `rankStateTaskQueue_`。
* **广播**: 辅助线程 `RankStateTask` 被唤醒，遍历 `rankStateWaiters_`（监听 `WATCH_RANK_STATE` 的 Client），主动推送
  `WATCH_RANK_LINK_DOWN` 消息。

### 2.2 群组引擎核心：`smem_net_group_engine.cpp`

`SmemNetGroupEngine` 运行在 Client 端，通过操作 Server 上的 Key 来实现分布式算法。

#### A. 动态组元数据管理

* **Key**: `SMEM_GROUP_DYNAMIC_SIZE_KEY` ("DSIZE")。
* **Value**: 64位整数，**高32位是 Group Version，低32位是 Rank Size**。
* **原子性**: 利用 `SplitSizeAndVersion` 和 `MergeSizeAndVersion` 辅助函数，配合 Server 的 `Add/Cas`
  操作，确保了“组版本号”和“组大小”的原子更新。任何成员变动都会导致 Version 自增，从而隔离不同拓扑下的 Barrier 操作。

#### B. 集合通信原语实现

* **Barrier (栅栏同步)**:
    1. **报数**: Client 调用 `store_->Add(key_BA, 1)`。
    2. **触发**: 最后一个到达者（返回值 == `rankSize`）执行 `store_->Set(key_BW, "ok")`。
    3. **等待**: 其他节点通过 `store_->Get(key_BW)` 挂起在 Server 端，直到被触发者唤醒。
    4. **GC**: 引入 `REMOVE_INTERVAL` (2)，在执行第 N 次 Barrier 时，异步删除第 N-2 次的 Key，防止 Server 内存泄漏。
* **AllGather (全收集)**:
    1. **上传**: 利用 `store_->Append` 将数据追加到 Server 的 Buffer。
    2. **排序**: 由于 Append 顺序不确定，Client 在发送数据前会在头部塞入 `rankId`。接收端 `SortGatherRecv` 解析头部
       ID，在本地内存重排数据。

---

## 3. 关键工作流程

### 3.1 连接建立与 Rank ID 分配

系统支持两种模式，完美兼容了首次启动和断链重连：

* **首次分配模式**: Client 发送 `rankId = -1`。Server 在 `LinkConnectedHandler` 中遍历寻找未使用的 Index，分配后存入
  `kvStore_` 的 `AutoRanking#<linkId>` 映射中。
* **断链汇报模式**: Client 重连时在 TCP 握手包中携带旧 `rankId`。Server 识别后直接将其加入 `aliveRankSet_`，**不重新分配**
  。这保证了业务层 Rank ID 的持久性。

### 3.2 集群加入 (Join)

1. **监听**: Client 后台线程 `GroupListenEvent` 持续 Watch `EVENT` Key。
2. **发起**: 新节点 CAS 修改 `EVENT` 为 `"J" + rankId`，并原子增加 `DSIZE`。
3. **同步**: Server 推送 Watch 事件，全网节点更新本地 `rankSize` 并触发回调（segment 的import。rdma 的import）。

### 3.3 故障检测与自愈 (Failover)

Server 充当被动故障检测器：

1. **感知**: Server 检测到 TCP 断开 (`LinkBrokenHandler`)，从 `aliveRankSet_` 移除节点。
2. **广播**: Server 向剩余节点推送 `WATCH_RANK_LINK_DOWN` 事件。
3. **自愈**: Client 收到通知后执行 `RankLinkDownEventProcess`。所有存活节点尝试 **CAS 修改 `DSIZE`**（减少 Size，增加
   Version）。CAS 保证了只有一个节点能成功修改 Server 状态，从而“踢出”死节点，解除因死节点导致的 Barrier 阻塞。

---

## 4. 基于 etcd 的可靠性提升方案

当前架构存在 **Server 单点故障 (SPOF)** 风险。Server 宕机将导致整个集群元数据丢失且无法恢复。以下方案在不修改核心业务逻辑（Barrier/AllGather
仍跑在内存中以保证高性能）的前提下，利用 etcd 实现高可用。

### 4.1 核心设计：P2P 对等架构

取消专用的 Server 节点，所有节点对等。通过 etcd 选主，任何节点均可晋升为 Server。

### 4.2 etcd 数据结构设计

仅持久化关键元数据，业务临时数据不持久化：

* `/smem/meta/leader`: `ip:port` (带 Lease)，用于服务发现。
* `/smem/lock/election`: 分布式锁，用于选主。
* `/smem/data/max_rank`: `int32`，**持久化**。对应 Server 的 `rankIndex_`，防止 Rank ID 重复分配。
* `/smem/data/world_size`: `int32`，**持久化**。对应 Server 的 `world_size_`，防止 Rank ID 分配错误。
* `/smem/data/group_ver`: `int64`，**持久化**。对应 `DSIZE` 的高位，保证组版本单调递增。
* `/smem/data/alive_rank_list`: `str`，**持久化**。服务和部分client同时挂掉场景。

### 4.3 统一状态机 (The Unified Loop)

所有节点运行同一套逻辑：

1. **探测阶段**: 读取 `/smem/meta/leader`。若有主且可连，作为 **Client** 接入（执行 `TcpConfigStore` 逻辑）。
2. **竞选阶段**: 若无主或连接失败，尝试获取 etcd 锁 `/smem/lock/election`。
3. **晋升阶段**:
    * 获锁成功后，从 etcd 加载 `max_rank` 和 `group_ver`和 `world_size`。
    * 启动 `AccStoreServer`，将恢复的数据注入内存。
    * 写入 `/smem/meta/leader` 并启动 Lease KeepAlive。
    * 身份切换为 **Server**。

### 4.4 状态重建与脑裂防护 (State Rebuild & Fencing)

新 Server 启动时内存为空，需通过以下机制恢复：

1. **Rank 映射重建**: 利用现有的“断链汇报模式”，Client 重连时上报 Rank ID，Server 自动重建 `aliveRankSet_`。
2. **双server**:
    * *问题*: 老server因为临时的网络问题，丢失leader。就会出现2个server。
    * *解决*: `group_ver` 每次连接检查。

### 4.5 方案优势

* **完全对等**: 无需部署额外组件，无单点故障。
* **高性能**: etcd 仅参与选主和低频元数据持久化，高频 Barrier/AllGather 依然在 Server 内存中以微秒级延迟完成。
* **兼容性**: 最大程度复用了现有的 `AccStoreServer` 和 `SmemNetGroupEngine` 代码逻辑。

## 5.问题

- 有没有必要引入client和etcd的长连接watch （ 开销大，不考虑）

- server和etcd 断开，其他连接正常。租约过期。leader会被etcd删除。这时候启动一个新节点，会竞争一个新server。而且老client还不知道。（server超时降级，自己杀掉server）
  套接字）

- client本地维护所有在线rank，server也有记录，多份存储，是否能有一致性的试图 （用etcd全局alive为准）
