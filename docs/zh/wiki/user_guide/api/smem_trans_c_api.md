# Transfer（TRANS）

## 公共接口列表

### 1. 服务初始化/退出

#### smem_init

初始化运行环境。

```c
int32_t smem_init(uint32_t flags);
```

|参数/返回值 |含义 |
|-|-|
|flags |预留参数 |
|返回值|成功返回0，其他为错误码 |

#### smem_uninit

退出运行环境。

```c
void smem_uninit();
```

### 2. 创建config store对象

#### smem_create_config_store

```c
int32_t smem_create_config_store(const char *storeUrl, uint64_t flags);
```

| 参数/返回值   | 含义                                                                                   |
|----------|--------------------------------------------------------------------------------------|
| storeUrl | 业务面地址，格式支持 `tcp://ip:port`、`etcd://ip:port`、`etcd://ip:port#instanceId`（etcd 多集群隔离）；如 `tcp://[::1]:5124`、`tcp://127.0.0.1:5124`、`etcd://127.0.0.1:5124#clusterA` |
| flags    | 创建标记位，可选标记 SMEM_STORE_SKIP_RECOVER                                                 |
| 返回值      | 成功返回0，其他为错误码                                                                         |

### 3. 日志设置

#### smem_set_extern_logger

设置自定义日志函数。

```c
int32_t smem_set_extern_logger(void (*func)(int level, const char *msg));
```

|参数/返回值|含义|
|-|-|
|func|函数指针|
|level|日志级别，0-debug 1-info 2-warn 3-error|
|msg|日志内容|
|返回值|成功返回0，其他为错误码|

#### smem_set_log_level

设置日志打印级别。

```c
int32_t smem_set_log_level(int level);
```

|参数/返回值|含义|
|-|-|
|level|日志级别，0-debug 1-info 2-warn 3-error|
|返回值|成功返回0，其他为错误码|

### 4. 安全证书设置

#### smem_set_conf_store_tls

安装证书设置。

```c
int32_t smem_set_conf_store_tls(bool enable, const char *tls_info, const uint32_t tls_info_len);
```

|参数/返回值|含义|
|-|-|
|enable|whether to enable tls（当前为预留接口，功能尚未实现）|
|tls_info|the format describle in memfabric SECURITYNOTE.md, if disabled tls_info won't be use|
|tls_info_len|length of tls_info, if disabled tls_info_len won't be use|
|返回值|成功返回0，其他为错误码|

#### smem_set_config_store_tls_key

设置私钥、口令和解密的函数，在开启TLS时，需要调用该接口。

```c
int32_t smem_set_config_store_tls_key(
    const char *tls_pk,
    const uint32_t tls_pk_len,
    const char *tls_pk_pw,
    const uint32_t tls_pk_pw_len,
    const smem_decrypt_handler h
);
```

|参数/返回值| 含义     |
|-|--------|
|tls_pk| 密钥内容   |
|tls_pk_len| 密钥内容长度 |
|tls_pk_pw| 口令内容   |
|tls_pk_pw_len| 口令内容长度 |
|h| 密钥解密函数 |
|返回值| 成功返回0，其他为错误码 |

```c
typedef int (*smem_decrypt_handler)(const char *cipherText, size_t cipherTextLen, char *plainText, size_t &plainTextLen);
```

|参数/返回值|含义|
|-|-|
|cipherText|密文（加密的用来加密私钥的密码）|
|cipherTextLen|密文的长度|
|plainText|解密后的密码（出参）|
|plainTextLen|解密后的密码长度（出参）|
|返回值|错误信息|

### 5. 错误信息获取/清理

#### smem_get_last_err_msg

获取最后一条错误信息。

```c
const char *smem_get_last_err_msg();
```

|参数/返回值|含义|
|-|-|
|返回值|错误信息|

#### smem_get_and_clear_last_err_msg

获取最后一条错误信息并清空所有错误信息。

```c
const char *smem_get_and_clear_last_err_msg();
```

|参数/返回值|含义|
|-|-|
|返回值|错误信息|

### 6. 错误码定义

| 错误码宏 | 值 | 说明 |
|---------|----|------|
| `SMEM_OK` | 0 | 成功 |
| `SMEM_ERROR` | -1 | 通用错误 |
| `SMEM_INVALID_PARAM` | -2000 | 无效参数 |
| `SMEM_MALLOC_FAILED` | -2001 | 内存分配失败 |
| `SMEM_NO_RESOURCES` | -2002 | 资源不足 |
| `SMEM_NOT_STARTED` | -2003 | 服务未启动 |
| `SMEM_TIMEOUT` | -2004 | 操作超时 |
| `SMEM_REPEAT_INVOKE` | -2005 | 重复调用 |
| `SMEM_DUPLICATED` | -2006 | 重复创建 |
| `SMEM_NOT_EXIST` | -2007 | 对象不存在 |
| `SMEM_NOT_INIT` | -2008 | 未初始化 |
| `SMEM_RES_IN_USE` | -2009 | 资源被占用 |
| `SMEM_PARTIAL_FAILED` | -2012 | 部分失败（批量操作） |
| `SMEM_NOT_CONNECTED` | -2014 | 连接断开 |

Store Backend 错误码（从 `smem_def.h`）：

| 错误码宏 | 值 | 说明 |
|---------|----|------|
| `SMEM_STORE_BACKEND_CODE_OK` | 0 | 后端操作成功 |
| `SMEM_STORE_BACKEND_CODE_INTERNAL` | -3000 | 后端内部错误 |
| `SMEM_STORE_BACKEND_CODE_INVAL` | -3001 | 后端参数无效 |
| `SMEM_STORE_BACKEND_CODE_BUFEX` | -3002 | 后端缓冲区不足 |
| `SMEM_STORE_BACKEND_CODE_PERM` | -3003 | 后端权限不足 |
| `SMEM_STORE_BACKEND_CODE_NORES` | -3004 | 后端资源不足 |
| `SMEM_STORE_BACKEND_CODE_NOENT` | -3005 | 后端条目不存在 |
| `SMEM_STORE_BACKEND_CODE_LOCKED` | -3006 | 后端锁冲突 |
| `SMEM_STORE_BACKEND_CODE_UNLOCKED` | -3007 | 后端未锁定 |

> 注：以上错误码定义位于 `smem.h`（通用错误码）和 `smem_def.h`（Store Backend 错误码）头文件中。

### 7. Fabric Host内存分配/释放

#### smem_mem_alloc

分配 A3 上可由 CPU 访问的 Fabric Host内存，接口定义在 `smem.h` 中。

```c
int32_t smem_mem_alloc(uint16_t deviceId, uint64_t size, uint32_t flags, void **address);
```

|参数/返回值|含义|
|-|-|
|deviceId|设备ID，调用线程须已建立该设备的 ACL context|
|size|可用内存大小，单位为字节，须大于0；内部物理申请按 GiB 向上对齐|
|flags|预留参数，须为0|
|address|输出内存地址，分配失败时为 NULL|
|返回值|成功返回0，其他为错误码|

此接口不创建 BM 通信组；远端访问前须通过 `smem_trans_register_mem` 注册返回的内存范围。

#### smem_mem_free

释放 `smem_mem_alloc` 分配的 Fabric Host内存。

```c
int32_t smem_mem_free(void *address);
```

|参数/返回值|含义|
|-|-|
|address|分配基地址或 NULL；不支持内部地址、外部分配或重复释放的地址|
|返回值|成功返回0，其他为错误码|

释放前须停止新的远端访问并等待在途请求完成，释放成功后不得再使用或注册该地址。
已注册的内存由 TRANS 保持引用，直至对应 TRANS 实例销毁后才释放底层内存。

## TRANS接口列表

### 1. TRANS初始化/退出

#### smem_trans_config_init

TRANS配置初始化。

```c
int32_t smem_trans_config_init(smem_trans_config_t *config);
```

|参数/返回值|含义|
|-|-|
|config|初始化参数|
|返回值|成功返回0，其他为错误码|

**smem_trans_config_t 结构体参数说明**：

|成员|含义|
|-|-|
|role|TRANS角色，取值参考smem_trans_role_t（`SMEM_TRANS_SENDER`/`SMEM_TRANS_RECEIVER`/`SMEM_TRANS_BOTH`），默认`SMEM_TRANS_SENDER`|
|initTimeout|初始化/建链等待超时时间，单位秒，默认120秒|
|deviceId|绑定的NPU设备ID|
|flags|预留参数|
|dataOpType|数据操作类型，取值参考smem_bm_data_op_type定义|
|nic|host_rdma场景使用的网卡地址，格式`tcp://ip:port`|
|url|本TRANS实例的唯一标识，格式`ip:port`，**必填**；端口填`0`表示按`MF_CONFIG_STORE_PORT_START`/`MF_CONFIG_STORE_PORT_END`自动选择可用端口，可通过`smem_trans_get_rpc_port`获取实际端口|
|hcomTlsConfig|host_rdma场景的hcom TLS配置|

#### smem_trans_init

TRANS初始化，所有进程在创建TRANS实例前必须调用。

```c
int32_t smem_trans_init(uint32_t deviceId, uint32_t flags);
```

|参数/返回值|含义|
|-|-|
|deviceId|NPU设备ID|
|flags|预留参数|
|返回值|成功返回0，失败返回错误码|

#### smem_trans_uninit

TRANS退出。

```c
void smem_trans_uninit(uint32_t flags);
```

|参数/返回值|含义|
|-|--|
|flags|预留参数|

### 2. 创建/销毁TRANS实例

#### smem_trans_create

创建TRANS实例。实例内部会按`config->url`创建自己的config store服务端（无需调用方提前创建config store），并在首次拷贝时按需与对端建立连接。

```c
smem_trans_t smem_trans_create(const smem_trans_config_t *config, uint32_t flags);
```

|参数/返回值|含义|
|-|--|
|config|TRANS初始化配置，其中`config->url`为必填的本实例唯一标识|
|flags|预留参数|
|返回值|成功返回 TRANS 实例句柄，失败返回 NULL|

#### smem_trans_destroy

销毁TRANS实例。

```c
void smem_trans_destroy(smem_trans_t handle, uint32_t flags);
```

|参数/返回值|含义|
|-|--|
|handle|TRANS对象handle|
|flags|预留参数|

#### smem_trans_get_rpc_port

获取TRANS实例内部config store服务的实际侦听端口。当`smem_trans_config_t.url`的端口填`0`（自动选择）时，用本接口取回真实端口并据此拼接对端可访问的url。

```c
int32_t smem_trans_get_rpc_port(smem_trans_t handle, uint16_t *port);
```

|参数/返回值|含义|
|-|-|
|handle|TRANS对象handle|
|port|出参，返回实际侦听的端口号|
|返回值|成功返回0，其他为错误码|

### 3. 注册/批量注册/注销内存

#### smem_trans_register_mem

注册内存。

```c
int32_t smem_trans_register_mem(smem_trans_t handle, void *address, size_t capacity, uint32_t flags);
```

|参数/返回值|含义|
|-|--|
|handle|TRANS对象handle|
|address|注册地址的起始地址指针|
|capacity|注册地址大小|
|flags|预留参数|
|返回值|成功返回0，其他为错误码|

#### smem_trans_batch_register_mem

批量注册内存。

```c
int32_t smem_trans_batch_register_mem(smem_trans_t handle, void *addresses[], size_t capacities[], uint32_t count,
                                       uint32_t flags);
```

|参数/返回值| 含义|
|-|----|
|handle|TRANS对象handle|
|addresses[]|批量注册地址的起始地址指针列表|
|capacities[]|批量注册地址大小列表|
|count|批量注册地址数量|
|flags|预留参数|
|返回值|成功返回0，其他为错误码|

#### smem_trans_deregister_mem

注销内存。

```c
int32_t smem_trans_deregister_mem(smem_trans_t handle, void *address);
```

|参数/返回值|含义|
|-|-----------|
|handle|TRANS对象handle|
|address|注销地址的起始地址指针|
|返回值|成功返回0，其他为错误码|

### 4. 同步读/写

#### smem_trans_read

同步读接口。

```c
int32_t smem_trans_read(smem_trans_t handle, void *localAddr, const char *remoteUniqueId,
                    const void *remoteAddr, size_t dataSize, uint32_t flags);
```

|参数/返回值|含义|
|-|---------|
|handle|TRANS对象handle|
|localAddr|本地用于接收读取数据的起始地址指针|
|remoteUniqueId|远端TRANS实例对应的标识|
|remoteAddr|远端待读取数据的起始地址指针|
|dataSize|传输数据大小，单位字节|
| flags          | 标记位                |
|返回值|成功返回0，其他为错误码|

#### smem_trans_batch_read

批量同步读接口

```c
int32_t smem_trans_batch_read(smem_trans_t handle, void *localAddrs[], const char *remoteUniqueId,
                          const void *remoteAddrs[], size_t dataSizes[], uint32_t batchSize, uint32_t flags);
```

|参数/返回值|含义|
|-|--------|
|handle|TRANS对象handle|
|localAddrs[]|本地用于接收读取数据的起始地址指针列表|
|remoteUniqueId|远端TRANS实例对应的标识|
|remoteAddrs[]|批量远端待读取数据的起始地址指针列表|
|dataSizes[]|批量传输数据大小列表，单位字节|
|batchSize|批量读操作的任务数|
| flags          | 标记位                |
|返回值|成功返回0，其他为错误码|

#### smem_trans_write

同步写接口。

```c
int32_t smem_trans_write(smem_trans_t handle, const void *localAddr, const char *remoteUniqueId,
                              void *remoteAddr, size_t dataSize, uint32_t flags);
```

|参数/返回值|含义|
|-|---------|
|handle|TRANS对象handle|
|localAddr|本地待写数据起始地址指针|
|remoteUniqueId|远端TRANS实例对应的标识|
|remoteAddr|远端存储数据起始地址指针|
|dataSize|传输数据大小，单位字节|
| flags          | 标记位                |
|返回值|成功返回0，其他为错误码|

#### smem_trans_batch_write

批量同步写接口。

```c
int32_t smem_trans_batch_write(smem_trans_t handle, const void *localAddrs[], const char *remoteUniqueId,
                                    void *remoteAddrs[], size_t dataSizes[], uint32_t batchSize, uint32_t flags);
```

|参数/返回值|含义|
|-|--------|
|handle|TRANS对象handle|
|localAddrs[]|批量本地待写数据起始地址指针列表|
|remoteUniqueId|远端TRANS实例对应的标识|
|remoteAddrs[]|批量远端存储数据起始地址指针列表|
|dataSizes[]|批量传输数据大小列表，单位字节|
|batchSize|批量写操作的任务数|
| flags          | 标记位                |
|返回值|成功返回0，其他为错误码|

### 5. 异步读/写提交

#### smem_trans_read_submit

异步读提交接口。

```c
int32_t smem_trans_read_submit(smem_trans_t handle, void *localAddr, const char *remoteUniqueId,
                               const void *remoteAddr, size_t dataSize, void *stream, uint32_t flags);
```

| 参数/返回值         | 含义                 |
|----------------|--------------------|
| handle         | TRANS对象handle      |
| localAddr      | 本地用于接收读取数据的起始地址指针  |
| remoteUniqueId | 远端TRANS实例对应的标识     |
| remoteAddr     | 远端待读取数据的起始地址指针     |
| dataSize       | 传输数据大小，单位字节        |
| stream         | 需要将任务提交到的aclrtStream |
| flags          | 标记位                |
| 返回值            | 成功返回0，其他为错误码       |

#### smem_trans_write_submit

异步写提交接口。

```c
int32_t smem_trans_write_submit(smem_trans_t handle, const void *localAddr, const char *remoteUniqueId,
                                void *remoteAddr, size_t dataSize, void *stream, uint32_t flags);
```

| 参数/返回值       | 含义                   |
|--------------|----------------------|
| handle       | TRANS对象handle        |
| localAddr   |本地待写数据起始地址指针          |
| remoteUniqueId | 远端TRANS实例对应的标识       |
| remoteAddr  | 远端存储数据起始地址指针         |
| dataSize     | 传输数据大小               |
| stream       | 需要将任务提交到的aclrtStream |
| 返回值          | 成功返回0，其他为错误码         |

### 6. 异步批量拷贝

#### smem_trans_batch_copy

提交异步批量拷贝任务。任务入队后由内部后台线程执行（线程数由环境变量`MF_TRANS_THREAD_NUM`控制），返回唯一`batchId`用于查询状态；`param->destUrl`对应的对端连接在首次拷贝时按需建立。

```c
int32_t smem_trans_batch_copy(smem_trans_t handle, smem_trans_batch_params_t *param, uint64_t *batchId);
```

|参数/返回值|含义|
|-|-|
|handle|TRANS对象handle|
|param|批量拷贝参数，参考smem_trans_batch_params_t定义|
|batchId|出参，本次任务的唯一标识|
|返回值|成功返回0，其他为错误码|

**smem_trans_batch_params_t 结构体参数说明**：

|成员|含义|
|-|-|
|destUrl|对端TRANS实例的唯一标识，格式`ip:port`|
|srcList|写（`SMEM_TRANS_OP_WRITE`）时为本地源地址列表；读（`SMEM_TRANS_OP_READ`）时为本地接收地址列表|
|destList|写时为远端目的地址列表；读时为远端源地址列表|
|sizeList|每个拷贝任务的字节数列表|
|batchSize|拷贝任务数量，必须大于0|
|opcode|拷贝方向，取值参考smem_trans_opcode_t|
|stream|需要将任务提交到的aclrtStream，可为空|
|flags|预留参数|

**smem_trans_opcode_t 枚举说明**：

|枚举值|含义|
|-|-|
|SMEM_TRANS_OP_READ|从对端读取数据到本地|
|SMEM_TRANS_OP_WRITE|将本地数据写入对端|

#### smem_trans_batch_status_query

查询`smem_trans_batch_copy`提交的任务状态。任务完成后本接口会把该任务从内部状态表中移除，即同一个`batchId`只应查询到一次完成结果。

```c
int32_t smem_trans_batch_status_query(smem_trans_t handle, uint64_t batchId, int32_t *result, uint32_t flags);
```

|参数/返回值|含义|
|-|-|
|handle|TRANS对象handle|
|batchId|smem_trans_batch_copy返回的任务标识|
|result|出参，任务完成时写入拷贝结果（0为成功，其他为错误码）|
|flags|预留参数|
|返回值|任务已完成返回0；任务仍在执行返回`-2015`（SM_NEED_RETRY）；`batchId`不存在或其他错误返回负错误码|

> [!CAUTION] 注意
> 如下接口对外封装了相同含义的Python接口，详细信息可参考`src/acc_offload/csrc/python_wrapper/pymf_acc_offload.cpp`。
