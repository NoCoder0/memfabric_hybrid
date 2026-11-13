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
#include "hybm_device_copy.h"
#include "hybm_entity.h"
#include "hybm_logger.h"

using namespace ock::mf;

HYBM_API int32_t hybm_prepare_device_copy(hybm_entity_t e, uint32_t rankId, uint64_t poolBase, uint64_t poolSize)
{
    if (e == nullptr || poolBase == 0 || poolSize == 0) {
        BM_LOG_ERROR("invalid device copy pool, rankId: " << rankId << " base: " << poolBase << " size: " << poolSize);
        return BM_INVALID_PARAM;
    }
    return static_cast<MemEntity *>(e)->PrepareDeviceCopy(rankId, poolBase, poolSize);
}

HYBM_API int32_t hybm_data_device_copy(hybm_entity_t e, const hybm_device_copy_params *params)
{
    if (e == nullptr || params == nullptr || params->stream == nullptr || params->srcPtrs == nullptr ||
        params->dstPtrs == nullptr || params->lenPtrs == nullptr || params->sizePtr == nullptr) {
        return BM_INVALID_PARAM;
    }
    const auto addresses = reinterpret_cast<uintptr_t>(params->srcPtrs) | reinterpret_cast<uintptr_t>(params->dstPtrs);
    const auto sizes = reinterpret_cast<uintptr_t>(params->lenPtrs) | reinterpret_cast<uintptr_t>(params->sizePtr);
    if (addresses % alignof(uint64_t) != 0 || sizes % alignof(uint32_t) != 0) {
        BM_LOG_ERROR("device copy metadata pointers are misaligned, rankId: " << params->rankId);
        return BM_INVALID_PARAM;
    }
    // The caller selects the device before submission; do not initialize it during capture.
    return static_cast<MemEntity *>(e)->DeviceCopy(*params);
}
