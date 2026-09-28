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
 * Uses separate per-direction SDMA channels for ring reduce and next-chunk prefetch.
 * Key differences from AllGather:
 *  1. cclBuf (op.buffer) as forwarding buffer — recvBuf is per-rank output
 *     only (dataSize/N), so cclBuf holds per-chunk working copies for ring
 *     accumulation. sendBuf stays read-only.
 *  2. SDMA reduce in Phase 2 (reduceOpCode = dataType<<4 | reduceOp).
 *  3. Outer chunks are capped by ZBAL_RS_CHUNK_BYTES and the CCL buffer capacity.
 *  4. Phase 1 copies N-1 chunks to cclBuf and the local chunk to recvBuf.
 *  5. A slot is reused only after local completion and downstream consumed acknowledgement.
*
* cclBuf layout (double-buffered):
*   [CW-A: N×cb][CW-B: N×cb][CCW-A: N×cb][CCW-B: N×cb]
*   Outer chunk K uses buffer A (even K) or B (odd K) — prevents Phase 1 of
*   K+1 from overwriting cclBuf data still being read for K.
*
* chunkCurBase bit-field: (outerIdx << OUTIDX_SHIFT) | (iter << ITER_SHIFT) | slice
*   slice (bits 0-1): pipeline slice index (currently only 0)
*   iter (bits 2-15): ring iteration round (0..N-2)
*   outerIdx (bits 16+): outer chunk sequence number
*/

/* CCL buffer size — must match host-side allocation in NpuCommunicatorAICPU. */
constexpr uint64_t ZBAL_RS_CCL_BUFFER_BYTES = 256ULL * 1024ULL * 1024ULL; /* 256M */

/* Per-rank, per-direction chunk limit; use 1, 2 or 4 MiB for comparison. */
constexpr uint64_t ZBAL_RS_CHUNK_BYTES = 1ULL * 1024ULL * 1024ULL;

/* Use one slice per core to avoid subdividing each ring chunk. */
constexpr uint32_t ZBAL_RS_SLICE_PER_CORE = 1;

class ReduceScatterDoubleRing {
public:
    /* Channel 0 reduces the current slot while channel 1 prepares the next slot. */
    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        if (numCores != 2 || numChPerCore != 2 || op.waitSymbol == 0 || op.waitSymbol > UINT32_MAX ||
            alg.ctx->rankNum < 2 || op.numCores != numCores || op.numChPerCore != numChPerCore) {
            return BUILD_ERROR;
        }
        if (channels[0] == nullptr || channels[1] == nullptr || channels[0]->sq_base == channels[1]->sq_base ||
            op.coreId != coreId || alg.ringBufs != ringBufs || op.channels != channels) {
            return ERR_CHANNEL_INVALID;
        }
        const uint32_t elemSize = ZBALDataTypeSize(op.dataType);
        if (elemSize == 0 || op.dataSize % (static_cast<uint64_t>(alg.ctx->rankNum) * elemSize) != 0) {
            return BUILD_ERROR;
        }
        return ExecutePipeline(alg, op, workspace);
    }

private:
    struct RingCtx;

    static uint64_t ChunkToken(const CommOpParams &op, uint32_t outerIdx)
    {
        return (op.waitSymbol << 32U) | static_cast<uint64_t>(outerIdx);
    }

    static int SubmitChannel(AicpuAlgorithmCtx &alg, const CommOpParams &op, volatile uint8_t *workspace,
                             uint32_t channel, bool wait)
    {
        const uint32_t fid = AicpuWorkspace::FlagIdx(op.coreId, op.numChPerCore, channel);
        int ret =
            AicpuLaunchTaskMc(&alg.ringBufs[channel], op.channels[channel], workspace, op.coreId, 1, channel, fid);
        if (ret < 0) {
            return ret;
        }
        return wait ? WaitChannel(op, workspace, channel) : 0;
    }

    static int WaitChannel(const CommOpParams &op, volatile uint8_t *workspace, uint32_t channel)
    {
        CompletionFlag flag(workspace, op.coreId, op.numChPerCore, channel);
        return flag.Wait();
    }

    static int StartPrefetch(AicpuAlgorithmCtx &slot, const CommOpParams &op, volatile uint8_t *workspace,
                             uint32_t outerIdx)
    {
        slot.chunkCurBase = static_cast<uint64_t>(outerIdx) << ZBAL_AICPU_OUTERIDX_SHIFT;
        slot.chunkActive = false;
        const RingCtx c = ComputeRingCtx(slot, op);
        if (outerIdx >= 2 && WaitConsumed(op, c, outerIdx - 2) < 0) {
            return BUILD_ERROR;
        }
        if (EmitPhase1Sqes(slot, op, c) < 0) {
            return BUILD_ERROR;
        }
        if (EmitPhase1ReadySqes(slot, op, c) < 0) {
            return BUILD_ERROR;
        }
        return SubmitChannel(slot, op, workspace, 1, false);
    }

    static int BuildAndSubmitReduceRound(AicpuAlgorithmCtx &slot, const CommOpParams &op, volatile uint8_t *workspace,
                                         const RingCtx &c, uint32_t &queuedSqes)
    {
        if (EmitPhase2SliceSqes(slot, op, c) < 0) {
            return BUILD_ERROR;
        }
        if (!slot.chunkActive && EmitConsumedSqe(slot, op, c) < 0) {
            return BUILD_ERROR;
        }
        return SubmitReduceRound(slot, op, workspace, queuedSqes);
    }

    static int EmitConsumedSqe(AicpuAlgorithmCtx &slot, const CommOpParams &op, const RingCtx &c)
    {
        /* Same-channel FIFO publishes consumed after the final remote read. */
        const int64_t delta = static_cast<int64_t>(c.readNeighborRank) - c.myRank;
        const uint64_t remote = op.exchangeGva + delta * slot.ctx->localDeviceMemSize;
        if (AicpuDispatcher::CopyData(slot.ringBufs, 0, c.statSrcGva, remote + c.statDirOff + sizeof(uint64_t),
                                      sizeof(uint64_t), op.channels[0]) != 0) {
            return BUILD_ERROR;
        }
        return 0;
    }

    static int SubmitReduceRound(AicpuAlgorithmCtx &slot, const CommOpParams &op, volatile uint8_t *workspace,
                                 uint32_t &queuedSqes)
    {
        const uint32_t pending = static_cast<uint32_t>(slot.ringBufs[0].sqeCnt);
        const auto *channel = op.channels[0];
        const uint32_t depth = channel == nullptr ? 0U : channel->sq_depth;
        /* Reserve a completion SQE and an empty slot, including on NoWait submissions. */
        if (static_cast<uint64_t>(queuedSqes) + pending + 1U >= depth) {
            return ERR_CAPACITY_EXCEEDED;
        }
        /* Normally only the final round drains; retain the capacity guard for long rings. */
        const bool capacityDrain = static_cast<uint64_t>(queuedSqes) + pending >= depth / 2U;
        if (!slot.chunkActive || capacityDrain) {
            const int ret = SubmitChannel(slot, op, workspace, 0, true);
            if (ret < 0) {
                return ret;
            }
            queuedSqes = 0;
            return 0;
        }
        const int ret = AicpuLaunchTaskNoWait(&slot.ringBufs[0], op.channels[0]);
        if (ret < 0) {
            return ret;
        }
        queuedSqes += pending;
        return 0;
    }

    static int FinishReduceSlot(AicpuAlgorithmCtx &slot, const CommOpParams &op, volatile uint8_t *workspace,
                                const RingCtx &c, uint32_t &queuedSqes)
    {
        while (slot.chunkActive) {
            if (BuildAndSubmitReduceRound(slot, op, workspace, c, queuedSqes) < 0) {
                return BUILD_ERROR;
            }
        }
        return 0;
    }

    /* NoWait work needs a fresh completion on error; an earlier flag cannot drain it. */
    static void DrainReduceOnError(AicpuAlgorithmCtx &slot, const CommOpParams &op, volatile uint8_t *workspace,
                                   const RingCtx &c, uint32_t queuedSqes)
    {
        if (queuedSqes == 0 && !slot.ringBufs[0].HasWork()) {
            return;
        }
        if (!slot.ringBufs[0].HasWork() && AicpuDispatcher::CopyData(slot.ringBufs, 0U, c.statSrcGva, c.statSrcGva,
                                                                     sizeof(uint64_t), op.channels[0]) != 0) {
            return;
        }
        (void)SubmitChannel(slot, op, workspace, 0, true);
    }

    static int ExecutePipeline(AicpuAlgorithmCtx &alg, const CommOpParams &op, volatile uint8_t *workspace)
    {
        AicpuAlgorithmCtx slots[2] = {alg, alg};
        const RingCtx initial = ComputeRingCtx(alg, op);
        if (initial.numOuterChunks == 0) {
            return 0;
        }
        if (StartPrefetch(slots[0], op, workspace, 0) < 0) {
            return BUILD_ERROR;
        }
        for (uint32_t outer = 0; outer < initial.numOuterChunks; ++outer) {
            auto &current = slots[outer & 1U];
            auto &next = slots[(outer + 1) & 1U];
            if (WaitChannel(op, workspace, 1) < 0) {
                return BUILD_ERROR;
            }
            const RingCtx c = ComputeRingCtx(current, op);
            /* Submit round zero NoWait before starting the next outer's prefetch. */
            uint32_t queuedSqes = 0;
            if (BuildAndSubmitReduceRound(current, op, workspace, c, queuedSqes) < 0) {
                DrainReduceOnError(current, op, workspace, c, queuedSqes);
                return BUILD_ERROR;
            }
            const bool hasNext = outer + 1 < initial.numOuterChunks;
            if (hasNext && StartPrefetch(next, op, workspace, outer + 1) < 0) {
                DrainReduceOnError(current, op, workspace, c, queuedSqes);
                return BUILD_ERROR;
            }
            if (FinishReduceSlot(current, op, workspace, c, queuedSqes) < 0) {
                DrainReduceOnError(current, op, workspace, c, queuedSqes);
                if (hasNext) {
                    (void)WaitChannel(op, workspace, 1);
                }
                return BUILD_ERROR;
            }
        }
        /* Drain downstream reads before a subsequent invocation can reuse either slot. */
        const uint32_t first = initial.numOuterChunks > 1 ? initial.numOuterChunks - 2 : 0;
        for (uint32_t outer = first; outer < initial.numOuterChunks; ++outer) {
            auto &slot = slots[outer & 1U];
            slot.chunkCurBase = static_cast<uint64_t>(outer) << ZBAL_AICPU_OUTERIDX_SHIFT;
            if (WaitConsumed(op, ComputeRingCtx(slot, op), outer) < 0) {
                return BUILD_ERROR;
            }
        }
        alg = slots[(initial.numOuterChunks - 1) & 1U];
        alg.chunkCurBase = static_cast<uint64_t>(initial.numOuterChunks) << ZBAL_AICPU_OUTERIDX_SHIFT;
        return 0;
    }

    /* Derived ring parameters for one outer chunk. */
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
    };

    static RingCtx ComputeRingCtx(const AicpuAlgorithmCtx &alg, const CommOpParams &op)
    {
        RingCtx c{};
        c.groupSize = alg.ctx->rankNum;
        c.myRank = alg.ctx->rankId;
        c.dataSize = op.dataSize;
        c.numCores = op.numCores;
        c.elemSize = ZBALDataTypeSize(op.dataType);

        c.coreNumPerRing = c.numCores / ZBAL_AICPU_RING_NUM;
        /* perRankBytes: dataSize is total = N × perRankBytes (unlike AllGather). */
        c.perRankBytes = c.dataSize / c.groupSize;
        const uint64_t halfBytes = (c.perRankBytes / c.elemSize / ZBAL_AICPU_RING_NUM) * c.elemSize;
        c.isCwRing = (op.coreId < c.coreNumPerRing);
        c.ringCoreId = c.isCwRing ? op.coreId : (op.coreId - c.coreNumPerRing);
        c.ringElemCount = c.isCwRing ? halfBytes : (c.perRankBytes - halfBytes);
        c.elemExtraOff = c.isCwRing ? 0 : halfBytes;
        c.streamId = 0U;
        c.statSizePerRank = c.coreNumPerRing * ZBAL_RS_SLICE_PER_CORE;
        c.strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));

        /* cclBuf double-buffer: outer chunk K uses A (even) or B (odd). */
        const uint64_t slotCapacity = ZBAL_RS_CCL_BUFFER_BYTES / (4ULL * c.groupSize);
        const uint64_t chunkLimit = slotCapacity < ZBAL_RS_CHUNK_BYTES ? slotCapacity : ZBAL_RS_CHUNK_BYTES;
        c.chunkBytes = (chunkLimit / c.elemSize) * c.elemSize;
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
        /* Each 64-byte stat stride contains A-ready/A-consumed/B-ready/B-consumed. */
        c.statDirOff += bufSlot * 2ULL * sizeof(uint64_t);

        /* Scratch at end of core ring buffer holds stat value for SDMA to read. */
        uint8_t *coreRingBase = alg.ringBufs[0].localBuff;
        constexpr uint32_t scratchSlots = 2;
        c.scratch = reinterpret_cast<volatile uint64_t *>(coreRingBase + ZBAL_AICPU_CORE_RINGBUF_SIZE -
                                                          (scratchSlots - bufSlot) * sizeof(uint64_t));
        c.statSrcGva = reinterpret_cast<uint64_t>(&c.scratch[0]);

        /* Neighbor cclBuf GVA — published by RingExchange slot 1. */
        const uintptr_t dataSlotAddr =
            op.exchangeGva + static_cast<uint64_t>(c.readNeighborRank) * c.strideBytes + sizeof(uint64_t);
        AicpuCacheInvalidate(dataSlotAddr);
        c.neighborCclBuf = PeerBufferGva(op.exchangeGva, c.readNeighborRank);

        c.reduceOpCode = (uint8_t)((op.dataType << ZBAL_AICPU_RS_DATATYE_SHIFT) | op.reduceOp);
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
    * Copies run on the dedicated prefetch channel, independently of ring reduce.
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

        const uint64_t curStatVal = ChunkToken(op, outerIdx);
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

        constexpr uint32_t ch = 1U; /* Prefetch is independent of channel 0 reduction. */
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
            }

            /* Self-copy: sendBuf[myRank] → recvBuf (pre-loads for final round reduce). */
            uint64_t selfSrc = op.sendBuf + static_cast<uint64_t>(c.myRank) * c.perRankBytes + absChunkStart + sOff;
            uint64_t selfDst = op.recvBuf + absChunkStart + sOff;
            if (AicpuDispatcher::CopyData(ring, ch, selfSrc, selfDst, static_cast<uint32_t>(sLen), op.channels[ch]) !=
                0) {
                alg.chunkActive = false;
                return BUILD_ERROR;
            }
        }
        return BUILD_MORE;
    }

    /* Append ready to the prefetch channel so it executes after all P1 writes. */
    static int EmitPhase1ReadySqes(AicpuAlgorithmCtx &alg, const CommOpParams &op, const RingCtx &c)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;
        constexpr uint32_t prefetchChannel = 1U;

        const uint32_t fwdChunk0 =
            c.isCwRing ? ((c.myRank + c.groupSize - 1) % c.groupSize) : ((c.myRank + 1) % c.groupSize);

        for (uint32_t s = 0; s < ZBAL_RS_SLICE_PER_CORE; s++) {
            const uint64_t statDst = PeerStatAddr(alg, op, c, fwdChunk0, s);
            if (AicpuDispatcher::CopyData(ring, prefetchChannel, c.statSrcGva, statDst, sizeof(uint64_t),
                                          op.channels[prefetchChannel]) != 0) {
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

        const uint64_t curStatVal = ChunkToken(op, outerIdx);

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

    static int WaitConsumed(const CommOpParams &op, const RingCtx &c, uint32_t outerIdx)
    {
        auto *ack = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + c.statDirOff + sizeof(uint64_t));
        if (!AicpuPollStat(ack, ChunkToken(op, outerIdx), 6000000)) {
            return ERR_WAIT_TIMEOUT;
        }
        return 0;
    }

    /* Poll local stat slot for upstream neighbor's signal. */
    static int WaitNeighborStat(const CommOpParams &op, const RingCtx &c, uint32_t reduceChunk, uint32_t slice,
                                uint64_t curStatVal)
    {
        volatile uint64_t *statBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + c.statDirOff);
        const uint32_t strideWords = c.strideBytes / static_cast<uint32_t>(sizeof(uint64_t));
        constexpr uint32_t maxPolls = 6000000U;
        volatile uint64_t *statSlot = statBase + static_cast<uint64_t>(reduceChunk) * c.statSizePerRank * strideWords +
                                      static_cast<uint64_t>(c.ringCoreId) * ZBAL_RS_SLICE_PER_CORE * strideWords +
                                      static_cast<uint64_t>(slice) * strideWords;
        if (!AicpuPollStat(statSlot, curStatVal, maxPolls)) {
            return ERR_WAIT_TIMEOUT;
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
};

#endif /* ZBAL_AICPU_REDUCESCATTER_DOUBLERING_H */
