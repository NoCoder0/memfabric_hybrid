/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file quantize_functions.h
 * \brief A5 (C310) MXFP8 quantization primitives. Copied from CANN reference
 *        implementation. Removed dependency on common.h (Ceil is provided by CANN SDK).
 */

#ifndef QUANTIZE_FUNCTIONS_H
#define QUANTIZE_FUNCTIONS_H

/* common.h dependency removed: Ceil is provided by CANN SDK kernel_operator.h
   (zbal_kernel_base.h includes kernel_operator.h before this header) */

namespace quant {

#ifdef __DAV_C310__

constexpr int DIGIT_TWO = 2;
constexpr uint16_t MAX_EXP_FOR_BF16 = 0x7f80;
constexpr uint16_t BF16_EXP_BIAS = 0x7f00;
constexpr uint16_t MAX_EXP_FOR_FP8 = 0x00ff;
constexpr uint16_t NAN_CUSTOMIZATION = 0x7f81;
constexpr uint16_t SPECIAL_EXP_THRESHOLD = 0x0040;
constexpr int16_t SHR_NUM_FOR_BF16 = 7;
constexpr uint16_t FP8_E4M3_MAX_EXP = 0x0400;
constexpr uint16_t FP8_E5M2_MAX_EXP = 0x0780;
constexpr int64_t OUT_ELE_NUM_ONE_BLK = 64;
constexpr float FP8_E5M2_MAX_VALUE = 57344.0f;
constexpr float FP8_E4M3_MAX_VALUE = 448.0f;
constexpr float HIFP8_MAX_VALUE = 32768.0f;
constexpr float INT8_MAX_VALUE = 127.0f;
constexpr uint16_t INVALID_FLOAT16 = 0x7c00;

using namespace AscendC;

__aicore__ inline constexpr uint32_t GetUbBlockSizeDispatch()
{
    return 32U;
}

__aicore__ inline constexpr uint32_t GetVRegSizeDispatch()
{
#if __CCE_AICORE__ == 310
    return AscendC::VECTOR_REG_WIDTH;
#else
    return 256U;
#endif
}

template<typename T>
__aicore__ inline void ComputeMaxExp(__ubuf__ T *srcAddr, __ubuf__ uint16_t *maxExpAddr, uint32_t totalCountInUB)
{
    uint32_t vlForHalfNumber = GetVRegSizeDispatch() / sizeof(T);
    uint16_t elementAfterReduce = GetVRegSizeDispatch() / GetUbBlockSizeDispatch();
    uint16_t loopNum = Ceil(totalCountInUB, 2 * vlForHalfNumber);

    __VEC_SCOPE__
    {
        MicroAPI::RegTensor<T> vdExp0;
        MicroAPI::RegTensor<T> vdExp1;
        MicroAPI::RegTensor<bfloat16_t> vdExp0BF16;
        MicroAPI::RegTensor<bfloat16_t> vdExp1BF16;
        MicroAPI::RegTensor<uint16_t> vdExpSelect0;
        MicroAPI::RegTensor<uint16_t> vdExpSelect1;
        MicroAPI::RegTensor<uint16_t> vdExpExtract0;
        MicroAPI::RegTensor<uint16_t> vdExpExtract1;

        MicroAPI::RegTensor<uint16_t> expMaskBF16;
        MicroAPI::Duplicate(expMaskBF16, MAX_EXP_FOR_BF16);

        MicroAPI::RegTensor<uint16_t> invalidMaskFP16;
        MicroAPI::Duplicate(invalidMaskFP16, INVALID_FLOAT16);
        MicroAPI::RegTensor<uint16_t> vdMaxExp;
        MicroAPI::MaskReg scaleMask1;
        MicroAPI::MaskReg scaleMask2;
        MicroAPI::MaskReg invalidDataMask0;
        MicroAPI::MaskReg invalidDataMask1;
        MicroAPI::UnalignReg u1;
        static constexpr MicroAPI::CastTrait castTraitHalf2Bf16 = {
            MicroAPI::RegLayout::UNKNOWN, MicroAPI::SatMode::UNKNOWN, MicroAPI::MaskMergeMode::ZEROING,
            RoundMode::CAST_TRUNC};
        for (uint16_t i = 0; i < loopNum; i++) {
            scaleMask1 = MicroAPI::UpdateMask<T>(totalCountInUB);
            scaleMask2 = MicroAPI::UpdateMask<T>(totalCountInUB);
            MicroAPI::DataCopy<T, MicroAPI::PostLiteral::POST_MODE_UPDATE, MicroAPI::LoadDist::DIST_DINTLV_B16>(
                vdExp0, vdExp1, srcAddr, vlForHalfNumber * DIGIT_TWO);
            if constexpr (Std::IsSame<T, half>::value) {
                MicroAPI::And(vdExpSelect0, (MicroAPI::RegTensor<uint16_t> &)vdExp0, invalidMaskFP16, scaleMask1);
                MicroAPI::And(vdExpSelect1, (MicroAPI::RegTensor<uint16_t> &)vdExp1, invalidMaskFP16, scaleMask1);
                MicroAPI::Compare<uint16_t, CMPMODE::NE>(invalidDataMask0, vdExpSelect0, invalidMaskFP16, scaleMask1);
                MicroAPI::Compare<uint16_t, CMPMODE::NE>(invalidDataMask1, vdExpSelect1, invalidMaskFP16, scaleMask1);
                MicroAPI::Cast<bfloat16_t, T, castTraitHalf2Bf16>(vdExp0BF16, vdExp0, scaleMask1);
                MicroAPI::Cast<bfloat16_t, T, castTraitHalf2Bf16>(vdExp1BF16, vdExp1, scaleMask1);
                MicroAPI::And(vdExpExtract0, (MicroAPI::RegTensor<uint16_t> &)vdExp0BF16, expMaskBF16, scaleMask1);
                MicroAPI::And(vdExpExtract1, (MicroAPI::RegTensor<uint16_t> &)vdExp1BF16, expMaskBF16, scaleMask1);
                MicroAPI::Select<uint16_t>(vdExpExtract0, vdExpExtract0, expMaskBF16, invalidDataMask0);
                MicroAPI::Select<uint16_t>(vdExpExtract1, vdExpExtract1, expMaskBF16, invalidDataMask1);
            } else {
                MicroAPI::And(vdExpExtract0, (MicroAPI::RegTensor<uint16_t> &)vdExp0, expMaskBF16, scaleMask1);
                MicroAPI::And(vdExpExtract1, (MicroAPI::RegTensor<uint16_t> &)vdExp1, expMaskBF16, scaleMask1);
            }
            MicroAPI::Max(vdMaxExp, vdExpExtract0, vdExpExtract1, scaleMask1);
            MicroAPI::ReduceMaxWithDataBlock(vdMaxExp, vdMaxExp, scaleMask1);

            MicroAPI::DataCopyUnAlign<uint16_t, MicroAPI::PostLiteral::POST_MODE_UPDATE>(maxExpAddr, vdMaxExp, u1,
                                                                                         elementAfterReduce);
        }
        MicroAPI::DataCopyUnAlignPost(maxExpAddr, u1, 0);
    }
}

template<typename T>
__aicore__ inline void ComputeScale(__ubuf__ uint16_t *maxExpAddr, __ubuf__ uint16_t *mxScaleLocalAddr,
                                    __ubuf__ uint16_t *halfScaleLocalAddr, uint32_t totalScaleInUB)
{
    uint32_t vlForHalfNumber = GetVRegSizeDispatch() / sizeof(uint16_t);
    uint16_t loopNumScale = Ceil(totalScaleInUB, vlForHalfNumber);
    uint16_t maxExponent;
    if constexpr (Std::IsSame<T, fp8_e4m3fn_t>::value) {
        maxExponent = FP8_E4M3_MAX_EXP;
    } else if constexpr (Std::IsSame<T, fp8_e5m2_t>::value) {
        maxExponent = FP8_E5M2_MAX_EXP;
    } else {
        maxExponent = 0;
    }

    __VEC_SCOPE__
    {
        MicroAPI::RegTensor<uint16_t> expMask, vdMaxExp;
        MicroAPI::Duplicate(expMask, MAX_EXP_FOR_BF16);
        MicroAPI::MaskReg cmpResult, zeroMask, preMaskScale;
        MicroAPI::RegTensor<uint16_t> maxExpValue;
        MicroAPI::Duplicate(maxExpValue, maxExponent);
        MicroAPI::RegTensor<uint16_t> sharedExp, scaleValue, scaleBias;
        MicroAPI::Duplicate(scaleBias, BF16_EXP_BIAS);
        MicroAPI::RegTensor<uint16_t> halfScale, fp8NanRegTensor;
        MicroAPI::Duplicate(fp8NanRegTensor, MAX_EXP_FOR_FP8);
        MicroAPI::RegTensor<uint16_t> zeroRegTensor;
        MicroAPI::Duplicate(zeroRegTensor, 0);
        MicroAPI::RegTensor<uint16_t> nanRegTensor;
        MicroAPI::Duplicate(nanRegTensor, NAN_CUSTOMIZATION);
        MicroAPI::MaskReg invalidDataMask, specialDataMask;
        MicroAPI::RegTensor<uint16_t> specialExpRegTensor;
        MicroAPI::Duplicate(specialExpRegTensor, SPECIAL_EXP_THRESHOLD);
        for (uint16_t i = 0; i < loopNumScale; i++) {
            preMaskScale = MicroAPI::UpdateMask<uint16_t>(totalScaleInUB);
            MicroAPI::DataCopy<uint16_t, MicroAPI::PostLiteral::POST_MODE_UPDATE>(vdMaxExp, maxExpAddr,
                                                                                  vlForHalfNumber);
            MicroAPI::Compare<uint16_t, CMPMODE::NE>(cmpResult, vdMaxExp, expMask, preMaskScale);
            MicroAPI::Compare<uint16_t, CMPMODE::NE>(zeroMask, vdMaxExp, zeroRegTensor, preMaskScale);
            MicroAPI::Compare<uint16_t, CMPMODE::LE>(invalidDataMask, vdMaxExp, maxExpValue, preMaskScale);
            MicroAPI::Select<uint16_t>(vdMaxExp, maxExpValue, vdMaxExp, invalidDataMask);
            MicroAPI::Sub(sharedExp, vdMaxExp, maxExpValue, preMaskScale);
            MicroAPI::ShiftRights(scaleValue, sharedExp, SHR_NUM_FOR_BF16, preMaskScale);
            MicroAPI::Select<uint16_t>(scaleValue, scaleValue, fp8NanRegTensor, cmpResult);
            MicroAPI::Select<uint16_t>(scaleValue, scaleValue, zeroRegTensor, zeroMask);

            MicroAPI::DataCopy<uint16_t, MicroAPI::PostLiteral::POST_MODE_UPDATE, MicroAPI::StoreDist::DIST_PACK_B16>(
                mxScaleLocalAddr, scaleValue, vlForHalfNumber / DIGIT_TWO, preMaskScale);

            MicroAPI::Compare<uint16_t, CMPMODE::EQ>(specialDataMask, sharedExp, scaleBias, preMaskScale);
            MicroAPI::Sub(halfScale, scaleBias, sharedExp, preMaskScale);
            MicroAPI::Select<uint16_t>(halfScale, halfScale, nanRegTensor, cmpResult);
            MicroAPI::Select<uint16_t>(halfScale, halfScale, zeroRegTensor, zeroMask);
            MicroAPI::Select<uint16_t>(halfScale, specialExpRegTensor, halfScale, specialDataMask);

            MicroAPI::DataCopy<uint16_t, MicroAPI::PostLiteral::POST_MODE_UPDATE>(halfScaleLocalAddr, halfScale,
                                                                                  vlForHalfNumber, preMaskScale);
        }
    }
}

template<typename T, typename U, RoundMode toBf16RoundMode, RoundMode roundMode>
__aicore__ inline void ComputeFp8Data(__ubuf__ T *srcAddr, __ubuf__ uint16_t *halfScaleLocalAddr,
                                      __ubuf__ int8_t *outLocalAddr, uint32_t totalCountInUB)
{
    uint32_t vlForHalfNumber = GetVRegSizeDispatch() / sizeof(T);
    uint16_t elementAfterReduce = GetVRegSizeDispatch() / GetUbBlockSizeDispatch();
    uint32_t totalCountInUB2 = totalCountInUB * DIGIT_TWO;
    uint16_t loopNum = Ceil(totalCountInUB, 2 * vlForHalfNumber);
    __VEC_SCOPE__
    {
        MicroAPI::MaskReg dataMask1;
        MicroAPI::MaskReg dataMask2;
        MicroAPI::MaskReg dataMask3;
        MicroAPI::MaskReg dataMask4;
        MicroAPI::MaskReg maskAll = MicroAPI::CreateMask<uint16_t, MicroAPI::MaskPattern::ALL>();
        MicroAPI::RegTensor<uint16_t> halfScaleForMul;
        MicroAPI::RegTensor<float> floatScaleForMul;
        MicroAPI::RegTensor<T> vdExp0;
        MicroAPI::RegTensor<T> vdExp1;

        MicroAPI::RegTensor<float> vdExp0FP32Zero;
        MicroAPI::RegTensor<float> vdExp0FP32One;
        MicroAPI::RegTensor<float> vdExp1FP32Zero;
        MicroAPI::RegTensor<float> vdExp1FP32One;
        MicroAPI::RegTensor<U> vdExp0FP8Zero;
        MicroAPI::RegTensor<U> vdExp0FP8One;
        MicroAPI::RegTensor<U> vdExp1FP8Zero;
        MicroAPI::RegTensor<U> vdExp1FP8One;
        static constexpr MicroAPI::CastTrait castTraitZero = {MicroAPI::RegLayout::ZERO, MicroAPI::SatMode::UNKNOWN,
                                                              MicroAPI::MaskMergeMode::ZEROING, RoundMode::UNKNOWN};
        static constexpr MicroAPI::CastTrait castTraitOne = {MicroAPI::RegLayout::ONE, MicroAPI::SatMode::UNKNOWN,
                                                             MicroAPI::MaskMergeMode::ZEROING, RoundMode::UNKNOWN};
        static constexpr MicroAPI::CastTrait castTrait32to8 = {MicroAPI::RegLayout::ZERO, MicroAPI::SatMode::SAT,
                                                               MicroAPI::MaskMergeMode::ZEROING, RoundMode::CAST_RINT};
        // e4m3fn/e5m2 reserve the all-ones-mantissa encoding (0x7F/0xFF) for NaN. The block
        // scale is a floor exponent, so in-block products reach up to 2x the format maximum
        // (e.g. 512 for e4m3); CAST_RINT of >= max + half-ulp rounds into the NaN encoding
        // and SatMode::SAT does not pull it back on C310, so real activations with outliers
        // ship NaN payload bytes. Clamp to the representable maximum before the cast, which
        // is what npu_dynamic_mx_quant (the GEMM-calibrated reference) effectively does.
        MicroAPI::RegTensor<float> fp8ClampHi, fp8ClampLo;
        float fp8ClampMax = FP8_E4M3_MAX_VALUE;
        if constexpr (Std::IsSame<U, fp8_e5m2_t>::value) {
            fp8ClampMax = FP8_E5M2_MAX_VALUE;
        }
        MicroAPI::Duplicate(fp8ClampHi, fp8ClampMax);
        MicroAPI::Duplicate(fp8ClampLo, -fp8ClampMax);
        for (uint16_t i = 0; i < loopNum; i++) {
            dataMask1 = MicroAPI::UpdateMask<T>(totalCountInUB);
            dataMask2 = MicroAPI::UpdateMask<T>(totalCountInUB);
            dataMask3 = MicroAPI::UpdateMask<T>(totalCountInUB2);
            dataMask4 = MicroAPI::UpdateMask<T>(totalCountInUB2);
            MicroAPI::DataCopy<T, MicroAPI::PostLiteral::POST_MODE_UPDATE, MicroAPI::LoadDist::DIST_DINTLV_B16>(
                vdExp0, vdExp1, srcAddr, vlForHalfNumber * DIGIT_TWO);
            MicroAPI::DataCopy<uint16_t, MicroAPI::PostLiteral::POST_MODE_UPDATE, MicroAPI::LoadDist::DIST_E2B_B16>(
                halfScaleForMul, halfScaleLocalAddr, elementAfterReduce);
            if constexpr (Std::IsSame<T, half>::value) {
                MicroAPI::Cast<float, T, castTraitZero>(vdExp0FP32Zero, vdExp0, dataMask1);
                MicroAPI::Cast<float, T, castTraitOne>(vdExp0FP32One, vdExp0, dataMask1);
                MicroAPI::Cast<float, bfloat16_t, castTraitZero>(
                    floatScaleForMul, (MicroAPI::RegTensor<bfloat16_t> &)halfScaleForMul, maskAll);
                MicroAPI::Mul(vdExp0FP32Zero, vdExp0FP32Zero, floatScaleForMul, dataMask3);
                MicroAPI::Mul(vdExp0FP32One, vdExp0FP32One, floatScaleForMul, dataMask4);
                MicroAPI::Interleave(vdExp0FP32Zero, vdExp0FP32One, vdExp0FP32Zero, vdExp0FP32One);
                MicroAPI::Cast<float, T, castTraitZero>(vdExp1FP32Zero, vdExp1, dataMask1);
                MicroAPI::Cast<float, T, castTraitOne>(vdExp1FP32One, vdExp1, dataMask1);
                MicroAPI::Mul(vdExp1FP32Zero, vdExp1FP32Zero, floatScaleForMul, dataMask3);
                MicroAPI::Mul(vdExp1FP32One, vdExp1FP32One, floatScaleForMul, dataMask4);
                MicroAPI::Interleave(vdExp1FP32Zero, vdExp1FP32One, vdExp1FP32Zero, vdExp1FP32One);
                MicroAPI::Interleave(vdExp0FP32Zero, vdExp1FP32Zero, vdExp0FP32Zero, vdExp1FP32Zero);
                MicroAPI::Interleave(vdExp0FP32One, vdExp1FP32One, vdExp0FP32One, vdExp1FP32One);
                MicroAPI::Min(vdExp0FP32Zero, vdExp0FP32Zero, fp8ClampHi, maskAll);
                MicroAPI::Max(vdExp0FP32Zero, vdExp0FP32Zero, fp8ClampLo, maskAll);
                MicroAPI::Min(vdExp1FP32Zero, vdExp1FP32Zero, fp8ClampHi, maskAll);
                MicroAPI::Max(vdExp1FP32Zero, vdExp1FP32Zero, fp8ClampLo, maskAll);
                MicroAPI::Min(vdExp0FP32One, vdExp0FP32One, fp8ClampHi, maskAll);
                MicroAPI::Max(vdExp0FP32One, vdExp0FP32One, fp8ClampLo, maskAll);
                MicroAPI::Min(vdExp1FP32One, vdExp1FP32One, fp8ClampHi, maskAll);
                MicroAPI::Max(vdExp1FP32One, vdExp1FP32One, fp8ClampLo, maskAll);
                MicroAPI::Cast<U, float, castTrait32to8>(vdExp0FP8Zero, vdExp0FP32Zero, dataMask3);
                MicroAPI::Cast<U, float, castTrait32to8>(vdExp0FP8One, vdExp1FP32Zero, dataMask3);
                MicroAPI::Cast<U, float, castTrait32to8>(vdExp1FP8Zero, vdExp0FP32One, dataMask4);
                MicroAPI::Cast<U, float, castTrait32to8>(vdExp1FP8One, vdExp1FP32One, dataMask4);
            } else {
                MicroAPI::Mul(vdExp0, vdExp0, (MicroAPI::RegTensor<T> &)halfScaleForMul, dataMask1);
                MicroAPI::Mul(vdExp1, vdExp1, (MicroAPI::RegTensor<T> &)halfScaleForMul, dataMask1);
                MicroAPI::Interleave(vdExp0, vdExp1, vdExp0, vdExp1);
                MicroAPI::Cast<float, T, castTraitZero>(vdExp0FP32Zero, vdExp0, dataMask1);
                MicroAPI::Cast<float, T, castTraitOne>(vdExp0FP32One, vdExp0, dataMask1);
                MicroAPI::Interleave(vdExp0FP32Zero, vdExp0FP32One, vdExp0FP32Zero, vdExp0FP32One);
                MicroAPI::Min(vdExp0FP32Zero, vdExp0FP32Zero, fp8ClampHi, maskAll);
                MicroAPI::Max(vdExp0FP32Zero, vdExp0FP32Zero, fp8ClampLo, maskAll);
                MicroAPI::Min(vdExp0FP32One, vdExp0FP32One, fp8ClampHi, maskAll);
                MicroAPI::Max(vdExp0FP32One, vdExp0FP32One, fp8ClampLo, maskAll);
                MicroAPI::Cast<U, float, castTrait32to8>(vdExp0FP8Zero, vdExp0FP32Zero, dataMask3);
                MicroAPI::Cast<U, float, castTrait32to8>(vdExp0FP8One, vdExp0FP32One, dataMask3);
                MicroAPI::Cast<float, T, castTraitZero>(vdExp1FP32Zero, vdExp1, dataMask2);
                MicroAPI::Cast<float, T, castTraitOne>(vdExp1FP32One, vdExp1, dataMask2);
                MicroAPI::Interleave(vdExp1FP32Zero, vdExp1FP32One, vdExp1FP32Zero, vdExp1FP32One);
                MicroAPI::Min(vdExp1FP32Zero, vdExp1FP32Zero, fp8ClampHi, maskAll);
                MicroAPI::Max(vdExp1FP32Zero, vdExp1FP32Zero, fp8ClampLo, maskAll);
                MicroAPI::Min(vdExp1FP32One, vdExp1FP32One, fp8ClampHi, maskAll);
                MicroAPI::Max(vdExp1FP32One, vdExp1FP32One, fp8ClampLo, maskAll);
                MicroAPI::Cast<U, float, castTrait32to8>(vdExp1FP8Zero, vdExp1FP32Zero, dataMask4);
                MicroAPI::Cast<U, float, castTrait32to8>(vdExp1FP8One, vdExp1FP32One, dataMask4);
            }
            MicroAPI::DataCopy<int8_t, MicroAPI::PostLiteral::POST_MODE_UPDATE, MicroAPI::StoreDist::DIST_PACK4_B32>(
                outLocalAddr, (MicroAPI::RegTensor<int8_t> &)vdExp0FP8Zero, OUT_ELE_NUM_ONE_BLK, dataMask3);
            MicroAPI::DataCopy<int8_t, MicroAPI::PostLiteral::POST_MODE_UPDATE, MicroAPI::StoreDist::DIST_PACK4_B32>(
                outLocalAddr, (MicroAPI::RegTensor<int8_t> &)vdExp0FP8One, OUT_ELE_NUM_ONE_BLK, dataMask3);
            MicroAPI::DataCopy<int8_t, MicroAPI::PostLiteral::POST_MODE_UPDATE, MicroAPI::StoreDist::DIST_PACK4_B32>(
                outLocalAddr, (MicroAPI::RegTensor<int8_t> &)vdExp1FP8Zero, OUT_ELE_NUM_ONE_BLK, dataMask4);
            MicroAPI::DataCopy<int8_t, MicroAPI::PostLiteral::POST_MODE_UPDATE, MicroAPI::StoreDist::DIST_PACK4_B32>(
                outLocalAddr, (MicroAPI::RegTensor<int8_t> &)vdExp1FP8One, OUT_ELE_NUM_ONE_BLK, dataMask4);
        }
    }
}

#endif // __DAV_C310__

} // namespace quant
#endif
