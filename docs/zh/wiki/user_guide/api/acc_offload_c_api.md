# ACC OFFLOAD

## 公共接口

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

## ACC OFFLOAD接口列表

### 1. 常用类型

#### offload_scene_t

offload内存池场景枚举.

```c
typedef enum {
    OFFLOAD_SCENE_LOCAL = 0,    /* single-rank local DRAM memory pool */
    OFFLOAD_SCENE_SHARED = 1,   /* multi-rank shared DRAM memory pool */
} offload_scene_t;
```

|枚举值|含义|
|-|-|
|OFFLOAD_SCENE_LOCAL|单卡本地DRAM内存池，每rank独占一份池|
|OFFLOAD_SCENE_SHARED|多卡共享DRAM内存池，reserveSize需保证相同，allocSize各rank可按需传入（允许不同）|

#### offload_config_t

offload初始化配置结构体。

```c
typedef struct {
    uint32_t deviceId;
    uint64_t reserveSize;
    uint64_t allocSize;
    uint32_t worldSize;
    uint32_t rankId;
    offload_scene_t scene;
    char storeUrl[64];
} offload_config_t;
```

|成员|含义|
|-|-|
|deviceId|绑定的device id|
|reserveSize|预留DRAM内存池大小，单位字节，内部会向上对齐到GB。SHARED场景需保证相同，作为全池槽位虚拟大小（初始化时校验）|
|allocSize|本地实际分配物理DRAM大小，单位字节，内部会向上对齐到GB。LOCAL场景需与reserveSize相等；SHARED场景为本rank实际贡献，需不超过reserveSize，各rank允许传入不同大小——是否要求各rank等大由业务场景决定，框架不校验一致性，需要等大的场景由调用方保证各rank传入相同值|
|worldSize|参与组网的rank数量（SHARED场景使用）|
|rankId|本地rank id（SHARED场景使用）|
|scene|内存池场景，取值参考offload_scene_t，默认OFFLOAD_SCENE_LOCAL|
|storeUrl|SHARED场景下共享池建立时的显式config store地址（如`tcp://127.0.0.1:8500`），同组各rank需保持一致；为空时回退为推导端口`8500 + deviceId / worldSize`|

### 2. 初始化/退出

#### offload_init

初始化offload模块，创建hybm大内存实体并加载sparse copy算子库。

```c
int32_t offload_init(const offload_config_t &config);
```

|参数/返回值|含义|
|-|-|
|config|初始化参数，参考offload_config_t定义|
|返回值|成功返回0，其他为错误码|

#### offload_uninit

退出offload模块，释放hybm大内存实体并卸载算子库。

```c
void offload_uninit();
```

### 3. 内存分配/释放

#### offload_malloc

从offload内存池中分配host内存，返回地址按16字节对齐。

```c
uint64_t offload_malloc(uint64_t size, uint64_t flags);
```

|参数/返回值|含义|
|-|-|
|size|分配内存大小，单位字节|
|flags|预留参数|
|返回值|成功返回非0地址，失败返回0|

#### offload_free

释放由offload_malloc分配的host内存。

```c
void offload_free(uint64_t ptr, uint64_t flags);
```

|参数/返回值|含义|
|-|-|
|ptr|offload_malloc返回的地址|
|flags|预留参数|

### 4. 稀疏拷贝

#### offload_sparse_copy

批量稀疏拷贝，在device上异步执行多个不连续地址间的数据拷贝。

```c
int32_t offload_sparse_copy(uint64_t srcPtr, uint64_t dstPtr, uint64_t lenPtr, uint64_t sizePtr, uint16_t deviceId);
```

|参数/返回值|含义|
|-|-|
|srcPtr|源地址数组首地址指针，数组元素为uint64_t地址|
|dstPtr|目的地址数组首地址指针，数组元素为uint64_t地址|
|lenPtr|每个拷贝任务长度数组首地址指针，数组元素为uint32_t，单位字节|
|sizePtr|指向上述数组中元素个数的指针|
|deviceId|执行拷贝的device id|
|返回值|成功返回0，其他为错误码|

## 环境变量

|环境变量|含义|
|-|-|
|LD_LIBRARY_PATH|动态链接库搜索路径|
|ASCEND_HOME_PATH|cann包安装路径|
|VERSION|编译whl包版本|
