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

#ifndef ZBAL_AICPU_COMM_OP_H
#define ZBAL_AICPU_COMM_OP_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/engine/sdma/zbal_aicpu_sdma_sqe_context.h"
#include "executor/zbal_aicpu_flag.h"
#include "ops/exchange/zbal_aicpu_exchange_impl.h"
#include "ops/allgather/zbal_aicpu_allgather_op.h"
#include "ops/reduce_scatter/zbal_aicpu_reducescatter_op.h"
#include "ops/scatter/zbal_aicpu_scatter_op.h"
#include "ops/broadcast/zbal_aicpu_broadcast_op.h"
#include "ops/allreduce/zbal_aicpu_allreduce_op.h"
#include "ops/alltoallv/zbal_aicpu_alltoallv_op.h"
#include "ops/p2p/zbal_aicpu_p2p_op.h"

class CommOpBase {
public:
    static int Execute(AicpuInitContext &ctx, const volatile AicpuWorkDesc *desc, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        if (desc == nullptr || channels == nullptr) {
            return ERR_CHANNEL_INVALID;
        }

        CommOpParams op;
        op.sendBuf = reinterpret_cast<uint64_t>(desc->sendBuffer);
        op.recvBuf = reinterpret_cast<uint64_t>(desc->recvBuffer);
        op.buffer = desc->buffer;
        op.dataSize = desc->count;
        op.exchangeGva = ctx.exchangeGva;
        op.root = desc->root;
        op.dataType = desc->dataType;
        op.reduceOp = desc->reduceOp;
        op.coreId = coreId;
        op.numCores = numCores;
        op.numChPerCore = numChPerCore;
        op.channels = channels;
        op.waitSymbol = desc->waitSymbol;
        op.reserved[0] = desc->reserved[0];
        op.reserved[1] = desc->reserved[1];
        op.reserved[2] = desc->reserved[2];

        /* Phase 0: Algorithm selection */
        op.commAlg = SelectAlgorithm(desc->commType, desc->commAlg, numCores, ctx.rankNum, op.dataSize);

        /* Phase 1: AICPU address exchange */
        {
            ExchangeContext exCtx;
            exCtx.aicpuCtx = &ctx;
            exCtx.desc = desc;
            exCtx.workspace = const_cast<volatile uint8_t *>(workspace);
            exCtx.channels = channels;
            exCtx.numChPerCore = numChPerCore;
            exCtx.numCores = numCores;
            exCtx.coreId = coreId;
            int exRet = AddrExchange(desc->commType, op.commAlg, exCtx);
            if (exRet < 0) {
                return exRet;
            }
        }

        AicpuAlgorithmCtx alg;
        AicpuAlgorithmInit(&alg, &ctx, ringBufs);

        /* Phase 2: Algorithm execution — delegated to operator (owns loop / ring logic) */
        return DispatchExecute(desc->commType, alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
    }

    static uint32_t SelectAlgorithm(uint32_t commType, uint32_t hostCommAlg, uint32_t numCores, uint32_t rankNum,
                                    uint64_t dataSize);
    static int DispatchExecute(uint32_t commType, AicpuAlgorithmCtx &alg, const CommOpParams &op,
                               SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                               uint32_t numChPerCore, volatile uint8_t *workspace, uint32_t coreId, uint32_t numCores);
    static int AddrExchange(uint32_t commType, uint32_t commAlg, const ExchangeContext &ctx);
};

/* SelectAlgorithm — resolves algorithm for given commType.
 * Delegates to Op-level SelectAlgorithm for heuristics. */
inline uint32_t CommOpBase::SelectAlgorithm(uint32_t commType, uint32_t hostCommAlg, uint32_t numCores,
                                            uint32_t rankNum, uint64_t dataSize)
{
    switch (commType) {
        case ZBAL_CMD_ALLGATHER:
            return AllGatherOp::SelectAlgorithm(hostCommAlg, numCores, rankNum, dataSize);
        default:
            return ZBAL_COMM_ALG_FULL_MESH;
    }
}

/* DispatchExecute — delegate to algorithm's Execute (owns loop/ring logic). */
inline int CommOpBase::DispatchExecute(uint32_t commType, AicpuAlgorithmCtx &alg, const CommOpParams &op,
                                       SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                                       uint32_t numChPerCore, volatile uint8_t *workspace, uint32_t coreId,
                                       uint32_t numCores)
{
    switch (commType) {
        case ZBAL_CMD_ALLGATHER:
            return AllGatherOp::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        case ZBAL_CMD_REDUCE_SCATTER:
            return ReduceScatterOp::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        case ZBAL_CMD_SCATTER:
            return ScatterOp::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        case ZBAL_CMD_BROADCAST:
            return BroadcastOp::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        case ZBAL_CMD_ALLREDUCE:
            return AllReduceOp::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        case ZBAL_CMD_ALLTOALLV:
            return AlltoAllVOp::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        case ZBAL_CMD_SEND:
            return SendOp::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        case ZBAL_CMD_RECV:
            return RecvOp::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        default:
            return BUILD_ERROR;
    }
}

/* AddrExchange — (commType, commAlg) → Exchange strategy.
 * Extension: add new Op case here, then Op::AddrExchange handles algorithm-level choice. */
inline int CommOpBase::AddrExchange(uint32_t commType, uint32_t commAlg, const ExchangeContext &ctx)
{
    switch (commType) {
        case ZBAL_CMD_ALLGATHER:
            return AllGatherOp::AddrExchange(commAlg, ctx);
        case ZBAL_CMD_REDUCE_SCATTER:
            return ReduceScatterOp::AddrExchange(commAlg, ctx);
        case ZBAL_CMD_SCATTER:
            return ScatterOp::AddrExchange(commAlg, ctx);
        case ZBAL_CMD_BROADCAST:
            return BroadcastOp::AddrExchange(commAlg, ctx);
        case ZBAL_CMD_ALLREDUCE:
            return AllReduceOp::AddrExchange(commAlg, ctx);
        case ZBAL_CMD_ALLTOALLV:
            return AlltoAllVOp::AddrExchange(commAlg, ctx);
        case ZBAL_CMD_SEND:
            return SendOp::AddrExchange(commAlg, ctx);
        case ZBAL_CMD_RECV:
            return RecvOp::AddrExchange(commAlg, ctx);
        case ZBAL_CMD_INIT:
        case ZBAL_CMD_FINALIZE:
        default:
            return NullExchange::Execute(ctx);
    }
}

#endif /* ZBAL_AICPU_COMM_OP_H */
