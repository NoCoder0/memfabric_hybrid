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

#ifndef ZBAL_AICPU_FLAG_H
#define ZBAL_AICPU_FLAG_H

#include <cstdint>
#include "executor/zbal_aicpu_workspace.h"

/* CompletionFlag — encapsulates done_flag/sentinel setup, flush, and poll.
 * Each (core, stream) pair has its own flag in workspace. */
class CompletionFlag {
public:
    /* Construct by (coreId, chPerCore, streamId) */
    CompletionFlag(volatile uint8_t *ws, uint32_t coreId, uint32_t chPerCore, uint32_t streamId)
        : CompletionFlag(ws, AicpuWorkspace::FlagIdx(coreId, chPerCore, streamId))
    {}

    /* Construct by direct flag index */
    CompletionFlag(volatile uint8_t *ws, uint32_t flagIdx) : idx_(flagIdx)
    {
        done_ = AicpuWorkspace::DoneFlag(ws, idx_);
        sentinel_ = AicpuWorkspace::Sentinel(ws, idx_);
    }

    /* Set done=0, sentinel=1 before submitting SQEs */
    void Setup()
    {
        *done_ = 0;
        *sentinel_ = 1;
    }

    /* Flush sentinel to PoC so STARS DMA can read it */
    void FlushSentinel()
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(sentinel_));
        constexpr uintptr_t cacheLineSize = 64;
        addr &= ~(cacheLineSize - 1);
        __asm__ __volatile__("dc cvac, %0" ::"r"(addr) : "memory");
    }

    /* Poll done_flag until STARS writes 1 (flag SQE completed).
     * Returns 0 on success, -1 on timeout. */
    int Wait(uint32_t timeoutUs = 6000000)
    {
        constexpr int POLL_BACKOFF_ITERS = 200;
        for (uint32_t t = 0; t < timeoutUs; t++) {
            uintptr_t fa = reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(done_));
            AicpuCacheInvalidate(fa);
            if (*done_ == 1)
                return 0;
            for (volatile int i = 0; i < POLL_BACKOFF_ITERS; i++) {}
        }
        return -1;
    }

    uint64_t DoneAddr() const
    {
        return reinterpret_cast<uint64_t>(const_cast<uint64_t *>(done_));
    }
    uint64_t SentinelAddr() const
    {
        return reinterpret_cast<uint64_t>(const_cast<uint64_t *>(sentinel_));
    }

private:
    uint32_t idx_;
    volatile uint64_t *done_;
    volatile uint64_t *sentinel_;
};

#endif /* ZBAL_AICPU_FLAG_H */
