# Transfer（TRANS）

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

## TRANSFER接口

### 1. 常用类型

#### TransferEngine类

```python
class TransferEngine:
    def __init__(self):
    def initialize(store_url: str, session_id: str, role: str, device_id: int, data_op_type = TransDataOpType.SDMA, store_server_role = "Decode", nic = "") -> int:
    def get_rpc_port() -> int:
    def transfer_sync_write(dest_session: str, buffer, peer_buffer, length, flags = 0) -> int:
    def batch_transfer_sync_write(dest_session: str, buffers, peer_buffers, lengths, flags = 0) -> int:
    def transfer_async_write_submit(dest_session: str, buffer, peer_buffer, length, stream, flags = 0) -> int:
    def transfer_async_read_submit(dest_session: str, buffer, peer_buffer, length, stream, flags = 0) -> int:
    def register_memory(buffer_addr, capacity) -> int:
    def unregister_memory(buffer_addr) -> int:
    def batch_register_memory(buffer_addrs, capacities) -> int:
    def transfer_sync_read(dest_session: str, buffer, peer_buffer, length, flags = 0) -> int:
    def batch_transfer_sync_read(dest_session: str, buffers, peer_buffers, lengths, flags = 0) -> int:
    def batch_transfer_async_write_submit(dest_session: str, buffers, peer_buffers, lengths, stream, flags = 0) -> int:
    def batch_transfer_async_read_submit(dest_session: str, buffers, peer_buffers, lengths, stream, flags = 0) -> int:
    def destroy() -> None:
    def unInitialize() -> None:
```

|属性|含义|
|-|-|
|initialize方法|TRANS配置初始化，成功返回0，其他为错误码|
|initialize参数store_url|兼容参数，当前版本不再生效：config store协议固定为tcp，且由本实例按`session_id`自行创建，对端发现只依赖各实例的`session_id`|
|initialize参数session_id|该TRANS实例的唯一标识，同时也是本实例内部config store的监听地址。支持`ip:port`（指定侦听端口）或`ip`/`ip:0`（由initialize自动选择可用端口，需在initialize后调用get_rpc_port获取真实端口）|
|initialize参数role|当前进程的角色|
|initialize参数device_id|当前设备的唯一标识|
|initialize参数data_op_type|数据传输操作类型，默认 TransDataOpType.SDMA|
|initialize参数store_server_role|兼容参数，当前版本不再生效（任意取值均不影响初始化）|
|initialize参数nic|host_rdma场景使用的网卡地址，默认空串|
|get_rpc_port方法|返回initialize中实际侦听的端口号（`int`）。**必须在initialize成功后调用**，否则返回`0`并打印WARN。端口范围由环境变量`MF_CONFIG_STORE_PORT_START`/`MF_CONFIG_STORE_PORT_END`控制（默认`9000`~`65535`）。|
|transfer_sync_write方法|同步写接口，成功返回0，其他为错误码|
|transfer_sync_write参数dest_session|目的TRANS实例对应的标识|
|transfer_sync_write参数buffer|源地址的起始地址指针|
|transfer_sync_write参数peer_buffer|目的地址的起始地址指针|
|transfer_sync_write参数length|传输数据大小|
|transfer_sync_write参数flags|标记位，默认0|
|transfer_async_write_submit方法|异步写任务提交接口，相比于transfer_async_write增加了入参stream，成功返回0，其他为错误码|
|transfer_async_write_submit参数stream|需要提交到的acl.rt.stream|
|batch_transfer_sync_write方法|批量同步写接口，成功返回0，其他为错误码|
|batch_transfer_sync_write参数dest_session|目的TRANS实例对应的标识|
|batch_transfer_sync_write参数buffer|源地址的起始地址指针列表|
|batch_transfer_sync_write参数peer_buffers|目的地址的起始地址指针列表|
|batch_transfer_sync_write参数lengths|传输数据大小列表|
|batch_transfer_sync_write参数flags|标记位，默认0|
|register_memory方法|注册内存，成功返回0，其他为错误码|
|register_memory参数buffer_addr|注册地址的起始地址指针|
|register_memory参数capacity|注册地址大小|
|unregister_memory方法|注销内存，成功返回0，其他为错误码|
|unregister_memory参数buffer_addr|注销地址的起始地址指针|
|batch_register_memory方法|批量注册内存，成功返回0，其他为错误码|
|batch_register_memory参数buffer_addrs|批量注册地址的起始地址指针列表|
|batch_register_memory参数capacities|批量注册地址大小列表|
|destroy方法|销毁TRANS实例|
|unInitialize方法|TRANS退出|
|transfer_sync_read方法|同步读接口，成功返回0，其他为错误码|
|transfer_sync_read参数dest_session|目的TRANS实例对应的标识|
|transfer_sync_read参数buffer|本地用于接收读取数据的起始地址指针|
|transfer_sync_read参数peer_buffer|远端待读取数据的起始地址指针|
|transfer_sync_read参数length|传输数据大小|
|transfer_sync_read参数flags|标记位，默认0|
|batch_transfer_sync_read方法|批量同步读接口|
|batch_transfer_sync_read参数dest_session|目的TRANS实例对应的标识|
|batch_transfer_sync_read参数buffers|本地接收数据指针列表|
|batch_transfer_sync_read参数peer_buffers|远端数据指针列表|
|batch_transfer_sync_read参数lengths|传输数据大小列表|
|batch_transfer_sync_read参数flags|标记位，默认0|
|batch_transfer_async_write_submit方法|批量异步写提交接口|
|batch_transfer_async_write_submit参数dest_session|目的TRANS实例对应的标识|
|batch_transfer_async_write_submit参数buffers|源地址指针列表|
|batch_transfer_async_write_submit参数peer_buffers|目的地址指针列表|
|batch_transfer_async_write_submit参数lengths|传输数据大小列表|
|batch_transfer_async_write_submit参数stream|需要提交到的acl.rt.stream|
|batch_transfer_async_write_submit参数flags|标记位，默认0|
|batch_transfer_async_read_submit方法|批量异步读提交接口|
|batch_transfer_async_read_submit参数dest_session|目的TRANS实例对应的标识|
|batch_transfer_async_read_submit参数buffers|本地接收数据指针列表|
|batch_transfer_async_read_submit参数peer_buffers|远端数据指针列表|
|batch_transfer_async_read_submit参数lengths|传输数据大小列表|
|batch_transfer_async_read_submit参数stream|需要提交到的acl.rt.stream|
|batch_transfer_async_read_submit参数flags|标记位，默认0|

#### TransDataOpType枚举类

```python
class TransDataOpType(Enum):
    SDMA
    DEVICE_RDMA
    DEVICE_URMA
```

#### TransferOpcode枚举类

```python
class TransferOpcode(Enum):
    Read
    Write
```
