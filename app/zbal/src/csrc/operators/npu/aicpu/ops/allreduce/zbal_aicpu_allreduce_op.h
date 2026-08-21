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
        const uint64_t totalBytes = op.dataSize;
        if (totalBytes == 0 || rankNum == 0) {
            return BUILD_DONE;
        }
        if (numChPerCore == 0) {
            return BUILD_ERROR;
        }

        /* When totalBytes is too small for RS+AG decomposition (slice per rank
         * would be < 1 element), use full reduce: each rank reduces ALL data
         * from ALL peers into its buffer, then copies buffer → recvBuf.
         * This matches AIV ProcessElemsLtGroupSize (totalElems <= groupSize).
         * Without this, slice=totalBytes/rankNum truncates to sub-element bytes
         * (e.g. 3 float32 / 8 ranks = 1 byte) → unaligned SDMA reduce → wrong result. */
        const uint32_t elemSize = ZBALDataTypeSize(op.dataType);
        const bool fullReduce = (elemSize > 0 && totalBytes < (uint64_t)rankNum * elemSize);

        /* Element-aligned slice: SDMA reduce requires elem-aligned src/dst/len.
         * Byte division misaligns rank offsets when totalBytes is not divisible
         * by rankNum × elemSize. Last rank takes the element remainder
         * (matches AIV zbal_kernel_allreduce.cpp). */
        const uint64_t slice = fullReduce ? totalBytes : (totalBytes / elemSize / rankNum) * elemSize;
        const uint64_t elements = (fullReduce || myRank != rankNum - 1) ? slice : totalBytes - (rankNum - 1) * slice;
        /* In full reduce, every rank processes offset 0 of the data (no scatter). */
        const uint64_t rankOff = fullReduce ? 0 : (uint64_t)myRank * slice;

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

        /* Self-copy: sendBuf[rankOff + coreOff] → buffer[coreOff]
         * (rankOff=0 for fullReduce, myRank*slice for RS+AG) */
        {
            uint64_t selfSrc = op.sendBuf + rankOff + coreOff;
            uint64_t selfDst = op.buffer + coreOff;
            if (AicpuDispatcher::CopyData(ringBufs, SID, selfSrc, selfDst, (uint32_t)coreLen, channels[SID]) != 0) {
                return BUILD_ERROR;
            }
        }
        if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
            return ERR_WAIT_TIMEOUT;
        }

        /* Reduce from peers: peerSendBuf[mySlice] → buffer (single-channel serial)
         * SDMA reduce (opcode!=0) is read-modify-write, NOT atomic across channels.
         * All peer reduces write to the SAME buffer+coreOff address — concurrent RMW
         * across channels loses updates (A reads 0, B reads 0, A writes 5, B writes 3
         * → result 3, lost A). Must serialize on SID=0: SDMA executes SQEs within a
         * channel in order, ensuring correct sequential accumulation.
         * (Contrast: AllGather uses memcpy opcode=0 to DIFFERENT dsts per rank →
         * multi-channel parallel is safe there.) */
        for (uint32_t r = 0; r < rankNum; r++) {
            if (r == myRank) {
                continue;
            }
            /* Invalidate cache before reading peer's sendBuf GVA (SDMA-written) */
            uintptr_t peerSlotAddr = reinterpret_cast<uintptr_t>(
                &reinterpret_cast<const uint64_t *>(op.exchangeGva)[r * ZBAL_AICPU_EXCHANGE_STRIDE]);
            AicpuCacheInvalidate(peerSlotAddr);
            uint64_t peerSendBuf = PeerOutputBuf(op.exchangeGva, r);
            uint64_t peerSrc = peerSendBuf + rankOff + coreOff;
            uint64_t dst = op.buffer + coreOff;
            if (AicpuDispatcher::CopyData(ringBufs, SID, peerSrc, dst, (uint32_t)coreLen, channels[SID],
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
                /* Flush scratch so SDMA reads the correct waitSymbol value */
                AicpuCacheFlush(reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(scratch)));
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
                /* Flush self flag so remote ranks' polling sees the correct value */
                AicpuCacheFlush(reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(selfFlag)));

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
         * Phase 2: AllGather (RS+AG) or local copy (fullReduce)
         *   RS+AG: copy each rank's buffer slice → recvBuf (multi-channel)
         *   fullReduce: buffer already has the complete result → local copy
         * ================================================================ */
        if (fullReduce) {
            /* Full reduce: each rank already has the full result in buffer.
             * Just copy buffer[coreOff] → recvBuf[coreOff] (local, no cross-rank). */
            uint64_t src = op.buffer + coreOff;
            uint64_t dst = op.recvBuf + coreOff;
            if (AicpuDispatcher::CopyData(ringBufs, SID, src, dst, (uint32_t)coreLen, channels[SID]) != 0) {
                return BUILD_ERROR;
            }
        } else {
            /* AllGather: each rank's copy targets a different recvBuf region,
             * so we can distribute across channels safely (no write conflicts). */
            for (uint32_t r = 0; r < rankNum; r++) {
                uint64_t src;
                uint64_t rElements = (r != rankNum - 1) ? slice : totalBytes - (rankNum - 1) * slice;

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
                    /* Invalidate cache before reading peer's buffer GVA (SDMA-written) */
                    uintptr_t peerSlotAddr = reinterpret_cast<uintptr_t>(
                        &reinterpret_cast<const uint64_t *>(op.exchangeGva)[r * ZBAL_AICPU_EXCHANGE_STRIDE + 1]);
                    AicpuCacheInvalidate(peerSlotAddr);
                    src = PeerBufferGva(op.exchangeGva, r) + rCoreOff;
                }
                uint64_t dst = op.recvBuf + (uint64_t)r * slice + rCoreOff;
                uint32_t sid = r % numChPerCore; /* round-robin across channels */
                if (AicpuDispatcher::CopyData(ringBufs, sid, src, dst, (uint32_t)rCoreLen, channels[sid]) != 0) {
                    return BUILD_ERROR;
                }
            }
        }

        if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
            return ERR_WAIT_TIMEOUT;
        }

        /* ================================================================
         * End-of-operation barrier: ensure all ranks completed Phase 2
         * before any rank's next operation can start AllReduceExchange.
         *
         * Without this, a fast rank's next AllReduceExchange overwrites
         * peers' exchange area slot[0]/slot[1] (sendBuf/buffer GVA) while
         * a slow rank is still reading them in Phase 2. Since buffer is
         * allocated fresh per operation (at::empty), the GVA changes
         * between operations → slow rank reads wrong buffer GVA → SDMA
         * copies from wrong address → data corruption.
         *
         * Uses flag area slot[2] (slot[0]=exchange, slot[1]=RS→AG barrier).
         * ================================================================ */
        {
            const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
            const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;
            const uint64_t waitSymbol = op.waitSymbol;

            if (coreId == 0) {
                volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(workspace, 0);
                volatile uint64_t *scratch =
                    reinterpret_cast<volatile uint64_t *>(myBuf + ZBAL_AICPU_CORE_RINGBUF_SIZE - sizeof(uint64_t));
                *scratch = waitSymbol;
                AicpuCacheFlush(reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(scratch)));
                uint64_t sentinelSrc = reinterpret_cast<uint64_t>(scratch);

                SqeLocalRingBuffer eb;
                eb.Init(const_cast<uint8_t *>(myBuf));

                for (uint32_t dstRank = 0; dstRank < rankNum; dstRank++) {
                    if (dstRank == myRank) {
                        continue;
                    }
                    int64_t delta = (int64_t)dstRank - (int64_t)myRank;
                    int64_t devOff = delta * (int64_t)alg.ctx->localDeviceMemSize;
                    /* flag slot[2] = flagArea + myRank*stride + 2*sizeof(uint64_t) */
                    uint64_t flagDst = alg.ctx->exchangeGva + (uint64_t)devOff + flagAreaOff +
                                       (uint64_t)myRank * strideBytes + 2 * sizeof(uint64_t);
                    if (AicpuDispatcher::CopyData(&eb, 0U, sentinelSrc, flagDst, sizeof(uint64_t), channels[SID]) !=
                        0) {
                        return BUILD_ERROR;
                    }
                }
                /* Also write to self (local) */
                volatile uint64_t *selfFlag = reinterpret_cast<volatile uint64_t *>(
                    alg.ctx->exchangeGva + flagAreaOff + (uint64_t)myRank * strideBytes + 2 * sizeof(uint64_t));
                *selfFlag = waitSymbol;
                AicpuCacheFlush(reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(selfFlag)));

                if (eb.HasWork()) {
                    uint32_t fid = AicpuWorkspace::FlagIdx(0, numChPerCore, 0);
                    if (AicpuLaunchTaskMc(&eb, channels[SID], workspace, 0, 1, 0, fid) < 0 ||
                        CompletionFlag(workspace, fid).Wait() < 0) {
                        return ERR_WAIT_TIMEOUT;
                    }
                }

                /* Poll for all peers' end-of-operation flags at slot[2] */
                volatile uint64_t *flagBase = reinterpret_cast<volatile uint64_t *>(alg.ctx->exchangeGva + flagAreaOff);
                constexpr uint32_t kEndBarrierTimeout = 6000000;
                for (uint32_t r = 0; r < rankNum; r++) {
                    if (r == myRank) {
                        continue;
                    }
                    volatile uint64_t *peerFlag = &flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE + 2];
                    bool ready = false;
                    for (uint32_t t = 0; t < kEndBarrierTimeout && !ready; t++) {
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

        return BUILD_DONE;
    }
};
#endif
