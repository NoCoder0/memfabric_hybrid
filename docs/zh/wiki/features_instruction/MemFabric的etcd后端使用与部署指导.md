# MemFabric 的 etcd 后端使用与部署指导

[TOC]

## 一、模块定位：ConfigStore 与 etcd 后端

MemFabric 的多个进程要组成一个"内存池集群"，彼此需要知道对方的地址、各自贡献了多少内存、谁是管理节点等信息。这些集群元数据统一存放在一个叫做 **ConfigStore（配置存储）** 的模块中。可以把它理解为整个集群的"通讯录"和"公告板"：新成员加入要先来这里登记，成员状态变化也通过它同步给所有人。

ConfigStore 有两种工作形态，区别在于底层存储放在哪里：

| 形态 | 地址格式 | 说明 |
|---|---|---|
| **TCP 模式（默认）** | `tcp://ip:port` | 集群中某一个进程兼任 store server，其余进程都连它。部署最简单，但该进程故障后集群控制面瘫痪，且元数据不持久，server 重启后全员需重新注册 |
| **etcd 模式** | `etcd://ip:port` | 元数据存放在独立的 etcd 服务中。控制面不再依赖任何 MF 进程存活，元数据持久化，并自动获得基于 etcd 的多活（HA）能力，生产环境推荐 |

选择哪种形态**不需要改任何代码**，只取决于传入的地址格式：把地址写成 `etcd://...`，MemFabric 就会自动使用 etcd 后端。本文指导如何完成 etcd 模式从零到可用的完整部署。

> 简单理解 etcd 模式下的协作关系：etcd 负责"存元数据 + 选出主节点"，被选为主节点的 MF 进程负责管理集群，其余进程通过它同步数据。整个过程对使用者透明。

## 二、环境准备

| 组件 | 要求 | 用途 |
|---|---|---|
| etcd 服务 | 3.5 及以上版本 | 存储集群元数据（安装见第三章） |
| Go 工具链 | 1.24 及以上 | 仅**源码出包**时需要，用于编译 etcd 客户端库；直接使用官方安装包可不装 |
| MemFabric 安装包 | run 包或 wheel 包 | 需确认包内包含 etcd 后端（见 4.1） |

> 提示：etcd 是一个独立的开源软件，与 MemFabric 分开安装。可以把它部署在任何 MF 节点可网络访问的机器上。

## 三、部署安装

整体分两步：先部署 etcd 服务端，再安装带 etcd 后端的 MemFabric。

### 3.1 部署 etcd 服务端

#### 单节点（测试/验证环境）

以 openEuler 系为例安装并启动（Ubuntu 使用 `apt install etcd-server`）：

```bash
yum install etcd.aarch64

nohup etcd \
  --name test-etcd \
  --data-dir /tmp/etcd-data \
  --listen-client-urls http://<本机IP>:12335 \
  --advertise-client-urls http://127.0.0.1:12335 \
  --listen-peer-urls http://<本机IP>:12336 \
  --initial-advertise-peer-urls http://127.0.0.1:12336 \
  --initial-cluster test-etcd=http://127.0.0.1:12336 \
  --initial-cluster-token tkn1 \
  --initial-cluster-state new &
```

> 示例使用 12335/12336 端口（etcd 标准端口为 2379 客户端 / 2380 节点间通信）。后文 MF 侧地址的端口必须与 `listen-client-urls` 一致。

验证 etcd 服务可用：

```bash
etcdctl --endpoints=http://127.0.0.1:12335 put k v    # 应输出 OK
etcdctl --endpoints=http://127.0.0.1:12335 get k      # 应输出 v
```

#### 多节点集群（生产环境）

生产环境应部署**奇数个（建议 3 个及以上）etcd 节点**组成集群，任一节点故障不影响服务。在三台机器上分别执行（替换各机 IP 与名称 etcd-1/2/3）：

```bash
nohup etcd \
  --name etcd-1 \
  --data-dir /data/etcd \
  --listen-client-urls http://<本机IP>:2379 \
  --advertise-client-urls http://<本机IP>:2379 \
  --listen-peer-urls http://<本机IP>:2380 \
  --initial-advertise-peer-urls http://<本机IP>:2380 \
  --initial-cluster etcd-1=http://<IP1>:2380,etcd-2=http://<IP2>:2380,etcd-3=http://<IP3>:2380 \
  --initial-cluster-token mf-etcd-cluster \
  --initial-cluster-state new &
```

MF 侧的地址填写任意一个存活节点即可，客户端会自动发现整个集群。

> 运维建议：`--data-dir` 放独立磁盘分区；对 etcd 数据量做监控（默认配额 2GB）；通过防火墙限制 2379 端口的访问来源——MemFabric 当前不支持 etcd 的用户名密码认证，需靠网络隔离保证安全。

#### MVCC 存储与历史版本管理配置

etcd 采用 MVCC（多版本并发控制）机制，每次写入都会保留历史版本而非直接覆盖。为防止历史版本无限累积导致磁盘空间耗尽（触发 `database space exceeded` 只读告警），建议在启动参数中配置自动压缩与合理的存储配额：

```bash
nohup etcd \
--name test-etcd \
--data-dir /tmp/etcd-data \
--listen-client-urls http://<本机IP>:12335 \
--advertise-client-urls http://127.0.0.1:12335 \
--listen-peer-urls http://<本机IP>:12336 \
--initial-advertise-peer-urls http://127.0.0.1:12336 \
--initial-cluster test-etcd=http://127.0.0.1:12336 \
--initial-cluster-token tkn1 \
--initial-cluster-state new \
--auto-compaction-mode periodic \
--auto-compaction-retention 1h \
--quota-backend-bytes 2147483648 &
```

MVCC 关键参数说明：

```bash
--auto-compaction-mode 与 --auto-compaction-retention：开启自动压缩，periodic 模式下 1h 表示仅保留最近 1 小时的历史版本，更早的版本将被自动清理。
--quota-backend-bytes：设置底层 BoltDB 的存储配额上限。默认值为 2GB（2147483648 字节），生产环境建议根据实际磁盘容量调大，但官方强烈建议不超过 8GB（8589934592 字节），否则会导致性能严重下降。
```

验证 MVCC 自动压缩与配额配置是否生效：

```bash
# 查看 etcd 状态及当前 DB 占用大小，确认未超出配额限制
etcdctl --endpoints=http://127.0.0.1:12335 endpoint status --write-out=table
# 手动触发压缩（测试自动压缩策略前，可先手动清理历史版本）
etcdctl --endpoints=http://127.0.0.1:12335 compact $(etcdctl endpoint status --write-out=json | jq -r '.[0].Status.header.revision')
```

### 3.2 安装带 etcd 后端的 MemFabric

MemFabric 的 etcd 客户端以一个独立动态库 `libetcd_client_v3.so` 的形式交付，**官方默认安装包中不包含它**，需要出包时显式开启，或单独获取该库。

#### 方式一：源码出包时开启（推荐）

构建命令增加 `--build_etcd_backend ON` 参数：

```bash
bash script/build_and_pack_run.sh --build_etcd_backend ON
```

该参数会自动完成：编译 etcd 客户端库（需要 Go 环境）→ 打入 run 包和 Python wheel 包。典型组合示例：

```bash
# 纯 CPU 环境 + etcd 后端
bash script/build_and_pack_run.sh --xpu_type NONE --build_hcom ON --build_etcd_backend ON
```

安装 run 包并加载环境：

```bash
bash memfabric_hybrid-*.run                # 默认装到 /usr/local/memfabric_hybrid
source /usr/local/memfabric_hybrid/set_env.sh
```

安装后确认库存在：

```bash
ls /usr/local/memfabric_hybrid/latest/*/lib64/libetcd_client_v3.so
```

#### 方式二：pip 安装官方 wheel 后手动补充该库

已通过 `pip install memfabric_hybrid` 安装、但包内没有 etcd 库时，可在有 Go 环境的机器上单独编译并拷贝：

```bash
cd memfabric_hybrid/src/util/etcd_client/etcd_store_backend
go mod tidy
go build -o libetcd_client_v3.so -buildmode=c-shared etcd_client_v3.go
```

将生成的 so 拷贝到 wheel 包的 lib 目录（`memfabric_hybrid/lib/`，可用 `python -c "import memfabric_hybrid; print(memfabric_hybrid.get_lib_path())"` 定位），Python 侧会在 import 时自动加载它。

> 注意：请使用与所装版本匹配的源码编译，旧版 so 会因缺少符号而加载失败。

## 四、使用方法

### 4.1 使能方式：把地址写成 etcd://

使用 etcd 后端不需要开关或配置文件，**集群地址写成 `etcd://` 格式即可**。地址格式如下：

```text
etcd://<ip>:<port>                基本格式
etcd://<ip>:<port>#<实例ID>        可选：多集群隔离（见第五章）
```

### 4.2 Python 使用示例

```python
import memfabric_hybrid as mf

# 方式一：显式创建 etcd 配置存储
mf.create_config_store("etcd://10.1.2.3:2379")

# 方式二：初始化 BM/Trans/SHM 时直接传 etcd 地址
mf.initialize("etcd://10.1.2.3:2379", world_size, device_id, config)
```

### 4.3 C API 使用示例

```c
// 显式创建 etcd 配置存储
smem_create_config_store("etcd://10.1.2.3:2379", 0);

// 或在各模块初始化接口的 storeUrl 参数中传入 etcd 地址
// 例如：smem_trans_create("etcd://10.1.2.3:2379", uniqueId, &config);
```

### 4.4 环境变量方式

部分场景（如 `launch_ascend_mf_store` 工具）通过环境变量指定地址：

```bash
export MF_CONFIG_STORE_URL=etcd://10.1.2.3:2379
```

### 4.5 验证接入成功

1. **看 MF 日志**，出现如下两行说明连接建立成功：

   ```text
   [ETCD] Initializing connection: http://10.1.2.3:2379
   [ETCD] Connection initialized successfully, endPoint: http://10.1.2.3:2379
   ```

2. **看 etcd 数据**，MF 集群运行后可查询到元数据键：

   ```bash
   etcdctl --endpoints=http://10.1.2.3:2379 get "" --prefix
   ```

3. **跑自带测试工具**（`--build_test ON` 构建时产出 `etcd_backend_test`）：

   ```bash
   ./etcd_backend_test etcd://127.0.0.1:2379 4   # 参数：etcd地址 线程数
   ```

   预期输出每个线程 `SUCCESS! Found X items.`。

## 五、进阶用法

### 5.1 多集群隔离（#实例ID）

当多套 MemFabric 集群需要共用同一个 etcd 服务时，在地址末尾追加 `#实例ID` 即可实现键空间隔离，两套集群的元数据互不可见、互不干扰：

```bash
etcd://10.1.2.3:2379#cluster-a    # 集群 A 全体成员使用
etcd://10.1.2.3:2379#cluster-b    # 集群 B 全体成员使用
```

实例 ID 仅允许字母、数字、下划线和连字符。

### 5.2 自动获得 HA 能力

etcd 模式下 MemFabric 会自动基于 etcd 的分布式锁在多个管理节点间选主，主节点故障后其余节点自动接管，无需额外配置。需要保证所有参与选主的 MF 进程使用**同一个 etcd 地址（含相同的实例 ID）**。

### 5.3 使用须知

- 集群中**所有成员必须使用相同的 etcd 地址**，否则无法互相发现；
- 同一个进程内只能连接一套 etcd，先后传入不同地址不会重建连接；
- 一个进程同时只能加入一个由该 etcd 管理的集群。

## 六、常见问题

| 现象（日志关键字） | 原因 | 处理方法 |
|---|---|---|
| `Failed to open library [libetcd_client_v3.so]` | 安装包未包含 etcd 后端，或库不在搜索路径 | 按 3.2 重新出包/补充该库；run 包方式安装后先 `source set_env.sh` |
| `Failed to load symbol Etcd_LockNamed` | 部署了旧版本的 etcd 客户端库 | 用匹配版本的源码重新编译 so |
| `[ETCD] Failed to init etcd client` | etcd 服务未启动、地址端口错误或网络不通 | 先用 `etcdctl put/get` 验证服务端，再核对 MF 地址与 etcd 的 `listen-client-urls` 一致 |
| `Invalid store url: cluster fragment is only supported for etcd/reg` | 给 `tcp://` 地址追加了 `#实例ID` | 去掉 `#` 部分，或改用 `etcd://` |
| `Invalid store url: cluster id contains unsupported characters` | 实例 ID 含非法字符 | 仅使用字母、数字、下划线、连字符 |
| `Client already initialized with different endpoints` | 同进程传入了不同的 etcd 地址 | 进程级只允许一套 etcd 地址，属预期告警，沿用首次连接 |
| 集群成员互相发现失败 | 各成员地址不一致（IP/端口/实例 ID 任一不同） | 统一所有成员的 storeUrl |

## 七、相关阅读

- [MemFabric 内存管理](MemFabric内存管理.md)
- [ConfigStore 集群管理与 etcd 高可用](ConfigStore集群管理原理解析及基于etcd的高可用方案.md)
- [软件安装](https://gitcode.com/Ascend/memfabric_hybrid/blob/master/docs/zh/installation.md)（run 包/whl 包安装与权限说明）
- [ETCD 存储后端](https://gitcode.com/Ascend/memfabric_hybrid/blob/master/docs/zh/etcd_store_backend.md)（etcd 编译安装细节）
