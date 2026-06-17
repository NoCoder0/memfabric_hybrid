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
#ifndef FUSED_DEEP_MOE_GEMM_DISPATCH_POLICY_H
#define FUSED_DEEP_MOE_GEMM_DISPATCH_POLICY_H
#include "catlass/gemm/dispatch_policy.hpp"

namespace Catlass::Gemm {

template<uint32_t PRELOAD_STAGES_, uint32_t L1A_STAGES_, uint32_t L1B_STAGES_, uint32_t L0A_STAGES_,
         uint32_t L0B_STAGES_, uint32_t L0C_STAGES_, bool ENABLE_UNIT_FLAG_, bool ENABLE_SHUFFLE_K_>
struct MmadAtlasA2PreloadAsyncWithCallbackResidentA : public MmadAtlasA2Async {
    static constexpr uint32_t PRELOAD_STAGES = PRELOAD_STAGES_; // Stages of emitting load instruction in advance
    static constexpr uint32_t L1A_STAGES = L1A_STAGES_;
    static constexpr uint32_t L1B_STAGES = L1B_STAGES_;
    static constexpr uint32_t L0A_STAGES = L0A_STAGES_;
    static constexpr uint32_t L0B_STAGES = L0B_STAGES_;
    static constexpr uint32_t L0C_STAGES = L0C_STAGES_;
    static constexpr bool ENABLE_UNIT_FLAG = ENABLE_UNIT_FLAG_;
    static constexpr bool ENABLE_SHUFFLE_K = ENABLE_SHUFFLE_K_;
};

} // namespace Catlass::Gemm

#endif // FUSED_DEEP_MOE_GEMM_DISPATCH_POLICY_H
