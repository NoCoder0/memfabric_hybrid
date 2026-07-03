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
#ifndef ZBAL_COMM_STRUCT_H
#define ZBAL_COMM_STRUCT_H

#include <string>
#include <vector>
#include <cstdint>

#define ZBAL_FLAG_SIZE 8

#define ZBAL_MAX_AIV_SIZE_PER_NPU 48

#define ZBAL_PROFILING_DEVICE_IDX_OFF   8
#define ZBAL_PROFILING_DEVICE_TRACE_OFF 16
#define ZBAL_PROFILING_FRAME_SHIFT      56
#define ZBAL_PROFILING_BE_SHIFT         62
#define ZBAL_MAX_RANKS                  1024

enum zbal_profiling_name_t : uint16_t {
    ZBAL_PROF_UNKNOWN = 0,
    ZBAL_PROF_LINENO,
    ZBAL_PROF_BARRIER,

    ZBAL_PROF_ALLGATHER_KERNEL_ALL,
    ZBAL_PROF_ALLGATHER_COPY,
    ZBAL_PROF_ALLGATHER_LOCAL_COPY,
    ZBAL_PROF_ALLGATHER_PREPARE_PTR,

    ZBAL_PROF_REDUCESCATTER_KERNEL_ALL,
    ZBAL_PROF_REDUCESCATTER_LOCAL_COPY,
    ZBAL_PROF_REDUCESCATTER_COPY,

    ZBAL_PROF_ALLTOALL_KERNEL_ALL,
    ZBAL_PROF_ALLTOALL_COPY,
    ZBAL_PROF_ALLTOALL_PREPARE_PTR,
    ZBAL_PROF_ALLTOALL_CORE_RANGE,
    ZBAL_PROF_ALLTOALL_INIT_STAT,
    ZBAL_PROF_ALLTOALL_ATOMIC_INC,
    ZBAL_PROF_ALLTOALL_WAIT_LOCAL_STAT,

    ZBAL_PROF_ALLREDUCE_KERNEL_ALL,
    ZBAL_PROF_ALLREDUCE_SCATTER_REDUCE,
    ZBAL_PROF_ALLREDUCE_ALLGATHER,

    ZBAL_PROF_BROADCAST_KERNEL_ALL,
    ZBAL_PROF_BROADCAST_SCATTER,
    ZBAL_PROF_BROADCAST_ALLGATHER,

    ZBAL_PROF_SCATTER_KERNEL_ALL,

    ZBAL_PROF_GATHER_KERNEL_ALL,
    ZBAL_PROF_GATHER_COPY,
    ZBAL_PROF_GATHER_PREPARE_PTR,

    ZBAL_PROF_SEND_KERNEL_ALL,
    ZBAL_PROF_RECV_KERNEL_ALL,

    ZBAL_PROF_EXCHANGE_ADDR,
    ZBAL_PROF_WAIT_FLAG,
    ZBAL_PROF_WRITE_STAT,
    ZBAL_PROF_WAIT_STAT,
    ZBAL_PROF_GM_2_UB,
    ZBAL_PROF_UB_2_GM,
    ZBAL_PROF_BUTT,
};

const std::vector<std::pair<std::string, bool>> g_profName = {
    {"UNKNOWN", false},
    {"LINE_NO", true},
    {"BARRIER", false},

    {"AG_KERNEL_ALL", true},
    {"AG_COPY", true},
    {"AG_LOCAL_COPY", true},
    {"AG_PREPARE_PTR", false},

    {"RS_KERNEL_ALL", false},
    {"RS_LOCAL_COPY", false},
    {"RS_COPY", false},

    {"A2A_KERNEL_ALL", true},
    {"A2A_COPY", true},
    {"A2A_PREPARE_PTR", true},
    {"A2A_CORE_RANGE", true},
    {"A2A_INIT_STAT", true},
    {"A2A_ATOMIC_INC", true},
    {"A2A_WAIT_LOCAL_STAT", true},

    {"AR_KERNEL_ALL", false},
    {"AR_REDUCE", false},
    {"AR_GATHER", false},

    {"BR_KERNEL_ALL", false},
    {"BR_SCATER_PART", false},
    {"BR_GATHER_PART", false},

    {"SC_KERNEL_ALL", false},

    {"GA_KERNEL_ALL", true},
    {"GA_COPY", true},
    {"GA_PREPARE_PTR", false},

    {"SEND_KERNEL_ALL", false},
    {"RECV_KERNEL_ALL", false},

    {"EXCHANGE_ADDR", true},
    {"WAIT_FLAG", true},
    {"WRITE_STAT", true},
    {"WAIT_STAT", true},
    {"GM_2_UB", false},
    {"UB_2_GM", false},
};

struct RankCoreMapping {
    uint16_t groupSize;
    uint32_t blockDim;
    uint64_t start;
    uint64_t end;
};

/**
 * @brief group info of this communicator, this struct will be copy to device, keep it simple
 */
struct CommGroupInfo {
    uint16_t groupSize = 0;                                /* the ranks in the group */
    uint16_t myGroupRank = 0;                              /* rank id in the group */
    uintptr_t myMetaGva = 0;                               /* gva of mine */
    uintptr_t myParamDataGva = 0;                          /* gva of for param exchange of operation */
    uintptr_t myAddressExchangeGva = 0;                    /* gva of for address exchange of operation */
    uint64_t sizeForCommGroupInfo = 0;                     /* max memory size of passing param from host to device */
    uint64_t sizeForParam = 0;                             /* max memory size of exchange param */
    uint64_t sizeForExchangeAddress = 0;                   /* max memory size for exchange operation data addresses */
    uint64_t fftsConfig;                                   /* copy from CommGroupOptions.fftsConfig */
    uint16_t peerGroupRank2WorldRank[ZBAL_MAX_RANKS] = {}; /* rank id in group to world rank id relationship */
    uint64_t localDeviceMemSize;                           /* copy from CommGroupOptions.localDeviceMemSize */
    uint64_t waitSymbol;                                   /* wait symbol for op cross ranks */
    uintptr_t hostMemoryForProfiling;                      /* memory for perf at host side */
    uintptr_t devMemoryForProfiling;                       /* memory for perf at host side */
    uint64_t tracePointPerCore;                            /* tracing points per core */
    uint16_t groupIndex;                                   /* index of this group, start from 0 */
    uint32_t dataOpType;                                   /* data operation type, see zbal_data_op_type_t */
};

/* Data type values — shared between host and device, matches zbal_datatype_t in zbal_def.h */
constexpr uint32_t ZBAL_DTYPE_INT8 = 0;
constexpr uint32_t ZBAL_DTYPE_INT16 = 1;
constexpr uint32_t ZBAL_DTYPE_INT32 = 2;
constexpr uint32_t ZBAL_DTYPE_INT64 = 3;
constexpr uint32_t ZBAL_DTYPE_UINT64 = 4;
constexpr uint32_t ZBAL_DTYPE_FP64 = 5;
constexpr uint32_t ZBAL_DTYPE_FP16 = 6;
constexpr uint32_t ZBAL_DTYPE_FP32 = 7;
constexpr uint32_t ZBAL_DTYPE_BFP16 = 8;
constexpr uint32_t ZBAL_DTYPE_UINT8 = 9;
constexpr uint32_t ZBAL_DTYPE_UINT16 = 10;
constexpr uint32_t ZBAL_DTYPE_UINT32 = 11;

/* Unified data type size function (host + device shared) */
inline uint32_t ZBALDataTypeSize(uint32_t dataType)
{
    switch (dataType) {
        case ZBAL_DTYPE_INT8:
        case ZBAL_DTYPE_UINT8:
            return 1; /* 1 byte */
        case ZBAL_DTYPE_INT16:
        case ZBAL_DTYPE_UINT16:
        case ZBAL_DTYPE_FP16:
        case ZBAL_DTYPE_BFP16:
            return 2; /* 2 bytes */
        case ZBAL_DTYPE_INT32:
        case ZBAL_DTYPE_UINT32:
        case ZBAL_DTYPE_FP32:
            return 4; /* 4 bytes */
        case ZBAL_DTYPE_INT64:
        case ZBAL_DTYPE_UINT64:
        case ZBAL_DTYPE_FP64:
            return 8; /* 8 bytes */
        default:
            return 1; /* fallback: 1 byte */
    }
}

/* ================================================================
 * AICPU workspace layout constants and shared structs (host + device)
 * ================================================================ */
constexpr uint32_t ZBAL_AICPU_INIT_CTX_OFFSET = 4096;
constexpr uint32_t ZBAL_AICPU_DEBUG_BUF_OFFSET = 4352;
constexpr uint32_t ZBAL_AICPU_DEBUG_BUF_SIZE = 4096;

/* ── Compile-time maximums (sized for worst-case op/algorithm) ──
 *  These determine the static workspace layout. Per-op runtime values
 *  (numCores / numChPerCore) flow through AicpuWorkDesc and may be smaller.
 *
 *  Known per-op requirements:
 *    - AllGather doublering / mesh_doublering : 2 cores × 1 channel
 *    - AllGather fullmesh                     : 1 core  × 8 channels
 *    - AlltoAllV                              : 1 core  × 8 channels
 *    - ReduceScatter                           : 1 core  × 16 channels
 *    - AllReduce / Broadcast / Scatter         : 1 core  × 8 channels (default)
 *    - Send / Recv                            : 1 core  × 1 channel
 */
constexpr uint32_t ZBAL_AICPU_MAX_NUM_CORES = 2;    /* doublering uses 2 cores */
constexpr uint32_t ZBAL_AICPU_MAX_CH_PER_CORE = 16; /* reducescatter uses 16 channels */

constexpr uint32_t ZBAL_AICPU_CORE_DATA_OFFSET = 9216;
constexpr uint32_t ZBAL_AICPU_SQE_SIZE = 64;
constexpr uint32_t ZBAL_AICPU_MAX_SQE_PER_CORE = 256; /* per channel */
constexpr uint32_t ZBAL_AICPU_PER_CH_RINGBUF_SIZE = ZBAL_AICPU_SQE_SIZE * ZBAL_AICPU_MAX_SQE_PER_CORE;
constexpr uint32_t ZBAL_AICPU_CORE_RING_BUF_SIZE = ZBAL_AICPU_PER_CH_RINGBUF_SIZE * ZBAL_AICPU_MAX_CH_PER_CORE;
constexpr uint32_t ZBAL_AICPU_TOTAL_RING_BUF_SIZE = ZBAL_AICPU_CORE_RING_BUF_SIZE * ZBAL_AICPU_MAX_NUM_CORES;
constexpr uint32_t ZBAL_AICPU_WORKSPACE_TOTAL_SIZE = ZBAL_AICPU_CORE_DATA_OFFSET + ZBAL_AICPU_TOTAL_RING_BUF_SIZE;

/* AICPU comm type — shared between host and device */
enum AicpuCommType : uint32_t {
    ZBAL_CMD_INIT = 0,
    ZBAL_CMD_ALLGATHER = 1,
    ZBAL_CMD_SCATTER = 2,
    ZBAL_CMD_REDUCE_SCATTER = 3,
    ZBAL_CMD_BROADCAST = 4,
    ZBAL_CMD_ALLREDUCE = 5,
    ZBAL_CMD_ALLTOALLV = 6,
    ZBAL_CMD_SEND = 7,
    ZBAL_CMD_RECV = 8,
    ZBAL_CMD_FINALIZE = 0xFF
};

/* AICPU comm algorithm — shared between host and device */
enum AicpuCommAlg : uint32_t {
    ZBAL_COMM_ALG_DEFAULT = 0,
    ZBAL_COMM_ALG_FULL_MESH = 1,
    ZBAL_COMM_ALG_DOUBLE_RING = 2,
    ZBAL_COMM_ALG_MESH_DOUBLE_RING = 3,
    ZBAL_COMM_ALG_MAX
};

/* Kernel param — passed via aclrtKernelArgsAppend, ACL runtime handles H2D automatically. */
struct AicpuWorkDesc {
    uint64_t sdmaWorkspaceGva; /* SDMA workspace GVA */
    uint32_t commType;         /* ZBAL_CMD_ALLGATHER / SCATTER / ... */
    uint32_t commAlg;          /* resolved algorithm (host fills via SelectCommAlg, device reads desc->commAlg) */
    uint64_t sendBuffer;       /* send buffer GVA */
    uint64_t recvBuffer;       /* recv buffer GVA */
    uint64_t buffer;           /* scratch buffer GVA (AllReduce temp workspace) */
    uint64_t count;            /* data byte count */
    uint32_t dataType;         /* zbal_datatype_t enum */
    uint32_t root;             /* root rank (scatter/broadcast) / peer (send/recv) */
    uint32_t reduceOp;         /* zbal_reduce_op_t (PROD=0, SUM=1, MAX=2, MIN=3) — maps to SDMA opCode */
    uint32_t numCores;         /* runtime per-op core count (host sets via GetCommOpConfig) */
    uint32_t numChPerCore;     /* runtime per-op channels per core (host sets via GetCommOpConfig) */
    uint64_t waitSymbol;       /* incrementing barrier flag — avoids cross-call stale-flag races */
    uint64_t reserved[4];      /* per-op extension: AlltoAllV uses [0]=sendCumSum, [1]=recvSplitCounts, [2]=elements */
};

struct AicpuInitContext {
    uint32_t rankId;
    uint32_t rankNum;
    uint64_t localDeviceMemSize;
    uint64_t exchangeGva;
};

#endif // ZBAL_COMM_STRUCT_H
