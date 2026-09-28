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
/*
 * bf16 entry for fused_deep_moe. Split from zbal_kernel_fused_deep_moe.cpp so the
 * bf16 and fp16 kernels compile in separate translation units (parallel build).
 * Host-side tiling lives in zbal_kernel_fused_deep_moe_host.cpp.
 */

#include "dl_cann_api.h"
#include "kernel_operator.h"

#include "zbal_kernel_fused_deep_moe_tiling.h"
#include "zbal_kernel_fused_deep_moe.h"

using namespace AscendC;
using namespace ZbalCam;

extern "C" __global__ __aicore__ void
FusedDeepMoeBf16(GM_ADDR x, GM_ADDR expert_ids, GM_ADDR gmm1_weight, GM_ADDR gmm1_weight_scale, GM_ADDR gmm2_weight,
                 GM_ADDR gmm2_weight_scale, GM_ADDR expert_scales, GM_ADDR share_gmm1_weight,
                 GM_ADDR share_gmm1_weight_scale, GM_ADDR share_gmm2_weight, GM_ADDR share_gmm2_weight_scale,
                 GM_ADDR expert_smooth_scales, GM_ADDR share_smooth_scales, GM_ADDR x_active_mask, GM_ADDR output,
                 GM_ADDR share_output, GM_ADDR expert_token_nums, GM_ADDR workspace, GM_ADDR tiling, GM_ADDR ep_meta_gm,
                 GM_ADDR tp_meta_gm, uint32_t exec_flag)
{
    icache_preload(8);
    __gm__ const FusedDeepMoeTilingData *tiling_data = reinterpret_cast<__gm__ FusedDeepMoeTilingData *>(tiling);

    // Only instantiate flags actually used (to keep code size within jump range).
    // Flags: DEEP_FUSE=1, TENSOR_LIST=2, X_ACTIVE_MASK=4, SHARED_EXPERT=8, SMOOTH_QUANT=16
    // DeepSeekV3 core: 1|8|16=25, +TENSOR_LIST=27, +X_ACTIVE_MASK=29, +both=31
    // Test & shallow dispatch: 0, 1, 9
    switch (exec_flag) {
        case 0: {
            FusedDeepMoe<bfloat16_t, float, bfloat16_t, int32_t, false, 0> op;
            op.Init(x, expert_ids, gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, expert_scales,
                    share_gmm1_weight, share_gmm1_weight_scale, share_gmm2_weight, share_gmm2_weight_scale,
                    expert_smooth_scales, share_smooth_scales, x_active_mask, output, share_output, expert_token_nums,
                    workspace, ep_meta_gm, tp_meta_gm, GetTPipePtr(), tiling_data);
            op.Process();
            break;
        }
        case 1: {
            FusedDeepMoe<bfloat16_t, float, bfloat16_t, int32_t, false, 1> op;
            op.Init(x, expert_ids, gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, expert_scales,
                    share_gmm1_weight, share_gmm1_weight_scale, share_gmm2_weight, share_gmm2_weight_scale,
                    expert_smooth_scales, share_smooth_scales, x_active_mask, output, share_output, expert_token_nums,
                    workspace, ep_meta_gm, tp_meta_gm, GetTPipePtr(), tiling_data);
            op.Process();
            break;
        }
        case 9: {
            FusedDeepMoe<bfloat16_t, float, bfloat16_t, int32_t, false, 9> op;
            op.Init(x, expert_ids, gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, expert_scales,
                    share_gmm1_weight, share_gmm1_weight_scale, share_gmm2_weight, share_gmm2_weight_scale,
                    expert_smooth_scales, share_smooth_scales, x_active_mask, output, share_output, expert_token_nums,
                    workspace, ep_meta_gm, tp_meta_gm, GetTPipePtr(), tiling_data);
            op.Process();
            break;
        }
        case 25: {
            FusedDeepMoe<bfloat16_t, float, bfloat16_t, int32_t, false, 25> op;
            op.Init(x, expert_ids, gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, expert_scales,
                    share_gmm1_weight, share_gmm1_weight_scale, share_gmm2_weight, share_gmm2_weight_scale,
                    expert_smooth_scales, share_smooth_scales, x_active_mask, output, share_output, expert_token_nums,
                    workspace, ep_meta_gm, tp_meta_gm, GetTPipePtr(), tiling_data);
            op.Process();
            break;
        }
        case 27: {
            FusedDeepMoe<bfloat16_t, float, bfloat16_t, int32_t, false, 27> op;
            op.Init(x, expert_ids, gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, expert_scales,
                    share_gmm1_weight, share_gmm1_weight_scale, share_gmm2_weight, share_gmm2_weight_scale,
                    expert_smooth_scales, share_smooth_scales, x_active_mask, output, share_output, expert_token_nums,
                    workspace, ep_meta_gm, tp_meta_gm, GetTPipePtr(), tiling_data);
            op.Process();
            break;
        }
        case 29: {
            FusedDeepMoe<bfloat16_t, float, bfloat16_t, int32_t, false, 29> op;
            op.Init(x, expert_ids, gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, expert_scales,
                    share_gmm1_weight, share_gmm1_weight_scale, share_gmm2_weight, share_gmm2_weight_scale,
                    expert_smooth_scales, share_smooth_scales, x_active_mask, output, share_output, expert_token_nums,
                    workspace, ep_meta_gm, tp_meta_gm, GetTPipePtr(), tiling_data);
            op.Process();
            break;
        }
        case 31: {
            FusedDeepMoe<bfloat16_t, float, bfloat16_t, int32_t, false, 31> op;
            op.Init(x, expert_ids, gmm1_weight, gmm1_weight_scale, gmm2_weight, gmm2_weight_scale, expert_scales,
                    share_gmm1_weight, share_gmm1_weight_scale, share_gmm2_weight, share_gmm2_weight_scale,
                    expert_smooth_scales, share_smooth_scales, x_active_mask, output, share_output, expert_token_nums,
                    workspace, ep_meta_gm, tp_meta_gm, GetTPipePtr(), tiling_data);
            op.Process();
            break;
        }
        default:
            break;
    }
}

int32_t ZBALLaunchFusedDeepMoeBf16(GM_ADDR xAddr, GM_ADDR expertIdsAddr, GM_ADDR gmm1WAddr, GM_ADDR gmm1ScaleAddr,
                                   GM_ADDR gmm2WAddr, GM_ADDR gmm2ScaleAddr, GM_ADDR expertScalesAddr,
                                   GM_ADDR smoothScalesAddr, GM_ADDR shareGmm1WAddr, GM_ADDR shareGmm1ScaleAddr,
                                   GM_ADDR shareGmm2WAddr, GM_ADDR shareGmm2ScaleAddr, GM_ADDR shareSmoothScalesAddr,
                                   GM_ADDR xActiveMaskAddr, GM_ADDR outputAddr, GM_ADDR shareOutputAddr,
                                   GM_ADDR expertTokenNumsAddr, GM_ADDR workspaceAddr, GM_ADDR tilingDevAddr,
                                   GM_ADDR epMetaAddr, GM_ADDR tpMetaAddr, uint32_t execFlag, uint32_t aicNum,
                                   aclrtStream stream)
{
    FusedDeepMoeBf16<<<aicNum, nullptr, stream>>>(
        xAddr, expertIdsAddr, gmm1WAddr, gmm1ScaleAddr, gmm2WAddr, gmm2ScaleAddr, expertScalesAddr, shareGmm1WAddr,
        shareGmm1ScaleAddr, shareGmm2WAddr, shareGmm2ScaleAddr, smoothScalesAddr, shareSmoothScalesAddr,
        xActiveMaskAddr, outputAddr, shareOutputAddr, expertTokenNumsAddr, workspaceAddr, tilingDevAddr, epMetaAddr,
        tpMetaAddr, execFlag);
    return 0;
}

// fp16 launch entry, defined in zbal_kernel_fused_deep_moe_fp16.cpp.
int32_t ZBALLaunchFusedDeepMoeFp16(GM_ADDR xAddr, GM_ADDR expertIdsAddr, GM_ADDR gmm1WAddr, GM_ADDR gmm1ScaleAddr,
                                   GM_ADDR gmm2WAddr, GM_ADDR gmm2ScaleAddr, GM_ADDR expertScalesAddr,
                                   GM_ADDR smoothScalesAddr, GM_ADDR shareGmm1WAddr, GM_ADDR shareGmm1ScaleAddr,
                                   GM_ADDR shareGmm2WAddr, GM_ADDR shareGmm2ScaleAddr, GM_ADDR shareSmoothScalesAddr,
                                   GM_ADDR xActiveMaskAddr, GM_ADDR outputAddr, GM_ADDR shareOutputAddr,
                                   GM_ADDR expertTokenNumsAddr, GM_ADDR workspaceAddr, GM_ADDR tilingDevAddr,
                                   GM_ADDR epMetaAddr, GM_ADDR tpMetaAddr, uint32_t execFlag, uint32_t aicNum,
                                   aclrtStream stream);

int32_t ZBALOpFusedDeepMoeLaunch(GM_ADDR xAddr, GM_ADDR expertIdsAddr, GM_ADDR gmm1WAddr, GM_ADDR gmm1ScaleAddr,
                                 GM_ADDR gmm2WAddr, GM_ADDR gmm2ScaleAddr, GM_ADDR expertScalesAddr,
                                 GM_ADDR smoothScalesAddr, GM_ADDR shareGmm1WAddr, GM_ADDR shareGmm1ScaleAddr,
                                 GM_ADDR shareGmm2WAddr, GM_ADDR shareGmm2ScaleAddr, GM_ADDR shareSmoothScalesAddr,
                                 GM_ADDR xActiveMaskAddr, GM_ADDR outputAddr, GM_ADDR shareOutputAddr,
                                 GM_ADDR expertTokenNumsAddr, GM_ADDR workspaceAddr, GM_ADDR tilingDevAddr,
                                 GM_ADDR epMetaAddr, GM_ADDR tpMetaAddr, uint32_t execFlag, uint32_t srcDataType,
                                 uint32_t aicNum, aclrtStream stream)
{
    if (srcDataType == ZBAL_DATA_TYPE_BFP16) {
        return ZBALLaunchFusedDeepMoeBf16(xAddr, expertIdsAddr, gmm1WAddr, gmm1ScaleAddr, gmm2WAddr, gmm2ScaleAddr,
                                          expertScalesAddr, smoothScalesAddr, shareGmm1WAddr, shareGmm1ScaleAddr,
                                          shareGmm2WAddr, shareGmm2ScaleAddr, shareSmoothScalesAddr, xActiveMaskAddr,
                                          outputAddr, shareOutputAddr, expertTokenNumsAddr, workspaceAddr,
                                          tilingDevAddr, epMetaAddr, tpMetaAddr, execFlag, aicNum, stream);
    }
    if (srcDataType == ZBAL_DATA_TYPE_FP16) {
        return ZBALLaunchFusedDeepMoeFp16(xAddr, expertIdsAddr, gmm1WAddr, gmm1ScaleAddr, gmm2WAddr, gmm2ScaleAddr,
                                          expertScalesAddr, smoothScalesAddr, shareGmm1WAddr, shareGmm1ScaleAddr,
                                          shareGmm2WAddr, shareGmm2ScaleAddr, shareSmoothScalesAddr, xActiveMaskAddr,
                                          outputAddr, shareOutputAddr, expertTokenNumsAddr, workspaceAddr,
                                          tilingDevAddr, epMetaAddr, tpMetaAddr, execFlag, aicNum, stream);
    }
    return 0;
}
