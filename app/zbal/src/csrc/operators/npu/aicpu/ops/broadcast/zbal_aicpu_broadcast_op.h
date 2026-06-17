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
#ifndef ZBAL_AICPU_BROADCAST_OP_H
#define ZBAL_AICPU_BROADCAST_OP_H
#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/*
 * Broadcast: root sends same data to all ranks.
 * All non-root ranks SDMA read from root's sendBuf.
 * Root also does self-copy if sendBuf != recvBuf.
 */
class BroadcastOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        return FullMeshExchange::Execute(ctx);
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t rootRank = op.root;
        const uint32_t myRank = alg.ctx->rankId;

        if (op.dataSize == 0) {
            return BUILD_DONE;
        }

        /* Root skip: host passes sendBuf == recvBuf for broadcast, no copy needed */
        if (myRank == rootRank) {
            return BUILD_DONE;
        }

        uint64_t myOff;
        uint64_t myLen;
        AicpuParallelSlice(op.dataSize, op.coreId, op.numCores, myOff, myLen);
        if (myLen == 0) {
            return BUILD_DONE;
        }

        uint64_t rootSrc = PeerOutputBuf(op.exchangeGva, rootRank) + myOff;
        if (AicpuDispatcher::CopyData(ringBufs, 0U, rootSrc, op.recvBuf + myOff, static_cast<uint32_t>(myLen),
                                      op.channels[0], op.reduceOp)) {
            return BUILD_ERROR;
        }

        return AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0 ? ERR_WAIT_TIMEOUT
                                                                                           : BUILD_DONE;
    }
};
#endif
