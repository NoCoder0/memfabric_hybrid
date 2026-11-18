/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
/*
 * Related upstream references; the original source is not established.
 * https://gitcode.com/cann/runtime/blob/v8.5.0/include/external/acl/acl_rt.h
 * Upstream copyright:
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * Compared with references: Selected ACL types.
 * Upstream copyright and license notices: Open_Source_Software_Notice.md
 */

#ifndef MF_HYBM_CORE_ACL_RT_DEF_H
#define MF_HYBM_CORE_ACL_RT_DEF_H

#include <cstddef>
#include <cstdint>

namespace ock {
namespace mf {

enum class aclrtMemLocationType {
    ACL_MEM_LOCATION_TYPE_HOST = 0, // Host内存
    ACL_MEM_LOCATION_TYPE_DEVICE,   // Device内存
};

using aclrtMemLocation = struct aclrtMemLocation;
struct aclrtMemLocation {
    uint32_t id;
    aclrtMemLocationType type; // 内存所在位置
};

using aclrtMemcpyBatchAttr = struct aclrtMemcpyBatchAttr;
struct aclrtMemcpyBatchAttr {
    aclrtMemLocation dstLoc;
    aclrtMemLocation srcLoc;
    uint8_t rsv[16];
};

typedef enum {
    ACL_STREAM_ATTR_FAILURE_MODE = 1,
    ACL_STREAM_ATTR_FLOAT_OVERFLOW_CHECK = 2,
    ACL_STREAM_ATTR_USER_CUSTOM_TAG = 3,
    ACL_STREAM_ATTR_CACHE_OP_INFO = 4,
} aclrtStreamAttr;

typedef union {
    uint64_t failureMode;
    uint32_t overflowSwitch;
    uint32_t userCustomTag;
    uint32_t cacheOpInfoSwitch;
    uint32_t reserve[4];
} aclrtStreamAttrValue;

using aclError = int32_t;
constexpr aclError ACL_SUCCESS = 0;

// CANN runtime 通用内部错误码：rtIpcOpenMemory 等接口返回。plog 证据表明该码在 IPC open 上
// 覆盖两类对端未就绪的瞬时拒绝：白名单未设置（drv: Find wlist fail / Wlist verify fail）
// 与节点不可见（drv: Query ipc node attr fail）；调用方可按可重试处理
constexpr aclError ACL_RT_INTERNAL_ERROR = 507899;

// 驱动 IPC 名字节点不存在：RtIpcDestroyMemoryName 对已销毁/对端先行退出的共享名返回，
// teardown 期属预期场景
constexpr aclError ACL_RT_IPC_NAME_NOT_FOUND = 107002;

// stream 被 aclrtStreamAbort 中止后，对该 stream 的同步接口（aclrtSynchronizeStream 等）返回该码。
constexpr aclError ACL_ERROR_RT_STREAM_ABORT = 107023;
using aclrtBinHandle = void *;
using aclrtFuncHandle = void *;
using aclrtArgsHandle = void *;
using aclrtParamHandle = void *;

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

typedef enum aclrtLaunchKernelAttrId {
    ACL_RT_LAUNCH_KERNEL_ATTR_SCHEM_MODE = 1, // 调度模式
    ACL_RT_LAUNCH_KERNEL_ATTR_DYN_UBUF_SIZE = 2, // 用于指定SIMT算子执行时需要的VECTOR CORE内部UB buffer的大小
    ACL_RT_LAUNCH_KERNEL_ATTR_ENGINE_TYPE = 3,     // 算子执行引擎
    ACL_RT_LAUNCH_KERNEL_ATTR_BLOCKDIM_OFFSET,     // numBlocks偏移量
    ACL_RT_LAUNCH_KERNEL_ATTR_BLOCK_TASK_PREFETCH, // 任务下发时，是否阻止硬件预取本任务的信息
    ACL_RT_LAUNCH_KERNEL_ATTR_DATA_DUMP,           // 是否开启Dump
    ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT,             // 任务调度器等待任务执行的超时时间，单位秒
    ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT_US = 8,      // 任务调度器等待任务执行的超时时间，单位微秒
} aclrtLaunchKernelAttrId;

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

typedef enum aclrtMemMallocPolicy {
    ACL_MEM_MALLOC_HUGE_FIRST,
    ACL_MEM_MALLOC_HUGE_ONLY,
    ACL_MEM_MALLOC_NORMAL_ONLY,
    ACL_MEM_MALLOC_HUGE_FIRST_P2P,
    ACL_MEM_MALLOC_HUGE_ONLY_P2P,
    ACL_MEM_MALLOC_NORMAL_ONLY_P2P,
    ACL_MEM_MALLOC_HUGE1G_ONLY,
    ACL_MEM_MALLOC_HUGE1G_ONLY_P2P,
    ACL_MEM_TYPE_LOW_BAND_WIDTH = 0x0100U,
    ACL_MEM_TYPE_HIGH_BAND_WIDTH = 0x1000U,
    ACL_MEM_ACCESS_USER_SPACE_READONLY = 0x100000U,
} aclrtMemMallocPolicy;

constexpr uint64_t ACL_NOTIFY_FLAG_DEVICE_ONLY = 0x00000001U;

} // namespace mf
} // namespace ock

#endif // MF_HYBM_CORE_ACL_RT_DEF_H
