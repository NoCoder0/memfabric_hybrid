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

#include "common/zbal_defines.h"
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

using namespace zbal;
/*
* DoubleRing AllGather — ring algorithm with CW + CCW data forwarding.
*
* Topology:
*   - 2 cores: core 0 → CW ring, core 1 → CCW ring. Each core uses 1 SDMA channel
*     (channel 0); CW/CCW is distinguished by coreId, not streamId.
*   - If numCores > 2, cores within each ring shard the byte-range via AicpuParallelSlice.
*
* Data flow (forwarding):
*   - Self-copy: each rank copies own data to recvBuf[self]
*   - CW ring:  read from prev rank's recvBuf, forward to next
*   - CCW ring: read from next rank's recvBuf, forward to prev
*   - Data propagates along the ring through recvBuf in (groupSize-1) rounds
*
* 3-slice pipelining (fixed ZBAL_AG_SLICE_PER_CORE=3):
*   - Each core's data is sliced into 3 chunks; Phase 2 processes ONE slice per
*     BuildSqes → AicpuSubmitAndWait doorbell, so the next rank can start slice-0
*     while the current rank is still transferring slice-1 (ring pipeline fill).
*
* Exchange area layout (RingExchange publishes recvBuf GVA to prev/next neighbors):
*   [0 .. rankNum*64)              data area (recvBuf GVA per rank)
*   [rankNum*64 .. 2*rankNum*64)   flag area (cross-device sync)
*   [2*rankNum*64 .. )             stat area (CW + CCW, statSizePerRank slots × 64B per rank)
*
* State machine:
*   chunkActive=false → Phase 1: self-copy + write init stats for all slices (1 batch)
*   chunkActive=true  → Phase 2: one slice per BuildSqes call (N-1 iters × 3 slices)
*   chunkCurBase = (iter << 2) | slice  — packed iteration + slice index
*
* Stat sync protocol (mirrors WriteStat + ZBALWaitFlag):
*   - CW:  read prev's stat, write stat to next
*   - CCW: read next's stat, write stat to prev
*   - Stat value: op.waitSymbol (incrementing per-op counter, avoids stale match)
*   - Each stat is at offset [rank*statSizePerRank + ringCoreId*SLICE + slice] × 64B
*/

/* Slices per core: 3 enables ring pipelining across ranks (mirrors classic AIV) */
constexpr uint32_t ZBAL_AG_SLICE_PER_CORE = 3;

/* Poll a stat address with DC CIVAC until it matches waitVal. Returns true on match.
* Two-phase polling:
*   Phase A (first FAST_POLLS iters): invalidate cache every iteration + tiny backoff.
*     Minimizes signal-detection latency — critical for ring pipeline fill, since the
*     first stat of each round arrives while the upstream is still copying.
*   Phase B (after): invalidate every 16 iterations + larger backoff to reduce
*     DC CIVAC overhead once the signal is known to be delayed. */
inline bool AicpuPollStat(volatile uint64_t *addr, uint64_t waitVal, uint32_t timeoutUs)
{
    constexpr uint32_t FAST_POLLS = 64;
    constexpr int FAST_BACKOFF_ITERS = 8;
    constexpr int SLOW_BACKOFF_ITERS = 200;
    for (uint32_t t = 0; t < timeoutUs; t++) {
        if (t < FAST_POLLS || (t & 0xF) == 0) {
            uintptr_t fa = reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(addr));
            AicpuCacheInvalidate(fa);
        } else {
            __asm__ __volatile__("" ::: "memory"); /* compiler barrier, skip cache op */
        }
        if (*addr == waitVal)
            return true;
        int backoff = (t < FAST_POLLS) ? FAST_BACKOFF_ITERS : SLOW_BACKOFF_ITERS;
        for (volatile int d = 0; d < backoff; d++) {}
    }
    return false;
}

class AllGatherDoubleRing {
public:
    /* Execute: owns ring loop (Phase 1 + N-1 rounds × 3 slices with stat sync).
    * Each slice triggered by a separate AicpuSubmitAndWait doorbell. */
    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t streamId = 0U; /* doublering uses 1 channel per core */
        bool phase1Inflight = false;
        int batch;
        do {
            batch = BuildSqes(alg, op);
            if (batch < 0)
                return batch;

            /* Phase 1 overlap: submit doorbell now, defer done-wait.
            * The next BuildSqes polls stat while Phase 1 SDMA runs,
            * hiding the self-copy latency. */
            if (IsPhase1OverlapStart(batch, phase1Inflight, alg)) {
                if (ringBufs[streamId].HasWork()) {
                    if (LaunchPhase1Doorbell(ringBufs, channels, workspace, coreId, numChPerCore, streamId) < 0)
                        return ERR_DOORBELL_FAILED;
                }
                phase1Inflight = true;
                continue; /* skip AicpuSubmitAndWait, go to Phase 2 slice 0 */
            }

            /* Before reusing the channel for Phase 2, wait for Phase 1 done */
            if (DrainPhase1IfNeeded(workspace, coreId, numChPerCore, streamId, phase1Inflight) < 0)
                return ERR_WAIT_TIMEOUT;

            if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0)
                return ERR_WAIT_TIMEOUT;
        } while (batch == BUILD_MORE);

        /* Final wait: if Phase 1 was the last thing submitted, wait for it */
        if (DrainPhase1IfNeeded(workspace, coreId, numChPerCore, streamId, phase1Inflight) < 0)
            return ERR_WAIT_TIMEOUT;
        return 0;
    }

    static int BuildSqes(AicpuAlgorithmCtx &alg, const CommOpParams &op)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;
        const uint32_t groupSize = alg.ctx->rankNum;
        const uint64_t dataSize = op.dataSize;

        if (groupSize <= 1) {
            uint64_t dstOff = static_cast<uint64_t>(alg.ctx->rankId) * dataSize;
            return AicpuDispatcher::CopyData(ring, 0U, op.sendBuf, op.recvBuf + dstOff, static_cast<uint32_t>(dataSize),
                                             op.channels[0], op.reduceOp);
        }

        /* Uneven split causes ringCoreId to escape [0, coreNumPerRing) and corrupt
        * neighboring data (convention §4.4). */
        if (op.numCores % ZBAL_AICPU_RING_NUM != 0) {
            return BUILD_ERROR;
        }

        RingCtx c = ComputeRingCtx(alg, op);
        if (c.myLen == 0)
            return BUILD_DONE;

        if (!alg.chunkActive) {
            return EmitPhase1Sqes(alg, op, c);
        }
        return EmitPhase2SliceSqes(alg, op, c);
    }

private:
    /* Derived ring parameters — computed once per BuildSqes entry.
    * Only fields used by Phase 1/2 emitters are kept; construction-only locals
    * (cwElemCount, ringElemCount, prevRank, nextRank) stay in ComputeRingCtx. */
    struct RingCtx {
        uint32_t groupSize;
        uint32_t myRank;
        uint64_t dataSize;
        uint32_t numCores;
        uint32_t coreNumPerRing;
        bool isCwRing;
        uint32_t ringCoreId;
        uint64_t elemExtraOff;
        uint32_t streamId;
        uint32_t statSizePerRank;
        uint32_t strideBytes;
        uint64_t myOff;
        uint64_t myLen;
        uint32_t readNeighborRank;
        uint32_t statWriteRank;
        uint64_t statDirOff;
        volatile uint64_t *scratch;
        uint64_t statSrcGva;
        uint64_t sliceBytes;
        uint64_t neighborRecvBuf;
    };

    static RingCtx ComputeRingCtx(const AicpuAlgorithmCtx &alg, const CommOpParams &op)
    {
        RingCtx c{};
        c.groupSize = alg.ctx->rankNum;
        c.myRank = alg.ctx->rankId;
        c.dataSize = op.dataSize;
        c.numCores = op.numCores;

        c.coreNumPerRing = c.numCores / ZBAL_AICPU_RING_NUM;
        /* Split data in half: CW handles [0, cwElemCount), CCW handles [cwElemCount, dataSize).
        * Use dataSize - cwElemCount for CCW so the tail byte is never lost when dataSize is odd. */
        const uint64_t cwElemCount = c.dataSize / ZBAL_AICPU_RING_NUM;
        const uint64_t ccwElemCount = c.dataSize - cwElemCount;
        c.isCwRing = (op.coreId < c.coreNumPerRing);
        c.ringCoreId = c.isCwRing ? op.coreId : (op.coreId - c.coreNumPerRing);
        const uint64_t ringElemCount = c.isCwRing ? cwElemCount : ccwElemCount;
        c.elemExtraOff = c.isCwRing ? 0 : cwElemCount;
        c.streamId = 0U;
        c.statSizePerRank = c.coreNumPerRing * ZBAL_AG_SLICE_PER_CORE;
        c.strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));

        AicpuParallelSlice(ringElemCount, c.ringCoreId, c.coreNumPerRing, c.myOff, c.myLen);
        c.myOff += c.elemExtraOff;

        const uint32_t prevRank = (c.myRank + c.groupSize - 1) % c.groupSize;
        const uint32_t nextRank = (c.myRank + 1) % c.groupSize;
        c.readNeighborRank = c.isCwRing ? prevRank : nextRank;
        c.statWriteRank = c.isCwRing ? nextRank : prevRank;

        const uint64_t statBaseOff = 2ULL * static_cast<uint64_t>(c.groupSize) * c.strideBytes;
        c.statDirOff = c.isCwRing
                           ? statBaseOff
                           : statBaseOff + static_cast<uint64_t>(c.groupSize) * c.statSizePerRank * c.strideBytes;

        /* Scratch at the end of the core ring buffer holds waitSymbol for SDMA to read. */
        uint8_t *coreRingBase = alg.ringBufs[0].localBuff;
        constexpr uint32_t scratchSlots = 1;
        c.scratch = reinterpret_cast<volatile uint64_t *>(coreRingBase + ZBAL_AICPU_CORE_RINGBUF_SIZE -
                                                          scratchSlots * sizeof(uint64_t));
        c.statSrcGva = reinterpret_cast<uint64_t>(&c.scratch[0]);
        c.sliceBytes = c.myLen / ZBAL_AG_SLICE_PER_CORE;
        /* Neighbor recvBuf GVA — published by RingExchange before Execute.
         * Phase 1's first ComputeRingCtx triggers the cache invalidate + load;
         * subsequent Phase 2 calls hit cache (same cache line, no extra cost). */
        c.neighborRecvBuf = PeerOutputBuf(op.exchangeGva, c.readNeighborRank);
        return c;
    }

    /* Compute peer stat address: devOff = (statWriteRank - myRank) * localDeviceMemSize,
    * then [statDirOff + rankIdx*statSizePerRank + ringCoreId*SLICE + sliceIdx] × strideBytes. */
    static uint64_t PeerStatAddr(const AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c,
                                 uint32_t rankIdx, uint32_t sliceIdx)
    {
        int64_t delta = static_cast<int64_t>(c.statWriteRank) - static_cast<int64_t>(c.myRank);
        int64_t devOff = delta * static_cast<int64_t>(alg.ctx->localDeviceMemSize);
        return op.exchangeGva + static_cast<uint64_t>(devOff) + c.statDirOff +
               static_cast<uint64_t>(rankIdx) * c.statSizePerRank * c.strideBytes +
               static_cast<uint64_t>(c.ringCoreId) * ZBAL_AG_SLICE_PER_CORE * c.strideBytes +
               static_cast<uint64_t>(sliceIdx) * c.strideBytes;
    }

    /* ================================================================
    * Phase 1: self-copy + init stats, interleaved per slice.
    *
    * Self-copy: my data → recvBuf[myRank], sliced into ZBAL_AG_SLICE_PER_CORE.
    * Init stat: per slice, signal readNeighborRank that "slice s of my data is in
    *   my recvBuf[myRank]" — written right after that slice's copy. SDMA processes
    *   SQEs in order, so stat[s] completes only after slice-s copy, letting the
    *   downstream rank start slice-0 after 1/3 of self-copy (ring pipeline fill).
    * ================================================================ */
    static int EmitPhase1Sqes(AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;
        alg.chunkActive = true;
        alg.chunkRemaining = c.groupSize - 1;
        alg.chunkCurBase = 0;

        c.scratch[0] = op.waitSymbol;
        AicpuCacheFlush(reinterpret_cast<uintptr_t>(&c.scratch[0]));

        for (uint32_t s = 0; s < ZBAL_AG_SLICE_PER_CORE; s++) {
            const uint64_t sliceOff = c.myOff + static_cast<uint64_t>(s) * c.sliceBytes;
            const uint64_t sliceLen =
                (s + 1 == ZBAL_AG_SLICE_PER_CORE) ? (c.myLen - static_cast<uint64_t>(s) * c.sliceBytes) : c.sliceBytes;
            const uint64_t selfSrc = op.sendBuf + sliceOff;
            const uint64_t selfDst = op.recvBuf + static_cast<uint64_t>(c.myRank) * c.dataSize + sliceOff;
            if (AicpuDispatcher::CopyData(ring, c.streamId, selfSrc, selfDst, static_cast<uint32_t>(sliceLen),
                                          op.channels[c.streamId], op.reduceOp) != 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
            const uint64_t statDst = PeerStatAddr(alg, op, c, c.myRank, s);
            if (AicpuDispatcher::CopyData(ring, c.streamId, c.statSrcGva, statDst, sizeof(uint64_t),
                                          op.channels[c.streamId]) != 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
        }
        return BUILD_MORE;
    }

    /* ================================================================
    * Phase 2: process ONE slice per call (enables ring pipelining).
    *
    * chunkCurBase = (iter << 2) | slice. The per-slice doorbell lets the next
    * rank start slice-0 while the current rank is still on slice-1.
    * ================================================================ */
    /* Wait for upstream stat for this slice, then copy data from neighbor's
     * recvBuf to local recvBuf (data forwarding along the ring). */
    static int WaitAndCopyNeighborSlice(AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c,
                                        uint32_t srcRank, uint64_t sliceOff, uint64_t sliceLen, uint32_t slice)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;
        /* DC CIVAC poll on local exchange area for upstream stat */
        volatile uint64_t *statBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + c.statDirOff);
        const uint32_t strideWords = c.strideBytes / static_cast<uint32_t>(sizeof(uint64_t));
        constexpr uint32_t timeoutUs = 6000000;
        volatile uint64_t *statSlot = statBase + srcRank * c.statSizePerRank * strideWords +
                                      c.ringCoreId * ZBAL_AG_SLICE_PER_CORE * strideWords + slice * strideWords;
        if (!AicpuPollStat(statSlot, op.waitSymbol, timeoutUs)) {
            return BUILD_ERROR;
        }

        /* Neighbor recvBuf GVA — from RingCtx (cache hit after Phase 1's first load). */
        const uint64_t remoteSrc = c.neighborRecvBuf + static_cast<uint64_t>(srcRank) * c.dataSize + sliceOff;
        const uint64_t localDst = op.recvBuf + static_cast<uint64_t>(srcRank) * c.dataSize + sliceOff;
        if (AicpuDispatcher::CopyData(ring, c.streamId, remoteSrc, localDst, static_cast<uint32_t>(sliceLen),
                                      op.channels[c.streamId], op.reduceOp) != 0) {
            return BUILD_ERROR;
        }
        return 0;
    }

    static int EmitPhase2SliceSqes(AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c)
    {
        uint64_t packedCur = alg.chunkCurBase;
        uint32_t iter = static_cast<uint32_t>(packedCur >> 2);
        uint32_t slice = static_cast<uint32_t>(packedCur & 0x3U);

        if (iter >= c.groupSize - 1) {
            alg.chunkActive = false;
            return BUILD_DONE;
        }

        const uint32_t lastRingIter = c.groupSize - 2;
        const uint32_t srcRank =
            c.isCwRing ? (c.myRank + c.groupSize - 1 - iter) % c.groupSize : (c.myRank + 1 + iter) % c.groupSize;

        const uint64_t sliceOff = c.myOff + static_cast<uint64_t>(slice) * c.sliceBytes;
        const uint64_t sliceLen = (slice + 1 == ZBAL_AG_SLICE_PER_CORE)
                                      ? (c.myLen - static_cast<uint64_t>(slice) * c.sliceBytes)
                                      : c.sliceBytes;

        /* Empty slice: skip data copy but still forward stat so downstream doesn't time out (§4.6). */
        if (sliceLen != 0) {
            if (WaitAndCopyNeighborSlice(alg, op, c, srcRank, sliceOff, sliceLen, slice) < 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
        }

        /* Forward signal to statWriteRank (skip on last round — no downstream) */
        if (iter < lastRingIter) {
            const uint64_t statDst = PeerStatAddr(alg, op, c, srcRank, slice);
            if (AicpuDispatcher::CopyData(alg.ringBufs, c.streamId, c.statSrcGva, statDst, sizeof(uint64_t),
                                          op.channels[c.streamId]) != 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
        }

        return AdvanceCursor(alg, iter, slice, lastRingIter);
    }

    /* Advance chunkCurBase to next slice, or next iteration. Returns BUILD_DONE on
    * the very last slice, BUILD_MORE otherwise. */
    static int AdvanceCursor(AicpuAlgorithmCtx &alg, uint32_t iter, uint32_t slice, uint32_t lastRingIter)
    {
        if (slice + 1 < ZBAL_AG_SLICE_PER_CORE) {
            alg.chunkCurBase = (static_cast<uint64_t>(iter) << ZBAL_CONST_2) | static_cast<uint64_t>(slice + 1);
        } else {
            alg.chunkCurBase = static_cast<uint64_t>(iter + 1) << ZBAL_CONST_2;
            alg.chunkRemaining = alg.chunkRemaining > 1 ? alg.chunkRemaining - 1 : 0;
        }
        return (iter == lastRingIter && slice + 1 >= ZBAL_AG_SLICE_PER_CORE) ? BUILD_DONE : BUILD_MORE;
    }

    /* True if this BuildSqes batch is the first Phase 2 slice (enables overlap). */
    static bool IsPhase1OverlapStart(int batch, bool phase1Inflight, const AicpuAlgorithmCtx &alg)
    {
        return batch == BUILD_MORE && !phase1Inflight && alg.chunkCurBase == 0 && alg.chunkActive;
    }

    /* Launch Phase 1 doorbell early so SDMA self-copy runs while we poll stat. */
    static int LaunchPhase1Doorbell(SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                                    volatile uint8_t *workspace, uint32_t coreId, uint32_t numChPerCore,
                                    uint32_t streamId)
    {
        uint32_t fid = AicpuWorkspace::FlagIdx(coreId, numChPerCore, streamId);
        return AicpuLaunchTaskMc(&ringBufs[streamId], channels[streamId], workspace, coreId, 1, streamId, fid);
    }

    /* Wait for Phase 1 completion if still inflight; clears the flag. */
    static int DrainPhase1IfNeeded(volatile uint8_t *workspace, uint32_t coreId, uint32_t numChPerCore,
                                   uint32_t streamId, bool &phase1Inflight)
    {
        if (!phase1Inflight)
            return 0;
        CompletionFlag flag(workspace, coreId, numChPerCore, streamId);
        if (flag.Wait() < 0)
            return ERR_WAIT_TIMEOUT;
        phase1Inflight = false;
        return 0;
    }
};

#endif /* ZBAL_AICPU_ALLGATHER_DOUBLERING_H */
