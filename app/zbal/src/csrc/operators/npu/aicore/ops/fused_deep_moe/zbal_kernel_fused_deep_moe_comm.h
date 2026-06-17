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

#ifndef ZBAL_KERNEL_FUSED_DEEP_MOE_COMM_H
#define ZBAL_KERNEL_FUSED_DEEP_MOE_COMM_H

#include "zbal_kernel_utils.h"

// Communication context for fused_deep_moe using zbal
// This replaces HcclOpResParam for zbal-based communication
struct ZbalCommContext {
    __gm__ CommGroupInfo *comm;
    __gm__ uint8_t *dataWindowBase;  // Points to myAddressExchangeGva
    __gm__ uint8_t *stateWindowBase; // Separate state area within meta
    uint64_t dataWindowSize;
    uint64_t stateWindowSize;
    uint32_t myRankId;
    uint32_t worldSize;
    uint64_t localDeviceMemSize;
};

__aicore__ inline __gm__ void *zbal_ptr(__gm__ void *ptr, int curPe, int dstPe, uint64_t localSize,
                                        __gm__ uint16_t *peerRanks)
{
    int worldDstPe = static_cast<int>(*((__gm__ uint16_t *)(peerRanks + dstPe)));
    int worldCurPe = static_cast<int>(*((__gm__ uint16_t *)(peerRanks + curPe)));
    uint64_t curPtr = reinterpret_cast<uint64_t>(ptr);
    uint64_t dstPtr = curPtr + (worldDstPe - worldCurPe) * localSize;
    return reinterpret_cast<__gm__ void *>(dstPtr);
}

// Get data window address for a specific rank (replaces GetWindAddrByRankId)
// This function uses zbal_ptr to calculate remote addresses via GVA
__aicore__ inline GM_ADDR GetZbalDataAddr(ZbalCommContext *ctx, int32_t rankId, uint64_t offset)
{
    if (ctx->myRankId == static_cast<uint32_t>(rankId)) {
        // Local access: direct offset from data window base
        return (GM_ADDR)(ctx->dataWindowBase + offset);
    }
    // Remote access: use zbal_ptr to calculate remote GVA address
    __gm__ void *remoteBase = zbal_ptr(ctx->dataWindowBase, ctx->myRankId, rankId, ctx->localDeviceMemSize,
                                       ctx->comm->peerGroupRank2WorldRank);
    return (GM_ADDR)((__gm__ uint8_t *)remoteBase + offset);
}

// Get state window address for a specific rank (replaces GetWindStateAddrByRankId)
// This function uses zbal_ptr to calculate remote addresses via GVA
__aicore__ inline GM_ADDR GetZbalStateAddr(ZbalCommContext *ctx, int32_t rankId, uint64_t offset)
{
    if (ctx->myRankId == static_cast<uint32_t>(rankId)) {
        // Local access: direct offset from state window base
        return (GM_ADDR)(ctx->stateWindowBase + offset);
    }
    // Remote access: use zbal_ptr to calculate remote GVA address
    __gm__ void *remoteBase = zbal_ptr(ctx->stateWindowBase, ctx->myRankId, rankId, ctx->localDeviceMemSize,
                                       ctx->comm->peerGroupRank2WorldRank);
    return (GM_ADDR)((__gm__ uint8_t *)remoteBase + offset);
}

#endif // ZBAL_KERNEL_FUSED_DEEP_MOE_COMM_H
