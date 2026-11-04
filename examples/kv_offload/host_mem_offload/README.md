# Host内存池接口示例

## 行为说明

本示例通过原有LOCAL和SHARED内存池验证 `malloc`、`free`、`host_register` 和
`host_get_device_pointer`。无需额外的host-mem标志。

- `initialize` 按GB对齐的 `alloc_size` 分配整池物理内存；SHARED同时完成整池交换和映射。
- LOCAL要求对齐后的 `reserve_size` 与 `alloc_size` 相等，分配策略沿用原LOCAL实现。
- SHARED要求各rank的 `reserve_size` 一致，作为槽位步长；各rank的 `alloc_size` 可以不同，但不能超过槽位大小。
- 本示例设置 `alloc_size = reserve_size`，实际占用为每实例 `align_up(alloc_size, 1GB)`。
- `malloc` 和 `free` 使用池内16字节对齐分配，支持空洞复用和相邻空闲块合并。
- `host_register(ptr)` 只检查本实例本rank的池内地址，重复调用成功，不注册外部内存。
- `host_get_device_pointer(ptr)` 在注册前即可使用，允许池内偏移，失败返回0。
- 实例 `free(ptr)` 成功返回0；非法地址、块内偏移及重复释放返回错误码。
- SHARED的不同实例使用不同store URL；示例中实例i使用 `store_port + i`，同一实例的各rank使用相同端口。

地址检查针对本rank实际分配的池范围，拒绝槽位中未分配的尾部和其他rank、其他实例的地址。
注册检查不跟踪分配块状态，已归还或尚未切分的池内地址也能通过检查，但不代表调用者拥有相应内存。
SHARED仅初始化包含集合通信，运行期的注册检查无需各rank同步调用。

## 必要条件

- 单机至少 `world_size` 张可用NPU，可通过 `--device_offset` 指定起始设备。
- 已安装 `memfabric_hybrid`、`torch` 和 `torch_npu`。
- `--reserve_mib` 至少1024；多实例的物理内存占用按实例数增加。
- SHARED需要连续的 `instances` 个可用store端口，以及独立的HCCL rendezvous端口。

## 运行方式

```bash
cd examples/kv_offload/host_mem_offload
# 每实例预分配1GB，LOCAL使用原有默认分配策略。
python3 host_mem_offload.py --scene local
python3 host_mem_offload.py --scene shared --world_size 8 --store_port 8531
# LOCAL大页配置：A5使用conn形态；A3保留原LOCAL大页策略。
python3 host_mem_offload.py --scene local --giant_page
# 每rank两个SHARED池，分别使用8531和8532端口。
python3 host_mem_offload.py --scene shared --instances 2 --world_size 8 --store_port 8531
python3 host_mem_offload.py --scene local --device_offset 4
```

LOCAL默认使用原有 `UNRESTRICTED_MEM` 路径；设置 `--giant_page` 时使用原有大页路径。
A5大页模式可能返回与host地址不同的设备地址，调用方必须使用 `host_get_device_pointer` 的返回值。

## 验证范围

- 相邻分配的地址增量等于前一块按16字节对齐后的大小，无逐块GB或MB取整。
- 注册前设备地址可用，池内偏移转换保持线性，重复注册成功。
- 释放后空洞可复用，相邻空闲块可合并，大请求不能跨不相邻空洞拼接。
- 700MiB与300MiB可同时放入1GiB池，超容量申请失败，释放后容量恢复。
- 分配块内容写入和读回一致。
- SHARED各rank允许申请不同大小的块，设备地址满足对称槽位寻址。
- 多实例之间的地址转换和释放拒绝跨池地址，且失败调用不影响原池。

成功时子进程全部正常退出，并打印 `host_mem_offload: all ranks OK, scene: <scene>`。
每个rank的每个实例还会打印对应的 `instance:i/n ok`。
