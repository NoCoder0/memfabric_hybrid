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
#ifndef FUSED_DEEP_MOE_EPILOGUE_DISPATCH_POLICY_H
#define FUSED_DEEP_MOE_EPILOGUE_DISPATCH_POLICY_H
#include "catlass/epilogue/dispatch_policy.hpp"

namespace Catlass::Epilogue {

template<uint32_t UB_STAGES_, uint32_t EXEC_FLAG_>
struct EpilogueAtlasA2PerTokenDequantSwiglu {
    using ArchTag = Arch::AtlasA2;
    static constexpr uint32_t UB_STAGES = UB_STAGES_;
    static constexpr uint32_t EXEC_FLAG = EXEC_FLAG_;
};

template<uint32_t UB_STAGES_, uint32_t EXEC_FLAG_>
struct EpilogueAtlasA2PerTokenDequantCombine {
    using ArchTag = Arch::AtlasA2;
    static constexpr uint32_t UB_STAGES = UB_STAGES_;
    static constexpr uint32_t EXEC_FLAG = EXEC_FLAG_;
};

} // namespace Catlass::Epilogue

#endif // FUSED_DEEP_MOE_EPILOGUE_DISPATCH_POLICY_H
