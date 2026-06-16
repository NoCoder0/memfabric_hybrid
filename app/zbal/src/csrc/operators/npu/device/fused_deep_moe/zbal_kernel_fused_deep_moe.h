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
/* Adapted from umdk/.../fused_deep_moe.h
 * Changes: include paths updated, namespace Cam → ZbalCam */

#ifndef ZBAL_KERNEL_FUSED_DEEP_MOE_H
#define ZBAL_KERNEL_FUSED_DEEP_MOE_H

#include <kernel_operator.h>
#include "lib/matmul_intf.h"

// Use Catlass from csrc/deepep instead of Catlass from zbal/third_party
#include "catlass/catlass.hpp"
#include "catlass/arch/arch.hpp"
#include "catlass/layout/layout.hpp"
#include "catlass/epilogue/tile/tile_broadcast_mul.hpp"
#include "catlass/epilogue/tile/tile_broadcast_one_blk.hpp"
#include "catlass/epilogue/tile/tile_swizzle.hpp"
#include "catlass/gemm/block/block_swizzle.hpp"
#include "catlass/gemm/gemm_type.hpp"
#include "epilogue/dispatch_policy.h"
#include "gemm/dispatch_policy.h"
#include "epilogue/block/block_epilogue.h"
#include "gemm/block/block_mmad.h"
#include "gemm/kernel/grouped_matmul_slice_m_per_token_dequant_multistage_workspace.h"
#include "gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h"

// RESTORED from umdk: Include dispatch header for shallow dispatch path
#include "raw_distributed/zbal_moe_distribute_dispatch.h"

#include "zbal_kernel_fused_deep_moe_tiling.h"
#include "zbal_kernel_fused_deep_moe_base.h"
#include "zbal_kernel_fused_deep_moe_comm.h"

using namespace Catlass;
using namespace ZbalCam;

using MmadAtlasA2Custom =
    Gemm::MmadAtlasA2PreloadAsyncWithCallback<CUSTOM_PRELOAD_STAGES, CUSTOM_L1_STAGES, CUSTOM_L0A_STAGES,
                                              CUSTOM_L0B_STAGES, CUSTOM_L0C_STAGES, CUSTOM_ENABLE_UNIT_FLAG,
                                              CUSTOM_ENABLE_SHUFFLE_K>;

using Gmm1L1TileShape = GemmShape<GMM1_L1M, GMM1_L1N, GMM1_L1K>;
using Gmm1L0TileShape = GemmShape<GMM1_L1M, GMM1_L1N, GMM1_L0K>;
using Gmm1EpilogueTileShape = MatrixShape<GMM1_EPIM, Gmm1L1TileShape::N>;
using Gmm1BlockScheduler = typename Gemm::Block::GemmIdentityBlockSwizzle<GMM1_SWIZZLE_OFFSET, GMM1_SWIZZLE_DIRECTION>;

using Gmm2L1TileShape = GemmShape<GMM2_L1M, GMM2_L1N, GMM2_L1K>;
using Gmm2L0TileShape = GemmShape<Gmm2L1TileShape::M, Gmm2L1TileShape::N, GMM2_L0K>;
using Gmm2EpilogueTileShape = MatrixShape<GMM2_EPIM, Gmm2L1TileShape::N>;
using Gmm2BlockScheduler = typename Gemm::Block::GemmIdentityBlockSwizzle<GMM2_SWIZZLE_OFFSET, GMM2_SWIZZLE_DIRECTION>;
using Gmm2DispatchPolicy =
    Gemm::MmadAtlasA2PreloadAsyncWithCallbackResidentA<CUSTOM_PRELOAD_STAGES, GMM2_L1A_STAGES, GMM2_L1B_STAGES,
                                                       GMM2_L0A_STAGES, GMM2_L0B_STAGES, CUSTOM_L0C_STAGES,
                                                       CUSTOM_ENABLE_UNIT_FLAG, CUSTOM_ENABLE_SHUFFLE_K>;

template<TemplateMC2TypeClass, class L1TileShape_, class L0TileShape_, class EpilogueTileShape_, class BlockScheduler_,
         class DispatchPolicy_ = MmadAtlasA2Custom>
CATLASS_DEVICE void GmmDeqSwigluQuant(
    GemmCoord problemShape, uint32_t groupCount, GM_ADDR gmGroupList, GM_ADDR gmA, layout::RowMajor layoutA,
    GM_ADDR gmShareB, layout::zN layoutShareB, GM_ADDR gmB, layout::zN layoutB, GM_ADDR gmShareScale,
    layout::VectorLayout layoutShareScale, GM_ADDR gmScale, layout::VectorLayout layoutScale, GM_ADDR gmPerTokenScale,
    layout::VectorLayout layoutPerTokenScale, GM_ADDR gmD, layout::RowMajor layoutD, GM_ADDR gmDequantScale,
    layout::VectorLayout layoutDequantScale, GM_ADDR gmShareX1, GM_ADDR gmShareX1Scale, GM_ADDR gmShareSwigluOut,
    GM_ADDR gmShareX2, layout::RowMajor layoutShareD, GM_ADDR gmShareX2Scale, GM_ADDR gmSwigluOut, GM_ADDR gmWorkspace,
    GM_ADDR gmX, GM_ADDR gmMoeSmoothScales, GM_ADDR gmShareSmoothScales, GM_ADDR gmexpertIds, GM_ADDR gmExpandIdx,
    GM_ADDR gmEpSendCount, GM_ADDR xActiveMask, GM_ADDR gmResvered, GM_ADDR gmExpertTokenNums,
    const __gm__ FusedDeepMoeInfo &disGmmDeqSwigluQuantGmmDeqComInfo, GM_ADDR epMetaGM = 0, GM_ADDR tpMetaGM = 0,
    GM_ADDR gmQuantWorkspace = 0)
{
    using ArchTag = Arch::AtlasA2;
    using DispatchPolicy = DispatchPolicy_;
    using L1TileShape = L1TileShape_;
    using L0TileShape = L0TileShape_;

    using AType = Gemm::GemmType<int8_t, layout::RowMajor>;
    using BType = Gemm::GemmType<int8_t, layout::zN>;
    using CType = Gemm::GemmType<int32_t, layout::RowMajor>;

    using BlockMmad = Gemm::Block::BlockMmad<DispatchPolicy, L1TileShape, L0TileShape, AType, BType, CType>;

    constexpr uint32_t ubStages = 1;
    using EpilogueDispatchPolicy = Epilogue::EpilogueAtlasA2PerTokenDequantSwiglu<ubStages, 0>;
    using ScaleType = Gemm::GemmType<W1ScaleType, layout::VectorLayout>;
    using PerTokenScaleType = Gemm::GemmType<float, layout::VectorLayout>;
    using DType = Gemm::GemmType<float, layout::RowMajor>;

    using RowBroadcastMulType = Gemm::GemmType<float, layout::RowMajor>;
    using BroadcastOneBlkType = Gemm::GemmType<float, layout::RowMajor>;
    using OneBlkColumnBroadcastMulType = Gemm::GemmType<float, layout::RowMajor>;

    using EpilogueTileShape = EpilogueTileShape_;
    using TileRowBroadcastMul = Epilogue::Tile::TileRowBroadcastMul<ArchTag, RowBroadcastMulType, EpilogueTileShape>;
    using TileBroadcastOneBlk =
        Epilogue::Tile::TileBroadcastOneBlk<ArchTag, BroadcastOneBlkType, EpilogueTileShape::ROW>;
    using TileOneBlkColumnBroadcastMul =
        Epilogue::Tile::TileOneBlkColumnBroadcastMul<ArchTag, OneBlkColumnBroadcastMulType, EpilogueTileShape>;
    using TileCopy = Epilogue::Tile::TileCopy<ArchTag, CType, ScaleType, PerTokenScaleType, DType>;
    using TileScheduler = Epilogue::Tile::EpilogueHorizontalTileSwizzle;

    using BlockEpilogue = Epilogue::Block::BlockEpilogue<EpilogueDispatchPolicy, CType, ScaleType, PerTokenScaleType,
                                                         DType, TileRowBroadcastMul, TileBroadcastOneBlk,
                                                         TileOneBlkColumnBroadcastMul, TileCopy, TileScheduler>;

    using BlockScheduler = BlockScheduler_;

    // kernel level
    using ElementGroupList = int64_t;

    // RESTORED from umdk: Use conditional kernel type based on EXEC_FLAG_DEEP_FUSE
    using GemmKernel = typename std::conditional<
        (EXEC_FLAG & EXEC_FLAG_DEEP_FUSE),
        Gemm::Kernel::GroupedMatmulSliceMPerTokenDequantSwigluQuantMultiStageWorkspace<
            TemplateMC2TypeFunc, BlockMmad, BlockEpilogue, BlockScheduler, WORKSPACE_STAGES, ElementGroupList>,
        Gemm::Kernel::GroupedMatmulSliceMPerTokenDequantSwigluQuantMultiStageWorkspaceWithShallowDispatch<
            TemplateMC2TypeFunc, BlockMmad, BlockEpilogue, BlockScheduler, WORKSPACE_STAGES, ElementGroupList>>::type;

    if constexpr (EXEC_FLAG & EXEC_FLAG_DEEP_FUSE) {
        typename GemmKernel::Params params{problemShape,
                                           groupCount,
                                           gmGroupList,
                                           gmA,
                                           layoutA,
                                           gmShareB,
                                           layoutShareB,
                                           gmB,
                                           layoutB,
                                           gmShareScale,
                                           layoutShareScale,
                                           gmScale,
                                           layoutScale,
                                           gmPerTokenScale,
                                           layoutPerTokenScale,
                                           gmD,
                                           layoutD,
                                           gmDequantScale,
                                           layoutDequantScale,
                                           gmWorkspace,
                                           gmX,
                                           gmMoeSmoothScales,
                                           gmShareSmoothScales,
                                           gmexpertIds,
                                           gmExpandIdx,
                                           gmEpSendCount,
                                           xActiveMask,
                                           gmResvered,
                                           gmExpertTokenNums,
                                           gmShareX1,
                                           gmShareX1Scale,
                                           gmShareSwigluOut,
                                           gmShareX2,
                                           layoutShareD,
                                           gmShareX2Scale,
                                           gmSwigluOut,
                                           epMetaGM,
                                           tpMetaGM,
                                           disGmmDeqSwigluQuantGmmDeqComInfo};
        params.gmQuantWorkspace = gmQuantWorkspace;
        // call a kernel
        GemmKernel gemm;
        gemm(params);
    } else {
        typename GemmKernel::Params params{
            problemShape, groupCount,      gmGroupList,         gmA,
            layoutA,      gmShareB,        layoutShareB,        gmB,
            layoutB,      gmShareScale,    layoutShareScale,    gmScale,
            layoutScale,  gmPerTokenScale, layoutPerTokenScale, gmD,
            layoutD,      gmDequantScale,  layoutDequantScale,  gmWorkspace,
            gmShareX1,    gmShareX1Scale,  gmShareSwigluOut,    gmShareX2,
            layoutShareD, gmShareX2Scale,  gmSwigluOut,         disGmmDeqSwigluQuantGmmDeqComInfo};
        // call a kernel
        GemmKernel gemm;
        gemm(params);
    }
}

template<TemplateMC2TypeClass, class L1TileShape_, class L0TileShape_, class EpilogueTileShape_, class BlockScheduler_,
         class DispatchPolicy_ = MmadAtlasA2Custom>
CATLASS_DEVICE void
GmmDeq(GemmCoord problemShape, uint32_t groupCount, GM_ADDR gmGroupList, GM_ADDR gmA, layout::RowMajor layoutA,
       GM_ADDR gmB, layout::zN layoutB, GM_ADDR gmScale, layout::VectorLayout layoutScale, GM_ADDR gmPerTokenScale,
       layout::VectorLayout layoutPerTokenScale, GM_ADDR gmD, layout::RowMajor layoutD, uint32_t batchSize,
       GemmCoord sharedGmm2ProblemShape, GM_ADDR gmSharedA, GM_ADDR gmSharedB, GM_ADDR gmSharedD, GM_ADDR gmSharedScale,
       GM_ADDR gmSharedPtrPerTokenScale, layout::RowMajor sharedLayoutA, layout::zN sharedLayoutB,
       layout::VectorLayout sharedLayoutPerTokenScale, layout::RowMajor sharedLayoutD, uint32_t epRankId,
       GM_ADDR epMetaGM, GM_ADDR gmWorkspace, void *combiner)
{
    using ArchTag = Arch::AtlasA2;
    using DispatchPolicy = DispatchPolicy_;
    using L1TileShape = L1TileShape_;
    using L0TileShape = L0TileShape_;

    using AType = Gemm::GemmType<int8_t, layout::RowMajor>;
    using BType = Gemm::GemmType<int8_t, layout::zN>;
    using CType = Gemm::GemmType<int32_t, layout::RowMajor>;

    using BlockMmad = Gemm::Block::BlockMmad<DispatchPolicy, L1TileShape, L0TileShape, AType, BType, CType>;

    constexpr uint32_t ubStages = 1;
    using EpilogueDispatchPolicy = Epilogue::EpilogueAtlasA2PerTokenDequantCombine<ubStages, EXEC_FLAG>;
    using ScaleType = Gemm::GemmType<W2ScaleType, layout::VectorLayout>;
    using PerTokenScaleType = Gemm::GemmType<float, layout::VectorLayout>;
    using DType = Gemm::GemmType<ExpandXType, layout::RowMajor>;

    using RowBroadcastMulType = Gemm::GemmType<float, layout::RowMajor>;
    using BroadcastOneBlkType = Gemm::GemmType<float, layout::RowMajor>;
    using OneBlkColumnBroadcastMulType = Gemm::GemmType<float, layout::RowMajor>;

    using EpilogueTileShape = EpilogueTileShape_;
    using TileRowBroadcastMul = Epilogue::Tile::TileRowBroadcastMul<ArchTag, RowBroadcastMulType, EpilogueTileShape>;
    using TileBroadcastOneBlk =
        Epilogue::Tile::TileBroadcastOneBlk<ArchTag, BroadcastOneBlkType, EpilogueTileShape::ROW>;
    using TileOneBlkColumnBroadcastMul =
        Epilogue::Tile::TileOneBlkColumnBroadcastMul<ArchTag, OneBlkColumnBroadcastMulType, EpilogueTileShape>;
    using TileCopy = Epilogue::Tile::TileCopy<ArchTag, CType, ScaleType, PerTokenScaleType, DType>;
    using TileScheduler = Epilogue::Tile::EpilogueHorizontalTileSwizzle;

    using BlockEpilogue = Epilogue::Block::BlockEpilogue<EpilogueDispatchPolicy, CType, ScaleType, PerTokenScaleType,
                                                         DType, TileRowBroadcastMul, TileBroadcastOneBlk,
                                                         TileOneBlkColumnBroadcastMul, TileCopy, TileScheduler>;

    using BlockScheduler = BlockScheduler_;

    // kernel level
    using ElementGroupList = int64_t;
    using GemmKernel = Gemm::Kernel::GroupedMatmulSliceMPerTokenDequantMultiStageWorkspace<
        TemplateMC2TypeFunc, BlockMmad, BlockEpilogue, BlockScheduler, WORKSPACE_STAGES, ElementGroupList>;

    typename GemmKernel::Params params{problemShape,
                                       groupCount,
                                       gmGroupList,
                                       gmA,
                                       layoutA,
                                       gmB,
                                       layoutB,
                                       gmScale,
                                       layoutScale,
                                       gmPerTokenScale,
                                       layoutPerTokenScale,
                                       gmD,
                                       layoutD,
                                       batchSize,
                                       sharedGmm2ProblemShape,
                                       gmSharedA,
                                       gmSharedB,
                                       gmSharedD,
                                       gmSharedScale,
                                       gmSharedPtrPerTokenScale,
                                       sharedLayoutA,
                                       sharedLayoutB,
                                       sharedLayoutPerTokenScale,
                                       sharedLayoutD,
                                       gmWorkspace,
                                       epMetaGM,
                                       combiner};

    // call a kernel
    GemmKernel gemm{epRankId};
    gemm(params);
}

template<TemplateMC2TypeClass>
class FusedDeepMoe {
public:
    __aicore__ inline FusedDeepMoe(){};
    __aicore__ inline void Init(
        // input
        GM_ADDR x, GM_ADDR expert_ids, GM_ADDR gmm1_weight, GM_ADDR gmm1_weight_scale, GM_ADDR gmm2_weight,
        GM_ADDR gmm2_weight_scale, GM_ADDR expert_scales, GM_ADDR share_gmm1_weight, GM_ADDR share_gmm1_weight_scale,
        GM_ADDR share_gmm2_weight, GM_ADDR share_gmm2_weight_scale, GM_ADDR expert_smooth_scales,
        GM_ADDR share_smooth_scales, GM_ADDR x_active_mask,
        // output
        GM_ADDR output, GM_ADDR share_output, GM_ADDR expertTokenNums,
        // system
        GM_ADDR workspaceGM, GM_ADDR epMetaGM, GM_ADDR tpMetaGM, AscendC::TPipe *pipe,
        __gm__ const FusedDeepMoeTilingData *tilingData);
    __aicore__ inline void Process();

private:
    GM_ADDR gmX_;
    GM_ADDR gmexpertIds_;
    GM_ADDR gmWeight1_;
    GM_ADDR gmScale1_;
    GM_ADDR gmWeight2_;
    GM_ADDR gmScale2_;
    GM_ADDR gmOutput_;
    GM_ADDR gmExpertTokenNums_;
    GM_ADDR workspaceGM_;
    GM_ADDR gmSmoothScales_;
    GM_ADDR gmexpertScales_;
    GM_ADDR xActiveMask_;
    GM_ADDR epMetaGM_;
    GM_ADDR tpMetaGM_;
    GM_ADDR gmShareWeight1_;
    GM_ADDR gmShareWeight1Scale_;
    GM_ADDR gmShareWeight2_;
    GM_ADDR gmShareWeight2Scale_;
    GM_ADDR gmShareOutput_;
    GM_ADDR gmShareSmoothScales_;
    GM_ADDR gmQuantWorkspace_; // local workspace for quantized tokens (pull-mode dispatch)
    ZbalCommContext epZbalContext_;
    ZbalCommContext tpZbalContext_;

    uint32_t maxTokenNum_{0};
    uint32_t gmm1OutputDim_{0};
    uint32_t tokenHiddenSize_{0};
    uint32_t groupCount_{0};
    uint32_t gmm2OutputDim_{0};
    uint32_t gmm2InputDim_{0};
    uint32_t globalRankId_{0};
    uint32_t winSizePerRank_{0};
    uint32_t blockDim_{0};
    uint32_t epRankSize_{0};
    uint32_t epRankId_{0};
    uint32_t moeExpertNumPerRank_{0};
    uint32_t globalBs_{0};
    uint32_t bs_{0};
    uint32_t maxBs_{0};
    uint32_t topK_{0};
    uint32_t shareGmm1OutputDim_{0};
    uint32_t shareGmm2InputDim_{0};

    AscendC::TPipe *tpipe_{nullptr};
    __gm__ const FusedDeepMoeTilingData *tilingData_;
};

template<TemplateMC2TypeClass>
__aicore__ inline void FusedDeepMoe<TemplateMC2TypeFunc>::Init(
    // input
    GM_ADDR x, GM_ADDR expert_ids, GM_ADDR gmm1_weight, GM_ADDR gmm1_weight_scale, GM_ADDR gmm2_weight,
    GM_ADDR gmm2_weight_scale, GM_ADDR expert_scales, GM_ADDR share_gmm1_weight, GM_ADDR share_gmm1_weight_scale,
    GM_ADDR share_gmm2_weight, GM_ADDR share_gmm2_weight_scale, GM_ADDR expert_smooth_scales,
    GM_ADDR share_smooth_scales, GM_ADDR x_active_mask,
    // output
    GM_ADDR output, GM_ADDR share_output, GM_ADDR expertTokenNums,
    // system
    GM_ADDR workspaceGM, GM_ADDR epMetaGM, GM_ADDR tpMetaGM, AscendC::TPipe *pipe,
    __gm__ const FusedDeepMoeTilingData *tilingData)
{
    tpipe_ = pipe;
    blockDim_ = AscendC::GetBlockNum();

    // Store meta GMs
    epMetaGM_ = epMetaGM;
    tpMetaGM_ = tpMetaGM;

    // Setup EP zbal context
    auto epComm = reinterpret_cast<__gm__ CommGroupInfo *>(epMetaGM);
    epZbalContext_.comm = epComm;
    epZbalContext_.dataWindowBase = reinterpret_cast<__gm__ uint8_t *>(epComm->myAddressExchangeGva);
    epZbalContext_.myRankId = epComm->myGroupRank;
    epZbalContext_.worldSize = epComm->groupSize;
    epZbalContext_.localDeviceMemSize = epComm->localDeviceMemSize;

    uint64_t totalMetaSize = epComm->sizeForExchangeAddress;
    constexpr uint64_t kGmAlign = 512;
    if constexpr (EXEC_FLAG & EXEC_FLAG_DEEP_FUSE) {
        // DEEP_FUSE: data window not used (dispatch uses quant workspace,
        // combine uses separate workspace).  Give it all to state window.
        epZbalContext_.dataWindowSize = 0;
        epZbalContext_.stateWindowSize = totalMetaSize;
    } else {
        epZbalContext_.dataWindowSize = (totalMetaSize * 2 / 3) & ~(kGmAlign - 1);
        epZbalContext_.stateWindowSize = totalMetaSize - epZbalContext_.dataWindowSize;
    }
    epZbalContext_.stateWindowBase = epZbalContext_.dataWindowBase + epZbalContext_.dataWindowSize;

    // Setup TP zbal context (use same comm for now since EP and TP share the same process group)
    auto tpComm = reinterpret_cast<__gm__ CommGroupInfo *>(tpMetaGM);
    tpZbalContext_.comm = tpComm;
    tpZbalContext_.dataWindowBase = reinterpret_cast<__gm__ uint8_t *>(tpComm->myAddressExchangeGva);
    tpZbalContext_.myRankId = tpComm->myGroupRank;
    tpZbalContext_.worldSize = tpComm->groupSize;
    tpZbalContext_.localDeviceMemSize = tpComm->localDeviceMemSize;

    totalMetaSize = tpComm->sizeForExchangeAddress;
    if constexpr (EXEC_FLAG & EXEC_FLAG_DEEP_FUSE) {
        tpZbalContext_.dataWindowSize = 0;
        tpZbalContext_.stateWindowSize = totalMetaSize;
    } else {
        tpZbalContext_.dataWindowSize = (totalMetaSize * 2 / 3) & ~(kGmAlign - 1);
        tpZbalContext_.stateWindowSize = totalMetaSize - tpZbalContext_.dataWindowSize;
    }
    tpZbalContext_.stateWindowBase = tpZbalContext_.dataWindowBase + tpZbalContext_.dataWindowSize;

    gmSmoothScales_ = expert_smooth_scales; // not used now
    gmX_ = x;                               // input token
    gmexpertIds_ = expert_ids;
    gmWeight1_ = gmm1_weight;
    gmScale1_ = gmm1_weight_scale;
    gmWeight2_ = gmm2_weight;
    gmScale2_ = gmm2_weight_scale;
    gmOutput_ = output;
    gmExpertTokenNums_ = expertTokenNums;
    workspaceGM_ = workspaceGM;
    gmexpertScales_ = expert_scales;
    xActiveMask_ = x_active_mask;
    tilingData_ = tilingData;
    epRankSize_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.epRankSize;
    epRankId_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.epRankId;
    moeExpertNumPerRank_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.moeExpertNumPerRank;
    globalBs_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.globalBs;
    bs_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.bs;
    topK_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.k;
    maxBs_ = globalBs_ / epRankSize_;

    maxTokenNum_ = maxBs_ * epRankSize_ * (topK_ < moeExpertNumPerRank_ ? topK_ : moeExpertNumPerRank_);

    gmm1OutputDim_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.gmm1HLen;
    tokenHiddenSize_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.h;
    groupCount_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.moeExpertNumPerRank;
    gmm2OutputDim_ = tokenHiddenSize_;
    gmm2InputDim_ = gmm1OutputDim_ / 2;

    gmShareWeight1_ = share_gmm1_weight;
    gmShareWeight1Scale_ = share_gmm1_weight_scale;
    gmShareWeight2_ = share_gmm2_weight;
    gmShareWeight2Scale_ = share_gmm2_weight_scale;
    gmShareOutput_ = share_output;
    gmShareSmoothScales_ = share_smooth_scales;
    shareGmm1OutputDim_ = tilingData->disGmmDeqSwigluQuantGmmDeqComInfo.shareGmm1HLen;
    shareGmm2InputDim_ = shareGmm1OutputDim_ / 2;
}

template<TemplateMC2TypeClass>
__aicore__ inline void FusedDeepMoe<TemplateMC2TypeFunc>::Process()
{
    GemmCoord gmm1ProblemShape{maxTokenNum_, gmm1OutputDim_, tokenHiddenSize_};
    GemmCoord gmm2ProblemShape{maxTokenNum_, gmm2OutputDim_, gmm2InputDim_};

    layout::RowMajor layoutX1{maxTokenNum_, tokenHiddenSize_};
    layout::zN layoutShareWeight1 = layout::zN::template MakeLayout<int8_t>(tokenHiddenSize_, shareGmm1OutputDim_);
    layout::zN layoutWeight1 = layout::zN::template MakeLayout<int8_t>(tokenHiddenSize_, gmm1OutputDim_);
    layout::VectorLayout layoutShareW1Scale{shareGmm1OutputDim_};
    layout::VectorLayout layoutW1Scale{gmm1OutputDim_};
    layout::VectorLayout layoutX1Scale{maxTokenNum_};
    layout::RowMajor layoutX2{maxTokenNum_, gmm2InputDim_};
    layout::zN layoutWeight2 = layout::zN::template MakeLayout<int8_t>(gmm2InputDim_, gmm2OutputDim_);
    layout::VectorLayout layoutW2Scale{gmm2OutputDim_};
    layout::VectorLayout layoutX2Scale{maxTokenNum_};
    layout::RowMajor layoutOutput{maxTokenNum_, gmm2OutputDim_};

    layout::RowMajor layoutShareX2{bs_, shareGmm2InputDim_};
    layout::zN layoutShareWeight2 = layout::zN::template MakeLayout<int8_t>(shareGmm2InputDim_, gmm2OutputDim_);
    GemmCoord shareGmm2ProblemShape{bs_, gmm2OutputDim_, shareGmm2InputDim_};
    layout::VectorLayout layoutShareX2Scale{bs_};
    layout::RowMajor layoutShareOutput{bs_, gmm2OutputDim_};

    GM_ADDR gmShareX1 = nullptr;
    GM_ADDR gmShareX1Scale = nullptr;
    GM_ADDR gmShareSwigluOut = nullptr;
    GM_ADDR gmShareX2 = nullptr;
    GM_ADDR gmShareX2Scale = nullptr;

    GM_ADDR gmX1 = nullptr;
    GM_ADDR gmX1Scale = nullptr;
    GM_ADDR gmSwigluOut = nullptr;
    GM_ADDR gmX2 = nullptr;
    GM_ADDR gmX2Scale = nullptr;
    size_t shareExpertTokenNum = 0;
    if constexpr (EXEC_FLAG & EXEC_FLAG_SHARED_EXPERT) {
        shareExpertTokenNum = bs_;
    }
    size_t maxHandleTokenNum = maxTokenNum_ + shareExpertTokenNum;
    size_t workspaceOffset = 0;
    constexpr int32_t resveredWorkSpaceSize = 256 * 1024;
    int64_t x1TokenSize = maxHandleTokenNum * tokenHiddenSize_ * sizeof(int8_t);
    int64_t x2TokenSize = (maxTokenNum_ * gmm2InputDim_ + shareExpertTokenNum * shareGmm2InputDim_) * sizeof(int8_t);
    int64_t maxTokenSize = x1TokenSize < x2TokenSize ? x2TokenSize : x1TokenSize;
    int64_t tokenScaleSize = maxHandleTokenNum * sizeof(float);
    gmShareX1 = workspaceGM_ + workspaceOffset;
    gmShareX2 = workspaceGM_ + workspaceOffset;
    gmX1 = gmShareX1 + (static_cast<size_t>(shareExpertTokenNum) * tokenHiddenSize_ * sizeof(int8_t));
    gmX2 = gmShareX2 + (static_cast<size_t>(shareExpertTokenNum) * shareGmm2InputDim_ * sizeof(int8_t));
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(maxTokenSize);
    gmShareX1Scale = workspaceGM_ + workspaceOffset;
    gmShareX2Scale = workspaceGM_ + workspaceOffset;
    gmX1Scale = gmShareX1Scale + (static_cast<size_t>(shareExpertTokenNum) * sizeof(float));
    gmX2Scale = gmShareX2Scale + (static_cast<size_t>(shareExpertTokenNum) * sizeof(float));
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(tokenScaleSize);
    GM_ADDR gmWorkspace = workspaceGM_ + workspaceOffset;
    GM_ADDR gmCVSwap = workspaceGM_ + workspaceOffset;
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(static_cast<size_t>(blockDim_) * (GMM1_L1M * GMM1_L1N) *
                                              WORKSPACE_STAGES * sizeof(int32_t));
    int64_t swigluOutSize = (maxTokenNum_ * gmm1OutputDim_ + shareExpertTokenNum * shareGmm1OutputDim_) * sizeof(float);
    int64_t gmm2OutSize = maxTokenNum_ * tokenHiddenSize_ * sizeof(ExpandXType);
    int64_t maxSwigluGmm2Size = swigluOutSize < gmm2OutSize ? gmm2OutSize : swigluOutSize;
    gmShareSwigluOut = workspaceGM_ + workspaceOffset;
    gmSwigluOut = gmShareSwigluOut + (static_cast<size_t>(shareExpertTokenNum) * shareGmm1OutputDim_ * sizeof(float));
    GM_ADDR gmGmm2DepOut = workspaceGM_ + workspaceOffset;
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(maxSwigluGmm2Size);
    GM_ADDR gmGroupList = workspaceGM_ + workspaceOffset;
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(static_cast<size_t>(groupCount_) * sizeof(int64_t));
    GM_ADDR gmExpandIdx = workspaceGM_ + workspaceOffset;
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(static_cast<size_t>(bs_) * topK_ * sizeof(int32_t));
    GM_ADDR gmEpSendCount = workspaceGM_ + workspaceOffset;
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(static_cast<size_t>(epRankSize_) * groupCount_ * sizeof(int32_t));
    GM_ADDR gmResvered = workspaceGM_ + workspaceOffset;
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(resveredWorkSpaceSize);
    // Pull-mode dispatch: each rank writes quantized tokens to local workspace.
    // Other ranks read via cross-rank GVA (zbal_ptr).  Max tokens = bs_ * topK_.
    // Each token = quantStride bytes (CEIL_UP(h * sizeof(int8_t) + sizeof(float))).
    int64_t quantStrideHost =
        static_cast<int64_t>(RoundUp<GM_ALIGN_BYTE>(tokenHiddenSize_ * sizeof(int8_t) + sizeof(float)));
    size_t quantWorkspaceBytes = static_cast<size_t>(bs_) * topK_ * static_cast<size_t>(quantStrideHost);
    gmQuantWorkspace_ = workspaceGM_ + workspaceOffset;
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(static_cast<int64_t>(quantWorkspaceBytes));
    // Combine workspace: replaces the zbal data window for GMM2 combine.
    // Each remote rank writes tokens for all experts here.
    // Layout: epRankSize_ × groupCount_ × maxBs_ × tokenHiddenSize_ × sizeof(ExpandXType)
    // (identical to the per-rank data-window payload that was in zbal meta).
    size_t combineWsSize =
        static_cast<size_t>(epRankSize_) * groupCount_ * maxBs_ * tokenHiddenSize_ * sizeof(ExpandXType);
    GM_ADDR gmCombineWs = workspaceGM_ + workspaceOffset;
    workspaceOffset += RoundUp<GM_ALIGN_BYTE>(static_cast<int64_t>(combineWsSize));

    // Run dispatch phase when EXEC_FLAG_DEEP_FUSE is NOT set
    if constexpr ((EXEC_FLAG & EXEC_FLAG_DEEP_FUSE) == 0) {
        if constexpr (g_coreType == AscendC::AIV) {
            AscendC::TPipe tpipe;
            MoeDistributeDispatchImpl::CamMoeDistributeDispatch<ExpandXType, int8_t, false, true,
                                                                static_cast<bool>(EXEC_FLAG & EXEC_FLAG_SMOOTH_QUANT),
                                                                false, EXEC_FLAG>
                dispatcher;
            dispatcher.Init(gmX_, gmexpertIds_, gmSmoothScales_, gmShareSmoothScales_, xActiveMask_, gmShareX1, gmX1,
                            gmShareX1Scale, gmX1Scale, gmExpandIdx, gmGroupList, gmEpSendCount, gmExpertTokenNums_,
                            nullptr, workspaceGM_, &tpipe, tilingData_, epMetaGM_, tpMetaGM_);
            dispatcher.Process();
            tpipe.Destroy();
            icache_preload(8);
        }

        AscendC::PipeBarrier<PIPE_ALL>();
        Arch::CrossCoreFlag gmm1AivFinished{0};
        if constexpr (g_coreType == AscendC::AIV) {
            Arch::CrossCoreBarrier<0x0, PIPE_MTE3>();
            Arch::CrossCoreSetFlag<0x2, PIPE_MTE3>(gmm1AivFinished);
        } else {
            Arch::CrossCoreWaitFlag(gmm1AivFinished);
        }
    }

    GmmDeqSwigluQuant<TemplateMC2TypeFunc, Gmm1L1TileShape, Gmm1L0TileShape, Gmm1EpilogueTileShape, Gmm1BlockScheduler>(
        gmm1ProblemShape, groupCount_, gmGroupList, gmX1, layoutX1, gmShareWeight1_, layoutShareWeight1, gmWeight1_,
        layoutWeight1, gmShareWeight1Scale_, layoutShareW1Scale, gmScale1_, layoutW1Scale, gmX1Scale, layoutX1Scale,
        gmX2, layoutX2, gmX2Scale, layoutX2Scale, gmShareX1, gmShareX1Scale, gmShareSwigluOut, gmShareX2, layoutShareX2,
        gmShareX2Scale, gmSwigluOut, gmWorkspace, gmX_, gmSmoothScales_, gmShareSmoothScales_, gmexpertIds_,
        gmExpandIdx, gmEpSendCount, xActiveMask_, gmResvered, gmExpertTokenNums_,
        tilingData_->disGmmDeqSwigluQuantGmmDeqComInfo, epMetaGM_, tpMetaGM_, gmQuantWorkspace_);
    AscendC::PipeBarrier<PIPE_ALL>();
    Arch::CrossCoreFlag gmm1AivFinished{0};
    if constexpr (g_coreType == AscendC::AIV) {
        Arch::CrossCoreBarrier<0x0, PIPE_MTE3>();
        Arch::CrossCoreSetFlag<0x2, PIPE_MTE3>(gmm1AivFinished);
    } else {
        Arch::CrossCoreWaitFlag(gmm1AivFinished);
    }

#ifdef DEBUG_DUMP_EXPANDIDX_EPSENDCOUNT
    // =====================================================================
    // DEBUG: dump intermediate data to gmOutput_ for push-vs-pull comparison.
    //
    // Build: -DDEBUG_DUMP_EXPANDIDX_EPSENDCOUNT
    //
    // gmOutput_ layout (int32 flat, reinterpreted bf16 output):
    //   [0]:  magic = 0xDB60E1D0
    //   [1]:  expandIdxCount  (= bs_ * topK_)
    //   [2]:  epSendCount     (= epRankSize_ * groupCount_)
    //   [3]:  epRankSize_
    //   [4]:  groupCount_ (localExpertNum)
    //   [5]:  bs_
    //   [6]:  topK_
    //   [7]:  x1DumpSz  (= 7168, one full token in int8 bytes)
    //   [8]:  x2DumpSz  (= 2048)
    //   [9]:  glEntries (= groupCount_)
    //   [10]: lqDumpSz  (= 256)
    //   [11]: rqDumpSz  (= 256)
    //   [12..12+expandIdxCount-1]: gmExpandIdx (int32)
    //   [eps_start..eps_start+epSendCount-1]: gmEpSendCount (int32)
    //   [x1_start..x1_start+x1DumpSz-1]: gmX1 first x1DumpSz int8 (as int32)
    //   [sc_start..sc_start+63]: gmX1Scale first 64 uint32 (as int32)
    //   [x2_start..x2_start+x2DumpSz-1]: gmX2 first x2DumpSz int8 (as int32)
    //   [gl_start..gl_start+glEntries*2-1]: gmGroupList int64 (as 2 int32 each)
    //   [lq_start..lq_start+lqDumpSz-1]: local quant buffer (uint8 as int32)
    //   [rq_start..rq_start+rqDumpSz-1]: rank1 remote quant buffer (uint8 as int32)
    // =====================================================================
    if constexpr (g_coreType == AscendC::AIV) {
        if (AscendC::GetBlockIdx() == 0) {
            uint32_t expandIdxCnt = static_cast<uint32_t>(bs_) * topK_;
            uint32_t epSendCnt = static_cast<uint32_t>(epRankSize_) * groupCount_;
            constexpr uint32_t kHdrWords = 12;
            constexpr uint32_t kX1Sz = 7168; // one full token (h=7168 int8 bytes)
            constexpr uint32_t kX2Sz = 2048;
            constexpr uint32_t kScSz = 64;
            constexpr uint32_t kLqSz = 256;
            constexpr uint32_t kRqSz = 256;

            AscendC::GlobalTensor<int32_t> dbgOut;
            dbgOut.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(gmOutput_));
            AscendC::GlobalTensor<int32_t> srcExpand;
            srcExpand.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(gmExpandIdx));
            AscendC::GlobalTensor<int32_t> srcEpSend;
            srcEpSend.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(gmEpSendCount));

            // ---- header (scalar writes) ----
            dbgOut.SetValue(0, 0xDB60E1D0);
            dbgOut.SetValue(1, static_cast<int32_t>(expandIdxCnt));
            dbgOut.SetValue(2, static_cast<int32_t>(epSendCnt));
            dbgOut.SetValue(3, static_cast<int32_t>(epRankSize_));
            dbgOut.SetValue(4, static_cast<int32_t>(groupCount_));
            dbgOut.SetValue(5, static_cast<int32_t>(bs_));
            dbgOut.SetValue(6, static_cast<int32_t>(topK_));
            dbgOut.SetValue(7, static_cast<int32_t>(kX1Sz));
            dbgOut.SetValue(8, static_cast<int32_t>(kX2Sz));
            dbgOut.SetValue(9, static_cast<int32_t>(groupCount_));
            dbgOut.SetValue(10, static_cast<int32_t>(kLqSz));
            dbgOut.SetValue(11, static_cast<int32_t>(kRqSz));

            // ---- gmExpandIdx (scalar copy) ----
            for (uint32_t i = 0; i < expandIdxCnt; i++) {
                dbgOut.SetValue(kHdrWords + i, srcExpand.GetValue(i));
            }

            // ---- gmEpSendCount (scalar copy) ----
            uint32_t epsOff = kHdrWords + expandIdxCnt;
            for (uint32_t i = 0; i < epSendCnt; i++) {
                dbgOut.SetValue(epsOff + i, srcEpSend.GetValue(i));
            }

            // ---- X1 int8: first kX1Sz=7168 bytes (one full token as int32) ----
            uint32_t x1Off = epsOff + epSendCnt;
            {
                AscendC::GlobalTensor<int8_t> x1Gm;
                x1Gm.SetGlobalBuffer(reinterpret_cast<__gm__ int8_t *>(gmX1));
                for (uint32_t i = 0; i < kX1Sz; i++) {
                    dbgOut.SetValue(x1Off + i, static_cast<int32_t>(x1Gm.GetValue(i)));
                }
            }

            // ---- X1Scale: first 64 scales (raw uint32 bits) ----
            uint32_t scOff = x1Off + kX1Sz;
            {
                AscendC::GlobalTensor<uint32_t> scGm32;
                scGm32.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(gmX1Scale));
                for (uint32_t i = 0; i < kScSz; i++) {
                    dbgOut.SetValue(scOff + i, static_cast<int32_t>(scGm32.GetValue(i)));
                }
            }

            // ---- X2 int8: first kX2Sz bytes (GMM1 output = GMM2 input) ----
            uint32_t x2Off = scOff + kScSz;
            {
                AscendC::GlobalTensor<int8_t> x2Gm;
                x2Gm.SetGlobalBuffer(reinterpret_cast<__gm__ int8_t *>(gmX2));
                for (uint32_t i = 0; i < kX2Sz; i++) {
                    dbgOut.SetValue(x2Off + i, static_cast<int32_t>(x2Gm.GetValue(i)));
                }
            }

            // ---- gmGroupList: int64 entries (2 int32 per entry) ----
            uint32_t glOff = x2Off + kX2Sz;
            {
                AscendC::GlobalTensor<int64_t> glGm;
                glGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t *>(gmGroupList));
                for (uint32_t i = 0; i < groupCount_; i++) {
                    int64_t val = glGm.GetValue(i);
                    dbgOut.SetValue(glOff + i * 2, static_cast<int32_t>(val & 0xFFFFFFFFULL));
                    dbgOut.SetValue(glOff + i * 2 + 1, static_cast<int32_t>((val >> 32) & 0xFFFFFFFFULL));
                }
            }

            // ---- Local quant dump: first 256 bytes from quant workspace ----
            uint32_t lqOff = glOff + groupCount_ * 2;
            {
                AscendC::GlobalTensor<uint8_t> lqGm;
                lqGm.SetGlobalBuffer((__gm__ uint8_t *)gmQuantWorkspace_);
                for (uint32_t i = 0; i < kLqSz; i++) {
                    dbgOut.SetValue(lqOff + i, static_cast<int32_t>(lqGm.GetValue(i)));
                }
            }

            // Remote quant dump: not populated (cross-NPU Scalar GetValue is unreliable).
            // gmX1 validates the remote-read path; this field is left zero-filled.
            uint32_t rqOff = lqOff + kLqSz;
        }
        AscendC::PipeBarrier<PIPE_MTE3>();
    }
    AscendC::PipeBarrier<PIPE_ALL>();
#else
    // ---- Original GmmDeq + combine ----
    MoeDistributeCombineImpl::CamMoeDistributeCombine<TemplateMC2TypeFunc> combiner;
    if (g_coreType == AscendC::AIV) {
        combiner.Init(gmGmm2DepOut, gmexpertIds_, gmExpandIdx, gmEpSendCount, nullptr, gmexpertScales_, xActiveMask_,
                      gmOutput_, workspaceGM_, nullptr, tilingData_, &epZbalContext_, &tpZbalContext_, gmCombineWs);
    }
    GmmDeq<TemplateMC2TypeFunc, Gmm2L1TileShape, Gmm2L0TileShape, Gmm2EpilogueTileShape, Gmm2BlockScheduler,
           Gmm2DispatchPolicy>(gmm2ProblemShape, groupCount_, gmGroupList, gmX2, layoutX2, gmWeight2_, layoutWeight2,
                               gmScale2_, layoutW2Scale, gmX2Scale, layoutX2Scale, gmGmm2DepOut, layoutOutput, bs_,
                               shareGmm2ProblemShape, gmShareX2, gmShareWeight2_, gmShareOutput_, gmShareWeight2Scale_,
                               gmShareX2Scale, layoutShareX2, layoutShareWeight2, layoutShareX2Scale, layoutShareOutput,
                               epRankId_, epMetaGM_, gmWorkspace, &combiner);
#endif
}
#endif // ZBAL_KERNEL_FUSED_DEEP_MOE_H
