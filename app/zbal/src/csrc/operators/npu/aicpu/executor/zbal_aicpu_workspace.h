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

#ifndef ZBAL_AICPU_WORKSPACE_H
#define ZBAL_AICPU_WORKSPACE_H

#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "zbal_comm_host_device_struct.h"

namespace AicpuWorkspace {

/* Flag completion entry: done + sentinel */
struct FlagEntry {
    volatile uint64_t done;
    volatile uint64_t sentinel;
};

/* Core barrier: counter + generation */
struct BarrierState {
    volatile uint32_t counter;
    volatile uint32_t generation;
};

/* The AICPU-managed workspace area */
struct AicpuWorkspaceLayout {
    AicpuInitContext initCtx;
    uint8_t initCtxPad[256 - sizeof(AicpuInitContext)];
    uint8_t debugBuffer[ZBAL_AICPU_DEBUG_BUFFER_SIZE];
    BarrierState barrier;
    volatile uint64_t atomicSqTail[ZBAL_AICPU_CH_PER_CORE];
    volatile uint32_t coreIdCounter;
    uint8_t controlPad[20];
    FlagEntry flags[ZBAL_AICPU_NUM_CORES * ZBAL_AICPU_MAX_CH_PER_CORE];
    uint8_t ringBuffers[ZBAL_AICPU_NUM_CORES][ZBAL_AICPU_CORE_RINGBUF_SIZE];
};

/* Get pointer to the AICPU area within workspace */
inline AicpuWorkspaceLayout *GetArea(volatile uint8_t *workspace)
{
    return reinterpret_cast<AicpuWorkspaceLayout *>(const_cast<uint8_t *>(workspace) + ZBAL_AICPU_INIT_CTX_OFFSET);
}

/* Computed offsets from workspace base (for verification and external use) */
constexpr uint32_t K_DEBUG_BUF_OFF = ZBAL_AICPU_INIT_CTX_OFFSET + 256;
constexpr uint32_t K_DEBUG_BUF_SIZE = ZBAL_AICPU_DEBUG_BUFFER_SIZE;

/* flag index */
inline uint32_t FlagIdx(uint32_t coreId, uint32_t chPerCore, uint32_t streamId)
{
    return coreId * chPerCore + streamId;
}

/* get flag done pointer */
inline volatile uint64_t *DoneFlag(volatile uint8_t *ws, uint32_t flagIdx)
{
    return &GetArea(ws)->flags[flagIdx].done;
}

/* get flag sentinel pointer */
inline volatile uint64_t *Sentinel(volatile uint8_t *ws, uint32_t flagIdx)
{
    return &GetArea(ws)->flags[flagIdx].sentinel;
}

/* get core ring buffer base */
inline volatile uint8_t *CoreRingBuf(volatile uint8_t *ws, uint32_t coreId)
{
    return reinterpret_cast<volatile uint8_t *>(GetArea(ws)->ringBuffers[coreId]);
}

/* get coreId counter */
inline volatile uint32_t *CoreIdCounter(volatile uint8_t *ws)
{
    return &GetArea(ws)->coreIdCounter;
}

/* get barrier state */
inline volatile BarrierState *Barrier(volatile uint8_t *ws)
{
    return &GetArea(ws)->barrier;
}

/* get atomic SQ tail */
inline volatile uint64_t *AtomicSqTail(volatile uint8_t *ws, uint32_t streamId)
{
    return &GetArea(ws)->atomicSqTail[streamId];
}
} // namespace AicpuWorkspace

#endif /* ZBAL_AICPU_WORKSPACE_H */
