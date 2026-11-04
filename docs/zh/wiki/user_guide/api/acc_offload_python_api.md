# ACC OFFLOAD

## ACC OFFLOAD接口

### 多实例调用

`offload.Offload()` 创建独立的 offload 对象。对象的 `initialize`、`uninitialize`、`malloc`、`free`、
`get_dva`、`empty`、`sparse_copy`、`group_pack_copy`、`kv_exchange_copy`、`register_entry_table` 和
`entry_gather` 与下述模块级接口使用相同的参数，无需传递实例句柄。
除实例 `free` 返回状态码外，返回值与模块级接口一致。
实例还提供 `host_register` 和 `host_get_device_pointer`，直接使用LOCAL或SHARED内存池。

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

LOCAL要求GB对齐后的 `alloc_size` 与 `reserve_size` 相等；SHARED以 `reserve_size` 作为各rank一致的槽位步长，
按本rank的 `alloc_size` 分配物理内存。SHARED的不同实例必须使用不同的 `store_url`，同一实例各rank使用相同URL。

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

实例接口签名为 `pool.free(ptr, flags=0) -> int`：成功返回0，非法地址、非分配块首地址、重复释放或未初始化时返回错误码。
模块级 `offload.free` 继续返回 `None`。释放只归还池内块，底层整池在 `uninitialize` 时释放。

#### host_register（仅实例接口）

```python
def host_register(ptr: int) -> int
```

|参数/返回值|含义|
|-|-|
|ptr|本实例本rank实际分配的池范围内的host地址，允许块内偏移|
|返回值|成功返回0，未初始化或地址不在本实例本rank池内时返回错误码|

整池在 `initialize` 时已完成映射，此接口只做幂等的池内地址检查，不注册外部内存，也不记录逐块注册状态。
SHARED场景下此接口不做集合通信，各rank无需对齐调用次数。未分配或已归还的池内地址也能通过范围检查，
但这不代表调用者拥有该地址对应的分配块。

#### host_get_device_pointer（仅实例接口）

```python
def host_get_device_pointer(ptr: int) -> int
```

|参数/返回值|含义|
|-|-|
|ptr|本实例本rank实际分配的池范围内的host地址，允许块内偏移|
|返回值|成功返回设备地址，失败返回0|

此接口与实例 `get_dva` 使用相同的地址校验和转换，不要求先调用 `host_register`。
SHARED的槽位中未实际分配的尾部、其他rank以及其他实例的地址均不接受。
LOCAL的设备地址可能与host地址不同，调用方应使用返回值。

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
|store_url属性|SHARED共享池的store地址。同组rank一致，不同实例独立；为空时先读取ASCEND_MF_STORE_URL，再按设备组推导端口|
