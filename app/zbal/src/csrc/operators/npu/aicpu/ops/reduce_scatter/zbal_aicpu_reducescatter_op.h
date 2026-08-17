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

/*
 * ReduceScatter: SDMA hardware reduce.
 * SDMA opCode = (dataType << 4) | reduceOp  (matches AIV SetAtomicOpSDMA).
 */
class ReduceScatterOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        return FullMeshExchange::Execute(ctx);
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t rankNum = alg.ctx->rankNum;
        const uint32_t myRank = alg.ctx->rankId;
        if (rankNum == 0 || numChPerCore == 0) {
            return BUILD_ERROR;
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

        const uint64_t mySliceOff = (uint64_t)myRank * perRankBytes;
        constexpr uint32_t SID = 0;
        const uint64_t lDst = op.recvBuf + coreOff;

        /* SDMA reduce opCode = (dataType << 4) | reduceOp (matches AIV SetAtomicOpSDMA) */
        const uint8_t reduceOpCode = (uint8_t)((op.dataType << 4) | op.reduceOp);

        /* Step 1: Self-copy (memcpy) — initialize recvBuf before atomic reduces */
        uint64_t selfSrc = op.sendBuf + mySliceOff + coreOff;
        if (AicpuDispatcher::CopyData(ringBufs, SID, selfSrc, lDst, (uint32_t)coreLen, channels[SID]) != 0) {
            return BUILD_ERROR;
        }
        if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
            return ERR_WAIT_TIMEOUT;
        }

        /* Step 2: SDMA hardware reduce from peers — single-channel serial.
         * SDMA reduce (opcode!=0) is read-modify-write, NOT atomic across channels.
         * All peer reduces write to the SAME lDst address — concurrent RMW across
         * channels loses updates (A reads 0, B reads 0, A writes 5, B writes 3 →
         * result 3, lost A). Must serialize on SID=0: SDMA executes SQEs within a
         * channel in order, ensuring correct sequential accumulation. */
        for (uint32_t r = 0; r < rankNum; r++) {
            if (r == myRank) {
                continue;
            }
            /* Invalidate cache before reading peer's sendBuf GVA (SDMA-written).
             * FullMeshExchange writes slot[0] via SDMA, but its cross-device sync
             * only invalidates the FLAG area cache line, not the DATA area. */
            uintptr_t peerSlotAddr = reinterpret_cast<uintptr_t>(
                &reinterpret_cast<const uint64_t *>(op.exchangeGva)[r * ZBAL_AICPU_EXCHANGE_STRIDE]);
            AicpuCacheInvalidate(peerSlotAddr);
            uint64_t peerSrc = PeerOutputBuf(op.exchangeGva, r) + mySliceOff + coreOff;
            if (AicpuDispatcher::CopyData(ringBufs, SID, peerSrc, lDst, (uint32_t)coreLen, channels[SID],
                                          reduceOpCode) != 0) {
                return BUILD_ERROR;
            }
        }

        return AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0 ? ERR_WAIT_TIMEOUT
                                                                                           : BUILD_DONE;
    }
};
#endif
