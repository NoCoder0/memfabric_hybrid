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
#ifndef ZBAL_AICPU_ALLTOALLV_OP_H
#define ZBAL_AICPU_ALLTOALLV_OP_H
#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/*
 * AlltoAllV: variable-length all-to-all.
 *
 * Exchange layout (within each rank's exchange area, STRIDE=8 words per rank):
 *   slot[r * STRIDE + 0] = rank r's sendBuffer GVA
 *   slot[r * STRIDE + 1] = rank r's sendOffset for myRank (cumSum[myRank])
 *   slot[r * STRIDE + 2] = rank r's sendCount for myRank (bytes to read)
 *   flag[r]              = sentinel (ready)
 *
 * After exchange, each rank's local exchange area contains all the info needed
 * to pull the correct slice from every other rank's sendBuffer.
 *
 * If sendCumSum (param0) is 0, falls back to equal-split (count/rankNum per peer).
 */
/* AlltoAllV exchange sub-slot indices within each rank's STRIDE-wide entry */
constexpr uint32_t A2AV_EXCH_SLOT_SENDBUF = 0;   /* sendBuffer GVA */
constexpr uint32_t A2AV_EXCH_SLOT_OFFSET = 1;    /* sendOffset for target rank */
constexpr uint32_t A2AV_EXCH_SLOT_COUNT = 2;     /* sendCount for target rank (bytes) */
constexpr uint32_t A2AV_EXCH_NUM_DATA_SLOTS = 3; /* data slots per rank in exchange area */
constexpr uint32_t A2AV_SCRATCH_FLAG_SLOT = 3;   /* flag sentinel slot in scratch block */
constexpr uint32_t A2AV_SCRATCH_SLOTS_PER_DST = A2AV_SCRATCH_FLAG_SLOT + 1;

class AlltoAllVOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        return AlltoAllVExchange(ctx);
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t rankNum = alg.ctx->rankNum;
        const uint32_t myRank = alg.ctx->rankId;

        if (rankNum == 0) {
            return BUILD_DONE;
        }

        for (uint32_t src = 0; src < rankNum; src++) {
            /* Invalidate exchange area cache lines: data written by remote SDMA */
            for (uint32_t slot = 0; slot < A2AV_EXCH_NUM_DATA_SLOTS; slot++) {
                uintptr_t exAddr = reinterpret_cast<uintptr_t>(op.exchangeGva) +
                                   src * ZBAL_AICPU_EXCHANGE_STRIDE * sizeof(uint64_t) + slot * sizeof(uint64_t);
                AicpuCacheInvalidate(exAddr);
            }
            uint64_t srcBufGva = ReadExchSlot(op.exchangeGva, src, A2AV_EXCH_SLOT_SENDBUF);
            uint64_t srcOffset = ReadExchSlot(op.exchangeGva, src, A2AV_EXCH_SLOT_OFFSET);
            uint64_t srcCount = ReadExchSlot(op.exchangeGva, src, A2AV_EXCH_SLOT_COUNT);
            if (srcCount == 0) {
                continue;
            }

            uint64_t myOff;
            uint64_t myLen;
            AicpuParallelSlice(srcCount, op.coreId, op.numCores, myOff, myLen);
            if (myLen == 0) {
                continue;
            }

            /* Output offset = sum of recvCounts for ranks < src */
            uint64_t outputOff = ComputeOutputOffset(op.exchangeGva, src, rankNum);

            uint32_t sid = src % op.numChPerCore;
            uint64_t rSrc = srcBufGva + srcOffset + myOff;
            uint64_t lDst = op.recvBuf + outputOff + myOff;
            if (AicpuDispatcher::CopyData(ringBufs, sid, rSrc, lDst, static_cast<uint32_t>(myLen), op.channels[sid],
                                          op.reduceOp)) {
                return BUILD_ERROR;
            }
        }

        return AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0 ? ERR_WAIT_TIMEOUT
                                                                                           : BUILD_DONE;
    }

private:
    /* Read a sub-slot from exchange area: exch[rank * STRIDE + subSlot] */
    static uint64_t ReadExchSlot(uint64_t exchangeGva, uint32_t rank, uint32_t subSlot)
    {
        const uint64_t *base = reinterpret_cast<const uint64_t *>(exchangeGva);
        return base[rank * ZBAL_AICPU_EXCHANGE_STRIDE + subSlot];
    }

    /* Compute output offset for data from srcRank by summing recvCounts for ranks 0..srcRank-1 */
    static uint64_t ComputeOutputOffset(uint64_t exchangeGva, uint32_t srcRank, uint32_t rankNum)
    {
        uint64_t offset = 0;
        for (uint32_t r = 0; r < srcRank; r++) {
            offset += ReadExchSlot(exchangeGva, r, A2AV_EXCH_SLOT_COUNT); /* srcCount for myRank */
        }
        return offset;
    }

    static int EnqueuePeerExchangeSqes(SqeLocalRingBuffer &eb, volatile uint64_t *ds, uint64_t baseOff,
                                       uint64_t flagDst, volatile stars_channel_info_t *channel)
    {
        /* slot 0: sendBuf GVA */
        if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&ds[A2AV_EXCH_SLOT_SENDBUF]), baseOff,
                                      sizeof(uint64_t), channel) != 0) {
            return BUILD_ERROR;
        }
        /* slot 1: sendOffset for dst in my buffer */
        if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&ds[A2AV_EXCH_SLOT_OFFSET]),
                                      baseOff + sizeof(uint64_t), sizeof(uint64_t), channel) != 0) {
            return BUILD_ERROR;
        }
        /* slot 2: sendCount for dst (bytes) */
        if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&ds[A2AV_EXCH_SLOT_COUNT]),
                                      baseOff + A2AV_EXCH_SLOT_COUNT * sizeof(uint64_t), sizeof(uint64_t),
                                      channel) != 0) {
            return BUILD_ERROR;
        }
        /* flag slot: exchange-complete sentinel (waitSymbol) */
        if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&ds[A2AV_SCRATCH_FLAG_SLOT]), flagDst,
                                      sizeof(uint64_t), channel) != 0) {
            return BUILD_ERROR;
        }
        return 0;
    }

    /* Fill the per-dstRank scratch block `ds` with (sendBuf, offset, count, flag).
     * cumSum values are in ELEMENTS — converted to BYTES. When cumSum is null, falls back to
     * equal-split (count / rankNum per peer). elemArr (reserved[2]) supplies the total for the
     * last rank. */
    static void FillPeerScratch(volatile uint64_t *ds, uint32_t dstRank, uint32_t rankNum, uint64_t sendBuf,
                                const uint64_t *cumSum, uint32_t elemSize, uint64_t count, uint64_t waitSymbol,
                                const volatile uint64_t *elemArr)
    {
        ds[A2AV_EXCH_SLOT_SENDBUF] = sendBuf;
        if (cumSum != nullptr) {
            uint64_t curOff = cumSum[dstRank * ZBAL_AICPU_EXCHANGE_STRIDE];
            uint64_t nextOff;
            if (dstRank + 1 < rankNum) {
                nextOff = cumSum[(dstRank + 1) * ZBAL_AICPU_EXCHANGE_STRIDE];
            } else {
                /* Last rank: total from elements array (reserved[2]) */
                nextOff = (elemArr != nullptr) ? elemArr[0] : curOff;
            }
            ds[A2AV_EXCH_SLOT_OFFSET] = curOff * elemSize;
            ds[A2AV_EXCH_SLOT_COUNT] = (nextOff - curOff) * elemSize;
        } else {
            ds[A2AV_EXCH_SLOT_OFFSET] = 0;
            ds[A2AV_EXCH_SLOT_COUNT] = count / rankNum;
        }
        ds[A2AV_SCRATCH_FLAG_SLOT] = waitSymbol;
    }

    /* Submit all enqueued SQEs and wait for local completion + all peers to publish.
     * Core 0 additionally polls the cross-device flag area. */
    static int SyncExchangeCompletion(SqeLocalRingBuffer &eb, const ExchangeContext &ctx, uint32_t myCore,
                                      uint32_t numCores, uint32_t rankNum, uint64_t flagAreaOff, uint64_t waitSymbol)
    {
        uint32_t fid = AicpuWorkspace::FlagIdx(myCore, ctx.numChPerCore, 0);
        int dbr = AicpuLaunchTaskMc(&eb, ctx.channels[0], ctx.workspace, myCore, 1, 0, fid);
        if (dbr == 0 && CompletionFlag(ctx.workspace, fid).Wait() < 0) {
            dbr = ERR_WAIT_TIMEOUT;
        }

        if (myCore == 0 && dbr == 0) {
            volatile uint64_t *flagBase =
                reinterpret_cast<volatile uint64_t *>(ctx.aicpuCtx->exchangeGva + flagAreaOff);
            if (BarrierAllRanks(flagBase, rankNum, waitSymbol, true) != 0) {
                dbr = ERR_WAIT_TIMEOUT;
            }
        }
        AicpuCoreBarrier(ctx.workspace, numCores);
        return (dbr < 0) ? ERR_WAIT_TIMEOUT : 0;
    }

    /* AlltoAllV-specific exchange: write sendBuf + offset + count + flag to all peers */
    static int AlltoAllVExchange(const ExchangeContext &ctx)
    {
        const uint32_t rankNum = ctx.aicpuCtx->rankNum;
        if (rankNum <= 1) {
            return 0;
        }

        const uint32_t numCores = ctx.numCores;
        const uint32_t myCore = ctx.coreId;
        const uint32_t myRank = ctx.aicpuCtx->rankId;

        /* Divide ranks across cores */
        const uint32_t ranksPerCore = (rankNum + numCores - 1) / numCores;
        const uint32_t startRank = myCore * ranksPerCore;
        uint32_t endRank = startRank + ranksPerCore;
        if (endRank > rankNum) {
            endRank = rankNum;
        }

        if (startRank >= rankNum) {
            AicpuCoreBarrier(ctx.workspace, numCores);
            return 0;
        }

        const uint32_t ringBufSize = ZBAL_AICPU_CORE_RINGBUF_SIZE;
        volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(ctx.workspace, myCore);

        /* Scratch area at end of ring buffer: per-dstRank block of A2AV_SCRATCH_SLOTS_PER_DST
         * uint64_t slots. SQEs hold source ADDRESSES (not inline data), so each dst needs its
         * own block — otherwise the loop overwrites scratch before SDMA executes. */
        constexpr uint32_t slotsPerDst = A2AV_SCRATCH_SLOTS_PER_DST;
        const uint32_t scratchSlots = slotsPerDst * ranksPerCore;
        volatile uint64_t *scratch =
            reinterpret_cast<volatile uint64_t *>(myBuf + ringBufSize - scratchSlots * sizeof(uint64_t));

        SqeLocalRingBuffer eb;
        eb.Init(const_cast<uint8_t *>(myBuf));

        /* cumSum (reserved[0]) is in ELEMENTS — convert to BYTES for SDMA. */
        const uint64_t *cumSum = reinterpret_cast<const uint64_t *>(ctx.desc->reserved[0]);
        const uint32_t elemSize = ZBALDataTypeSize(ctx.desc->dataType);

        const volatile AicpuInitContext *initCtx =
            reinterpret_cast<const volatile AicpuInitContext *>(ctx.workspace + ZBAL_AICPU_INIT_CTX_OFFSET);
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));
        const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;

        for (uint32_t dstRank = startRank; dstRank < endRank; dstRank++) {
            int64_t delta = static_cast<int64_t>(dstRank) - static_cast<int64_t>(myRank);
            int64_t devOff = delta * static_cast<int64_t>(initCtx->localDeviceMemSize);

            uint32_t dstIdx = dstRank - startRank;
            volatile uint64_t *ds = scratch + dstIdx * slotsPerDst;

            FillPeerScratch(ds, dstRank, rankNum, ctx.desc->sendBuffer, cumSum, elemSize, ctx.desc->count,
                            ctx.desc->waitSymbol, reinterpret_cast<const volatile uint64_t *>(ctx.desc->reserved[2]));
            AicpuCacheFlush(reinterpret_cast<uintptr_t>(&ds[A2AV_EXCH_SLOT_SENDBUF]));

            uint64_t baseOff =
                initCtx->exchangeGva + static_cast<uint64_t>(devOff) + static_cast<uint64_t>(myRank) * strideBytes;
            uint64_t flagDst = initCtx->exchangeGva + static_cast<uint64_t>(devOff) + flagAreaOff +
                               static_cast<uint64_t>(myRank) * strideBytes;
            if (EnqueuePeerExchangeSqes(eb, ds, baseOff, flagDst, ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
        }

        return SyncExchangeCompletion(eb, ctx, myCore, numCores, rankNum, flagAreaOff, ctx.desc->waitSymbol);
    }
};
#endif