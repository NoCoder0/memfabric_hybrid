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
#ifndef ZBAL_AICPU_REDUCESCATTER_OP_H
#define ZBAL_AICPU_REDUCESCATTER_OP_H
#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"
#include "ops/reduce_scatter/zbal_aicpu_reducescatter_doublering.h"

/*
 * ReduceScatter: SDMA hardware reduce.
 * opCode = (dataType << 4) | reduceOp  (matches AIV SetAtomicOpSDMA).
 *
 * Two algorithms (selected by data size):
 *   - FULL_MESH  (≤32MB): 1 core, 8 channels. Batched progressive barrier —
 *     self-copy (NoWait) starts immediately, then batched poll submits reduce
 *     SQEs for ready peers. Slow peers don't block fast ones.
 *   - DOUBLE_RING (>32MB): 2 cores (CW+CCW), 3-slice stat pipeline.
 *     NoWait submission for slices 0/1, Mc+flag only for slice 2.
 */
class ReduceScatterOp {
public:
    /* Exchange strategy by commAlg: DOUBLE_RING→RingExchange, FULL_MESH→FullMeshExchange */
    static int AddrExchange(uint32_t commAlg, const ExchangeContext &ctx)
    {
        switch (commAlg) {
            case ZBAL_COMM_ALG_DOUBLE_RING:
                return RingExchange::Execute(ctx);
            default:
                return FullMeshExchange::Execute(ctx);
        }
    }

    /* Execution dispatch by commAlg */
    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        switch (op.commAlg) {
            case ZBAL_COMM_ALG_DOUBLE_RING:
                return ReduceScatterDoubleRing::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId,
                                                        numCores);
            default:
                return ExecuteFullMesh(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        }
    }

private:
    /* Per-channel layout: dst GVA, length, offset-in-slice, has-work flag. */
    struct ChLayout {
        uint64_t chDst[ZBAL_AICPU_MAX_CH_PER_CORE];
        uint64_t chLen[ZBAL_AICPU_MAX_CH_PER_CORE];
        uint64_t chOffInSlice[ZBAL_AICPU_MAX_CH_PER_CORE];
        bool chHasWork[ZBAL_AICPU_MAX_CH_PER_CORE];
    };

    /* Split coreLen across channels by element boundary (SDMA requires elem-aligned length). */
    static int InitChLayout(const CommOpParams &op, uint32_t numChPerCore, uint64_t coreOff, uint64_t coreLen,
                            ChLayout &layout)
    {
        const uint32_t elemSize = ZBALDataTypeSize(op.dataType);
        if (elemSize == 0 || numChPerCore == 0) {
            return BUILD_ERROR;
        }
        const uint64_t totalElems = coreLen / elemSize;
        const uint64_t elemsPerCh = totalElems / numChPerCore;
        const uint64_t elemRem = totalElems % numChPerCore;
        uint64_t off = 0;
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            uint64_t thisChElems = elemsPerCh + (ch < elemRem ? 1 : 0);
            layout.chLen[ch] = thisChElems * elemSize;
            layout.chOffInSlice[ch] = off;
            layout.chDst[ch] = op.recvBuf + coreOff + off;
            layout.chHasWork[ch] = (layout.chLen[ch] > 0);
            off += layout.chLen[ch];
        }
        return 0;
    }

    /* Fill self-copy SQEs into ringBufs (no submit — caller chooses NoWait or Mc). */
    static int FillSelfCopySqes(const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                                volatile stars_channel_info_t **channels, uint32_t numChPerCore, const ChLayout &layout,
                                uint64_t mySliceOff, uint64_t coreOff)
    {
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            if (!layout.chHasWork[ch]) {
                continue;
            }
            uint64_t selfSrc = op.sendBuf + mySliceOff + coreOff + layout.chOffInSlice[ch];
            if (AicpuDispatcher::CopyData(ringBufs, ch, selfSrc, layout.chDst[ch], (uint32_t)layout.chLen[ch],
                                          channels[ch]) != 0) {
                return BUILD_ERROR;
            }
        }
        return 0;
    }

    /* Fill peer reduce SQEs (RMW) into ringBufs (no submit). */
    static int FillPeerReduceSqes(const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                                  volatile stars_channel_info_t **channels, uint32_t numChPerCore,
                                  const ChLayout &layout, uint64_t mySliceOff, uint64_t coreOff, uint64_t peerSendBuf,
                                  uint8_t reduceOpCode)
    {
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            if (!layout.chHasWork[ch]) {
                continue;
            }
            uint64_t peerSrc = peerSendBuf + mySliceOff + coreOff + layout.chOffInSlice[ch];
            if (AicpuDispatcher::CopyData(ringBufs, ch, peerSrc, layout.chDst[ch], (uint32_t)layout.chLen[ch],
                                          channels[ch], reduceOpCode) != 0) {
                return BUILD_ERROR;
            }
        }
        return 0;
    }

    /* NoWait submit on all channels that have work. */
    static void SubmitNoWaitAllChannels(SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                                        uint32_t numChPerCore)
    {
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            if (ringBufs[ch].HasWork()) {
                AicpuLaunchTaskNoWait(&ringBufs[ch], channels[ch]);
            }
        }
    }

    /* Mc submit (flag) on all channels that have work; marks submitted[]. */
    static int SubmitMcAllChannels(SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                                   volatile uint8_t *workspace, uint32_t numChPerCore, uint32_t coreId, bool *submitted)
    {
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            if (!ringBufs[ch].HasWork()) {
                continue;
            }
            uint32_t fid = AicpuWorkspace::FlagIdx(coreId, numChPerCore, ch);
            if (AicpuLaunchTaskMc(&ringBufs[ch], channels[ch], workspace, coreId, 1, ch, fid) < 0) {
                return ERR_DOORBELL_FAILED;
            }
            submitted[ch] = true;
        }
        return 0;
    }

    /* Wait for completion flags on submitted channels. */
    static int WaitCompletion(volatile uint8_t *workspace, uint32_t coreId, uint32_t numChPerCore,
                              const bool *submitted)
    {
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            if (!submitted[ch]) {
                continue;
            }
            CompletionFlag flag(workspace, coreId, numChPerCore, ch);
            if (flag.Wait() < 0) {
                return ERR_WAIT_TIMEOUT;
            }
        }
        return 0;
    }

    /* Submit built reduce SQEs: Mc (flag) if last pending peer, else NoWait. */
    static int SubmitPeerReduce(SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                                volatile uint8_t *workspace, uint32_t numChPerCore, uint32_t coreId,
                                uint32_t pendingCount, bool *submitted)
    {
        if (pendingCount == 0) {
            return SubmitMcAllChannels(ringBufs, channels, workspace, numChPerCore, coreId, submitted);
        }
        SubmitNoWaitAllChannels(ringBufs, channels, numChPerCore);
        return 0;
    }

    /* Process one ready peer: read peer sendBuf GVA, build+submit reduce SQEs.
     * Returns 0 on success, negative on error, 1 if peer flag not yet ready (skip). */
    static int ProcessReadyPeer(const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                                volatile stars_channel_info_t **channels, uint32_t numChPerCore, const ChLayout &layout,
                                uint64_t mySliceOff, uint64_t coreOff, uint8_t reduceOpCode, uint32_t r,
                                uint32_t coreId, volatile uint8_t *workspace, volatile uint64_t *flagBase,
                                volatile const uint64_t *exchBase, uint64_t &pendingMask, uint32_t &pendingCount,
                                bool *submitted)
    {
        uintptr_t flagAddr =
            reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(&flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE]));
        AicpuCacheInvalidate(flagAddr);
        if (flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE] != op.waitSymbol) {
            return 1; /* not ready — caller skips */
        }
        pendingMask &= ~(1ULL << r);
        pendingCount--;

        uintptr_t peerSlotAddr =
            reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(&exchBase[r * ZBAL_AICPU_EXCHANGE_STRIDE]));
        AicpuCacheInvalidate(peerSlotAddr);
        const uint64_t peerSendBuf = exchBase[r * ZBAL_AICPU_EXCHANGE_STRIDE];

        if (FillPeerReduceSqes(op, ringBufs, channels, numChPerCore, layout, mySliceOff, coreOff, peerSendBuf,
                               reduceOpCode) != 0) {
            return BUILD_ERROR;
        }
        return SubmitPeerReduce(ringBufs, channels, workspace, numChPerCore, coreId, pendingCount, submitted);
    }

    /* Batched progressive barrier: scan pending peers, submit reduce SQEs for ready ones.
     * Last pending peer uses Mc submit (flag); others use NoWait.
     * Slow peers don't block fast ones — ready peers are processed immediately. */
    static int BatchedProgressiveBarrier(const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                                         volatile stars_channel_info_t **channels, uint32_t numChPerCore,
                                         const ChLayout &layout, uint64_t mySliceOff, uint64_t coreOff,
                                         uint8_t reduceOpCode, uint32_t rankNum, uint32_t myRank, uint32_t coreId,
                                         volatile uint8_t *workspace, bool *submitted)
    {
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
        const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;
        volatile uint64_t *flagBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + flagAreaOff);
        volatile const uint64_t *exchBase = reinterpret_cast<volatile const uint64_t *>(op.exchangeGva);

        uint64_t pendingMask = 0;
        uint32_t pendingCount = 0;
        for (uint32_t r = 0; r < rankNum; r++) {
            if (r != myRank) {
                pendingMask |= (1ULL << r);
                pendingCount++;
            }
        }

        constexpr uint32_t kBatchedTimeout = 6000000;
        uint32_t totalTimeout = 0;
        while (pendingCount > 0) {
            uint32_t submittedThisRound = 0;
            uint64_t mask = pendingMask;
            while (mask != 0) {
                uint32_t r = static_cast<uint32_t>(__builtin_ctzll(mask));
                mask &= mask - 1;
                int ret =
                    ProcessReadyPeer(op, ringBufs, channels, numChPerCore, layout, mySliceOff, coreOff, reduceOpCode, r,
                                     coreId, workspace, flagBase, exchBase, pendingMask, pendingCount, submitted);
                if (ret < 0) {
                    return ret;
                }
                if (ret == 0) {
                    submittedThisRound++;
                }
            }
            if (submittedThisRound == 0) {
                if (++totalTimeout >= kBatchedTimeout) {
                    return ERR_WAIT_TIMEOUT;
                }
            } else {
                totalTimeout = 0;
            }
        }
        return 0;
    }

    /* FullMesh: batched progressive barrier.
     * 1. self-copy (NoWait) → SDMA starts immediately
     * 2. batched poll: scan pending peers, submit reduce SQEs (NoWait) for ready ones
     * 3. last pending peer: Mc submit (flag) fires after all self-copy + reduces
     * RMW safety: serial within channel (in-order), disjoint data ranges across channels. */
    static int ExecuteFullMesh(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                               volatile stars_channel_info_t **channels, uint32_t numChPerCore,
                               volatile uint8_t *workspace, uint32_t coreId, uint32_t numCores)
    {
        const uint32_t rankNum = alg.ctx->rankNum;
        const uint32_t myRank = alg.ctx->rankId;
        if (rankNum == 0 || numChPerCore == 0 || rankNum > ZBAL_CONST_64) {
            return BUILD_ERROR; /* rankNum > 64 overflows uint64 pendingMask */
        }
        const uint32_t perRankBytes = op.dataSize / rankNum;
        if (perRankBytes == 0) {
            return BUILD_DONE;
        }

        uint64_t coreOff;
        uint64_t coreLen;
        AicpuParallelSlice(perRankBytes, op.coreId, op.numCores, coreOff, coreLen);
        if (coreLen == 0) {
            return BUILD_DONE;
        }

        ChLayout layout{};
        if (InitChLayout(op, numChPerCore, coreOff, coreLen, layout) != 0) {
            return BUILD_ERROR;
        }

        const uint64_t mySliceOff = (uint64_t)myRank * perRankBytes;
        bool submitted[ZBAL_AICPU_MAX_CH_PER_CORE] = {};

        /* rankNum == 1: self-copy + Mc submit (flag) + wait — no peers to reduce. */
        if (rankNum == 1) {
            if (FillSelfCopySqes(op, ringBufs, channels, numChPerCore, layout, mySliceOff, coreOff) != 0) {
                return BUILD_ERROR;
            }
            if (SubmitMcAllChannels(ringBufs, channels, workspace, numChPerCore, coreId, submitted) != 0) {
                return ERR_DOORBELL_FAILED;
            }
            return WaitCompletion(workspace, coreId, numChPerCore, submitted);
        }

        /* Step 1: self-copy (NoWait) — starts immediately, overlaps with poll */
        if (FillSelfCopySqes(op, ringBufs, channels, numChPerCore, layout, mySliceOff, coreOff) != 0) {
            return BUILD_ERROR;
        }
        SubmitNoWaitAllChannels(ringBufs, channels, numChPerCore);

        /* Step 2: batched progressive barrier — submit reduce SQEs for ready peers */
        const uint8_t reduceOpCode = (uint8_t)((op.dataType << 4) | op.reduceOp);
        int barRet = BatchedProgressiveBarrier(op, ringBufs, channels, numChPerCore, layout, mySliceOff, coreOff,
                                               reduceOpCode, rankNum, myRank, coreId, workspace, submitted);
        if (barRet != 0) {
            return barRet;
        }

        /* Step 3: wait for completion flags */
        return WaitCompletion(workspace, coreId, numChPerCore, submitted);
    }
};
#endif
