/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifndef MEMFABRIC_OPENDCC_CONFIG_H
#define MEMFABRIC_OPENDCC_CONFIG_H
#include "mf_env_define.h"
#include "mf_env_util.h"

namespace ock {
namespace mf {
inline bool OpenDccEnabled()
{
    static const bool enabled = MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_RDMA_USE_OPENDCC, 0U) != 0;
    return enabled;
}
inline bool OpenDccHostEnabled()
{
#if defined(BUILD_OPENDCC) && defined(NO_XPU)
    return OpenDccEnabled();
#else
    return false;
#endif
}
} // namespace mf
} // namespace ock
#endif
