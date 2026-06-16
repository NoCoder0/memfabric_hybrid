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
#include <cstring>
#include <string>
#include <acl/acl_rt.h>
#include "dl_cann_api.h"
#include "zbal_def.h"
#include "zbal_env_helper.h"
#include "zbal_kernel_fused_deep_moe_tiling.h"
#include "zbal_comm_host_device_struct.h"

// Forward declaration of the launch function compiled by tikcpp (in device/zbal_kernel_fused_deep_moe.cpp)
int32_t ZBALOpFusedDeepMoeLaunch(uint8_t *xAddr, uint8_t *expertIdsAddr, uint8_t *gmm1WAddr, uint8_t *gmm1ScaleAddr,
                                 uint8_t *gmm2WAddr, uint8_t *gmm2ScaleAddr, uint8_t *expertScalesAddr,
                                 uint8_t *smoothScalesAddr, uint8_t *shareGmm1WAddr, uint8_t *shareGmm1ScaleAddr,
                                 uint8_t *shareGmm2WAddr, uint8_t *shareGmm2ScaleAddr, uint8_t *shareSmoothScalesAddr,
                                 uint8_t *xActiveMaskAddr, uint8_t *outputAddr, uint8_t *shareOutputAddr,
                                 uint8_t *expertTokenNumsAddr, uint8_t *workspaceAddr, uint8_t *tilingDevAddr,
                                 uint8_t *epMetaAddr, uint8_t *tpMetaAddr, uint32_t execFlag, uint32_t srcDataType,
                                 uint32_t aicNum, aclrtStream stream);

using namespace ZbalCam;
using namespace zbal;

namespace {

constexpr uint32_t SYSTEM_NEED_WORKSPACE = 16 * 1024 * 1024;
constexpr uint32_t GM_ALIGN_SIZE = 512;
constexpr uint32_t TOKEN_DTYPE_BYTE_SIZE = 2;
constexpr uint32_t L1_TILE_BYTE_SIZE = 32 * 1024;
constexpr uint32_t CUBE_WORKSPACE_STAGE = 4;
constexpr uint32_t RESERVED_WORKSPACE_SIZE = 256 * 1024;
constexpr float MB_SIZE = 1024.0 * 1024.0;

// Debug logging infrastructure
#define DEBUG_PRINT(...)                        \
    do {                                        \
        if (EnvHelper::OP_DEBUG_FUSED_MOE) {    \
            printf("[FUSED_MOE] " __VA_ARGS__); \
        }                                       \
    } while (0)

static size_t CeilUpTiling(size_t x, size_t y)
{
    return (x + y - 1) / y * y;
}

static size_t CalcFusedDeepMoeWorkspaceSize(const FusedDeepMoeTilingData &tilingData)
{
    uint32_t batchSize = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.bs;
    uint32_t globalBs = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.globalBs;
    uint32_t maxBatchSize = globalBs / tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.epRankSize;
    uint32_t topK = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.k;
    uint32_t moeExpertNumPerRank = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.moeExpertNumPerRank;
    uint32_t epRankSize = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.epRankSize;
    uint32_t h = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.h;
    uint32_t aicNum = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.aicNum;
    uint64_t gmm1HLen = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.gmm1HLen;
    uint64_t gmm2HLen = gmm1HLen / 2;
    uint64_t shareGmm1HLen = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.shareGmm1HLen;
    uint64_t shareGmm2HLen = shareGmm1HLen / 2;
    uint32_t shareExpertTokenNum = (shareGmm1HLen > 0) ? batchSize : 0;

    uint32_t minTopkPerRank = (topK < moeExpertNumPerRank) ? topK : moeExpertNumPerRank;
    size_t maxTokenNum = static_cast<size_t>(maxBatchSize * epRankSize * minTopkPerRank);
    size_t maxHandleTokenNum = maxTokenNum + shareExpertTokenNum;

    size_t x1TokenSize = CeilUpTiling(maxHandleTokenNum * h * sizeof(int8_t), GM_ALIGN_SIZE);
    size_t x2TokenSize =
        CeilUpTiling((maxTokenNum * gmm2HLen + shareExpertTokenNum * shareGmm2HLen) * sizeof(int8_t), GM_ALIGN_SIZE);
    size_t maxTokenSize = (x1TokenSize > x2TokenSize) ? x1TokenSize : x2TokenSize;
    size_t tokenScaleSize = CeilUpTiling(maxHandleTokenNum * sizeof(float), GM_ALIGN_SIZE);
    size_t CVSwapBufferSize = CeilUpTiling(
        static_cast<size_t>(aicNum * L1_TILE_BYTE_SIZE * CUBE_WORKSPACE_STAGE * sizeof(int32_t)), GM_ALIGN_SIZE);
    size_t swigluOutSize = (maxTokenNum * gmm1HLen + shareExpertTokenNum * shareGmm1HLen) * sizeof(float);
    size_t gmm2DepOutSize = maxTokenNum * h * TOKEN_DTYPE_BYTE_SIZE;
    size_t maxSwigluGmm2Size =
        CeilUpTiling((swigluOutSize > gmm2DepOutSize) ? swigluOutSize : gmm2DepOutSize, GM_ALIGN_SIZE);
    size_t groupListSize = CeilUpTiling(moeExpertNumPerRank * sizeof(int64_t), GM_ALIGN_SIZE);
    size_t expandIdxSize = CeilUpTiling(static_cast<size_t>(batchSize * topK * sizeof(int32_t)), GM_ALIGN_SIZE);
    size_t epSendCountSize = CeilUpTiling(static_cast<size_t>(tilingData.disGmmDeqSwigluQuantGmmDeqComInfo.epRankSize *
                                                              moeExpertNumPerRank * sizeof(int32_t)),
                                          GM_ALIGN_SIZE);
    size_t resveredSize = CeilUpTiling(RESERVED_WORKSPACE_SIZE, GM_ALIGN_SIZE);
    // Pull-mode dispatch: each rank writes quantized tokens to local workspace.
    // Other ranks read via cross-rank GVA (zbal_ptr).  Max tokens = bs_ * topK_.
    // Each token = CEIL_UP(h * sizeof(int8_t) + sizeof(float)) bytes (32B-aligned).
    size_t quantTokenCount = static_cast<size_t>(batchSize * topK);
    size_t quantTokenStride = CeilUpTiling(h * sizeof(int8_t) + sizeof(float), GM_ALIGN_SIZE);
    size_t quantWorkspaceSize = CeilUpTiling(quantTokenCount * quantTokenStride, GM_ALIGN_SIZE);
    // Combine workspace: replaces zbal data window. Layout:
    //   epRankSize × moeExpertNumPerRank × maxBatchSize × h × sizeof(ExpandXType)
    size_t combineWsSize =
        CeilUpTiling(static_cast<size_t>(moeExpertNumPerRank) * epRankSize * maxBatchSize * h * TOKEN_DTYPE_BYTE_SIZE,
                     GM_ALIGN_SIZE);

    size_t usrSize = maxTokenSize + tokenScaleSize + CVSwapBufferSize + maxSwigluGmm2Size + groupListSize +
                     expandIdxSize + epSendCountSize + resveredSize + quantWorkspaceSize + combineWsSize;

    DEBUG_PRINT("=== Workspace Calculation ===\n");
    DEBUG_PRINT("maxTokenNum=%zu\n", maxTokenNum);
    DEBUG_PRINT("maxHandleTokenNum=%zu\n", maxHandleTokenNum);
    DEBUG_PRINT("x1TokenSize=%zu bytes (%.2f MB)\n", x1TokenSize, x1TokenSize / MB_SIZE);
    DEBUG_PRINT("x2TokenSize=%zu bytes (%.2f MB)\n", x2TokenSize, x2TokenSize / MB_SIZE);
    DEBUG_PRINT("maxTokenSize=%zu bytes (%.2f MB)\n", maxTokenSize, maxTokenSize / MB_SIZE);
    DEBUG_PRINT("tokenScaleSize=%zu bytes (%.2f MB)\n", tokenScaleSize, tokenScaleSize / MB_SIZE);
    DEBUG_PRINT("CVSwapBufferSize=%zu bytes (%.2f MB)\n", CVSwapBufferSize, CVSwapBufferSize / MB_SIZE);
    DEBUG_PRINT("swigluOutSize=%zu bytes (%.2f MB)\n", swigluOutSize, swigluOutSize / MB_SIZE);
    DEBUG_PRINT("gmm2DepOutSize=%zu bytes (%.2f MB)\n", gmm2DepOutSize, gmm2DepOutSize / MB_SIZE);
    DEBUG_PRINT("maxSwigluGmm2Size=%zu bytes (%.2f MB)\n", maxSwigluGmm2Size, maxSwigluGmm2Size / MB_SIZE);
    DEBUG_PRINT("groupListSize=%zu bytes\n", groupListSize);
    DEBUG_PRINT("expandIdxSize=%zu bytes\n", expandIdxSize);
    DEBUG_PRINT("epSendCountSize=%zu bytes\n", epSendCountSize);
    DEBUG_PRINT("resveredSize=%zu bytes\n", resveredSize);
    DEBUG_PRINT("usrSize=%zu bytes (%.2f MB)\n", usrSize, usrSize / MB_SIZE);
    DEBUG_PRINT("SYSTEM_NEED_WORKSPACE=%d bytes\n", SYSTEM_NEED_WORKSPACE);
    DEBUG_PRINT("Total workspace=%zu bytes (%.2f MB)\n", SYSTEM_NEED_WORKSPACE + usrSize,
                (SYSTEM_NEED_WORKSPACE + usrSize) / MB_SIZE);

    return SYSTEM_NEED_WORKSPACE + usrSize;
}

} // namespace

int32_t ZBALOpFusedDeepMoe(const zbal_tensor_info_t *x, const zbal_tensor_info_t *expertIds,
                           const zbal_tensor_info_t *gmm1Weight, const zbal_tensor_info_t *gmm1Scale,
                           const zbal_tensor_info_t *gmm2Weight, const zbal_tensor_info_t *gmm2Scale,
                           const zbal_tensor_info_t *expertScales, const zbal_tensor_info_t *expertSmoothScales,
                           const zbal_tensor_info_t *shareGmm1Weight, const zbal_tensor_info_t *shareGmm1Scale,
                           const zbal_tensor_info_t *shareGmm2Weight, const zbal_tensor_info_t *shareGmm2Scale,
                           const zbal_tensor_info_t *shareSmoothScales, const zbal_tensor_info_t *xActiveMask,
                           const zbal_tensor_info_t *output, const zbal_tensor_info_t *shareOutput,
                           const zbal_tensor_info_t *expertTokenNums, const zbal_tensor_info_t *workspace,
                           int64_t moeExpertNum, int64_t quantMode, int64_t globalBs, int64_t gmm1HLen,
                           int64_t shareGmm1HLen, bool isTensorList, void *tilingDevBuf, const std::string &groupName,
                           aclrtStream stream, const CommGroupInfo &epGroupInfo, const CommGroupInfo &tpGroupInfo,
                           int64_t flags, bool needTilingCopy)
{
    uint32_t aicNum = 0;
    auto ret = underapi::DlCannApi::AclrtGetAICCountInCurrentThread(&aicNum);
    if (ret != 0 || aicNum == 0) {
        ZBAL_LOG_ERROR("ZBALOpFusedDeepMoe: failed to get AIC core count (ret=" << ret << ", aicNum=" << aicNum
                                                                                << "), defaulting to 40\n");
        aicNum = 40;
    }
    uint32_t aivNum = 0;
    underapi::DlCannApi::AclrtGetAIVCountInCurrentThread(&aivNum);
    if (aivNum == 0) {
        aivNum = aicNum * 2;
    }

    FusedDeepMoeTilingData tilingData;
    (void)memset(&tilingData, 0, sizeof(tilingData));

    uint32_t epRankSize = static_cast<uint32_t>(epGroupInfo.groupSize);
    uint32_t epRankId = static_cast<uint32_t>(epGroupInfo.myGroupRank);
    uint32_t moeExpertNumU32 = static_cast<uint32_t>(moeExpertNum);
    uint32_t moeExpertNumPerRank = moeExpertNumU32 / epRankSize;

    uint32_t bs = static_cast<uint32_t>(x->shape[0]);
    uint32_t h = static_cast<uint32_t>(x->shape[1]);
    uint32_t topK = static_cast<uint32_t>(expertIds->shape[1]);
    uint32_t gBs = (globalBs > 0) ? static_cast<uint32_t>(globalBs) : epRankSize * bs;

    FusedDeepMoeInfo &info = tilingData.disGmmDeqSwigluQuantGmmDeqComInfo;
    info.epRankSize = epRankSize;
    info.epRankId = epRankId;
    info.moeExpertNum = moeExpertNumU32;
    info.moeExpertNumPerRank = moeExpertNumPerRank;
    info.quantMode = static_cast<uint32_t>(quantMode);
    info.globalBs = gBs;
    info.bs = bs;
    info.k = topK;
    info.h = h;
    info.aicNum = aicNum;
    info.aivNum = aivNum;
    info.gmm1HLen = static_cast<uint64_t>(gmm1HLen);
    info.shareGmm1HLen = static_cast<uint64_t>(shareGmm1HLen);
    info.isTensorList = isTensorList;

    // useless
    info.totalUbSize = 512 * 1024;

    // useless
    info.totalWinSize = 0;

    /* mc2InitTiling and mc2CcTiling are only used by the CANN opbuild framework (Mc2CcTilingConfig).
     * zbal bypasses opbuild entirely; the device-side combine/dispatch code does not read these
     * fields, so we leave them zeroed (already done by the memset above). */

    DEBUG_PRINT("=== Tiling Data ===\n");
    DEBUG_PRINT("info.epRankSize=%u\n", info.epRankSize);
    DEBUG_PRINT("info.epRankId=%u\n", info.epRankId);
    DEBUG_PRINT("info.moeExpertNum=%u\n", info.moeExpertNum);
    DEBUG_PRINT("info.moeExpertNumPerRank=%u\n", info.moeExpertNumPerRank);
    DEBUG_PRINT("info.quantMode=%u\n", info.quantMode);
    DEBUG_PRINT("info.globalBs=%u\n", info.globalBs);
    DEBUG_PRINT("info.bs=%u\n", info.bs);
    DEBUG_PRINT("info.k=%u\n", info.k);
    DEBUG_PRINT("info.h=%u\n", info.h);
    DEBUG_PRINT("info.aicNum=%u\n", info.aicNum);
    DEBUG_PRINT("info.aivNum=%u\n", info.aivNum);
    DEBUG_PRINT("info.gmm1HLen=%lu\n", info.gmm1HLen);
    DEBUG_PRINT("info.shareGmm1HLen=%lu\n", info.shareGmm1HLen);
    DEBUG_PRINT("info.isTensorList=%u\n", info.isTensorList);
    DEBUG_PRINT("info.totalUbSize=%lu\n", info.totalUbSize);
    DEBUG_PRINT("info.totalWinSize=%lu\n", info.totalWinSize);

    // Check if workspace size is sufficient
    size_t requiredWorkspaceSize = CalcFusedDeepMoeWorkspaceSize(tilingData);
    size_t actualWorkspaceSize = static_cast<size_t>(workspace->shape[0]);

    if (actualWorkspaceSize < requiredWorkspaceSize) {
        ZBAL_LOG_ERROR("[ZBAL] ERROR: Workspace size is too SMALL!\n");
        ZBAL_LOG_ERROR("  Required: " << requiredWorkspaceSize << " bytes (" << requiredWorkspaceSize / MB_SIZE
                                      << " MB)\n");
        ZBAL_LOG_ERROR("  Actual: " << actualWorkspaceSize << " bytes (" << actualWorkspaceSize / MB_SIZE << " MB)\n");
        ZBAL_LOG_ERROR("  Shortfall: " << requiredWorkspaceSize - actualWorkspaceSize << " bytes ("
                                       << (requiredWorkspaceSize - actualWorkspaceSize) / MB_SIZE << " MB)\n");
        return -1;
    }
    DEBUG_PRINT("Workspace size check passed\n");

    if (needTilingCopy) {
        ret = underapi::DlCannApi::AclrtMemcpyAsync(tilingDevBuf, sizeof(FusedDeepMoeTilingData), &tilingData,
                                                    sizeof(FusedDeepMoeTilingData), ACL_MEMCPY_HOST_TO_DEVICE, stream);
        if (ret != 0) {
            ZBAL_LOG_ERROR("ZBALOpFusedDeepMoe: H2D tiling memcpy failed, ret=" << ret << "\n");
            return ret;
        }
    }

    uint32_t execFlag = 0;
    if (moeExpertNumPerRank != 1) {
        execFlag |= EXEC_FLAG_DEEP_FUSE;
    }
    if (isTensorList) {
        execFlag |= EXEC_FLAG_TENSOR_LIST;
    }
    if (xActiveMask != nullptr && xActiveMask->data != 0) {
        execFlag |= EXEC_FLAG_X_ACTIVE_MASK;
    }
    if (shareGmm1Weight != nullptr && shareGmm1Weight->data != 0) {
        execFlag |= EXEC_FLAG_SHARED_EXPERT;
    }
    if (expertSmoothScales != nullptr && expertSmoothScales->data != 0) {
        execFlag |= EXEC_FLAG_SMOOTH_QUANT;
    }

    uint8_t *xAddr = reinterpret_cast<uint8_t *>(x->data);
    uint8_t *expertIdsAddr = reinterpret_cast<uint8_t *>(expertIds->data);
    uint8_t *gmm1WAddr = reinterpret_cast<uint8_t *>(gmm1Weight->data);
    uint8_t *gmm1ScaleAddr = reinterpret_cast<uint8_t *>(gmm1Scale->data);
    uint8_t *gmm2WAddr = reinterpret_cast<uint8_t *>(gmm2Weight->data);
    uint8_t *gmm2ScaleAddr = reinterpret_cast<uint8_t *>(gmm2Scale->data);
    uint8_t *expertScalesAddr = reinterpret_cast<uint8_t *>(expertScales->data);
    uint8_t *smoothScalesAddr = (expertSmoothScales != nullptr && expertSmoothScales->data != 0)
                                    ? reinterpret_cast<uint8_t *>(expertSmoothScales->data)
                                    : nullptr;
    uint8_t *xActiveMaskAddr =
        (xActiveMask != nullptr && xActiveMask->data != 0) ? reinterpret_cast<uint8_t *>(xActiveMask->data) : nullptr;
    uint8_t *outputAddr = reinterpret_cast<uint8_t *>(output->data);
    uint8_t *expertTokenNumsAddr = reinterpret_cast<uint8_t *>(expertTokenNums->data);
    uint8_t *workspaceAddr = reinterpret_cast<uint8_t *>(workspace->data);
    uint8_t *tilingDevAddr = reinterpret_cast<uint8_t *>(tilingDevBuf);
    uint8_t *shareGmm1WAddr = (shareGmm1Weight != nullptr && shareGmm1Weight->data != 0)
                                  ? reinterpret_cast<uint8_t *>(shareGmm1Weight->data)
                                  : nullptr;
    uint8_t *shareGmm1ScaleAddr = (shareGmm1Scale != nullptr && shareGmm1Scale->data != 0)
                                      ? reinterpret_cast<uint8_t *>(shareGmm1Scale->data)
                                      : nullptr;
    uint8_t *shareGmm2WAddr = (shareGmm2Weight != nullptr && shareGmm2Weight->data != 0)
                                  ? reinterpret_cast<uint8_t *>(shareGmm2Weight->data)
                                  : nullptr;
    uint8_t *shareGmm2ScaleAddr = (shareGmm2Scale != nullptr && shareGmm2Scale->data != 0)
                                      ? reinterpret_cast<uint8_t *>(shareGmm2Scale->data)
                                      : nullptr;
    uint8_t *shareSmoothScalesAddr = (shareSmoothScales != nullptr && shareSmoothScales->data != 0)
                                         ? reinterpret_cast<uint8_t *>(shareSmoothScales->data)
                                         : nullptr;
    uint8_t *shareOutputAddr =
        (shareOutput != nullptr && shareOutput->data != 0) ? reinterpret_cast<uint8_t *>(shareOutput->data) : nullptr;

    uint32_t srcDataType = static_cast<uint32_t>(x->dataType);

    // Get meta addresses for both EP and TP groups
    uint8_t *epMetaAddr = reinterpret_cast<uint8_t *>(epGroupInfo.myMetaGva);
    uint8_t *tpMetaAddr = reinterpret_cast<uint8_t *>(tpGroupInfo.myMetaGva);

    // Diagnostic: check zbal state window size for soft sync
    {
        // DEEP_FUSE no longer uses the data window — dispatch and combine
        // exchange data through separate workspace allocations.  The entire
        // exchange space is available for the state window.
        uint64_t dataWindowSize;
        uint64_t stateWindowSize;
        if (execFlag & EXEC_FLAG_DEEP_FUSE) {
            dataWindowSize = 0;
            stateWindowSize = epGroupInfo.sizeForExchangeAddress;
        } else {
            dataWindowSize = (epGroupInfo.sizeForExchangeAddress * 2 / 3) & ~(GM_ALIGN_SIZE - 1ULL);
            stateWindowSize = epGroupInfo.sizeForExchangeAddress - dataWindowSize;
        }
        constexpr uint64_t SOFT_SYNC_OFFSET = 964 * 1024;
        constexpr uint64_t SOFT_SYNC_SPACE_SIZE = 128;
        constexpr uint64_t CORE_NUM_PER_GROUP = 3;
        constexpr uint64_t WORKSPACE_STAGES = 4;
        uint64_t maxSoftSyncOffset =
            SOFT_SYNC_OFFSET +
            (static_cast<uint64_t>(aicNum + aivNum) / CORE_NUM_PER_GROUP + 1) * WORKSPACE_STAGES * SOFT_SYNC_SPACE_SIZE;

        DEBUG_PRINT("=== ZBAL State Window Diagnostic ===\n");
        DEBUG_PRINT("dataWindowSize=%lu bytes (%.2f MB)\n", dataWindowSize, dataWindowSize / MB_SIZE);
        DEBUG_PRINT("stateWindowSize=%lu bytes (%.2f MB)\n", stateWindowSize, stateWindowSize / MB_SIZE);
        DEBUG_PRINT("maxSoftSyncOffset=%lu bytes (%.2f MB)\n", maxSoftSyncOffset, maxSoftSyncOffset / MB_SIZE);
        DEBUG_PRINT("soft sync requires stateWindowSize >= maxSoftSyncOffset: %s\n",
                    stateWindowSize >= maxSoftSyncOffset ? "PASS" : "FAIL — WILL CAUSE EZ9999!");
        DEBUG_PRINT("dataWindowBase = myAddressExchangeGva = %p\n", (void *)epGroupInfo.myAddressExchangeGva);
        DEBUG_PRINT("soft sync address range: [%p, %p)\n",
                    (void *)(epGroupInfo.myAddressExchangeGva + dataWindowSize + SOFT_SYNC_OFFSET),
                    (void *)(epGroupInfo.myAddressExchangeGva + dataWindowSize + maxSoftSyncOffset));

        // Hard fatal — state window must be large enough for soft sync flags
        if (stateWindowSize < maxSoftSyncOffset) {
            ZBAL_LOG_ERROR("[ZBAL] FATAL: zbal state window (winState) too small for soft sync!\n");
            ZBAL_LOG_ERROR("  stateWindowSize=" << stateWindowSize << " bytes (" << stateWindowSize / MB_SIZE
                                                << " MB)\n");
            ZBAL_LOG_ERROR("  required=" << maxSoftSyncOffset << " bytes (" << maxSoftSyncOffset / MB_SIZE << " MB)\n");
            ZBAL_LOG_ERROR("  Shortfall=" << maxSoftSyncOffset - stateWindowSize << " bytes ("
                                          << (maxSoftSyncOffset - stateWindowSize) / MB_SIZE << " MB)\n");
            ZBAL_LOG_ERROR("  Increase sizeForExchangeAddress (currently "
                           << epGroupInfo.sizeForExchangeAddress / MB_SIZE << " MB) to at least "
                           << (maxSoftSyncOffset * 3.0) / MB_SIZE << " MB.\n");
            return -1;
        }

        // Validate zbal data window (winIn) is large enough for dispatch double-buffering.
        // Ported from UMDK CheckHcclBufferSize() in fused_deep_moe_tiling.cpp:594-610.
        //
        // The dispatch double-buffers writes to winIn (data window) using dataState 0/1.
        // Required space = moeExpertNumPerRank * globalBatchSize * h * sizeof(bf16) * 2.
        // If this exceeds dataWindowSize, dataState=1 writes overflow into the state
        // window area and corrupt soft-sync / group-token-count flags.
        //
        // Unlike UMDK which compares against Mc2TilingUtils::GetMaxWindowSize() (the full
        // HCCL buffer), ZBAL's data window is only 2/3 of the zbal exchange space
        // (the other 1/3 is winState for sync flags).
        {
            constexpr uint64_t DOUBLE_BUFFER = 2;
            uint64_t bufferDemand =
                static_cast<uint64_t>(moeExpertNumPerRank) * gBs * h * TOKEN_DTYPE_BYTE_SIZE * DOUBLE_BUFFER;

            DEBUG_PRINT("=== ZBAL Data Window (winIn) Diagnostic ===\n");
            DEBUG_PRINT("globalBatchSize (gBs)=%u\n", gBs);
            DEBUG_PRINT("moeExpertNumPerRank=%u, h=%u\n", moeExpertNumPerRank, h);
            DEBUG_PRINT("bufferDemand (nle*gBs*h*sizeof(bf16)*2)=%lu bytes (%.2f MB)\n", bufferDemand,
                        bufferDemand / MB_SIZE);
            DEBUG_PRINT("dataWindowSize (winIn)=%lu bytes (%.2f MB)\n", dataWindowSize, dataWindowSize / MB_SIZE);
            DEBUG_PRINT("dataWindowSize >= bufferDemand: %s\n", dataWindowSize >= bufferDemand ? "PASS" : "FAIL");

            // DEEP_FUSE does not use the data window — skip this check.
            if (!(execFlag & EXEC_FLAG_DEEP_FUSE) && dataWindowSize < bufferDemand) {
                ZBAL_LOG_ERROR("[ZBAL] FATAL: zbal data window (winIn) too small for dispatch double-buffering!\n");
                ZBAL_LOG_ERROR("  dataWindowSize (winIn) = sizeForExchangeAddress * 2/3\n");
                ZBAL_LOG_ERROR("    = " << epGroupInfo.sizeForExchangeAddress << " * 2/3 = " << dataWindowSize
                                        << " bytes (" << dataWindowSize / MB_SIZE << " MB)\n");
                ZBAL_LOG_ERROR(
                    "  bufferDemand = moeExpertNumPerRank * globalBatchSize * h * sizeof(bf16) * DOUBLE_BUFFER\n");
                ZBAL_LOG_ERROR("    = " << moeExpertNumPerRank << " * " << gBs << " * " << h << " * "
                                        << TOKEN_DTYPE_BYTE_SIZE << " * " << DOUBLE_BUFFER << " = " << bufferDemand
                                        << " bytes (" << bufferDemand / MB_SIZE << " MB)\n");
                ZBAL_LOG_ERROR("  Shortfall = " << bufferDemand - dataWindowSize << " bytes ("
                                                << (bufferDemand - dataWindowSize) / MB_SIZE << " MB)\n");
                ZBAL_LOG_ERROR("  Increase sizeForExchangeAddress (currently "
                               << epGroupInfo.sizeForExchangeAddress / MB_SIZE << " MB) to at least "
                               << (bufferDemand * 3.0 / 2) / MB_SIZE << " MB.\n");
                ZBAL_LOG_ERROR("  Calculated as: bufferDemand * 3/2 (because winIn = exchangeSpace * 2/3).\n");
                return -1;
            }
        }
    }

    int32_t launchRet = ZBALOpFusedDeepMoeLaunch(
        xAddr, expertIdsAddr, gmm1WAddr, gmm1ScaleAddr, gmm2WAddr, gmm2ScaleAddr, expertScalesAddr, smoothScalesAddr,
        shareGmm1WAddr, shareGmm1ScaleAddr, shareGmm2WAddr, shareGmm2ScaleAddr, shareSmoothScalesAddr, xActiveMaskAddr,
        outputAddr, shareOutputAddr, expertTokenNumsAddr, workspaceAddr, tilingDevAddr, epMetaAddr, tpMetaAddr,
        execFlag, srcDataType, aicNum, stream);
    return launchRet;
}
