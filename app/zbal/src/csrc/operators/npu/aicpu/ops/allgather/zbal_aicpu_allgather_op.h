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

#ifndef ZBAL_AICPU_ALLGATHER_OP_H
#define ZBAL_AICPU_ALLGATHER_OP_H

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "ops/exchange/zbal_aicpu_exchange_impl.h"
#include "ops/allgather/zbal_aicpu_allgather_fullmesh.h"
#include "ops/allgather/zbal_aicpu_allgather_doublering.h"
#include "ops/allgather/zbal_aicpu_allgather_mesh_doublering.h"

class AllGatherOp {
public:
    /* ================================================================
     * Layer 1: Decision — moved to host side (SelectCommAlg).
     * Device reads desc->commAlg directly; see zbal_comm_host_device_struct.h.
     * ================================================================ */

    /* ================================================================
     * Layer 2: Exchange — which exchange strategy for this algorithm?
     *
     * | commAlg                         |    Exchange        |
     * |---------------------------------|--------------------|
     * | ZBAL_COMM_ALG_FULL_MESH         | FullMeshExchange   |
     * | ZBAL_COMM_ALG_MESH_DOUBLE_RING  | FullMeshExchange   |
     * | ZBAL_COMM_ALG_DOUBLE_RING       | RingExchange       |
     * ================================================================ */
    static int AddrExchange(uint32_t commAlg, const ExchangeContext &ctx)
    {
        switch (commAlg) {
            case ZBAL_COMM_ALG_DOUBLE_RING:
                return RingExchange::Execute(ctx);
            case ZBAL_COMM_ALG_MESH_DOUBLE_RING:
            case ZBAL_COMM_ALG_FULL_MESH:
            default:
                return FullMeshExchange::Execute(ctx);
        }
    }

    /* ================================================================
     * Layer 3: Execution — owns loop/ring logic, calls SubmitAndWait.
     * Different algorithms may have different execution patterns.
     * ================================================================ */
    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        switch (op.commAlg) {
            case ZBAL_COMM_ALG_DOUBLE_RING:
                return AllGatherDoubleRing::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId,
                                                    numCores);
            case ZBAL_COMM_ALG_MESH_DOUBLE_RING:
                return AllGatherMeshDoubleRing::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId,
                                                        numCores);
            case ZBAL_COMM_ALG_FULL_MESH:
            default:
                return AllGatherFullMesh::Execute(alg, op, ringBufs, channels, numChPerCore, workspace, coreId,
                                                  numCores);
        }
    }
};

#endif /* ZBAL_AICPU_ALLGATHER_OP_H */
