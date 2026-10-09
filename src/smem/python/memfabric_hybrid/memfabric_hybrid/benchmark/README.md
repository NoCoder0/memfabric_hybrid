# mfcli工具 benchmark命令介绍

本benchmark基于smem_bm层的python接口，测试如下路径性能：

- D2RD
- RH2D

本样例需要在npu环境下编译运行

首先,请在环境上提前安装NPU固件驱动和CANN包([环境安装参考链接](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0000.html))

当前HDK固件驱动需要使用特定版本([社区版HDK下载链接](https://www.hiascend.com/hardware/firmware-drivers/community))

安装完成后需要配置CANN环境变量
([参考安装Toolkit开发套件包的第三步配置环境变量](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0008.html?Mode=PmIns&OS=Ubuntu&Software=cannToolKit))

## 编译

打包安装时同源码一起编译

```bash

bash script/build_and_pack_run.sh --build_mode RELEASE --build_python ON --xpu_type NPU --build_test ON

```

安装whl

```bash

pip install output/memfabric_hybrid/wheel/mem*.whl

```

## benchmark命令详解

完整命令格式为
mfcli benchmark [data-op] [framework] [model] [protocol] [rdma]

- [] 表示可选参数
- data-op:表示数据操作类型，可以填写 read 或 write, 或trans(暂不支持)，默认值为read,read的数据传输方向是RH2D,write的数据传输方向是D2RH
- framework:表示框架类型，可以填写 vllm 或 sglang，默认值为vllm
- model:表示模型名称，可以填写 glm5.2或者dsv4, 默认值为glm5.2
- protocol:表示协议类型，可以填写sdma, device_rdma, device_urma, device_uboe, host_rdma, host_urma, host_tcp, 默认值为device_rdma
- rdma:表示rdma ip地址, 当协议类型是host_rdma时，必须指定该参数

- 示例，测试单机版本write性能,采用device_rdma协议时，框架使用vllm, 模型使用dsv4时，命令行如下：
mfcli benchmark write --framework vllm --model dsv4 --protocol device_rdma

- 如果命令行仅输入mfcli benchmark, 程序将默认使用read操作类型, 模型使用glm5.2, 协议类型使用device_rdma

## 单机卡配置数量

- 可以在环境变量中配置单机测试用的NPU卡数量，配置参数为ASCEND_RT_VISIBLE_DEVICES
- 比如配置任意的两张NPU卡，ASCEND_RT_VISIBLE_DEVICES=2,3
- 如果没有配置该环境变量，程序将默认检测使用本机所有NPU卡
