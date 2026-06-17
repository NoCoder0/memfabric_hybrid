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

#ifndef ZBAL_AICPU_ALLGATHER_DOUBLERING_H
#define ZBAL_AICPU_ALLGATHER_DOUBLERING_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/*
 * DoubleRing AllGather — mirrors ZBALAllGatherBigKernel::Process() with stat sync.
 *
 * Ring algorithm with CW + CCW propagation:
 *   - Data split in half: CW handles [0..N/2), CCW handles [N/2..N)
 *   - 4 cores: core 0,1 → CW ring (ch[0]), core 2,3 → CCW ring (ch[1])
 *   - Within each ring, cores are byte-sliced via AicpuParallelSlice
 *
 * Exchange area layout:
 *   [0 .. rankNum*64)              data area (output buffer GVA per rank)
 *   [rankNum*64 .. 2*rankNum*64)   flag area (cross-device sync)
 *   [2*rankNum*64 .. )             stat area (CW + CCW, statSizePerRank slots × 64B per rank)
 *
 * Execution (called 1 + groupSize times by AllGatherOp::Execute):
 *   Phase 1 (chunkActive=false): self-copy + write init stat → BUILD_MORE
 *   Phase 2 (iter=0..N-2): wait stat → copy data → write stat → BUILD_MORE
 *   Phase 3 (iter=N-1): wait stat → copy data → BUILD_DONE
 *
 * Stat sync protocol (mirrors WriteStat + ZBALWaitFlag):
 *   - CW: wait for prevRank's stat, copy data from target rank, write stat to nextRank
 *   - CCW: wait for nextRank's stat, copy data from target rank, write stat to prevRank
 *   - Each stat is a waitSymbol at offset [rank*statSize + coreIndex*slicePerCore + slice] × 64B
 */

/* Slices per core within each ring (matches classic ZBAL_AG_SLICE_PER_CORE) */
constexpr uint32_t ZBAL_AG_SLICE_PER_CORE = 3;

/* Poll a stat address with DC CIVAC until it matches waitVal. Returns true on match. */
inline bool AicpuPollStat(volatile uint64_t *addr, uint64_t waitVal, uint32_t timeoutUs)
{
    for (uint32_t t = 0; t < timeoutUs; t++) {
        uintptr_t fa = reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(addr));
        AicpuCacheInvalidate(fa);
        if (*addr == waitVal)
            return true;
        constexpr int POLL_BACKOFF_ITERS = 200;
        for (volatile int d = 0; d < POLL_BACKOFF_ITERS; d++) {}
    }
    return false;
}

class AllGatherDoubleRing {
public:
    /* Execute: owns ring loop (self-copy + N-1 ring rounds with stat sync) */
    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        int batch;
        do {
            batch = BuildSqes(alg, op);
            if (batch < 0)
                return batch;
            if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0)
                return ERR_WAIT_TIMEOUT;
        } while (batch == BUILD_MORE);
        return 0;
    }

    static int BuildSqes(AicpuAlgorithmCtx &alg, const CommOpParams &op)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;
        const uint32_t groupSize = alg.ctx->rankNum;
        const uint32_t myRank = alg.ctx->rankId;
        const uint64_t dataSize = op.dataSize;
        const uint32_t numCores = op.numCores;

        if (groupSize <= 1) {
            uint64_t dstOff = static_cast<uint64_t>(myRank) * dataSize;
            return AicpuDispatcher::CopyData(ring, 0U, op.sendBuf, op.recvBuf + dstOff, static_cast<uint32_t>(dataSize),
                                             op.channels[0], op.reduceOp);
        }

        /* ── Ring params ── */
        const uint32_t coreNumPerRing = numCores / ZBAL_AICPU_RING_NUM;
        if (coreNumPerRing == 0)
            return BUILD_ERROR;
        const uint64_t elemPerRing = dataSize / ZBAL_AICPU_RING_NUM;
        const bool isCwRing = (op.coreId < coreNumPerRing);
        const uint32_t ringCoreId = isCwRing ? op.coreId : (op.coreId - coreNumPerRing);
        const uint64_t elemExtraOff = isCwRing ? 0 : elemPerRing;
        const uint32_t streamId = isCwRing ? 0U : 1U;
        const uint32_t statSizePerRank = coreNumPerRing * ZBAL_AG_SLICE_PER_CORE;
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));

        uint64_t myOff;
        uint64_t myLen;
        AicpuParallelSlice(elemPerRing, ringCoreId, coreNumPerRing, myOff, myLen);
        myOff += elemExtraOff;
        if (myLen == 0)
            return BUILD_DONE;

        /* ── Stat area base ── */
        /* dataArea (rankNum*strideBytes) + flagArea (rankNum*strideBytes) */
        const uint64_t statBaseOff = 2ULL * static_cast<uint64_t>(groupSize) * strideBytes;
        /* CW stat starts at statBaseOff, CCW stat at statBaseOff + statSizePerRank*strideBytes */
        const uint64_t cwStatOff = statBaseOff;
        const uint64_t ccwStatOff = statBaseOff + static_cast<uint64_t>(statSizePerRank) * strideBytes;

        /* ── Stat direction (constant per core) ── */
        const uint32_t statWriteRank = isCwRing ? ((myRank + 1) % groupSize)              /* next: CW→next */
                                                : ((myRank + groupSize - 1) % groupSize); /* prev: CCW→prev */
        const uint64_t statDirOff = isCwRing ? cwStatOff : ccwStatOff;

        /* ── Wait symbol: use sendBuffer GVA (unique per operation) ── */
        const uint64_t waitSym = op.sendBuf;

        /* ================================================================
         * Phase 1: self-copy + write init stat
         * ================================================================ */
        if (!alg.chunkActive) {
            alg.chunkActive = true;
            alg.chunkRemaining = groupSize - 1; /* ring iteration counter */
            alg.chunkCurBase = 0;               /* current iteration */

            /* Self-copy: local data to output[myRank] */
            uint64_t selfSrc = op.sendBuf + myOff;
            uint64_t selfDst = op.recvBuf + static_cast<uint64_t>(myRank) * dataSize + myOff;
            if (AicpuDispatcher::CopyData(ring, streamId, selfSrc, selfDst, static_cast<uint32_t>(myLen),
                                          op.channels[streamId], op.reduceOp) != 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }

            /* Write init stat to receiver — signals "my data is ready".
             * Slot offset uses myRank (source), destination is statWriteRank's device. */
            for (uint32_t s = 0; s < ZBAL_AG_SLICE_PER_CORE; s++) {
                int64_t delta = static_cast<int64_t>(statWriteRank) - static_cast<int64_t>(myRank);
                int64_t devOff = delta * static_cast<int64_t>(alg.ctx->localDeviceMemSize);
                uint64_t statDst = op.exchangeGva + static_cast<uint64_t>(devOff) + statDirOff +
                                   static_cast<uint64_t>(myRank) * statSizePerRank * strideBytes +
                                   static_cast<uint64_t>(ringCoreId) * ZBAL_AG_SLICE_PER_CORE * strideBytes +
                                   static_cast<uint64_t>(s) * strideBytes;
                if (AicpuDispatcher::CopyData(ring, streamId, op.sendBuf, statDst, sizeof(uint64_t),
                                              op.channels[streamId]) != 0) {
                    alg.chunkActive = false;
                    return BUILD_ERROR;
                }
            }
            return BUILD_MORE;
        }

        /* ================================================================
         * Phase 2: ring iterations with stat sync
         * ================================================================ */
        uint32_t iter = static_cast<uint32_t>(alg.chunkCurBase);
        if (iter >= groupSize - 1) {
            alg.chunkActive = false;
            return BUILD_DONE;
        }

        /* ── Determine source rank for this round ── */
        uint32_t srcRank = isCwRing ? (myRank + groupSize - 1 - iter) % groupSize /* CW: shift left each round */
                                    : (myRank + 1 + iter) % groupSize;            /* CCW: shift right each round */

        /* ── Wait for source rank's stat (DC CIVAC poll on local exchange area) ── */
        /* Stat slot uses srcRank (the data source), NOT statReadRank.
         * The upstream rank wrote srcRank's stat to our exchange area. */
        volatile uint64_t *statBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + statDirOff);
        const uint32_t strideWords = strideBytes / static_cast<uint32_t>(sizeof(uint64_t));
        constexpr uint32_t timeoutUs = 6000000; /* 6s cross-device stat poll timeout */
        for (uint32_t s = 0; s < ZBAL_AG_SLICE_PER_CORE; s++) {
            volatile uint64_t *statSlot = statBase + srcRank * statSizePerRank * strideWords +
                                          ringCoreId * ZBAL_AG_SLICE_PER_CORE * strideWords + s * strideWords;
            if (!AicpuPollStat(statSlot, waitSym, timeoutUs)) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
        }

        /* ── Copy data from source rank's sendBuf (exchange publishes sendBuf GVA) ── */
        uint64_t remoteSrc = PeerOutputBuf(op.exchangeGva, srcRank) + myOff;
        uint64_t localDst = op.recvBuf + static_cast<uint64_t>(srcRank) * dataSize + myOff;
        if (AicpuDispatcher::CopyData(ring, streamId, remoteSrc, localDst, static_cast<uint32_t>(myLen),
                                      op.channels[streamId], op.reduceOp) != 0) {
            alg.chunkActive = false;
            return BUILD_ERROR;
        }

        /* ── Write stat to receiver (unless last round) ──
         * Propagates srcRank's data availability to the next rank in the ring. */
        const uint32_t lastRingIter = groupSize - 2;
        if (iter < lastRingIter) {
            for (uint32_t s = 0; s < ZBAL_AG_SLICE_PER_CORE; s++) {
                int64_t wDelta = static_cast<int64_t>(statWriteRank) - static_cast<int64_t>(myRank);
                int64_t wDevOff = wDelta * static_cast<int64_t>(alg.ctx->localDeviceMemSize);
                uint64_t statDst = op.exchangeGva + static_cast<uint64_t>(wDevOff) + statDirOff +
                                   static_cast<uint64_t>(srcRank) * statSizePerRank * strideBytes +
                                   static_cast<uint64_t>(ringCoreId) * ZBAL_AG_SLICE_PER_CORE * strideBytes +
                                   static_cast<uint64_t>(s) * strideBytes;
                if (AicpuDispatcher::CopyData(ring, streamId, op.sendBuf, statDst, sizeof(uint64_t),
                                              op.channels[streamId]) != 0) {
                    alg.chunkActive = false;
                    return BUILD_ERROR;
                }
            }
        }

        /* Advance iteration counter */
        alg.chunkCurBase = static_cast<uint64_t>(iter + 1);
        alg.chunkRemaining = alg.chunkRemaining > 1 ? alg.chunkRemaining - 1 : 0;

        return (iter == lastRingIter) ? BUILD_DONE : BUILD_MORE;
    }
};

#endif /* ZBAL_AICPU_ALLGATHER_DOUBLERING_H */
