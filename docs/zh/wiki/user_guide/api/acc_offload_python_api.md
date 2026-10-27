# ACC OFFLOAD

## 公共接口

### 1. 初始化/退出函数

#### initialize

初始化运行环境。

```python
def initialize(flags = 0) -> int
```

|参数/返回值|含义|
|-|-|
|flags|int类型，预留参数|
|返回值|成功返回0，其他为错误码|

#### uninitialize

退出运行环境。

```python
def uninitialize()
```

### 2. 创建config store对象

#### create_config_store

创建config store对象。

```python
def create_config_store(store_url: str) -> int
```

|参数/返回值|含义|
|-|-|
|store_url|业务面地址，格式支持 `tcp://ip:port`、`etcd://ip:port`、`etcd://ip:port#instanceId`（etcd 多集群隔离）|
|返回值|成功返回0，其他为错误码|

### 3. 日志设置

#### set_log_level

设置日志打印级别。

```python
def set_log_level(level)
```

|参数/返回值|含义|
|-|-|
|level|int类型，日志级别，0-debug 1-info 2-warn 3-error|
|返回值|成功返回0，其他为错误码|

#### set_extern_logger

设置自定义日志函数。

```python
def set_extern_logger(log_fn:Callable[[int, str], None]) -> int
```

|参数/返回值|含义|
|-|-|
|log_fn|函数指针|
|level|日志级别，0-debug 1-info 2-warn 3-error|
|message|日志内容|
|返回值|成功返回0，其他为错误码|

### 4. 安全证书设置

#### set_conf_store_tls_key

注册Python解密处理程序。

```python
def set_conf_store_tls_key(tls_pk, tls_pk_pw, py_decrypt_func:Callable[[str], str]) -> int
```

|参数/返回值|含义|
|-|-|
|tls_pk|私钥|
|tls_pk_pw|私钥口令|
|py_decrypt_func|口令解密函数|
|返回值|成功返回0，其他为错误码|

#### set_conf_store_tls

设置配置存储的TLS信息。

```python
def set_conf_store_tls(enable, tls_info) -> int
```

|参数/返回值|含义|
|-|-|
|enable(boolean)|是否启用配置存储的TLS|
|tls_info(str)|TLS配置字符串|
|返回值|成功时返回零，出错时返回非零值|

### 5. 错误信息获取/清理

#### get_last_err_msg

获取最后一条错误信息。

```python
def get_last_err_msg() -> str
```

|参数/返回值|含义|
|-|-|
|返回值|错误信息|

#### get_and_clear_last_err_msg

获取最后一条错误信息并清空所有错误信息。

```python
def get_and_clear_last_err_msg() -> str
```

|参数/返回值|含义|
|-|-|
|返回值|错误信息|

## ACC OFFLOAD接口

### 1. 初始化/退出接口

#### initialize

初始化offload运行环境。

```python
def initialize(config: OffloadConfig) -> int
```

|参数/返回值|含义|
|-|-|
|config|OffloadConfig对象，初始化配置|
|返回值|成功返回0，其他为错误码|

#### uninitialize

退出offload运行环境。

```python
def uninitialize() -> None
```

### 2. 内存分配/释放

#### malloc

从offload内存池分配host内存。

```python
def malloc(size, flags = 0) -> int
```

|参数/返回值|含义|
|-|-|
|size|分配内存大小，单位字节|
|flags|预留参数|
|返回值|成功返回非0地址，失败返回0|

#### free

释放由malloc分配的内存。

```python
def free(ptr, flags = 0) -> None
```

|参数/返回值|含义|
|-|-|
|ptr|malloc返回的地址|
|flags|预留参数|

#### empty

从offload内存池分配内存并返回torch.Tensor。

```python
def empty(sizes, dtype = None, pin_memory = False) -> torch.Tensor
```

|参数/返回值|含义|
|-|-|
|sizes|tensor形状|
|dtype|tensor数据类型，默认torch.bfloat16|
|pin_memory|预留参数|
|返回值|基于offload内存的torch.Tensor|

### 3. 稀疏拷贝

#### sparse_copy

批量稀疏拷贝（torch友好封装），在device上异步执行多个不连续地址间的数据拷贝。

```python
def sparse_copy(srcPtrs, dstPtrs, lenPtrs, sizePtr, deviceId) -> int
```

|参数/返回值|含义|
|-|-|
|srcPtrs|torch.Tensor，每个元素为源地址|
|dstPtrs|torch.Tensor，每个元素为目的地址|
|lenPtrs|torch.Tensor，每个元素为对应拷贝的字节数|
|sizePtr|torch.Tensor，包含一个元素表示任务数量|
|deviceId|torch.device，执行拷贝的设备|
|返回值|成功返回0，其他为错误码|

### 4. 常用类型

#### Scene枚举类

offload内存池场景枚举，通过 `offload.Scene` 访问。

```python
class Scene(Enum):
    LOCAL
    SHARED
```

|属性|含义|
|-|-|
|LOCAL|单卡本地DRAM内存池，每rank独占一份池|
|SHARED|多卡共享DRAM内存池|

#### OffloadConfig类

```python
class OffloadConfig:
    def __init__(self) -> None
```

|构造函数/属性|含义|
|-|-|
|构造函数|offload配置初始化|
|device_id属性|绑定的device id|
|reserve_size属性|预留DRAM内存池大小，单位字节，内部会向上对齐到GB|
|alloc_size属性|本地实际分配物理DRAM大小，单位字节，内部会向上对齐到GB。LOCAL场景需与reserve_size相等；SHARED场景支持按需传入|
|world_size属性|参与组网的rank数量（SHARED场景使用）|
|rank_id属性|本地rank id（SHARED场景使用）|
|scene属性|内存池场景，取值参考Scene枚举，默认LOCAL|
|store_url属性|SHARED场景下共享池建立时的显式config store地址（如`tcp://127.0.0.1:8500`），同组各rank需保持一致；为空时回退为推导端口`8500 + device_id // world_size`|
