/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef DL_CANN_API_DEF_H
#define DL_CANN_API_DEF_H

namespace zbal {
namespace underapi {
using aclrtBinHandle = void *;
using aclrtFuncHandle = void *;
using aclrtArgsHandle = void *;
using aclrtParamHandle = void *;
using aclrtStream = void *;
using aclrtContext = void *;
using aclError = int32_t;
using aclrtMemAttr = uint32_t;
using aclrtMemHandleType = uint32_t;
using aclrtMemAllocationType = uint32_t;
using aclrtMemLocationType = uint32_t;
using aclrtDevResLimitType = uint32_t;

typedef enum {
    ACL_RT_DEV_RES_CUBE_CORE = 0, /* AI Core | Cube Core */
    ACL_RT_DEV_RES_VECTOR_CORE,   /* Vector Core */
} aclrtDevResType;

typedef enum {
    ACL_HOST_REGISTER_MAPPED = 0U, /* accessed by NPU */
} aclrtHostRegisterType;

typedef enum aclrtMemcpyKind {
    ACL_MEMCPY_HOST_TO_HOST,
    ACL_MEMCPY_HOST_TO_DEVICE,
    ACL_MEMCPY_DEVICE_TO_HOST,
    ACL_MEMCPY_DEVICE_TO_DEVICE,
    ACL_MEMCPY_DEFAULT,
    ACL_MEMCPY_HOST_TO_BUF_TO_DEVICE,
    ACL_MEMCPY_INNER_DEVICE_TO_DEVICE,
    ACL_MEMCPY_INTER_DEVICE_TO_DEVICE,
} aclrtMemcpyKind;

typedef enum aclrtMemMallocPolicy {
    ACL_MEM_MALLOC_HUGE_FIRST,
    ACL_MEM_MALLOC_HUGE_ONLY,
    ACL_MEM_MALLOC_NORMAL_ONLY,
    ACL_MEM_MALLOC_HUGE_FIRST_P2P,
    ACL_MEM_MALLOC_HUGE_ONLY_P2P,
    ACL_MEM_MALLOC_NORMAL_ONLY_P2P,
    ACL_MEM_MALLOC_HUGE1G_ONLY,
    ACL_MEM_MALLOC_HUGE1G_ONLY_P2P,
    ACL_MEM_TYPE_LOW_BAND_WIDTH = 0x0100,
    ACL_MEM_TYPE_HIGH_BAND_WIDTH = 0x1000,
    ACL_MEM_ACCESS_USER_SPACE_READONLY = 0x100000,
} aclrtMemMallocPolicy;

enum class aclrtBinaryLoadOptionType : int32_t {
    ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD = 1,
    ACL_RT_BINARY_LOAD_OPT_LAZY_MAGIC = 2,
    ACL_RT_BINARY_LOAD_OPT_MAGIC = 2,
    ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE = 3,
};

union aclrtBinaryLoadOptionValue {
    uint32_t isLazyLoad;
    uint32_t magic;
    int32_t cpuKernelMode;
    uint32_t rsv[4];
};

struct aclrtBinaryLoadOption {
    aclrtBinaryLoadOptionType type;
    aclrtBinaryLoadOptionValue value;
};

struct aclrtBinaryLoadOptions {
    aclrtBinaryLoadOption *options;
    size_t numOpt;
};

enum class aclrtLaunchKernelAttrId : int32_t {
    ACL_RT_LAUNCH_KERNEL_ATTR_SCHEM_MODE = 1,
    ACL_RT_LAUNCH_KERNEL_ATTR_LOCAL_MEMORY_SIZE = 2,
    ACL_RT_LAUNCH_KERNEL_ATTR_ENGINE_TYPE = 3,
    ACL_RT_LAUNCH_KERNEL_ATTR_NUMBLOCKS_OFFSET = 4,
    ACL_RT_LAUNCH_KERNEL_ATTR_BLOCK_TASK_PREFETCH = 5,
    ACL_RT_LAUNCH_KERNEL_ATTR_DATA_DUMP = 6,
    ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT = 7,
    ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT_US = 8,
};

struct aclrtTimeoutUs {
    uint32_t timeoutLow;
    uint32_t timeoutHigh;
};

union aclrtLaunchKernelAttrValue {
    uint8_t schemMode;
    uint32_t localMemorySize;
    uint32_t engineType;
    uint32_t numBlocksOffset;
    uint8_t isBlockTaskPrefetch;
    uint8_t isDataDump;
    uint16_t timeout;
    aclrtTimeoutUs timeoutUs;
    uint32_t rsv[4];
};

struct aclrtLaunchKernelAttr {
    aclrtLaunchKernelAttrId id;
    aclrtLaunchKernelAttrValue value;
};

struct aclrtLaunchKernelCfg {
    aclrtLaunchKernelAttr *attrs;
    size_t numAttrs;
};

typedef struct aclrtMemLocation {
    uint32_t id;
    aclrtMemLocationType type;
} aclrtMemLocation;

typedef struct aclrtPhysicalMemProp {
    aclrtMemHandleType handleType;
    aclrtMemAllocationType allocationType;
    aclrtMemAttr memAttr;
    aclrtMemLocation location;
    uint64_t reserve;
} aclrtPhysicalMemProp;
} // namespace underapi
} // namespace zbal

#endif // DL_CANN_API_DEF_H
