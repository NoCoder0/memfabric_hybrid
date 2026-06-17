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
#ifndef ZBAL_AICPU_ALLREDUCE_OP_H
#define ZBAL_AICPU_ALLREDUCE_OP_H
#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/*
 * AllReduce = ReduceScatter + AllGather (matches AIV logic).
 * Exchange publishes two GVAs:
 *   slot[0] = sendBuf GVA  (for RS: peers read our input)
 *   slot[1] = buffer GVA   (for AG: peers read our reduced slice)
 */
class AllReduceOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        return AllReduceExchange::Execute(ctx);
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t rankNum = alg.ctx->rankNum;
        const uint32_t myRank = alg.ctx->rankId;
        const uint32_t totalBytes = op.dataSize;
        if (totalBytes == 0 || rankNum == 0) {
            return BUILD_DONE;
        }
        if (numChPerCore == 0) {
            return BUILD_ERROR;
        }

        const uint32_t slice = totalBytes / rankNum;
        const uint32_t elements = (myRank != rankNum - 1) ? slice : totalBytes - (rankNum - 1) * slice;

        if (coreId == 0) {
            AICPU_DBG(TAG_ENTRY, op.sendBuf, op.buffer);
            AICPU_DBG(TAG_UPDATE_CTX, totalBytes, elements);
        }

        const uint8_t reduceOpCode = (uint8_t)((op.dataType << 4) | op.reduceOp);
        constexpr uint32_t SID = 0;

        /* Parallel slice over this rank's elements */
        uint64_t coreOff;
        uint64_t coreLen;
        AicpuParallelSlice(elements, op.coreId, op.numCores, coreOff, coreLen);
        if (coreLen == 0) {
            return BUILD_DONE;
        }

        /* ================================================================
         * Phase 1: ReduceScatter
         *   - Self-copy: sendBuf[mySlice] → buffer (initialize with local data)
         *   - Peer reduce: peerSendBuf[mySlice] → buffer (SDMA hardware reduce)
         * ================================================================ */

        /* Self-copy: sendBuf[myRank*slice + coreOff] → buffer[coreOff] */
        {
            uint64_t selfSrc = op.sendBuf + (uint64_t)myRank * slice + coreOff;
            uint64_t selfDst = op.buffer + coreOff;
            if (AicpuDispatcher::CopyData(ringBufs, SID, selfSrc, selfDst, (uint32_t)coreLen, channels[SID]) != 0) {
                return BUILD_ERROR;
            }
        }
        if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
            return ERR_WAIT_TIMEOUT;
        }

        /* Reduce from peers: peerSendBuf[mySlice] → buffer (multi-channel parallel)
         * SDMA reduce is hardware-atomic per element — safe for concurrent writes. */
        for (uint32_t r = 0; r < rankNum; r++) {
            if (r == myRank) {
                continue;
            }
            uint64_t peerSendBuf = PeerOutputBuf(op.exchangeGva, r);
            uint64_t peerSrc = peerSendBuf + (uint64_t)myRank * slice + coreOff;
            uint64_t dst = op.buffer + coreOff;
            uint32_t sid = r % numChPerCore;
            if (AicpuDispatcher::CopyData(ringBufs, sid, peerSrc, dst, (uint32_t)coreLen, channels[sid],
                                          reduceOpCode) != 0) {
                return BUILD_ERROR;
            }
        }
        if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
            return ERR_WAIT_TIMEOUT;
        }

        /* ================================================================
         * Cross-device barrier: ensure all ranks completed RS before AG.
         * Core 0 writes waitSymbol to peers' flag slot[1] and polls for match.
         * Uses flag area slot[1] (slot[0] was used by exchange-complete).
         * ================================================================ */
        {
            const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
            const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;
            const uint64_t waitSymbol = op.waitSymbol;

            if (coreId == 0) {
                /* Write waitSymbol to all peers' flag slot[1] */
                volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(workspace, 0);
                volatile uint64_t *scratch =
                    reinterpret_cast<volatile uint64_t *>(myBuf + ZBAL_AICPU_CORE_RINGBUF_SIZE - sizeof(uint64_t));
                *scratch = waitSymbol;
                uint64_t sentinelSrc = reinterpret_cast<uint64_t>(scratch);

                SqeLocalRingBuffer eb;
                eb.Init(const_cast<uint8_t *>(myBuf));

                for (uint32_t dstRank = 0; dstRank < rankNum; dstRank++) {
                    if (dstRank == myRank) {
                        continue;
                    }
                    int64_t delta = (int64_t)dstRank - (int64_t)myRank;
                    int64_t devOff = delta * (int64_t)alg.ctx->localDeviceMemSize;
                    /* flag slot[1] = flagArea + myRank*stride + sizeof(uint64_t) */
                    uint64_t flagDst = alg.ctx->exchangeGva + (uint64_t)devOff + flagAreaOff +
                                       (uint64_t)myRank * strideBytes + sizeof(uint64_t);
                    if (AicpuDispatcher::CopyData(&eb, 0U, sentinelSrc, flagDst, sizeof(uint64_t), channels[SID]) !=
                        0) {
                        return BUILD_ERROR;
                    }
                }
                /* Also write to self (local) */
                volatile uint64_t *selfFlag = reinterpret_cast<volatile uint64_t *>(
                    alg.ctx->exchangeGva + flagAreaOff + (uint64_t)myRank * strideBytes + sizeof(uint64_t));
                *selfFlag = waitSymbol;

                if (eb.HasWork()) {
                    uint32_t fid = AicpuWorkspace::FlagIdx(0, numChPerCore, 0);
                    if (AicpuLaunchTaskMc(&eb, channels[SID], workspace, 0, 1, 0, fid) < 0 ||
                        CompletionFlag(workspace, fid).Wait() < 0) {
                        return ERR_WAIT_TIMEOUT;
                    }
                }

                /* Poll for all peers' RS-done flags at slot[1] */
                volatile uint64_t *flagBase = reinterpret_cast<volatile uint64_t *>(alg.ctx->exchangeGva + flagAreaOff);
                constexpr uint32_t kBarrierTimeout = 6000000;
                for (uint32_t r = 0; r < rankNum; r++) {
                    if (r == myRank) {
                        continue;
                    }
                    volatile uint64_t *peerFlag = &flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE + 1];
                    bool ready = false;
                    for (uint32_t t = 0; t < kBarrierTimeout && !ready; t++) {
                        uintptr_t fa = reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(peerFlag));
                        AicpuCacheInvalidate(fa);
                        if (*peerFlag == waitSymbol) {
                            ready = true;
                        }
                    }
                    if (!ready) {
                        return ERR_WAIT_TIMEOUT;
                    }
                }
            }
            AicpuCoreBarrier(workspace, numCores);
        }

        /* ================================================================
         * Phase 2: AllGather — multi-channel parallel
         *   Each rank's copy targets a different recvBuf region, so we can
         *   distribute across channels safely (no write conflicts).
         * ================================================================ */
        for (uint32_t r = 0; r < rankNum; r++) {
            uint64_t src;
            uint32_t rElements = (r != rankNum - 1) ? slice : totalBytes - (rankNum - 1) * slice;

            /* Recompute core slice for this rank's elements */
            uint64_t rCoreOff;
            uint64_t rCoreLen;
            AicpuParallelSlice(rElements, op.coreId, op.numCores, rCoreOff, rCoreLen);
            if (rCoreLen == 0) {
                continue;
            }

            if (r == myRank) {
                src = op.buffer + rCoreOff;
            } else {
                src = PeerBufferGva(op.exchangeGva, r) + rCoreOff;
            }
            uint64_t dst = op.recvBuf + (uint64_t)r * slice + rCoreOff;
            uint32_t sid = r % numChPerCore; /* round-robin across channels */
            if (AicpuDispatcher::CopyData(ringBufs, sid, src, dst, (uint32_t)rCoreLen, channels[sid]) != 0) {
                return BUILD_ERROR;
            }
        }

        return AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0 ? ERR_WAIT_TIMEOUT
                                                                                           : BUILD_DONE;
    }
};
#endif
