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

#ifndef ZBAL_AICPU_DEFINES_H
#define ZBAL_AICPU_DEFINES_H

#include <cstdint>
#include "zbal_comm_host_device_struct.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/* Error codes */
constexpr int ERR_CHANNEL_INVALID = -2;
constexpr int ERR_CAPACITY_EXCEEDED = -3;
constexpr int ERR_HAL_LOAD_FAILED = -4;
constexpr int ERR_DOORBELL_FAILED = -5;
constexpr int ERR_WAIT_TIMEOUT = -6;
constexpr int ERR_WORKSPACE_CORRUPT = -7;

/* Sentinel for CopyData remoteRank: indicates local (same-device) copy */
constexpr uint32_t ZBAL_SDMA_RANK_LOCAL = 0xFFFFFFFF;

/* Aliases for device-side names that differ from shared header */
constexpr uint32_t ZBAL_AICPU_DEBUG_BUFFER_OFFSET = ZBAL_AICPU_DEBUG_BUF_OFFSET;
constexpr uint32_t ZBAL_AICPU_DEBUG_BUFFER_SIZE = ZBAL_AICPU_DEBUG_BUF_SIZE;
constexpr uint32_t ZBAL_AICPU_CORE_RINGBUF_SIZE = ZBAL_AICPU_CORE_RING_BUF_SIZE;

/* Device-only constants */
constexpr uint32_t ZBAL_AICPU_RING_NUM = 2;
constexpr uint32_t ZBAL_AICPU_MAX_CH_PER_CORE = 8;

/* Exchange area: exch[rank * stride] = output buffer GVA per rank */
constexpr uint32_t ZBAL_AICPU_EXCHANGE_STRIDE = 8;

enum AicpuCommAlg : uint32_t { ZBAL_COMM_ALG_FULL_MESH = 0, ZBAL_COMM_ALG_DOUBLE_RING = 1, ZBAL_COMM_ALG_MAX };

/* ================================================================
 * Cache / barrier utilities — centralized for readability and tuning.
 * All use dsb ish (inner-shareable): AICPU + SDMA are in same domain.
 * ================================================================ */

/* Invalidate cache line at addr, then barrier — ensures subsequent load sees DMA-written value */
inline void AicpuCacheInvalidate(uintptr_t addr)
{
    __asm__ __volatile__("dc civac, %0" ::"r"(addr) : "memory");
    __asm__ __volatile__("dsb ish" ::: "memory");
}

/* Flush dirty cache line at addr to PoC, then barrier — ensures DMA can read CPU-written value */
inline void AicpuCacheFlush(uintptr_t addr)
{
    __asm__ __volatile__("dc cvac, %0" ::"r"(addr) : "memory");
    __asm__ __volatile__("dsb ish" ::: "memory");
}

/* Memory barrier (inner-shareable) — orders all prior loads+stores before subsequent accesses */
inline void AicpuMemBarrier()
{
    __asm__ __volatile__("dsb ish" ::: "memory");
}

/* Exchange context — passed to all exchange implementations */
struct ExchangeContext {
    const AicpuInitContext *aicpuCtx;   /* rankId, rankNum, exchangeGva */
    const volatile AicpuWorkDesc *desc; /* sendBuffer, waitSymbol, etc. */
    volatile uint8_t *workspace;        /* flags + ringBufs */
    volatile stars_channel_info_t **channels;
    uint32_t numChPerCore;
    uint32_t numCores;
    uint32_t coreId;
};

#endif /* ZBAL_AICPU_DEFINES_H */
