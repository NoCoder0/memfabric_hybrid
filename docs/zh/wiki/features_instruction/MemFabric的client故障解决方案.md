# MemFabric的client故障解决方案

[TOC]

## ConfigStore基础流程

[configStore基础流程参考该文档描述](https://gitcode.com/Ascend/memfabric_hybrid/wiki/ConfigStore%E9%9B%86%E7%BE%A4%E7%AE%A1%E7%90%86%E5%8E%9F%E7%90%86%E8%A7%A3%E6%9E%90%E5%8F%8A%E5%9F%BA%E4%BA%8Eetcd%E7%9A%84%E9%AB%98%E5%8F%AF%E7%94%A8%E6%96%B9%E6%A1%88.md)

## SmemNetGroupEngine

configStore是一个server+client的运行模式，server和client通过kv语义交互，server不感知client存储的具体内容。对于configStore来说，client不存储任何数据，其主要依赖如etcd的持久化能力和选主能力来保障server故障之后重新选主和恢复server上存储的kv数据
`SmemNetGroupEngine`是MemFabric的集群核心组件，基于configStore的kv接口实现，负责MemFabric的集群交互和故障恢复

memfabric主要对外呈现trans、shm、bm三种语义
对于shm场景，暂不支持集群扩缩容，是一个相对简单的固定大小的集群，也就不涉及故障恢复和处理，这里暂不讨论
对于trans和bm场景，集群都是支持扩缩容的，需要使用一套相同的框架完成集群管理

### 主要数据结构

![image.png](https://raw.gitcode.com/user-images/assets/7672916/2e2790e2-28cc-415e-b6c1-7b3b6d4d72b0/image.png 'image.png')
`SmemGroupInfo`是`SmemNetGroupEngine`的核心数据结构，是server侧key=SMEM_GROUP_LISTEN_EVENT_KEY的value，各个client会通过cas命令尝试修改该结构体，修改成功之后会通过watch命令广播给所有client，由此触发某个集群事件或宣告某个事件结束
各变量含义如下：

- version：版本号，奇数表示本次事件开始，偶数表示本次事件结束，另一视角来看，奇数是集群加锁，偶数是集群解锁
- groupSize：已join的rank数量，包括当前正在join的
- curEvent：当前事件类型，参考GroupEventType
- targetRank：本次事件的目标rank（每个client都有一个唯一rank，该rank可能是autoRank方式生成或用户配置的），比如join/leave的rank
- submitRank：本次事件的提交rank，即发起本次事件的rank
- joinedRanksBitmap：集群中已join的rank的bitmap，每次join/leave都会同时修改groupSize和joinedRanksBitmap

`GroupEventType`是事件类型，当前主要有：

- JOIN_EVENT：join事件，加入集群
- LEAVE_EVENT：leave事件，退出集群
- RECOVER_EVENT：修复事件，server故障后重连发起的事件
- LINK_DOWN_EVENT：断链事件，server感知到某个client断链后，广播给所有client，由任一client发起该事件，将故障client移除集群
- NULL_EVNET：空事件，表示事件结束

`eventListenThread_` 事件监听线程，初始化时启动，向server发起watch操作，接受并处理所有的事件
`eventListenSignal_` 事件监听信号，当server发送watch结果时唤醒该信号，从而唤醒事件监听线程
`eventCtx_` 事件上下文，包含已接收事件的队列和接收结果
`currentLeaveCount_` 当前已接收的leave事件数量，因为leave事件具有高优先级，单独计数用于判断队列中是否存在leave事件

`linkListenThread_` 断链监听线程，初始化时启动，接收server广播的断链信息，在本地尝试发起断链事件
`linkListenSignal_`断链监听信号，，当server发送断链信息时唤醒该信号，从而唤醒断链监听线程
`linkCtx_`断链线程上下文，记录已接收的断链信息数量
`currentLinkDownCount_` 当前已接收的断链信息数量，断链也具有高优先级，主要用于join事件处理时判断是否存在断链，从而终止join事件

`linkOpSignal_,linkOpRet_`断链线程等待事件结果的信号量和result，断链线程发起断链事件后，需要等待事件处理结果
`localOpSignal_,localOpRet_`用户本地操作等待事件结果的信号量和result，用户做join/leave时，需要等待事件处理结果

`lastSubmitVersion_` 本rank上一次提交的event的version，用于处理新加入的client被分配了一个旧的rank时，旧rank刚好发起了某个事件且未广播结束的场景，需要由本rank识别当前事件是无效事件，并广播事件结束
`lastUpdateTime_` 上一次事件更新时间，用于识别事件长时间未更新场景，触发检查本事件的submitRank是否已经离线，从而广播事件结束，释放集群锁

`aliveRankSet_` 这是一个在server侧维护的map，用于记录已经合server建链的rank信息

### 正常流程分析

#### join

下面简单示意有两个进程并发join时无故障处理流程，进程A抢先更新事件成功，然后优先完成join，进程B等待进程A完成join后重新更新事件也完成join
![image.png](https://raw.gitcode.com/user-images/assets/7672916/439c15b4-9244-410f-a69a-248c03da0c8f/image.png 'image.png')

#### leave

leave相对简单，无需分析

#### join_handle

- 在创建SmemNetGroupEngine时需要注册join/leave的handle，用于执行join/leave的本地处理
- 这里我们主要讨论bm场景
- 对于leave来说，仅需要remove掉对应rank在join时import到本进程的内容即可，相对比较简单，具体可以查看函数`SmemBmEntry::LeaveHandle`
- 对于join来说，需要进行三个主要步骤：export-->import-->mmap，参见`SmemBmEntry::JoinHandle`
- 一般情况下，每个进程的本地内存申请都是在join前完成的，同时也做了本地内存和进程信息的导出，参见`SmemBmEntry::Initialize`
- 而在joinHandle中，export流程需要将本地导出的string写到configStore中，以便各个rank读取，该string的key我们定义为prefix_key，是一个前缀固定且以rank为后缀的key，这样每个rank都有一个唯一的key。每个rank的prefix_key都是在自身做join时写入的。所以在执行joinHandle时，分两种情况：对于正在join的rank，它需要去configStore查询prefix相同的所有key，也就是获取所有已加入集群的rank的prefix_key；对于已加入集群的rank，它只需要去获取当前join的rank的prefix_key。具体实现参见`GroupGatherPrefixKey`
- 在export完成后，各个rank都会做`hybm_import`和`hybm_mmap`。这些操作因为底层驱动原因，需要保证顺序性，也就是说，必须大家都完成import后才能进行mmap。同时，也为了处理部分rank在做import和mmap时出现异常的场景，需要在import和mmap操作后做一次barrier操作并收集所有rank的执行结果，即函数`GroupGatherResult`

### client故障场景分析

基础处理流程如下，主要依赖感知断链消息来触发linkdown事件，从而清理故障rank
![memfabric的client故障.JPG](https://raw.gitcode.com/user-images/assets/7672916/89b0006b-bf91-41f4-8979-fe8faf4b515f/memfabric的client故障.JPG 'memfabric的client故障.JPG')

#### 所有rank都故障

该场景主要依赖server侧感知到整个集群都断链了，发现`aliveRankSet_`为空，就会清理所有残留key，让集群恢复到初始状态，参见函数`AccStoreServer::LinkBrokenHandler`

#### event的submitRank故障

一般情况下，submitRank故障，集群中其它rank会收到断链消息，并将当前event回退
但是，考虑如下时序:

- A和server建链并发起join事件
- B和server建链
- A断链,join事件未完成
- B发起断链监听，没有来得及收到断链消息，同时，server检查到`aliveRankSet_`不为空，不会清理残留信息
对于这种场景，依赖`lastUpdateTime_` 检测到当前event长时间未更新(当前超时时间为`SMEM_EVNET_KEEP_TIME`)，从而触发client去server查询submitRank是否存活，发现submitRank已离线，从而发起join事件终止操作
进一步，考虑到B还未触发超时时，C被分配了一个和A相同的rank，并和server成功建链，这时候B去server查询存活会发现submitRank并没有离线，则无法发起事件终止操作。但是，该场景下，可以由C去发现submitRank和自己相同，从而发起join事件终止操作。
具体参见`SmemNetGroupEngine::TryCleanOldEvent`

#### 同名rank故障后又join

因为memfabric支持autorank，即支持server自动分配rank，当某个client故障后，该client的rank就空出来了，有新client和server建链时，就有可能分配这个rank给它，这就会产生一种场景：

- clientA的rank=x,和server建链并发起join
- clientA故障，server广播rank=x的断链消息
- clientB和server建链并分配了rank=x，然后发起join
可能存在B先抢到了event，集群中其它client虽然收到了断链消息但是因为已有join事件，无法发起event回退
这种情况需要让B在join前，发现集群中已有同名rank，则优先发起该rank的leave事件，即同一个rank不能重复join

#### client故障感知

client当前主要依赖以下几种方式识别某个rank的故障：

- server主动广播的断链消息
- 某个rank持有事件长时间未结束，主动向server查询该rank是否存活
- Get操作长时间未结束，即在做类似barrier等操作时等待太久，主动向server查询集群中是否有rank已经离线

### server故障场景分析

- server故障场景依赖etcd或其它组件完成主备倒换，需要拉起一个新的server，并让各个client向该server建链
- 备server处理逻辑为:被etcd选为主或被动的感知到有client的建链，则触发recover，进去recover状态，仅接受重连的cleint，不接受新链接，recover状态结束后(有固定时长)才支持新链接
- 在client发现和旧server断链后，自身会进入recover状态，当前状态不能正常工作，当server的recover结束后，会广播一个消息，通知各个client能正常工作了，并且告知recover阶段重连的所有rank信息，由client确认哪些rank离线了，从而发起linkdown事件
- 每个client在向备server建链后，都会同步一下server当前的event信息，如果server没有该信息则尝试把本地的event信息写入server侧，同时刷新自身的prefix_key到server侧
![image.png](https://raw.gitcode.com/user-images/assets/7672916/3cc65f64-237b-4e7d-8076-264867b17fe1/image.png 'image.png')
