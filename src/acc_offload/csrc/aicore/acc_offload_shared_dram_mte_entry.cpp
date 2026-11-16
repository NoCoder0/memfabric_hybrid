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
#include <algorithm>
#include "dl_acl_api.h"
#include "acc_offload_shared_dram_mte_entry.h"

namespace ock::offload {

int32_t AccOffloadSharedDramMteEntry::ValidateBackendConfig(const offload_config_t &config) const
{
    const auto soc = mf::DlAclApi::GetAscendSocType();
    const bool multiNode = config.localWorldSize != 0 && config.localWorldSize != config.worldSize;
    if ((soc != mf::ASCEND_910C && soc != mf::ASCEND_950) || (multiNode && soc != mf::ASCEND_910C)) {
        OFFLOAD_LOG_ERROR("unsupported shared MTE configuration, soc=" << static_cast<uint32_t>(soc)
                                                                       << " worldSize=" << config.worldSize
                                                                       << " localWorldSize=" << config.localWorldSize);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramMteEntry::RegisterMemory(const void *, uint64_t)
{
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramMteEntry::InitializePool(const offload_config_t &config)
{
    auto ret = ValidateBackendConfig(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    // Bind custom AICPU before importing A3 REMOTE_MAP mappings into the device address space.
    if (ock::mf::DlAclApi::GetAscendSocType() == ock::mf::ASCEND_910C) {
        ret = AccOffloadLaunchApi::InitializeAicpu(config.deviceId);
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_WARN("initialize A3 custom AICPU failed, deviceId: " << config.deviceId << ", ret: " << ret);
        }
    }
    ret = ResolveStoreUrl(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = CreateGroupStore(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    constexpr uint64_t gibMask = (1ULL << 30) - 1;
    options_ = {};
    options_.bmType = HYBM_TYPE_HOST_INITIATE;
    options_.memType = HYBM_MEM_TYPE_HOST;
    options_.bmDataOpType = HYBM_DOP_TYPE_MTE;
    options_.rankCount = config.worldSize;
    options_.rankId = config.rankId;
    options_.devId = config.deviceId;
    options_.maxDRAMSize = (config.reserveSize + gibMask) & ~gibMask;
    options_.hostVASpace = (config.allocSize + gibMask) & ~gibMask;
    options_.role = HYBM_ROLE_PEER;
    options_.scene = HYBM_SCENE_OFFLOAD; // offload 独立 VA 区段(常规 GVM 区最前2T)，与 BM 物理隔离
    options_.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
    options_.dramShmFd = -1;
    options_.enable56BitsGva = false;
    if (storeUrl_.size() >= sizeof(options_.transUrl)) {
        OFFLOAD_LOG_ERROR("store url too long, storeUrl: " << storeUrl_ << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    std::copy_n(storeUrl_.c_str(), storeUrl_.size(), options_.transUrl);

    ret = CreateEntity();
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = ReserveSharedPool();
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = AllocAndExportHostSlices();
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = ExchangeSliceInfos();
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = ExportAndImportEntityInfo();
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = MapPool(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    return ExchangeAndValidatePoolFingerprint(config);
}

int32_t AccOffloadSharedDramMteEntry::GetDva(uint64_t hostPtr, uint64_t *dvaPtr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (dvaPtr == nullptr) {
        OFFLOAD_LOG_ERROR("device pointer output is null, hostPtr: " << hostPtr << ", entityId: " << entityId_);
        return OFFLOAD_ERROR;
    }
    if (ValidateHostPointer(hostPtr) != OFFLOAD_OK) {
        return OFFLOAD_ERROR;
    }

    *dvaPtr = hostPtr;
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramMteEntry::SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                                 uint32_t *sizePtr, uint8_t devIdx)
{
    OFFLOAD_LOG_DEBUG("shared sparse copy, src: " << reinterpret_cast<uint64_t>(srcPtrs)
                                                  << ", dst: " << reinterpret_cast<uint64_t>(dstPtrs) << ", len: "
                                                  << reinterpret_cast<uint64_t>(lenPtrs) << ", devIdx: " << devIdx);

    return AccOffloadLaunchApi::AccOffloadSparseCopy(srcPtrs, dstPtrs, lenPtrs, sizePtr, devIdx);
}

int32_t AccOffloadSharedDramMteEntry::GroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                                    uint32_t *numLocalExpertPtr, int64_t *groupList,
                                                    int64_t *packedGroupList, uint8_t devIdx)
{
    return AccOffloadLaunchApi::AccOffloadGroupPackCopy(srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtr, groupList,
                                                        packedGroupList, devIdx);
}

int32_t AccOffloadSharedDramMteEntry::KvExchangeCopy(uint64_t *metaPtr, uint8_t devIdx)
{
    return AccOffloadLaunchApi::AccOffloadKvExchange(metaPtr, devIdx);
}

int32_t AccOffloadSharedDramMteEntry::EntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint8_t devIdx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_ || !entryTableRegistered_) {
        OFFLOAD_LOG_ERROR("entry not initialized or no table registered"
                          << ", inited: " << inited_ << ", tableRegistered: " << entryTableRegistered_
                          << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }

    AccOffloadEntryGatherLayout layout = entryTable_;
    layout.poolGva = reinterpret_cast<uint64_t>(hostGva_);
    return AccOffloadLaunchApi::AccOffloadEntryGather(dstPtr, idsPtr, countPtr, layout, devIdx);
}

} // namespace ock::offload
