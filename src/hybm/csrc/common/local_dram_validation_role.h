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

#ifndef MF_HYBRID_LOCAL_DRAM_VALIDATION_ROLE_H
#define MF_HYBRID_LOCAL_DRAM_VALIDATION_ROLE_H

#if defined(MF_LOCAL_DRAM_VALIDATION)

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "hybm_def.h"
#include "hybm_logger.h"

namespace ock {
namespace mf {

enum class LocalDramValidationRole { DEVICE, HOST, INVALID };

inline LocalDramValidationRole GetLocalDramValidationRole(uint32_t rankId, uint32_t protocol)
{
    const char *role = std::getenv("MF_LOCAL_DRAM_VALIDATION_ROLE");
    if (role == nullptr) {
        return LocalDramValidationRole::DEVICE;
    }
    if (std::strcmp(role, "host") != 0) {
        BM_LOG_ERROR("invalid local DRAM validation role, role=" << role << " rankId=" << rankId);
        return LocalDramValidationRole::INVALID;
    }
    if (protocol != HYBM_DOP_TYPE_HOST_DEVICE_URMA) {
        BM_LOG_ERROR("local DRAM validation requires HOST_DEVICE_URMA only, role=" << role << " rankId=" << rankId
                                                                                   << " protocol=" << protocol);
        return LocalDramValidationRole::INVALID;
    }
    const char *hostEid = std::getenv("MF_HOST_URMA_EID");
    if (hostEid == nullptr || hostEid[0] == '\0') {
        BM_LOG_ERROR("local DRAM validation host role requires MF_HOST_URMA_EID, rankId=" << rankId);
        return LocalDramValidationRole::INVALID;
    }
    return LocalDramValidationRole::HOST;
}

} // namespace mf
} // namespace ock

#endif
#endif // MF_HYBRID_LOCAL_DRAM_VALIDATION_ROLE_H
