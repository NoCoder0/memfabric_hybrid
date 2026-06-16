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
/* Adapted from umdk/src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe_tiling.h */

#ifndef ZBAL_KERNEL_FUSED_DEEP_MOE_TILING_H
#define ZBAL_KERNEL_FUSED_DEEP_MOE_TILING_H

#include <cstdint>
#include "kernel_tiling/kernel_tiling.h"

namespace ZbalCam {

struct FusedDeepMoeInfo {
    uint32_t epRankSize;          // epRankSize
    uint32_t epRankId;            // epRankId
    uint32_t moeExpertNum;        // moe expert number
    uint32_t moeExpertNumPerRank; // moe expert number per rank
    uint32_t quantMode;           // quant mode
    uint32_t globalBs;            // globalBs = BS * worldSize
    uint32_t bs;                  // bs
    uint32_t k;                   // topK
    uint32_t h;                   // hidden size
    uint32_t aicNum;              // AIC core number
    uint32_t aivNum;              // AIV core number
    uint64_t totalUbSize;
    uint64_t totalWinSize;
    uint64_t gmm1HLen;      // GMM1 weight N dim (routed expert ffn hidden)
    uint64_t shareGmm1HLen; // GMM1 weight N dim (shared expert ffn hidden)
    bool isTensorList;      // whether weights are in TensorList format
};

struct FusedDeepMoeTilingData {
    Mc2InitTiling mc2InitTiling;
    Mc2CcTiling mc2CcTiling;
    FusedDeepMoeInfo disGmmDeqSwigluQuantGmmDeqComInfo;
};

constexpr uint32_t GM_ALIGN_BYTE = 512;
constexpr uint32_t CUSTOM_PRELOAD_STAGES = 1;
constexpr uint32_t CUSTOM_L1_STAGES = 2;
constexpr uint32_t CUSTOM_L0A_STAGES = 2;
constexpr uint32_t CUSTOM_L0B_STAGES = 2;
constexpr uint32_t CUSTOM_L0C_STAGES = 1;
constexpr bool CUSTOM_ENABLE_UNIT_FLAG = true;
constexpr bool CUSTOM_ENABLE_SHUFFLE_K = true;

constexpr uint32_t GMM1_L1M = 256;
constexpr uint32_t GMM1_L1N = 128;
constexpr uint32_t GMM1_L1K = 512;
constexpr uint32_t GMM1_L0K = 128;
constexpr uint32_t GMM1_EPIM = 64;
constexpr uint32_t GMM1_SWIZZLE_OFFSET = 3;
constexpr uint32_t GMM1_SWIZZLE_DIRECTION = 0;

constexpr uint32_t GMM2_L1A_STAGES = 4;
constexpr uint32_t GMM2_L1B_STAGES = 2;
constexpr uint32_t GMM2_L0A_STAGES = 4;
constexpr uint32_t GMM2_L0B_STAGES = 2;
constexpr uint32_t GMM2_L1M = 128;
constexpr uint32_t GMM2_L1N = 256;
constexpr uint32_t GMM2_L1K = 512;
constexpr uint32_t GMM2_L0K = 128;
constexpr uint32_t GMM2_EPIM = 32;
constexpr uint32_t GMM2_SWIZZLE_OFFSET = 3;
constexpr uint32_t GMM2_SWIZZLE_DIRECTION = 0;

constexpr uint32_t WORKSPACE_STAGES = 4;

constexpr uint32_t EXEC_FLAG_DEEP_FUSE = (1U << 0);
constexpr uint32_t EXEC_FLAG_TENSOR_LIST = (1U << 1);
constexpr uint32_t EXEC_FLAG_X_ACTIVE_MASK = (1U << 2);
constexpr uint32_t EXEC_FLAG_SHARED_EXPERT = (1U << 3);
constexpr uint32_t EXEC_FLAG_SMOOTH_QUANT = (1U << 4);

} // namespace ZbalCam

#endif // ZBAL_KERNEL_FUSED_DEEP_MOE_TILING_H
