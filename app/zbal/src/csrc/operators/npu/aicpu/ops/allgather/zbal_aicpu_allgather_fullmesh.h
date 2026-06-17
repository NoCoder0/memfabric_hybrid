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

#ifndef ZBAL_AICPU_ALLGATHER_FULLMESH_H
#define ZBAL_AICPU_ALLGATHER_FULLMESH_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/* Full-mesh allgather: per-core multi-channel, byte-sliced across all ranks. */
class AllGatherFullMesh {
public:
    /* Execute: single batch, always fits within SDMA capacity */
    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        if (BuildSqes(alg, op) < 0)
            return BUILD_ERROR;
        if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0)
            return ERR_WAIT_TIMEOUT;
        return 0;
    }

    static int BuildSqes(AicpuAlgorithmCtx &alg, const CommOpParams &op)
    {
        SqeLocalRingBuffer *ring = alg.ringBufs;
        uint32_t nch = op.numChPerCore;
        uint64_t myOff;
        uint64_t myLen;
        AicpuParallelSlice(op.dataSize, op.coreId, op.numCores, myOff, myLen);
        if (myLen == 0)
            return BUILD_DONE;

        for (uint32_t r = 0; r < alg.ctx->rankNum; r++) {
            uint32_t sid = r % nch;
            uint64_t remoteSrc = PeerOutputBuf(op.exchangeGva, r) + myOff;
            uint64_t localDst = op.recvBuf + static_cast<uint64_t>(r) * op.dataSize + myOff;
            if (AicpuDispatcher::CopyData(ring, sid, remoteSrc, localDst, static_cast<uint32_t>(myLen),
                                          op.channels[sid], op.reduceOp) != 0)
                return BUILD_ERROR;
        }
        return BUILD_DONE;
    }
};

#endif /* ZBAL_AICPU_ALLGATHER_FULLMESH_H */
