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
constexpr uint32_t A2AV_EXCH_SLOT_SENDBUF = 0; /* sendBuffer GVA */
constexpr uint32_t A2AV_EXCH_SLOT_OFFSET = 1;  /* sendOffset for target rank */
constexpr uint32_t A2AV_EXCH_SLOT_COUNT = 2;   /* sendCount for target rank (bytes) */

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
            /* Read per-peer info from local exchange area */
            uint64_t srcBufGva = ReadExchSlot(op.exchangeGva, src, A2AV_EXCH_SLOT_SENDBUF);
            uint64_t srcOffset = ReadExchSlot(op.exchangeGva, src, A2AV_EXCH_SLOT_OFFSET);
            uint64_t srcCount = ReadExchSlot(op.exchangeGva, src, A2AV_EXCH_SLOT_COUNT);
            if (srcCount == 0) {
                continue;
            }

            /* Multi-core parallel slice within this peer's data */
            uint64_t myOff;
            uint64_t myLen;
            AicpuParallelSlice(srcCount, op.coreId, op.numCores, myOff, myLen);
            if (myLen == 0) {
                continue;
            }

            /* Compute output offset: sum of all recvCounts for ranks < src */
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

        /* Per-core scratch and ring buffer */
        const uint32_t ringBufSize = ZBAL_AICPU_CORE_RINGBUF_SIZE;
        volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(ctx.workspace, myCore);

        /* scratch area: 4 × uint64_t at end of ring buffer
         * [0] = sendBuf GVA, [1] = offset for dst, [2] = count for dst, [3] = flag sentinel */
        constexpr uint32_t scratchSlots = 4;
        volatile uint64_t *scratch =
            reinterpret_cast<volatile uint64_t *>(myBuf + ringBufSize - scratchSlots * sizeof(uint64_t));

        SqeLocalRingBuffer eb;
        eb.Init(const_cast<uint8_t *>(myBuf));

        /* Read cumSum from local device memory (reserved[0] = sendCumSum GVA).
         * cumSum values are in ELEMENTS (same as AIV). Convert to BYTES for SDMA. */
        const uint64_t *cumSum = reinterpret_cast<const uint64_t *>(ctx.desc->reserved[0]);
        const uint32_t elemSize = ZBALDataTypeSize(ctx.desc->dataType);

        const volatile AicpuInitContext *initCtx =
            reinterpret_cast<const volatile AicpuInitContext *>(ctx.workspace + ZBAL_AICPU_INIT_CTX_OFFSET);
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));
        const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;

        for (uint32_t dstRank = startRank; dstRank < endRank; dstRank++) {
            int64_t delta = static_cast<int64_t>(dstRank) - static_cast<int64_t>(myRank);
            int64_t devOff = delta * static_cast<int64_t>(initCtx->localDeviceMemSize);

            /* Prepare scratch values for this dst.
             * cumSum is stored with ZBAL_AICPU_EXCHANGE_STRIDE spacing (matching exchange area).
             * Values are in ELEMENTS — convert to BYTES for SDMA. */
            scratch[0] = ctx.desc->sendBuffer;
            if (cumSum != nullptr) {
                uint64_t curOff = cumSum[dstRank * ZBAL_AICPU_EXCHANGE_STRIDE];
                uint64_t nextOff;
                if (dstRank + 1 < rankNum) {
                    nextOff = cumSum[(dstRank + 1) * ZBAL_AICPU_EXCHANGE_STRIDE];
                } else {
                    /* Last rank: total from elements array (reserved[2]) */
                    volatile uint64_t *elemArr = reinterpret_cast<volatile uint64_t *>(ctx.desc->reserved[2]);
                    nextOff = (elemArr != nullptr) ? elemArr[0] : curOff;
                }
                scratch[A2AV_EXCH_SLOT_OFFSET] = curOff * elemSize;
                scratch[A2AV_EXCH_SLOT_COUNT] = (nextOff - curOff) * elemSize;
            } else {
                scratch[A2AV_EXCH_SLOT_OFFSET] = 0;
                scratch[A2AV_EXCH_SLOT_COUNT] = ctx.desc->count / rankNum;
            }
            scratch[3] = 0x1U; /* non-zero flag sentinel */

            /* Write 3 data slots + flag to dst's exchange area at myRank position */
            uint64_t baseOff =
                initCtx->exchangeGva + static_cast<uint64_t>(devOff) + static_cast<uint64_t>(myRank) * strideBytes;

            /* slot 0: sendBuf GVA */
            if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&scratch[0]), baseOff, sizeof(uint64_t),
                                          ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
            /* slot 1: offset for dst in my buffer */
            if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&scratch[1]), baseOff + sizeof(uint64_t),
                                          sizeof(uint64_t), ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
            /* slot 2: count for dst */
            if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&scratch[A2AV_EXCH_SLOT_COUNT]),
                                          baseOff + A2AV_EXCH_SLOT_COUNT * sizeof(uint64_t), sizeof(uint64_t),
                                          ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
            /* flag */
            uint64_t flagDst = initCtx->exchangeGva + static_cast<uint64_t>(devOff) + flagAreaOff +
                               static_cast<uint64_t>(myRank) * strideBytes;
            if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&scratch[3]), flagDst, sizeof(uint64_t),
                                          ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
        }

        /* Submit + wait */
        uint32_t fid = AicpuWorkspace::FlagIdx(myCore, ctx.numChPerCore, 0);
        int dbr = AicpuLaunchTaskMc(&eb, ctx.channels[0], ctx.workspace, myCore, 1, 0, fid);
        if (dbr == 0 && CompletionFlag(ctx.workspace, fid).Wait() < 0) {
            dbr = ERR_WAIT_TIMEOUT;
        }

        /* Cross-device sync: poll flag area */
        if (myCore == 0 && dbr == 0) {
            volatile uint64_t *flagBase =
                reinterpret_cast<volatile uint64_t *>(ctx.aicpuCtx->exchangeGva + flagAreaOff);
            if (BarrierAllRanks(flagBase, rankNum, 0, false) != 0) {
                dbr = ERR_WAIT_TIMEOUT;
            }
        }
        AicpuCoreBarrier(ctx.workspace, numCores);
        return (dbr < 0) ? ERR_WAIT_TIMEOUT : 0;
    }
};
#endif
