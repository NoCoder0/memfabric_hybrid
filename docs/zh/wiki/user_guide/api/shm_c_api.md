# Share Memory（SHM）

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

## SHM接口列表

### 1. SHM初始化/退出

#### smem_shm_config_init

SHM配置初始化。

```c
int32_t smem_shm_config_init(smem_shm_config_t *config);
```

|参数/返回值|含义|
|-|-|
|config|初始化配置|
|返回值|成功返回0，失败返回错误码|

#### smem_shm_init

SHM初始化。

```c
int32_t smem_shm_init(const char *configStoreIpPort, uint32_t worldSize, uint32_t rankId,
    uint16_t deviceId, smem_shm_config_t *config);
```

|参数/返回值|含义|
|-|-|
|configStoreIpPort|config store地址，格式支持 `tcp://ip:port`、`tcp6://[ip]:port`、`etcd://ip:port`、`etcd://ip:port#instanceId`（etcd 多集群隔离）|
|worldSize|参与SHM初始化rank数量，最大支持1024|
|rankId|当前rank id|
|deviceId|当前rank的device id|
|config|初始化SHM配置|
|返回值|成功返回0，失败返回错误码|

#### smem_shm_uninit

SHM退出。

```c
void smem_shm_uninit(uint32_t flags);
```

|参数/返回值|含义|
|-|-|
|flags|预留参数|

### 2. 创建/销毁SHM

#### smem_shm_create

创建SHM。

```c
smem_shm_t smem_shm_create(uint32_t id, uint32_t rankSize, uint32_t rankId, uint64_t localSize,
    smem_shm_data_op_type dataOpType, uint32_t flags, void **gva);
```

|参数/返回值|含义|
|-|-|
|id|SHM对象id，用户指定，与其他SHM对象不重复，范围为[0, 63]|
|rankSize|参与创建SHM的rank数量，最大支持1024|
|rankId|当前rank id|
|localSize|每个rank贡献到创建SHM对象的对称空间大小，单位字节，范围为[0, 64GB]（对应头文件参数名 localSize）|
|dataOpType|数据操作类型，参考smem_shm_data_op_type类型定义|
|flags|预留参数|
|gva|出参，gva空间地址|
|返回值|SHM对象handle|

#### smem_shm_destroy

销毁SHM。

```c
int32_t smem_shm_destroy(smem_shm_t handle, uint32_t flags);
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|flags|预留参数|
|返回值|成功返回0，失败返回错误码|

### 3. 查询接口

#### smem_shm_query_support_data_operation

查询支持的数据操作。

```c
uint32_t smem_shm_query_support_data_operation(void);
```

|参数/返回值|含义|
|-|-|
|返回值|参考smem_shm_data_op_type类型定义|

#### smem_shm_get_global_rank

获取rank id。

```c
uint32_t smem_shm_get_global_rank(smem_shm_t handle);
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|返回值|在SHM里的rank id|

#### smem_shm_get_global_rank_size

获取rank数量。

```c
uint32_t smem_shm_get_global_rank_size(smem_shm_t handle);
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|返回值|在SHM里的rank个数|

### 4. 设置用户context

#### smem_shm_set_extra_context

设置用户context。

```c
int32_t smem_shm_set_extra_context(smem_shm_t handle, const void *context, uint32_t size);
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|context|用户context指针|
|size|用户context大小，最大64K，单位字节|
|返回值|成功返回0，失败返回错误码|

### 5. 在SHM对象执行barrier/allgather

#### smem_shm_control_barrier

在SHM对象执行barrier。

```c
int32_t smem_shm_control_barrier(smem_shm_t handle);
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|返回值|成功返回0，失败返回错误码|

#### smem_shm_control_allgather

在SHM对象执行allgather。

```c
int32_t smem_shm_control_allgather(smem_shm_t handle, const char *sendBuf, uint32_t sendSize,
    char *recvBuf, uint32_t recvSize);
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|sendBuf|发送数据buffer|
|sendSize|发送数据大小，单位字节|
|recvBuf|接收数据buffer|
|recvSize|接收数据大小，单位字节|
|返回值|成功返回0，失败返回错误码|

### 6. rank连通检查

#### smem_shm_topology_can_reach

rank连通检查。

```c
int32_t smem_shm_topology_can_reach(smem_shm_t handle, uint32_t remoteRank, uint32_t *reachInfo);
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|remoteRank|待检查rank id|
|reachInfo|连通信息类型，参考smem_shm_data_op_type定义|
|返回值|成功返回0，失败返回错误码|

### 7. 注册退出回调函数

#### smem_shm_register_exit

注册退出回调函数。

```c
int32_t smem_shm_register_exit(smem_shm_t handle, void (*exit)(int));
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|exit|退出函数|
|返回值|成功返回0，失败返回错误码|

### 8. PE主动退出接口

#### smem_shm_global_exit

PE主动退出接口。

```c
void smem_shm_global_exit(smem_shm_t handle, int status);
```

|参数/返回值|含义|
|-|-|
|handle|SHM对象handle|
|status|退出状态|

> [!CAUTION] 注意
> 如下接口对外封装了相同含义的Python接口，详细信息可参考`src/smem/csrc/python_wrapper/mk_transfer_adapter/csrc/pytransfer.cpp`。
