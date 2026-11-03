# ACC OFFLOAD

## ACC OFFLOAD接口

### 多实例调用

`offload.Offload()` 创建独立的 offload 对象。对象的 `initialize`、`uninitialize`、`malloc`、`free`、
`get_dva`、`empty`、`sparse_copy`、`group_pack_copy`、`kv_exchange_copy`、`register_entry_table` 和
`entry_gather` 与下述模块级接口使用相同的参数和返回值，无需传递实例句柄。

```python
from memfabric_hybrid import offload

pool_a = offload.Offload()
pool_b = offload.Offload()
config_a.store_url = "tcp://127.0.0.1:8500"
config_b.store_url = "tcp://127.0.0.1:8501"
assert pool_a.initialize(config_a) == 0
assert pool_b.initialize(config_b) == 0
tensor_a = pool_a.empty([512, 1])
tensor_b = pool_b.empty([512, 1])
```

原有 `offload.initialize(config)` 等调用仍操作默认实例，默认实例与显式创建的实例相互独立。
实例也提供 `OffloadConfig`、`Scene` 和 `OFFLOAD_FLAG_GIANT_PAGE`，可以将原有代码中的 `offload`
变量替换为实例后继续使用。C 接口和 `OffloadConfig` 的字段不变。

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
