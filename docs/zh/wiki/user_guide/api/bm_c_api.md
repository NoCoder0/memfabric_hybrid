# Big Memory（BM）

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
| `SMEM_GVA_NOT_FOUND` | -2017 | GVA 注册记录不存在或已卸载；获取有效 GVA 后重试 |

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

## BM接口列表

### 1. BM初始化/退出

#### smem_bm_config_init

BM配置初始化。

```c
int32_t smem_bm_config_init(smem_bm_config_t *config);
```

|参数/返回值|含义|
|-|-|
|config|初始化参数|
|返回值|成功返回0，其他为错误码|

#### smem_bm_init

初始化BM。

```c
int32_t smem_bm_init(const char *storeURL, uint32_t worldSize, uint16_t deviceId, const smem_bm_config_t *config);
```

| 参数/返回值    | 含义                                                |
|-----------|---------------------------------------------------|
| storeURL  | config store地址，格式支持 `tcp://ip:port`、`etcd://ip:port`、`etcd://ip:port#instanceId`（etcd 多集群隔离） |
| worldSize | 参与初始化BM的rank数量，最大支持1024                           |
| deviceId  | 当前rank的deviceId                                   |
| config    | BM初始化配置                                           |
| 返回值       | 成功返回0，其他为错误码                                      |

**smem_bm_config_t 结构体参数说明**：

| 参数                | 含义                                                                 |
|---------------------|--------------------------------------------------------------------|
| initTimeout         | smem_bm_init 函数超时时间，默认120秒（最小1秒，最大SMEM_BM_TIMEOUT_MAX） |
| createTimeout       | smem_bm_create 函数超时时间，默认120秒（最小1秒，最大SMEM_BM_TIMEOUT_MAX） |
| controlOperationTimeout | 控制操作超时时间，默认120秒（最小1秒，最大SMEM_BM_TIMEOUT_MAX） |
| startConfigStoreServer | 是否启动config store，默认true |
| startConfigStoreOnly | 仅启动config store |
| dynamicWorldSize    | 成员能否动态加入 |
| unifiedAddressSpace | 全局统一地址空间，默认true，当前仅支持true |
| autoRanking         | 自动分配rank ID，默认false |
| rankId              | 用户指定的rank ID，仅当autoRanking为false时有效 |
| flags               | 其他标志，默认0 |
| hcomUrl             | hcom地址 |
| hcomTlsConfig       | hcom的TLS配置 |
| storeTlsConfig      | store的TLS配置 |

#### smem_bm_uninit

BM退出。

```c
void smem_bm_uninit(uint32_t flags);
```

|参数/返回值|含义|
|-|-|
|flags|预留参数|

#### smem_bm_update_store_url

更新config store URL，用于MetaService重启变更IP地址时，在smem_bm_init之后动态更新store地址。

```c
int32_t smem_bm_update_store_url(const char *storeURL);
```

|参数/返回值|含义|
|-|-|
|storeURL|新的config store地址，格式支持 `tcp://ip:port` 等|
|返回值|成功返回0，其他为错误码|

### 2. 创建/销毁BM

#### smem_bm_create

创建BM。

 ```c
smem_bm_t smem_bm_create(uint32_t id, uint32_t memberSize,
    smem_bm_data_op_type_t dataOpType, uint64_t localDRAMSize,
    uint64_t localHBMSize, uint32_t flags);
 ```

|参数/返回值| 含义                                     |
|-|----------------------------------------|
|id| BM id，用户自定义，BM之间取不同值                   |
|memberSize| 创建BM的rank数量（保留参数，后续迭代使用）               |
|dataOpType| 数据操作类型，取值内容参考smem_bm_data_op_type定义    |
|localDRAMSize| 创建BM当前rank贡献的DRAM空间大小，单位字节，范围为[0, 2TB] |
|localHBMSize| 创建BM当前rank贡献的HBM空间大小，单位字节，范围为[0, 64GB] |
|flags| 创建标记位，预留                               |
|返回值| 成功返回BM handle，失败返回空指针                  |

#### smem_bm_create2

创建BM。

 ```c
smem_bm_t smem_bm_create2(uint32_t id, const smem_bm_create_option_t *option);
 ```

| 参数/返回值       | 含义                                                              |
|--------------|-----------------------------------------------------------------|
| id           | BM id，用户自定义，BM之间取不同值                                            |
| option       | 创建BM的配置参数 (注：如果开启`device_urma`协议，必须打开`enable_56bits_gva`为`True`) |
| 返回值          | 成功返回BM handle，失败返回空指针                                           |

**smem_bm_create_option_t 结构体参数说明**：

| 参数                | 含义                                                                                                                                                                         |
|---------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| maxDramSize         | 所有rank贡献给BM对象的最大DRAM内存大小                                                                                                                                                   |
| maxHbmSize          | 所有rank贡献给BM对象的最大HBM内存大小                                                                                                                                                    |
| localDRAMSize       | 当前rank贡献给BM对象的DRAM内存大小，单位字节，范围为[0, 2TB]                                                                                                                                    |
| localHBMSize        | 当前rank贡献给BM对象的HBM内存大小，单位字节，范围为[0, 64GB]                                                                                                                                    |
| 说明                | 当前rank的 `localDRAMSize` 与 `localHBMSize` 同时为 0 时，该rank被视为 Proxy（不贡献内存，仅参与读写），否则为 Provider（贡献内存）；两个 Proxy rank 之间不会建立链路，其余角色组合保持全互联。角色未上报时（如与老版本对端混部）记为 unknown，按非 Proxy 处理 |
| dataOpType          | 数据操作类型，如果tag或tagOpInfo为空，则使用此类型                                                                                                                                            |
| enable56BitsGva     | 当总地址空间超过32TB时启用56位GVA                                                                                                                                                      |
| flags               | 可选标志，默认0                                                                                                                                                                   |
| tag                 | BM的标识tag，例如：tag_1                                                                                                                                                          |
| tagOpInfo           | tag间的操作类型，例如：tag1:DEVICE_SDMA:tag1,tag1:DEVICE_RDMA:tag2                                                                                                                   |
| dramShmFd           | DRAM共享内存文件描述符                                                                                                                                                              |

#### smem_bm_destroy

销毁BM。

```c
void smem_bm_destroy(smem_bm_t handle);
```

|参数/返回值|含义|
|-|-|
|handle|待销毁BM handle|

### 3. 加入/退出BM

#### smem_bm_join

加入BM。

```c
int32_t smem_bm_join(smem_bm_t handle, uint32_t flags);
```

|参数/返回值|含义|
|-|-|
|handle|待加入BM handle|
|flags|预留参数|
|返回值|成功返回0，否则返回错误码|

#### smem_bm_leave

退出BM。

```c
int32_t smem_bm_leave(smem_bm_t handle, uint32_t flags);
```

|参数/返回值| 含义                                                    |
|-|-------------------------------------------------------|
|handle| 待退出BM handle                                          |
|flags| 预留参数 |
|返回值| 成功返回0，否则返回错误码                                         |

#### smem_bm_extend_local_mem

在本地 `rank` 上动态扩增內存。待增加内存必须由 `smem_bm_create()` 创建。

```c
int32_t smem_bm_extend_local_mem(smem_bm_t handle, smem_bm_mem_type_t memType, uint64_t size);
```

| 参数/返回值          | 含义                            |
|-----------------|-------------------------------|
| handle          | 待扩展内存 handle                  |
| memType         | 内存类型，支持 SMEM_MEM_TYPE_DEVICE、SMEM_MEM_TYPE_HOST |
| size | 当前rank扩展内存的大小                 |
| 返回值             | 成功返回0，否则返回错误码                 |

### 4. 拷贝/批量拷贝数据对象

#### smem_bm_copy

拷贝数据对象。

```c
int32_t smem_bm_copy(smem_bm_t handle, smem_copy_params_t *params,
    smem_bm_copy_type_t t, uint32_t flags);
```

|参数/返回值| 含义                                                                     |
|-|------------------------------------------------------------------------|
|handle| BM handle                                                              |
|params| 拷贝数据的相关参数                                                              |
|t| 数据拷贝类型，L2G/G2L/G2H/H2G，L=local HBM memory，G=global space，H=Host memory |
|flags| `ASYNC_COPY_FLAG`：异步执行；`COPY_EXTEND_FLAG`：在 A3 超节点内由 AIV 使用 MTE 搬运数据；`COPY_AIV_DIRECT_SDMA_FLAG`：在 A3 超节点内由 AIV 提交 SDMA 队列、由 SDMA 引擎搬运数据。两种加速标记不能同时使用。 |
|返回值| 成功返回0，失败返回错误码                                                          |

#### smem_bm_copy_batch

批量拷贝数据对象。

```c
int32_t smem_bm_copy_batch(smem_bm_t handle, smem_batch_copy_params_t *params, smem_bm_copy_type_t t, uint32_t flags);
```

|参数/返回值|含义|
|-|-|
|handle|BM handle|
|params|拷贝数据的相关参数|
|t|数据拷贝类型，L2G/G2L/G2H/H2G，L=local HBM memory，G=global space，H=Host memory|
|flags|`ASYNC_COPY_FLAG`：异步执行；`COPY_EXTEND_FLAG`：在 A3 超节点内由 AIV 使用 MTE 搬运数据；`COPY_AIV_DIRECT_SDMA_FLAG`：在 A3 超节点内由 AIV 提交 SDMA 队列、由 SDMA 引擎搬运数据。两种加速标记不能同时使用。|
|返回值|成功返回0，失败返回错误码|

### 5. 查询接口

#### smem_bm_get_rank_id

获取当前rank的id。

```c
uint32_t smem_bm_get_rank_id(void);
```

|参数/返回值|含义|
|-|-|
|返回值|成功返回当前rank id，失败返回u32最大值|

#### smem_bm_get_local_mem_size_by_mem_type

获取创建BM本地贡献的空间大小。

```c
uint64_t smem_bm_get_local_mem_size_by_mem_type(smem_bm_t handle, smem_bm_mem_type_t memType);
```

|参数/返回值|含义|
|-|-|
|handle|BM handle|
|memType|Bmemory type, device or host|
|返回值|本地贡献空间大小，单位byte|

#### smem_bm_ptr_by_mem_type

获取rank id对应在gva上的地址位置。

```c
void *smem_bm_ptr_by_mem_type(smem_bm_t handle, smem_bm_mem_type_t memType, uint16_t peerRankId);
```

|参数/返回值|含义|
|-|-|
|handle|BM handle|
|memType|memory type, SMEM_MEM_TYPE_DEVICE or SMEM_MEM_TYPE_HOST|
|peerRankId|rank id|
|返回值|rank地址对应空间位置指针|

#### smem_bm_get_rank_id_by_gva

根据全局地址获取rankId。

```c
uint32_t smem_bm_get_rank_id_by_gva(smem_bm_t handle, void *gva);
```

|参数/返回值|含义|
|-|-|
|handle|BM handle|
|gva|addr|
|返回值|rank id if successful, UINT32_MAX is returned if failed|

### 6. 用户内存register/unregister

#### smem_bm_register_user_mem

注册一段本地的用户内存。

```c
int32_t smem_bm_register_user_mem(smem_bm_t handle, uint64_t addr, uint64_t size);
```

|参数/返回值|含义|
|-|-|
|handle|BM handle|
|addr|注册地址的起始地址指针|
|size|注册地址的大小|
|返回值|成功返回0，失败返回错误码|

> [!CAUTION] 注意
> device_rdma场景下注册的DRAM buffer需要保证首地址4K对齐，否则无法注册成功。

#### smem_bm_unregister_user_mem

对一段注册过的本地用户内存执行反注册。

```c
int32_t smem_bm_unregister_user_mem(smem_bm_t handle, uint64_t addr);
```

|参数/返回值|含义|
|-|-|
|handle|BM handle|
|addr|注销地址的起始地址指针|
|返回值|成功返回0，失败返回错误码|

### 7. 等待异步操作完成

#### smem_bm_wait

等待异步操作完成。

```c
int32_t smem_bm_wait(smem_bm_t handle);
```

|参数/返回值|含义|
|-|-|
|handle|BM handle|
|返回值|成功返回0，失败返回错误码|
