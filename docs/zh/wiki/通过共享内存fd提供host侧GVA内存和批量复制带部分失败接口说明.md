# 通过共享内存fd提供host侧GVA内存和批量复制带部分失败接口说明

[TOC]

## 1 通过共享内存fd提供host侧GVA内存

### 1.1 对应接口说明

关联的接口如下：

```cpp
/**
 * @brief Create a Big Memory object locally after initialized, this only create local memory segment and after
 * call <i>smem_bm_join</i> the local memory segment will be joined into global space. One Big Memory object is
 * a global memory space, data operation does work across different Big Memory object.
 * We need to specify different <i>id</i> for different Big Memory object.
 *
 * @param id               [in] identity of the Big Memory object
 * @param option           [in] smme bm create option
 * @return Big Memory object handle if successful, nullptr if failed
 */
smem_bm_t smem_bm_create2(uint32_t id, const smem_bm_create_option_t *option);
```

接口的参数列表与返回值类型没有变化，变化的是在结构体入参`const smem_bm_create_option_t *option`中增加一个参数:

```cpp
typedef struct {
    uint64_t maxDramSize;             /* the max size of all rank DRAM memory contributes to Big Memory object */
    uint64_t maxHbmSize;              /* the max size of all rank HBM memory contributes to Big Memory object */
    uint64_t localDRAMSize;           /* the size of local DRAM memory contributes to Big Memory object */
    uint64_t localHBMSize;            /* the size of local HBM memory contributes to Big Memory object */
    smem_bm_data_op_type dataOpType;  /* if tag or tagOpInfo is empty, use dataOpType */
    bool isSecondMapping;               /* whether support 128TB memory pool, default false */
    uint32_t flags;                   /* optional flags, default 0 */
    char tag[32];                     /* tag of bm, eg:tag_1 */
    char tagOpInfo[256];              /* optype of tag to tag, eg: tag1:DEVICE_SDMA:tag1,tag1:DEVICE_RDMA:tag2 */
    int dramShmFd;
} smem_bm_create_option_t;
```

最后一个字段`int dramShmFd`就是传入的共享内存文件描述符。要激活此功能，除了对`dramShmFd`赋值外，还需要设置结构体中的`flags`字段，将`bit`位`SMEM_BM_FLAG_CREATE_WITH_SHM`设置为`1`。

### 1.2 使用代码段示例

**1. 创建共享内存得到文件描述符**

```cpp
...
const char *shmName = ...;
auto shm_fd = memfd_create(shmName, MFD_CLOEXEC);
```

> [!NOTE] 说明
>
> - 系统函数`memfd_create`是用于创建匿名共享内存的方法，也可能通过其它方法创建共享内存，如`shm_open`。（不支持`System V` 共享内存）
> - `MFD_CLOEXEC`是示例中使用的`flag`，具体使用时根据实际情况填写。

**2. 构造参数创建bm对象**

```cpp
...
uint32_t instance_id = ..; // bm对象的id
smem_bm_create_option_t create_options{};

create_options.xx = xx; // 其它字段赋值
create_options.flags |= SMEM_BM_FLAG_CREATE_WITH_SHM; // 关键！设置flag
create_options.dramShmFd = shm_fd; // 关键！设置共享内存文件描述符

auto handle = smem_bm_create2(instance_id, &create_options);
```

## 2 批量复制带部分失败接口说明

原有复制接口原型如下：

```cpp
/**
 * @brief Perform batch data copy operations on Big Memory
 * @param handle           [in] Big Memory object handle, created and returned by the smem_bm_create function,
 *                              used to identify the target shared memory instance
 * @param params           [in] Pointer to the batch copy parameter structure
 *                              - sources: Pointer to an array of source addresses
 *                              - destinations: Pointer to an array of destination addresses
 *                              - dataSizes: Pointer to an array of data lengths
 *                              - batchSize: Number of copy groups in the batch
 * @param t                [in] copy type, L2G, G2L, G2H, H2G
 * @param flags            [in] optional flags
 * @return 0 if successful
 */
int32_t smem_bm_copy_batch(smem_bm_t handle, smem_batch_copy_params *params, smem_bm_copy_type t, uint32_t flags);

```

此接口对于批量操作，只能返回成功或失败，无法得到批量操作中的具体哪一个IO成功，哪一个失败，为此增加一个接口：

```cpp
/**
  * @brief Perform batch data copy operations on Big Memory
  * @param handle           [in] Big Memory object handle, created and returned by the smem_bm_create function,
  *                              used to identify the target shared memory instance
  * @param params           [in] Pointer to the batch copy parameter structure
  *                              - sources: Pointer to an array of source addresses
  *                              - destinations: Pointer to an array of destination addresses
  *                              - dataSizes: Pointer to an array of data lengths
  *                              - batchSize: Number of copy groups in the batch
  * @param t                [in] copy type, L2G, G2L, G2H, H2G
  * @param flags            [in] optional flags
  * @param result           [out] When the error code is SMEM_PARTIAL_FAILED, the code indicates which operations are
  *                               successful and which operations have failed.
  * @return 0 if successful
  */
int32_t smem_bm_copy_batch_partial_succeed(smem_bm_t handle, smem_batch_copy_params *params, smem_bm_copy_type t,
                                           uint32_t flags, smem_batch_copy_result *result);
```

相对于前一个接口，增加一个出参`uint32_t flags, smem_batch_copy_result *result`，出参类型如下：

```cpp
typedef struct {
    int32_t *results;
    uint32_t batchSize;
} smem_batch_copy_result;
```

> [!NOTE] 使用说明
>
> - `result`中的`batchSize`的值由调用者指定，必须与入参`params`中的`batchSize`值相同。
> - `result`中的指针字段`int32_t *results`需要由调用者申请好，长度不小于`sizeof(int32_t) * batchSize`。
> - 返回值需要判断以下三种类型：
    >
    >   - SMEM_OK(0) 批量中全部成功，`MemFabric`不会填充`result`中的值，此时调用者根据返回值直接认为复制全部成功；
    >   - SMEM_PARTIAL_FAILED(-2012) 批量复制部分成功，调用者需要判断`result`中的指针字段`int32_t *results`，判断其中具体哪个成功，哪个失败。
    >   - 其它值，批量中全部失败，`MemFabric`不会填充`result`中的值，此时调用者根据返回值直接认为复制全部失败。

### 2.2 使用代码段示例

```cpp
...
uint32_t batch_size = ...;
smem_bm_copy_type copy_type = ...;
uint32_t flags = ...;

std::vector<void *> sourcesv;
std::vector<void *> destinationsv;
std::vector<uint64_t> lengthsv;
std::vector<int32_t> resultsv(batch_size);

// 填写批量中的信息到sourcesv， destinationsv， lengthsv
...

mem_batch_copy_params batch_params = {sourcesv.data(),  destinationsv.data(), lengthsv.data(),  batch_size};
smem_batch_copy_result copy_result = {resultsv.data(), batch_size};
auto ret = smem_bm_copy_batch_partial_succeed(handle, &batch_params, copy_type, flags, &copy_result);
if (ret == SMEM_OK) {
    //  全部成功处理
} else if (ret != SMEM_PARTIAL_FAILED) {
    // 全部失败处理
} else {
    for (auto i = 0U; i < batch_size; i++) {
        if (resultsv[i] == SMEM_OK) {
            // 批量中的第i个成功
        } else {
            // 批量中的第i个失败
        }
    }
}

```
