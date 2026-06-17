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

#ifndef ZBAL_AICPU_THREAD_H
#define ZBAL_AICPU_THREAD_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_workspace.h"
#include "executor/zbal_aicpu_flag.h"
#include "executor/engine/sdma/zbal_aicpu_sdma_sqe.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

inline void AicpuCoreBarrier(volatile uint8_t *workspace, uint32_t numCores)
{
    if (numCores <= 1) {
        return;
    }
    volatile AicpuWorkspace::BarrierState *barrier = AicpuWorkspace::Barrier(workspace);
    volatile uint32_t *counter = &barrier->counter;
    volatile uint32_t *generation = &barrier->generation;
    uint32_t gen = __atomic_load_n(const_cast<uint32_t *>(generation), __ATOMIC_ACQUIRE);
    uint32_t arrived = __atomic_fetch_add(const_cast<uint32_t *>(counter), 1U, __ATOMIC_RELAXED) + 1U;
    if (arrived == numCores) {
        *const_cast<uint32_t *>(counter) = 0;
        __atomic_store_n(const_cast<uint32_t *>(generation), gen + 1U, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(const_cast<uint32_t *>(generation), __ATOMIC_ACQUIRE) == gen) {}
    }
}

inline int AicpuAtomicLaunchTask(uint32_t streamId, volatile stars_channel_info_t *channel, volatile uint8_t *workspace,
                                 uint32_t coreId, uint32_t numCores, const uint8_t *localBuf, uint32_t sqeCnt,
                                 uint32_t chIdx = 0)
{
    if (channel == nullptr) {
        return -1;
    }

    uint32_t sqDepth = channel->sq_depth;
    uint64_t sqBase = channel->sq_base;
    volatile uint32_t *sqTailAtomic =
        reinterpret_cast<volatile uint32_t *>(AicpuWorkspace::AtomicSqTail(workspace, streamId));
    volatile uint8_t *sqBasePtr = reinterpret_cast<volatile uint8_t *>(sqBase);

    /* Atomically reserve sqeCnt slots */
    uint32_t claimedTail = __atomic_fetch_add(const_cast<uint32_t *>(sqTailAtomic), sqeCnt, __ATOMIC_RELAXED);

    if (sqeCnt > 0 && localBuf != nullptr) {
        uint32_t hwBase = claimedTail % sqDepth;

        for (uint32_t i = 0; i < sqeCnt; i++) {
            AicpuStarsSdmaSqe *sqe =
                reinterpret_cast<AicpuStarsSdmaSqe *>(const_cast<uint8_t *>(localBuf) + i * ZBAL_AICPU_SQE_SIZE);
            sqe->header.task_id = static_cast<uint16_t>((hwBase + i) % sqDepth);

            uint32_t hwIdx = (claimedTail + i) % sqDepth;
            volatile uint8_t *dst = sqBasePtr + hwIdx * ZBAL_AICPU_SQE_SIZE;
            const uint8_t *src = localBuf + i * ZBAL_AICPU_SQE_SIZE;
            for (uint32_t j = 0; j < ZBAL_AICPU_SQE_SIZE; j++) {
                dst[j] = src[j];
            }
        }
    }

    __asm__ __volatile__("dsb st" : : : "memory");
    AicpuCoreBarrier(workspace, numCores);
    if (coreId == 0) {
        uint32_t totalSqes = __atomic_load_n(const_cast<uint32_t *>(sqTailAtomic), __ATOMIC_RELAXED);
        if (totalSqes > 0) {
            uint32_t flagSlot = __atomic_fetch_add(const_cast<uint32_t *>(sqTailAtomic), 1, __ATOMIC_RELAXED);
            uint32_t flagIdx = flagSlot % sqDepth;

            CompletionFlag flag(workspace, chIdx);
            flag.Setup();
            AicpuBuildFlagSqe(sqBasePtr + flagIdx * ZBAL_AICPU_SQE_SIZE, flag.SentinelAddr(), flag.DoneAddr(),
                              sizeof(uint64_t)); /* flag value size */

            flag.FlushSentinel();
            uintptr_t flushAddr = reinterpret_cast<uintptr_t>(sqBasePtr);
            uintptr_t flushEnd = flushAddr + sqDepth * ZBAL_AICPU_SQE_SIZE;
            constexpr uintptr_t cacheLineSize = 64;
            flushAddr &= ~(cacheLineSize - 1);
            for (; flushAddr < flushEnd; flushAddr += cacheLineSize) {
                AicpuCacheFlush(flushAddr);
            }
            uint32_t newTail = (flagSlot + 1) % sqDepth;
            AicpuSqDoorbell(channel, newTail);
        }
        return 0;
    }
    return 0;
}

#endif /* ZBAL_AICPU_THREAD_H */
