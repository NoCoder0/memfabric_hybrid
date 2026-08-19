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
#ifndef ZBAL_AICPU_REDUCESCATTER_DOUBLERING_H
#define ZBAL_AICPU_REDUCESCATTER_DOUBLERING_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

using namespace zbal;

/* DoubleRing ReduceScatter — true ring topology with cclBuf forwarding.
 *
 * Mirrors AllGatherDoubleRing (RingCtx + Emit helpers + Phase 1 overlap).
 * Key differences from AllGather:
 *  1. cclBuf (op.buffer) as forwarding buffer — recvBuf is per-rank output
 *     only (dataSize/N), so cclBuf holds per-chunk working copies for ring
 *     accumulation. sendBuf stays read-only.
 *  2. SDMA reduce in Phase 2 (reduceOpCode = dataType<<4 | reduceOp).
 *  3. Outer chunk loop: cclBuf is fixed 256M, so data is split into outer
 *     chunks when perRankBytes > 256M/(2*N).
 *  4. Phase 1 copies all N chunks to cclBuf (AllGather only copies 1).
*
* cclBuf layout (double-buffered):
*   [CW-A: N×cb][CW-B: N×cb][CCW-A: N×cb][CCW-B: N×cb]
*   Outer chunk K uses buffer A (even K) or B (odd K) — prevents Phase 1 of
*   K+1 from overwriting cclBuf data still being read for K.
*
* chunkCurBase bit-field: (outerIdx << OUTIDX_SHIFT) | (iter << ITER_SHIFT) | slice
*   slice (bits 0-1): 3 pipeline slices (0/1/2)
*   iter (bits 2-3): ring iteration round (0..N-2)
*   outerIdx (bits 16+): outer chunk sequence number
*/

/* CCL buffer size — must match host-side allocation in NpuCommunicatorAICPU. */
constexpr uint64_t ZBAL_RS_CCL_BUFFER_BYTES = 256ULL * 1024ULL * 1024ULL; /* 256M */

/* Slices per core: 3 enables ring pipelining across ranks (mirrors AllGather) */
constexpr uint32_t ZBAL_RS_SLICE_PER_CORE = 3;

class ReduceScatterDoubleRing {
public:
    /* Execute: owns ring loop (Phase 1 + N-1 rounds × 3 slices with stat sync).
     * Phase 1: multi-channel data copy, then stat on ch0 after data completes.
     * Phase 2: ch0 only, NoWait for slices 0/1, Submit+Wait for slice 2. */
    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        bool noWaitInflight = false;
        int batch;
        do {
            batch = BuildSqes(alg, op);
            if (batch < 0)
                return batch;

            if (IsPhase1Start(batch, alg)) {
                if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0)
                    return ERR_WAIT_TIMEOUT;
                RingCtx c = ComputeRingCtx(alg, op);
                if (EmitPhase1StatSqes(alg, op, c) < 0)
                    return BUILD_ERROR;
                if (ringBufs[0].HasWork() && AicpuLaunchTaskNoWait(&ringBufs[0], channels[0]) < 0)
                    return ERR_DOORBELL_FAILED;
                continue;
            }

            /* Phase 2: NoWait for non-last slices, Submit+Wait for last slice. */
            const bool isLastSlice = (batch != BUILD_MORE) || !alg.chunkActive || IsLastSliceInGroup(alg);
            if (!isLastSlice && !AnyHwSqNearFull(channels, numChPerCore)) {
                if (LaunchNoWaitAllChannels(ringBufs, channels, numChPerCore) < 0)
                    return ERR_DOORBELL_FAILED;
                noWaitInflight = true;
            } else {
                if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0)
                    return ERR_WAIT_TIMEOUT;
                noWaitInflight = false;
            }
        } while (batch == BUILD_MORE);

        /* Final drain: safety net for pending NoWait SQEs. */
        if (noWaitInflight) {
            if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0)
                return ERR_WAIT_TIMEOUT;
        }
        return 0;
    }

    static int BuildSqes(AicpuAlgorithmCtx &alg, const CommOpParams &op)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;
        const uint32_t groupSize = alg.ctx->rankNum;
        const uint64_t dataSize = op.dataSize;

        if (groupSize <= 1) {
            return AicpuDispatcher::CopyData(ring, 0U, op.sendBuf, op.recvBuf, static_cast<uint32_t>(dataSize),
                                             op.channels[0]);
        }
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
    /* Derived ring parameters — computed once per BuildSqes entry. */
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
        uint64_t perRankBytes;
        uint64_t ringElemCount;
        uint64_t chunkBytes;
        uint64_t ringBufOff;
        uint32_t numOuterChunks;
        uint64_t neighborCclBuf;
        uint8_t reduceOpCode;
        uint32_t elemSize;
        uint32_t numChPerCore;
    };

    static RingCtx ComputeRingCtx(const AicpuAlgorithmCtx &alg, const CommOpParams &op)
    {
        RingCtx c{};
        c.groupSize = alg.ctx->rankNum;
        c.myRank = alg.ctx->rankId;
        c.dataSize = op.dataSize;
        c.numCores = op.numCores;

        c.coreNumPerRing = c.numCores / ZBAL_AICPU_RING_NUM;
        /* perRankBytes: dataSize is total = N × perRankBytes (unlike AllGather). */
        c.perRankBytes = c.dataSize / c.groupSize;
        const uint64_t halfBytes = c.perRankBytes / ZBAL_AICPU_RING_NUM;
        c.isCwRing = (op.coreId < c.coreNumPerRing);
        c.ringCoreId = c.isCwRing ? op.coreId : (op.coreId - c.coreNumPerRing);
        c.ringElemCount = c.isCwRing ? halfBytes : (c.perRankBytes - halfBytes);
        c.elemExtraOff = c.isCwRing ? 0 : halfBytes;
        c.streamId = 0U;
        c.statSizePerRank = c.coreNumPerRing * ZBAL_RS_SLICE_PER_CORE;
        c.strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));

        /* cclBuf double-buffer: outer chunk K uses A (even) or B (odd). */
        c.chunkBytes = ZBAL_RS_CCL_BUFFER_BYTES / (4ULL * c.groupSize);
        const uint32_t outerIdx = static_cast<uint32_t>(alg.chunkCurBase >> ZBAL_AICPU_OUTERIDX_SHIFT);
        const uint64_t bufSlot = static_cast<uint64_t>(outerIdx & 1U);
        const uint64_t dirBase = c.isCwRing ? 0ULL : (2ULL * static_cast<uint64_t>(c.groupSize) * c.chunkBytes);
        c.ringBufOff = dirBase + bufSlot * static_cast<uint64_t>(c.groupSize) * c.chunkBytes;
        c.numOuterChunks = static_cast<uint32_t>((c.ringElemCount + c.chunkBytes - 1) / c.chunkBytes);

        AicpuParallelSlice(c.ringElemCount, c.ringCoreId, c.coreNumPerRing, c.myOff, c.myLen);
        c.myOff += c.elemExtraOff;

        const uint32_t prevRank = (c.myRank + c.groupSize - 1) % c.groupSize;
        const uint32_t nextRank = (c.myRank + 1) % c.groupSize;
        c.readNeighborRank = c.isCwRing ? prevRank : nextRank;
        c.statWriteRank = c.isCwRing ? nextRank : prevRank;

        const uint64_t statBaseOff = 2ULL * static_cast<uint64_t>(c.groupSize) * c.strideBytes;
        c.statDirOff = c.isCwRing
                           ? statBaseOff
                           : statBaseOff + static_cast<uint64_t>(c.groupSize) * c.statSizePerRank * c.strideBytes;

        /* Scratch at end of core ring buffer holds stat value for SDMA to read. */
        uint8_t *coreRingBase = alg.ringBufs[0].localBuff;
        constexpr uint32_t scratchSlots = 1;
        c.scratch = reinterpret_cast<volatile uint64_t *>(coreRingBase + ZBAL_AICPU_CORE_RINGBUF_SIZE -
                                                          scratchSlots * sizeof(uint64_t));
        c.statSrcGva = reinterpret_cast<uint64_t>(&c.scratch[0]);

        /* Neighbor cclBuf GVA — published by RingExchange slot 1. */
        const uintptr_t dataSlotAddr =
            op.exchangeGva + static_cast<uint64_t>(c.readNeighborRank) * c.strideBytes + sizeof(uint64_t);
        AicpuCacheInvalidate(dataSlotAddr);
        c.neighborCclBuf = PeerBufferGva(op.exchangeGva, c.readNeighborRank);

        c.reduceOpCode = (uint8_t)((op.dataType << ZBAL_AICPU_RS_DATATYE_SHIFT) | op.reduceOp);
        c.elemSize = ZBALDataTypeSize(op.dataType);
        if (c.elemSize == 0) {
            c.elemSize = 1;
        }
        c.numChPerCore = op.numChPerCore;
        if (c.numChPerCore == 0) {
            c.numChPerCore = 1;
        }
        return c;
    }

    /* Compute peer stat address. */
    static uint64_t PeerStatAddr(const AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c,
                                 uint32_t rankIdx, uint32_t sliceIdx)
    {
        int64_t delta = static_cast<int64_t>(c.statWriteRank) - static_cast<int64_t>(c.myRank);
        int64_t devOff = delta * static_cast<int64_t>(alg.ctx->localDeviceMemSize);
        return op.exchangeGva + static_cast<uint64_t>(devOff) + c.statDirOff +
               static_cast<uint64_t>(rankIdx) * c.statSizePerRank * c.strideBytes +
               static_cast<uint64_t>(c.ringCoreId) * ZBAL_RS_SLICE_PER_CORE * c.strideBytes +
               static_cast<uint64_t>(sliceIdx) * c.strideBytes;
    }

    /* ================================================================
    * Phase 1: copy sendBuf → cclBuf for all ranks + self-copy → recvBuf.
    * Must initialize ALL N chunks (ring reduce needs every rank's raw data).
    * Multi-channel: (N+1) copies per slice distributed round-robin.
    * cclBuf[myRank] skipped — never read by downstream nor reduced by self.
    * ================================================================ */
    static int EmitPhase1Sqes(AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;

        uint32_t outerIdx = static_cast<uint32_t>(alg.chunkCurBase >> ZBAL_AICPU_OUTERIDX_SHIFT);
        if (outerIdx >= c.numOuterChunks) {
            return BUILD_DONE;
        }

        alg.chunkActive = true;
        alg.chunkRemaining = c.groupSize - 1;
        alg.chunkCurBase = (static_cast<uint64_t>(outerIdx) << ZBAL_AICPU_OUTERIDX_SHIFT);

        const uint64_t curStatVal = op.waitSymbol + outerIdx;
        c.scratch[0] = curStatVal;
        AicpuCacheFlush(reinterpret_cast<uintptr_t>(&c.scratch[0]));

        const uint64_t chunkOff = static_cast<uint64_t>(outerIdx) * c.chunkBytes;
        const uint64_t chunkEnd = (outerIdx + 1 == c.numOuterChunks) ? c.ringElemCount : (chunkOff + c.chunkBytes);
        const uint64_t absChunkStart = chunkOff + c.elemExtraOff;
        const uint64_t absChunkEnd = chunkEnd + c.elemExtraOff;

        uint64_t coreStart = (c.myOff > absChunkStart) ? c.myOff : absChunkStart;
        uint64_t coreEnd = (c.myOff + c.myLen < absChunkEnd) ? (c.myOff + c.myLen) : absChunkEnd;
        if (coreStart >= coreEnd) {
            alg.chunkActive = false;
            alg.chunkCurBase = (static_cast<uint64_t>(outerIdx + 1) << ZBAL_AICPU_OUTERIDX_SHIFT);
            return (outerIdx + 1 >= c.numOuterChunks) ? BUILD_DONE : BUILD_MORE;
        }
        const uint64_t coreChunkOff = coreStart - absChunkStart;
        const uint64_t coreChunkLen = coreEnd - coreStart;
        /* Element-aligned sliceBytes: SDMA reduce requires elem-aligned src/dst/len. */
        const uint64_t sliceBytes = (coreChunkLen / ZBAL_RS_SLICE_PER_CORE / c.elemSize) * c.elemSize;

        uint32_t ch = 0;
        for (uint32_t s = 0; s < ZBAL_RS_SLICE_PER_CORE; s++) {
            const uint64_t sOff = coreChunkOff + static_cast<uint64_t>(s) * sliceBytes;
            const uint64_t sLen =
                (s + 1 == ZBAL_RS_SLICE_PER_CORE) ? (coreChunkLen - static_cast<uint64_t>(s) * sliceBytes) : sliceBytes;

            if (sLen == 0) {
                continue;
            }
            /* Copy sendBuf[k] → cclBuf[k] for all k except myRank. */
            for (uint32_t k = 0; k < c.groupSize; k++) {
                if (k == c.myRank) {
                    continue;
                }
                uint64_t src = op.sendBuf + static_cast<uint64_t>(k) * c.perRankBytes + absChunkStart + sOff;
                uint64_t dst = op.buffer + c.ringBufOff + static_cast<uint64_t>(k) * c.chunkBytes + sOff;
                if (AicpuDispatcher::CopyData(ring, ch, src, dst, static_cast<uint32_t>(sLen), op.channels[ch]) != 0) {
                    alg.chunkActive = false;
                    return BUILD_ERROR;
                }
                ch = (ch + 1) % c.numChPerCore;
            }

            /* Self-copy: sendBuf[myRank] → recvBuf (pre-loads for final round reduce). */
            uint64_t selfSrc = op.sendBuf + static_cast<uint64_t>(c.myRank) * c.perRankBytes + absChunkStart + sOff;
            uint64_t selfDst = op.recvBuf + absChunkStart + sOff;
            if (AicpuDispatcher::CopyData(ring, ch, selfSrc, selfDst, static_cast<uint32_t>(sLen), op.channels[ch]) !=
                0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
            ch = (ch + 1) % c.numChPerCore;
        }
        return BUILD_MORE;
    }

    /* Phase 1 stat: build stat SQEs on ch0 for all 3 slices.
     * Called AFTER data channels complete — stat fires after ALL data written. */
    static int EmitPhase1StatSqes(AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;

        const uint32_t fwdChunk0 =
            c.isCwRing ? ((c.myRank + c.groupSize - 1) % c.groupSize) : ((c.myRank + 1) % c.groupSize);

        for (uint32_t s = 0; s < ZBAL_RS_SLICE_PER_CORE; s++) {
            const uint64_t statDst = PeerStatAddr(alg, op, c, fwdChunk0, s);
            if (AicpuDispatcher::CopyData(ring, 0U, c.statSrcGva, statDst, sizeof(uint64_t), op.channels[0U]) != 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
        }
        return BUILD_MORE;
    }

    /* ================================================================
    * Phase 2: process ONE slice per call (enables ring pipelining).
    * Waits for upstream stat, reduces neighbor's cclBuf → local dst,
    * forwards stat to downstream (skip on final round).
    * ================================================================ */
    static int EmitPhase2SliceSqes(AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;

        uint64_t packedCur = alg.chunkCurBase;
        uint32_t outerIdx = static_cast<uint32_t>(packedCur >> ZBAL_AICPU_OUTERIDX_SHIFT);
        uint32_t iter = static_cast<uint32_t>((packedCur >> ZBAL_AICPU_ITER_SHIFT) & 0x3FFFU);
        uint32_t slice = static_cast<uint32_t>(packedCur & 0x3U);

        if (iter >= c.groupSize - 1) {
            alg.chunkActive = false;
            alg.chunkCurBase = (static_cast<uint64_t>(outerIdx + 1) << ZBAL_AICPU_OUTERIDX_SHIFT);
            return (outerIdx + 1 >= c.numOuterChunks) ? BUILD_DONE : BUILD_MORE;
        }

        const uint32_t lastRingIter = c.groupSize - 2;
        const bool isFinalRound = (iter == lastRingIter);

        const uint64_t chunkOff = static_cast<uint64_t>(outerIdx) * c.chunkBytes;
        const uint64_t chunkEnd = (outerIdx + 1 == c.numOuterChunks) ? c.ringElemCount : (chunkOff + c.chunkBytes);
        const uint64_t absChunkStart = chunkOff + c.elemExtraOff;
        const uint64_t absChunkEnd = chunkEnd + c.elemExtraOff;

        uint64_t coreStart = (c.myOff > absChunkStart) ? c.myOff : absChunkStart;
        uint64_t coreEnd = (c.myOff + c.myLen < absChunkEnd) ? (c.myOff + c.myLen) : absChunkEnd;
        if (coreStart >= coreEnd) {
            return AdvanceCursor(alg, outerIdx, c.numOuterChunks, iter, slice, lastRingIter);
        }
        const uint64_t coreChunkOff = coreStart - absChunkStart;
        const uint64_t coreChunkLen = coreEnd - coreStart;
        const uint64_t sliceBytes = (coreChunkLen / ZBAL_RS_SLICE_PER_CORE / c.elemSize) * c.elemSize;

        const uint64_t sliceOff = coreChunkOff + static_cast<uint64_t>(slice) * sliceBytes;
        const uint64_t sliceLen = (slice + 1 == ZBAL_RS_SLICE_PER_CORE)
                                      ? (coreChunkLen - static_cast<uint64_t>(slice) * sliceBytes)
                                      : sliceBytes;

        const uint64_t curStatVal = op.waitSymbol + outerIdx;

        /* reduceChunk: the chunk I recv & reduce this round.
         * CW: (i-2-r) mod N; CCW: (i+2+r) mod N. Final r=N-2 gives i (myRank). */
        const uint32_t reduceChunk =
            c.isCwRing ? ((c.myRank + c.groupSize - 2 - iter) % c.groupSize) : ((c.myRank + 2 + iter) % c.groupSize);

        if (sliceLen != 0) {
            if (WaitNeighborStat(op, c, reduceChunk, slice, curStatVal) < 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
            /* Reduce: neighbor's cclBuf → local dst (cclBuf for partial, recvBuf for final). */
            const uint64_t remoteSrc =
                c.neighborCclBuf + c.ringBufOff + static_cast<uint64_t>(reduceChunk) * c.chunkBytes + sliceOff;
            const uint64_t localDst =
                isFinalRound
                    ? (op.recvBuf + absChunkStart + sliceOff)
                    : (op.buffer + c.ringBufOff + static_cast<uint64_t>(reduceChunk) * c.chunkBytes + sliceOff);
            if (AicpuDispatcher::CopyData(ring, c.streamId, remoteSrc, localDst, static_cast<uint32_t>(sliceLen),
                                          op.channels[c.streamId], c.reduceOpCode) != 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
        }

        /* Forward stat to downstream (skip final round — no downstream).
         * Always forwarded even for empty slices so downstream doesn't timeout. */
        if (!isFinalRound) {
            const uint64_t statDst = PeerStatAddr(alg, op, c, reduceChunk, slice);
            if (AicpuDispatcher::CopyData(ring, c.streamId, c.statSrcGva, statDst, sizeof(uint64_t),
                                          op.channels[c.streamId]) != 0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
        }

        return AdvanceCursor(alg, outerIdx, c.numOuterChunks, iter, slice, lastRingIter);
    }

    /* Poll local stat slot for upstream neighbor's signal. */
    static int WaitNeighborStat(const CommOpParams &op, const RingCtx &c, uint32_t reduceChunk, uint32_t slice,
                                uint64_t curStatVal)
    {
        volatile uint64_t *statBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + c.statDirOff);
        const uint32_t strideWords = c.strideBytes / static_cast<uint32_t>(sizeof(uint64_t));
        constexpr uint32_t timeoutUs = 6000000;
        volatile uint64_t *statSlot = statBase + static_cast<uint64_t>(reduceChunk) * c.statSizePerRank * strideWords +
                                      static_cast<uint64_t>(c.ringCoreId) * ZBAL_RS_SLICE_PER_CORE * strideWords +
                                      static_cast<uint64_t>(slice) * strideWords;
        if (!AicpuPollStat(statSlot, curStatVal, timeoutUs)) {
            return BUILD_ERROR;
        }
        return 0;
    }

    /* Advance cursor: slice → slice+1, or slice last → next iter, or iter last → next outer chunk. */
    static int AdvanceCursor(AicpuAlgorithmCtx &alg, uint32_t outerIdx, uint32_t numOuterChunks, uint32_t iter,
                             uint32_t slice, uint32_t lastRingIter)
    {
        if (slice + 1 < ZBAL_RS_SLICE_PER_CORE) {
            alg.chunkCurBase = (static_cast<uint64_t>(outerIdx) << ZBAL_AICPU_OUTERIDX_SHIFT) |
                               (static_cast<uint64_t>(iter) << ZBAL_AICPU_ITER_SHIFT) |
                               static_cast<uint64_t>(slice + 1);
            return BUILD_MORE;
        }
        if (iter < lastRingIter) {
            alg.chunkCurBase = (static_cast<uint64_t>(outerIdx) << ZBAL_AICPU_OUTERIDX_SHIFT) |
                               (static_cast<uint64_t>(iter + 1) << ZBAL_AICPU_ITER_SHIFT);
            alg.chunkRemaining = alg.chunkRemaining > 1 ? alg.chunkRemaining - 1 : 0;
            return BUILD_MORE;
        }
        /* Outer chunk done — advance to next */
        alg.chunkActive = false;
        alg.chunkCurBase = (static_cast<uint64_t>(outerIdx + 1) << ZBAL_AICPU_OUTERIDX_SHIFT);
        return (outerIdx + 1 >= numOuterChunks) ? BUILD_DONE : BUILD_MORE;
    }

    /* True if Phase 1 just finished (lower 16 bits of chunkCurBase are 0). */
    static bool IsPhase1Start(int batch, const AicpuAlgorithmCtx &alg)
    {
        return batch == BUILD_MORE && (alg.chunkCurBase & 0xFFFFU) == 0 && alg.chunkActive;
    }

    /* True if the slice just processed was the last in its 3-slice group. */
    static bool IsLastSliceInGroup(const AicpuAlgorithmCtx &alg)
    {
        return (alg.chunkCurBase & 0x3U) == 0U;
    }

    /* SQ overflow protection: returns true if HW SQ ring is past half capacity. */
    static bool HwSqNearFull(volatile stars_channel_info_t *channel)
    {
        if (channel == nullptr || channel->sq_depth == 0) {
            return false;
        }
        const uint32_t depth = channel->sq_depth;
        const uint32_t head = channel->sq_head;
        const uint32_t tail = channel->sq_tail;
        const uint32_t inFlight = (tail >= head) ? (tail - head) : (depth - head + tail);
        return inFlight >= depth / 2U;
    }

    static bool AnyHwSqNearFull(volatile stars_channel_info_t **channels, uint32_t numChPerCore)
    {
        for (uint32_t s = 0; s < numChPerCore; s++) {
            if (HwSqNearFull(channels[s])) {
                return true;
            }
        }
        return false;
    }

    static int LaunchNoWaitAllChannels(SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                                       uint32_t numChPerCore)
    {
        for (uint32_t s = 0; s < numChPerCore; s++) {
            if (!ringBufs[s].HasWork()) {
                continue;
            }
            if (AicpuLaunchTaskNoWait(&ringBufs[s], channels[s]) < 0) {
                return ERR_DOORBELL_FAILED;
            }
        }
        return 0;
    }
};

#endif /* ZBAL_AICPU_REDUCESCATTER_DOUBLERING_H */
