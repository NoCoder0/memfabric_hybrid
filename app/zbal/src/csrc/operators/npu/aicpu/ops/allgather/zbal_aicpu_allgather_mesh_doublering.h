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

#ifndef ZBAL_AICPU_ALLGATHER_MESH_DOUBLERING_H
#define ZBAL_AICPU_ALLGATHER_MESH_DOUBLERING_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/*
* MeshDoubleRing AllGather — bidirectional ring with full-mesh address exchange.
*
* Address exchange: FullMeshExchange publishes every rank's sendBuf GVA, so all
* ranks' sendBuf are globally readable once the exchange barrier completes.
* No ring forwarding or stat synchronization is needed — each rank reads the
* source rank's sendBuf directly.
*
* Algorithm (per rank, groupSize = N):
*   Self-copy: sendBuf → recvBuf[myRank]
*              CW ring copies [0, cwElemCount), CCW ring copies [cwElemCount, dataSize).
*
*   Peer copies: ceil((N-1)/2) steps, each step copies one rank from each ring
*   direction:
*     CW  step i → srcRank = (myRank + 1 + i) % N   (next direction)
*     CCW step i → srcRank = (myRank - 1 - i + N) % N (prev direction)
*
*   Byte-range per step:
*     - If cwSrcRank != ccwSrcRank (different ranks): each ring copies the
*       FULL [0, dataSize) for its srcRank — both rings work in parallel on
*       different ranks.
*     - If cwSrcRank == ccwSrcRank (happens when N is even, last step): each
*       ring copies half — CW copies [0, cwElemCount), CCW copies [cwElemCount, dataSize).
*
* Parallelism:
*   - 2 rings (CW/CCW) each occupy one AICPU core, each using 1 SDMA channel (channel 0).
*   - Peer copies are spread across ALL available channels via srcRank % numChPerCore,
*     maximizing SDMA engine parallelism (same strategy as AllGatherFullMesh).
*   - Cores within each ring shard the byte-range via AicpuParallelSlice.
*
* Execution: single BuildSqes → AicpuSubmitAndWait (no multi-phase state machine).
* All copies are independent (read from sendBuf, write to disjoint recvBuf slots),
* so they can be submitted in a single doorbell with no inter-phase bubbles.
*
* Exchange area layout (owned by FullMeshExchange):
*   [0 .. rankNum*stride)            data area (sendBuf GVA per rank)
*   [rankNum*stride .. 2*rankNum*stride)  flag area (cross-device sync)
*/

class AllGatherMeshDoubleRing {
public:
    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        if (BuildSqes(alg, op) < 0) {
            return BUILD_ERROR;
        }
        if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
            return ERR_WAIT_TIMEOUT;
        }
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

        /* ── Ring params ──
        * 2 rings (CW/CCW) each use 1 channel for self-copy (channel 0).
        * Cores within each ring shard the byte-range via AicpuParallelSlice. */
        const uint32_t coreNumPerRing = numCores / ZBAL_AICPU_RING_NUM;
        if (coreNumPerRing == 0) {
            return BUILD_ERROR;
        }
        const bool isCwRing = (op.coreId < coreNumPerRing);
        const uint32_t ringCoreId = isCwRing ? op.coreId : (op.coreId - coreNumPerRing);
        /* mesh_doublering: each core uses 1 channel for self-copy (channel 0).
         *  CW/CCW distinguished by coreId. Peer copies use srcRank % numChPerCore. */
        const uint32_t streamId = 0U;
        /* Split data in half: CW handles [0, cwElemCount), CCW handles [cwElemCount, dataSize).
         * Use dataSize - cwElemCount for CCW so the tail byte is never lost when dataSize is odd. */
        const uint64_t cwElemCount = dataSize / ZBAL_AICPU_RING_NUM;
        const uint64_t ccwElemCount = dataSize - cwElemCount;
        const uint32_t numChPerCore = op.numChPerCore;

        /* ── Self-copy: sendBuf → recvBuf[myRank] ──
        * CW ring copies first half, CCW ring copies second half. */
        {
            const uint64_t ringElemCount = isCwRing ? cwElemCount : ccwElemCount;
            const uint64_t baseOff = isCwRing ? 0ULL : cwElemCount;
            uint64_t sliceOff;
            uint64_t sliceLen;
            AicpuParallelSlice(ringElemCount, ringCoreId, coreNumPerRing, sliceOff, sliceLen);
            if (sliceLen > 0) {
                uint64_t myOff = sliceOff + baseOff;
                uint64_t selfSrc = op.sendBuf + myOff;
                uint64_t selfDst = op.recvBuf + static_cast<uint64_t>(myRank) * dataSize + myOff;
                if (AicpuDispatcher::CopyData(ring, streamId, selfSrc, selfDst, static_cast<uint32_t>(sliceLen),
                                              op.channels[streamId], op.reduceOp) != 0) {
                    return BUILD_ERROR;
                }
            }
        }

        /* ── Peer copies: ceil((groupSize-1)/2) steps, all in one doorbell ──
        *
        * CW  step i → srcRank = (myRank + 1 + i) % groupSize   (next dir)
        * CCW step i → srcRank = (myRank - 1 - i + groupSize) % groupSize (prev dir)
        *
        * When both directions point to the same rank (N even, last step),
        * each ring copies half the data. Otherwise, each ring copies full data.
        *
        * Peer copies are spread across all channels via srcRank % numChPerCore
        * for maximum SDMA parallelism. No stat synchronization — sendBuf GVA
        * is globally readable after FullMeshExchange. */
        const uint32_t totalSteps = (groupSize - 1U + 1U) / 2U; /* ceil((N-1)/2) */
        for (uint32_t step = 0; step < totalSteps; step++) {
            uint32_t cwSrcRank = (myRank + 1U + step) % groupSize;
            uint32_t ccwSrcRank = (myRank + groupSize - 1U - step) % groupSize;
            uint32_t srcRank = isCwRing ? cwSrcRank : ccwSrcRank;

            /* Determine byte-range for this step */
            uint64_t elemLen;
            uint64_t elemOff;
            if (cwSrcRank == ccwSrcRank) {
                /* Last step, both rings target the same rank — split bytes.
                 * CW covers [0, cwElemCount), CCW covers [cwElemCount, dataSize)
                 * so the tail byte is preserved when dataSize is odd. */
                elemLen = isCwRing ? cwElemCount : ccwElemCount;
                elemOff = isCwRing ? 0ULL : cwElemCount;
            } else {
                /* Different ranks — each ring copies full data */
                elemLen = dataSize;
                elemOff = 0ULL;
            }

            uint64_t sliceOff;
            uint64_t sliceLen;
            AicpuParallelSlice(elemLen, ringCoreId, coreNumPerRing, sliceOff, sliceLen);
            if (sliceLen > 0) {
                uint64_t myOff = sliceOff + elemOff;
                uint64_t remoteSrc = PeerOutputBuf(op.exchangeGva, srcRank) + myOff;
                uint64_t localDst = op.recvBuf + static_cast<uint64_t>(srcRank) * dataSize + myOff;
                /* Spread peer copies across all channels for parallel SDMA */
                uint32_t sid = srcRank % numChPerCore;
                if (AicpuDispatcher::CopyData(ring, sid, remoteSrc, localDst, static_cast<uint32_t>(sliceLen),
                                              op.channels[sid], op.reduceOp) != 0) {
                    return BUILD_ERROR;
                }
            }
        }

        return BUILD_DONE;
    }
};

#endif /* ZBAL_AICPU_ALLGATHER_MESH_DOUBLERING_H */
