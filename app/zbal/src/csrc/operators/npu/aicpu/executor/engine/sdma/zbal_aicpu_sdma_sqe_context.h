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

#ifndef ZBAL_AICPU_SDMA_SQE_CONTEXT_H
#define ZBAL_AICPU_SDMA_SQE_CONTEXT_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_workspace.h"
#include "executor/zbal_aicpu_flag.h"
#include "executor/engine/sdma/zbal_aicpu_sdma_sqe.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"
#include "executor/zbal_aicpu_thread.h"

struct SqeLocalRingBuffer {
    uint8_t *localBuff;
    uint16_t tailSqeIdx;
    uint16_t sqeCnt;

    SqeLocalRingBuffer() : localBuff(nullptr), tailSqeIdx(0), sqeCnt(0) {}

    void Init(uint8_t *buf)
    {
        localBuff = buf;
        tailSqeIdx = 0;
        sqeCnt = 0;
    }

    uint8_t *NextAddr()
    {
        if (sqeCnt >= ZBAL_AICPU_MAX_SQE_PER_CORE) {
            return nullptr;
        }
        uint8_t *sqe = localBuff + tailSqeIdx * ZBAL_AICPU_SQE_SIZE;
        tailSqeIdx = (tailSqeIdx + 1) % ZBAL_AICPU_MAX_SQE_PER_CORE;
        sqeCnt++;
        return sqe;
    }

    bool HasWork() const
    {
        return sqeCnt > 0;
    }
};

inline void AicpuCoreRingbufsInit(SqeLocalRingBuffer *ringBufs, volatile uint8_t *workspace, uint32_t coreId)
{
    uint8_t *coreBase = const_cast<uint8_t *>(
        reinterpret_cast<const volatile uint8_t *>(AicpuWorkspace::CoreRingBuf(workspace, coreId)));
    for (uint32_t s = 0; s < ZBAL_AICPU_CH_PER_CORE; s++) {
        ringBufs[s].Init(coreBase + s * ZBAL_AICPU_SQE_SIZE * ZBAL_AICPU_MAX_SQE_PER_CORE);
    }
}

inline int AicpuLaunchTask(SqeLocalRingBuffer *buf, volatile stars_channel_info_t *channel, volatile uint8_t *workspace,
                           uint32_t chIdx)
{
    if (channel == nullptr || buf == nullptr || workspace == nullptr) {
        return ERR_CHANNEL_INVALID;
    }
    if (!buf->HasWork()) {
        return 0;
    }

    uint32_t sqDepth = channel->sq_depth;
    uint64_t sqBase = channel->sq_base;
    uint32_t curTail = channel->sq_tail;

    if (buf->sqeCnt + 1 >= sqDepth) {
        return ERR_CAPACITY_EXCEEDED;
    }

    CompletionFlag flag(workspace, chIdx);
    flag.Setup();

    for (uint32_t i = 0; i < buf->sqeCnt; i++) {
        AicpuStarsSdmaSqe *sqe = reinterpret_cast<AicpuStarsSdmaSqe *>(buf->localBuff + i * ZBAL_AICPU_SQE_SIZE);
        sqe->header.task_id = static_cast<uint16_t>((curTail + i) % sqDepth);
    }

    uint32_t totalSqe = buf->sqeCnt + 1;
    uint32_t flagIdx = (curTail + buf->sqeCnt) % sqDepth;
    volatile uint8_t *sqBasePtr = reinterpret_cast<volatile uint8_t *>(sqBase);
    AicpuBuildFlagSqe(sqBasePtr + flagIdx * ZBAL_AICPU_SQE_SIZE, flag.SentinelAddr(), flag.DoneAddr(),
                      sizeof(uint64_t)); /* flag value size */

    for (uint32_t i = 0; i < buf->sqeCnt; i++) {
        uint32_t hwIdx = (curTail + i) % sqDepth;
        volatile uint8_t *dst = sqBasePtr + hwIdx * ZBAL_AICPU_SQE_SIZE;
        const uint8_t *src = buf->localBuff + i * ZBAL_AICPU_SQE_SIZE;
        for (uint32_t j = 0; j < ZBAL_AICPU_SQE_SIZE; j++) {
            dst[j] = src[j];
        }
    }

    flag.FlushSentinel();
    uintptr_t flushEnd = reinterpret_cast<uintptr_t>(sqBasePtr) + (curTail + totalSqe) * ZBAL_AICPU_SQE_SIZE;
    uintptr_t flushAddr = reinterpret_cast<uintptr_t>(sqBasePtr) + curTail * ZBAL_AICPU_SQE_SIZE;
    constexpr uintptr_t cacheLineSize = 64;
    flushAddr &= ~(cacheLineSize - 1);
    for (; flushAddr < flushEnd; flushAddr += cacheLineSize) {
        __asm__ __volatile__("dc cvac, %0" ::"r"(flushAddr) : "memory");
    }
    AicpuMemBarrier();

    uint32_t newTail = (curTail + totalSqe) % sqDepth;
    AicpuSqDoorbell(channel, newTail);

    buf->tailSqeIdx = 0;
    buf->sqeCnt = 0;
    return 0;
}

inline int AicpuLaunchTaskMc(SqeLocalRingBuffer *buf, volatile stars_channel_info_t *channel,
                             volatile uint8_t *workspace, uint32_t coreId, uint32_t numCores, uint32_t streamId,
                             uint32_t chIdx)
{
    if (buf == nullptr || workspace == nullptr) {
        return ERR_CHANNEL_INVALID;
    }

    int ret;
    if (numCores <= 1) {
        if (!buf->HasWork()) {
            return 0;
        }
        ret = AicpuLaunchTask(buf, channel, workspace, chIdx);
    } else {
        ret = AicpuAtomicLaunchTask(streamId, channel, workspace, coreId, numCores, buf->localBuff, buf->sqeCnt, chIdx);
    }

    buf->tailSqeIdx = 0;
    buf->sqeCnt = 0;
    return ret;
}

/* Free function: batch submit all streams, then parallel wait.
 * Usable from any algorithm without circular include issues. */
inline int AicpuSubmitAndWait(SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                              uint32_t numChPerCore, volatile uint8_t *workspace, uint32_t coreId)
{
    bool submitted[ZBAL_AICPU_CH_PER_CORE];
    for (uint32_t s = 0; s < numChPerCore; s++) {
        submitted[s] = ringBufs[s].HasWork();
        if (!submitted[s]) {
            continue;
        }
        uint32_t fid = AicpuWorkspace::FlagIdx(coreId, numChPerCore, s);
        if (AicpuLaunchTaskMc(&ringBufs[s], channels[s], workspace, coreId, 1, s, fid) < 0) {
            return ERR_DOORBELL_FAILED;
        }
    }
    for (uint32_t s = 0; s < numChPerCore; s++) {
        if (!submitted[s]) {
            continue;
        }
        CompletionFlag flag(workspace, coreId, numChPerCore, s);
        if (flag.Wait() < 0) {
            return ERR_WAIT_TIMEOUT;
        }
    }
    return 0;
}

#endif /* ZBAL_AICPU_SDMA_SQE_CONTEXT_H */
