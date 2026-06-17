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

        /* Step 2: SDMA hardware reduce from peers — multi-channel parallel
         * SDMA reduce is hardware-atomic per element, safe for concurrent writes. */
        for (uint32_t r = 0; r < rankNum; r++) {
            if (r == myRank) {
                continue;
            }
            uint64_t peerSrc = PeerOutputBuf(op.exchangeGva, r) + mySliceOff + coreOff;
            uint32_t sid = r % numChPerCore;
            if (AicpuDispatcher::CopyData(ringBufs, sid, peerSrc, lDst, (uint32_t)coreLen, channels[sid],
                                          reduceOpCode) != 0) {
                return BUILD_ERROR;
            }
        }

        return AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0 ? ERR_WAIT_TIMEOUT
                                                                                           : BUILD_DONE;
    }
};
#endif
