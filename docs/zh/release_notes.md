# 版本说明书

## 版本配套说明

### 产品版本信息

| 项目 | 内容 |
| ---- | ---- |
| 产品名称 | memfabric-hybrid |
| 产品版本 | develop |
| 版本类型 | 正式版本 |
| 维护周期 | 三个月 |

### 相关产品版本配套说明

**硬件版本配套表**

| 产品名称 | 版本 |
| --- | --- |
| 服务器 | <ul><li> Atlas 200T A2 Box16 </li><li> Atlas 800I A2/A3 系列产品 </li><li> Atlas 800T A2/A3 系列产品 </li><li> Atlas 900 A3 SuperPoD </li></ul> |
| 平台 | <ul><li>aarch64</li> <li>x86</li> </ul>|

**软件版本配套表**

| 产品名称 | 版本 |
| --- | --- |
|CANN| ≥ 8.1.RC1|
|cmake| ≥ 3.19|
|GLIBC | ≥ 2.28 |
|Ascend HDK| 最低版本：<ul><li> HBM池化：24.1.RC2 </li> <li> DRAM池化：25.5.0</li></ul> 推荐版本：<ul><li> HBM池化：24.1.RC2 </li> <li> DRAM池化：25.5.1</li></ul>  |
|LingQu Computing Network|1.5.0|

## 版本兼容性说明

无

## 更新说明

### 关键特性变更

- 提供zero buffer的算子加速库，支持DeepSeek大模型训练。
- 提供zero buffer的算子加速库，支持大模型推理加速，减少内存占用。
- 基于zero buffer框架，支持P和D的D&C算子，加速大模型推理。
- A5场景基础能力验证。
- A5硬件支持AIV驱动MTE的dispatch、combine、classic算子。
- 解决AIV占核问题，支持deepspeed训练classic算子不占用AIV核。

### 接口变更说明

无

### 已解决的问题

无

### 遗留问题

无

## 升级影响

### 升级过程中对现行系统的影响

- 对业务的影响

    软件版本升级过程中会导致业务中断。

- 对网络通信的影响

    对通信无影响。

### 升级后对现行系统的影响

无

## 版本配套文档

|文档名称|内容简介|
|---|---|
|《[编译安装](./installation.md)》|介绍组件编译和安装教程。|
|《[安装 HYBM AICPU OPS](./installation_aicpu_kernel.md)》|提供 HYBM AICPU OPS 的构建、安装、卸载、使用和常见问题等。|
|《[LingQu Computing Network 安装包升级示例](./CCLink.md)》|提供LingQu Computing Network 安装包升级示例。|
|《[故障注入的实现与使用](./fault_injection.md)》|提供故障注入的具体操作步骤。|
|《[API 介绍](./API.md)》|MemFabric提供的多种API的简介。|
|《[C 接口](./API.md)》|C接口介绍以及对应的API列表。|
|《[Python 接口](./pythonAPI.md)》|Python接口介绍已经对应的API列表。|
|《[ptracer 性能打点工具](./ptracer.md)》|MemFabric内置性能打点工具简介。|
|《[安全说明](./SECURITYNOTE.md)》|提供了MemFabric Hybrid安全配置相关的内容。|
|《[DevContainer 快速入门](./devcontainer_quickstart.md)》|提供MemFabric Hybrid在VS Code Remote + DevContainer 一键环境搭建 + 全量用例运行指南。|
|《[配置存储集群高可用性](./config_store_cluster_ha.md)》|提供Config Store在不同模式下的操作步骤。|
|《[ETCD 存储后端](./etcd_store_backend.md)》|提供ETCD的安装、编译、启动等操作。|
|《[IDE 代码跳转配置指南](./ide_code_navigation_guide.md)》|提供在IDE无法正确跳转到定义/声明时的解决方法。|
|《[环境变量](./environment_variables.md)》|提供运行、构建、安装时的环境变量及其使用示例。|
|《[acc_links](./acc_links/API.md)》|提供Acc相关的接口。|
