/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
/**
 * @file hcc.cpp
 * @brief Implementation of the public HCC lifecycle and logging APIs.
 */

#include "hcc.h"
#include "common/hcc_common_includes.h"
#include "genetty.h"

namespace ock {
namespace openDCC {
#ifdef __cplusplus
extern "C" {
#endif

void HCC_API hcc_set_log_func(void (*logger)(int32_t level, const char *msg))
{
    /* step1: check param */
    HCC_ASSERT_MSG_NO_RETURN(logger != nullptr, "hcc set log func failed as logger is null");

    /* step2: call out logger api to set */
    ock::mf::OutLogger::Instance().SetExternalLogFunction(logger, true);

    /* step3: call genetty api to set */
    ::genetty_set_log_func(logger);
}

void HCC_API hcc_set_log_level(int32_t level)
{
    /* step1: call out logger api to set */
    if (!ock::mf::OutLogger::ValidateLevel(level)) {
        HCC_LOG_ERROR("hcc set log level failed as level(" << level << ") is invalid");
        return;
    }
    ock::mf::OutLogger::Instance().SetLogLevel(static_cast<ock::mf::LogLevel>(level));

    /* step2: call genetty api to set */
    ::genetty_set_log_level(level);
}

int32_t HCC_API hcc_memory_info_encode(hcc_memory_segment_t *segment, uint32_t flags, hcc_memory_info_t *info)
{
    auto ret = ::genetty_memory_info_encode(segment, flags, info);
    return ret == GEN_OK ? HCC_OK : HCC_INVALID_PARAM;
}

int32_t HCC_API hcc_memory_info_decode(hcc_memory_info_t *info, hcc_memory_segment_t *segment, uint32_t flags)
{
    auto ret = ::genetty_memory_info_decode(info, segment, flags);
    return ret == GEN_OK ? HCC_OK : HCC_INVALID_PARAM;
}

#ifdef __cplusplus
}
#endif
} // namespace openDCC
} // namespace ock
