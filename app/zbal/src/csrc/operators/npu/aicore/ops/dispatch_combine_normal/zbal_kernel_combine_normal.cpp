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

#include <cstdio>

#include "dl_cann_api.h"
#include "zbal_functions.h"
#include "zbal_kernel_combine_normal.h"

using namespace AscendC;

extern "C" __global__ __aicore__ void combine_normal(uint64_t fftsAddr, GM_ADDR metaAddr, GM_ADDR srcTokens,
                                                     GM_ADDR putOffset, GM_ADDR topKWeight, GM_ADDR topkIndex,
                                                     GM_ADDR sendTokensIndex, GM_ADDR balanceMatrix, uint32_t rank,
                                                     uint32_t numExperts, uint32_t bs, uint32_t hidden, uint32_t topK,
                                                     bool enableBalance, GM_ADDR destTokens, uint32_t srcDataType,
                                                     uint32_t dstDataType)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_AIV_1_0);
    AscendC::SetSyncBaseAddr(fftsAddr);
    AscendC::TPipe pipe;
    if (srcDataType == ZBAL_DATA_TYPE_BFP16) {
        MoeCombineNormal::CombineNormal<bfloat16_t, bfloat16_t, int32_t> op;
        op.Init(metaAddr, srcTokens, putOffset, topKWeight, topkIndex, sendTokensIndex, balanceMatrix, rank, numExperts,
                bs, hidden, topK, enableBalance, destTokens, &pipe);
        op.Process();
        return;
    } else if (srcDataType == ZBAL_DATA_TYPE_FP16) {
        MoeCombineNormal::CombineNormal<float16_t, float16_t, int32_t> op;
        op.Init(metaAddr, srcTokens, putOffset, topKWeight, topkIndex, sendTokensIndex, balanceMatrix, rank, numExperts,
                bs, hidden, topK, enableBalance, destTokens, &pipe);
        op.Process();
        return;
    }
}

namespace {
// Resolve the combine kernel launch blockDim: env var ZBAL_COMBINE_BLOCK_DIM > auto by chip type
// (A5-like backs off to 36 cores, others use full cores) > current-thread AIV count.
// Fills blockDim and returns Z_OK on success; leaves blockDim at 0 and returns an error code on failure.
int32_t ResolveCombineBlockDim(uint32_t &blockDim)
{
    uint32_t envBlockDim = zbal::Func::GetEnv<uint32_t>("ZBAL_COMBINE_BLOCK_DIM", 0);
    if (envBlockDim > ZBAL_A5_MAX_AIV_CORES) {
        printf("ZBALOpCombineNormal failed as invalid ZBAL_COMBINE_BLOCK_DIM, blockDim:%u, expect 0(auto) or [1,%u]\n",
               envBlockDim, ZBAL_A5_MAX_AIV_CORES);
        return zbal::Z_INVALID_PARAM;
    }
    if (envBlockDim != 0U) {
        blockDim = envBlockDim;
        return zbal::Z_OK;
    }
    uint32_t aivNum = 0;
    auto ret = zbal::underapi::DlCannApi::AclrtGetAIVCountInCurrentThread(&aivNum);
    if (ret != 0 || aivNum == 0U) {
        printf("ZBALOpCombineNormal failed as blockDim get failed, ret:%d, aivNum:%u\n", ret, aivNum);
        return (ret != 0) ? ret : zbal::Z_ERROR;
    }
    // AIV count beyond A3 max cores indicates an A5-like chip: full-core launch triggers NoC/UB traffic
    // backpressure (see CLAUDE.md), so back off to the measured-optimal 36 cores
    blockDim = (aivNum > ZBAL_A3_MAX_AIV_CORES) ? ZBAL_COMBINE_DEFAULT_A5_BLOCK : aivNum;
    return zbal::Z_OK;
}
} // namespace

int32_t ZBALOpCombineNormal(const zbal_tensor_info_t *srcTokens, const zbal_tensor_info_t *putOffset,
                            const zbal_tensor_info_t *topKWeight, const zbal_tensor_info_t *topkIndex,
                            const zbal_tensor_info_t *sendTokensIndex, const zbal_tensor_info_t *balanceMatrix,
                            uint16_t expertNum, const zbal_tensor_info_t *destTokens, bool enableBalance,
                            aclrtStream stream, const CommGroupInfo &groupInfo, int64_t flags)
{
    // blockDim is resolved only on the first call and cached in the static below; later calls reuse it
    // directly, skipping both the env lookup and the AIV count query. 0 doubles as the unresolved
    // sentinel because a valid blockDim lies in [1, ZBAL_A5_MAX_AIV_CORES].
    static uint32_t blockDim = 0;
    if (blockDim == 0U) {
        auto ret = ResolveCombineBlockDim(blockDim);
        if (ret != zbal::Z_OK) {
            return ret;
        }
    }
    uint32_t rank = static_cast<uint32_t>(groupInfo.myGroupRank);
    uint32_t numExperts = static_cast<uint32_t>(expertNum);
    uint32_t hidden = static_cast<uint32_t>(srcTokens->shape[1]);
    uint32_t bs = static_cast<uint32_t>(topkIndex->shape[0]);
    uint32_t topK = static_cast<uint32_t>(topkIndex->shape[1]);
    uint64_t fftsAddr = groupInfo.fftsConfig;
    GM_ADDR metaAddr = reinterpret_cast<uint8_t *>(groupInfo.myMetaGva);

    GM_ADDR srcTokensAddr = reinterpret_cast<uint8_t *>(srcTokens->data);
    GM_ADDR putOffsetAddr = reinterpret_cast<uint8_t *>(putOffset->data);
    GM_ADDR topKWeightAddr = reinterpret_cast<uint8_t *>(topKWeight->data);
    GM_ADDR topkIndexAddr = reinterpret_cast<uint8_t *>(topkIndex->data);
    GM_ADDR sendTokensIndexAddr = reinterpret_cast<uint8_t *>(sendTokensIndex->data);
    GM_ADDR balanceMatrixAddr = reinterpret_cast<uint8_t *>(balanceMatrix->data);
    GM_ADDR destTokensAddr = reinterpret_cast<uint8_t *>(destTokens->data);

    zbal_datatype_t srcDataType = static_cast<zbal_datatype_t>(srcTokens->dataType);
    zbal_datatype_t dstDataType = static_cast<zbal_datatype_t>(destTokens->dataType);

    // launch kernel
    combine_normal<<<blockDim, nullptr, stream>>>(
        fftsAddr, metaAddr, srcTokensAddr, putOffsetAddr, topKWeightAddr, topkIndexAddr, sendTokensIndexAddr,
        balanceMatrixAddr, rank, numExperts, bs, hidden, topK, enableBalance, destTokensAddr, srcDataType, dstDataType);

    return 0;
}
