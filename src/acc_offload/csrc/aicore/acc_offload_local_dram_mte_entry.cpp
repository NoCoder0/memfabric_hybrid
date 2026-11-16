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
#include "hybm_big_mem.h"
#include "acc_offload_launch.h"
#include "acc_offload_local_dram_mte_entry.h"

namespace ock::offload {

int32_t AccOffloadLocalDramMteEntry::InitializePool(const offload_config_t &config)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.memType = HYBM_MEM_TYPE_HOST;
    options.bmDataOpType = HYBM_DOP_TYPE_MTE;
    options.rankCount = 1;
    options.devId = deviceId_;
    options.maxDRAMSize = size_;
    options.hostVASpace = size_;
    options.scene = HYBM_SCENE_OFFLOAD;
    options.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
    options.dramShmFd = -1;
    options.flags |=
        (config.flags & OFFLOAD_FLAG_GIANT_PAGE) ? HYBM_FLAG_OFFLOAD_GIANT_PAGE : HYBM_FLAG_UNRESTRICTED_MEM;
    return CreateHostPool(options);
}

int32_t AccOffloadLocalDramMteEntry::GetDva(uint64_t hostPtr, uint64_t *dvaPtr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (dvaPtr == nullptr) {
        OFFLOAD_LOG_ERROR("device pointer output is null, hostPtr: " << hostPtr << ", entityId: " << entityId_);
        return OFFLOAD_ERROR;
    }
    if (ValidateHostPointer(hostPtr) != OFFLOAD_OK) {
        return OFFLOAD_ERROR;
    }
    auto ret = hybm_gva_to_va(hostPtr, HYBM_MEM_TYPE_DEVICE, dvaPtr);
    if (ret != 0) {
        OFFLOAD_LOG_ERROR("hybm_gva_to_va failed, hostPtr: 0x" << std::hex << hostPtr << ", ret: " << ret);
        return OFFLOAD_ERROR;
    }

    return OFFLOAD_OK;
}

int32_t AccOffloadLocalDramMteEntry::SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                                uint32_t *sizePtr, uint8_t devIdx)
{
    OFFLOAD_LOG_DEBUG("sparse copy, src: " << reinterpret_cast<uint64_t>(srcPtrs)
                                           << ", dst: " << reinterpret_cast<uint64_t>(dstPtrs) << ", len: "
                                           << reinterpret_cast<uint64_t>(lenPtrs) << ", devIdx: " << devIdx);

    return AccOffloadLaunchApi::AccOffloadSparseCopy(srcPtrs, dstPtrs, lenPtrs, sizePtr, devIdx);
}

int32_t AccOffloadLocalDramMteEntry::GroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                                   uint32_t *numLocalExpertPtr, int64_t *groupList,
                                                   int64_t *packedGroupList, uint8_t devIdx)
{
    return AccOffloadLaunchApi::AccOffloadGroupPackCopy(srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtr, groupList,
                                                        packedGroupList, devIdx);
}

int32_t AccOffloadLocalDramMteEntry::KvExchangeCopy(uint64_t *metaPtr, uint8_t devIdx)
{
    return AccOffloadLaunchApi::AccOffloadKvExchange(metaPtr, devIdx);
}

} // namespace ock::offload
