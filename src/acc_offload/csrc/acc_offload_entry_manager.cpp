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
#include "acc_offload_entry_manager.h"
#include "acc_offload_local_dram_entry.h"
#include "acc_offload_shared_dram_entry.h"
#include "acc_offload_define.h"

namespace ock {
namespace offload {

std::atomic<uint16_t> AccOffloadEntryManager::nextEntityId_{0};

AccOffloadEntryManager &AccOffloadEntryManager::Instance()
{
    static AccOffloadEntryManager instance;
    return instance;
}

AccOffloadEntryManager::~AccOffloadEntryManager()
{
    UnInitialize();
}

int32_t AccOffloadEntryManager::Initialize(const offload_config_t &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        OFFLOAD_LOG_WARN("entry manager already initialized, scene: " << static_cast<uint32_t>(scene_));
        return OFFLOAD_OK;
    }

    auto ret = CreateEntry(config.scene);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    scene_ = config.scene;

    ret = entry_->Initialize(config);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("entry initialize failed, scene: " << static_cast<uint32_t>(scene_)
                                                             << ", entityId: " << entityId_ << ", ret: " << ret);
        entry_->UnInitialize();
        entry_.reset();
        scene_ = OFFLOAD_SCENE_LOCAL;
        return ret;
    }

    inited_ = true;
    OFFLOAD_LOG_INFO("entry manager initialized, scene: "
                     << static_cast<uint32_t>(scene_) << ", deviceId: " << config.deviceId << ", reserveSize: "
                     << config.reserveSize << ", allocSize: " << config.allocSize << ", worldSize: " << config.worldSize
                     << ", rankId: " << config.rankId << ", entityId: " << entityId_);
    return OFFLOAD_OK;
}

int32_t AccOffloadEntryManager::CreateEntry(offload_scene_t scene)
{
    auto id = nextEntityId_++;
    if (id >= HYBM_ENTITY_ID_SEGMENT_SIZE) {
        OFFLOAD_LOG_ERROR("offload entity IDs exhausted, scene: " << static_cast<uint32_t>(scene) << ", id: " << id
                                                                  << ", limit: " << HYBM_ENTITY_ID_SEGMENT_SIZE);
        return OFFLOAD_ERROR;
    }
    entityId_ = HYBM_ENTITY_ID_OFFLOAD_BASE + id;
    if (scene == OFFLOAD_SCENE_LOCAL) {
        entry_ = std::make_unique<AccOffloadLocalDramEntry>(entityId_);
    } else if (scene == OFFLOAD_SCENE_SHARED) {
        entry_ = std::make_unique<AccOffloadSharedDramEntry>(entityId_);
    } else {
        OFFLOAD_LOG_ERROR("invalid scene: " << static_cast<uint32_t>(scene));
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

void AccOffloadEntryManager::UnInitialize()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        return;
    }

    if (entry_ != nullptr) {
        entry_->UnInitialize();
    }
    entry_.reset();
    inited_ = false;
    OFFLOAD_LOG_INFO("entry manager uninitialized, scene: " << static_cast<uint32_t>(scene_)
                                                            << ", entityId: " << entityId_);
}

void *AccOffloadEntryManager::MallocHost(size_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, malloc failed, size: " << size);
        return nullptr;
    }
    return entry_->MallocHost(size);
}

void AccOffloadEntryManager::FreeHost(void *ptr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr || ptr == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null or ptr is null, free failed");
        return;
    }
    entry_->FreeHost(ptr);
}

int32_t AccOffloadEntryManager::GetDva(uint64_t hostPtr, uint64_t *dvaPtr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr || dvaPtr == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null or dvaPtr is null, get dva failed, hostPtr: " << hostPtr);
        return OFFLOAD_ERROR;
    }
    return entry_->GetDva(hostPtr, dvaPtr);
}

int32_t AccOffloadEntryManager::SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr,
                                           uint8_t devIdx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, sparse copy failed");
        return OFFLOAD_ERROR;
    }
    return entry_->SparseCopy(srcPtrs, dstPtrs, lenPtrs, sizePtr, devIdx);
}

int32_t AccOffloadEntryManager::GroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                              uint32_t *numLocalExpertPtr, int64_t *groupList, int64_t *packedGroupList,
                                              uint8_t devIdx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, group pack copy failed");
        return OFFLOAD_ERROR;
    }
    return entry_->GroupPackCopy(srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtr, groupList, packedGroupList, devIdx);
}

int32_t AccOffloadEntryManager::KvExchangeCopy(uint64_t *metaPtr, uint8_t devIdx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, kv exchange copy failed");
        return OFFLOAD_ERROR;
    }
    return entry_->KvExchangeCopy(metaPtr, devIdx);
}

int32_t AccOffloadEntryManager::RegisterEntryTable(uint32_t entryBytes, uint32_t rowsPerSlot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, register entry table failed");
        return OFFLOAD_ERROR;
    }
    if (scene_ != OFFLOAD_SCENE_SHARED) {
        OFFLOAD_LOG_ERROR("entry table only supported in SHARED scene, scene: " << static_cast<uint32_t>(scene_));
        return OFFLOAD_ERROR;
    }
    return entry_->RegisterEntryTable(entryBytes, rowsPerSlot);
}

int32_t AccOffloadEntryManager::EntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint8_t devIdx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, entry gather failed");
        return OFFLOAD_ERROR;
    }
    if (scene_ != OFFLOAD_SCENE_SHARED) {
        OFFLOAD_LOG_ERROR("entry gather only supported in SHARED scene, scene: " << static_cast<uint32_t>(scene_)
                                                                                 << ", deviceId: " << devIdx);
        return OFFLOAD_ERROR;
    }
    return entry_->EntryGather(dstPtr, idsPtr, countPtr, devIdx);
}

uint64_t AccOffloadEntryManager::HostMalloc(uint64_t size, uint64_t flags)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, host malloc failed, size: " << size);
        return 0;
    }
    return entry_->HostMalloc(size, flags);
}

int32_t AccOffloadEntryManager::HostRegister(uint64_t hostPtr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, host register failed, hostPtr: " << hostPtr);
        return OFFLOAD_ERROR;
    }
    return entry_->HostRegister(hostPtr);
}

int32_t AccOffloadEntryManager::HostGetDevicePtr(uint64_t hostPtr, uint64_t *dvaPtr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr || dvaPtr == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null or dvaPtr is null, host get device ptr failed, hostPtr: " << hostPtr);
        return OFFLOAD_ERROR;
    }
    return entry_->HostGetDevicePtr(hostPtr, dvaPtr);
}

int32_t AccOffloadEntryManager::HostFree(uint64_t hostPtr, uint64_t flags)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry_ == nullptr) {
        OFFLOAD_LOG_ERROR("entry is null, host free failed, hostPtr: " << hostPtr);
        return OFFLOAD_ERROR;
    }
    return entry_->HostFree(hostPtr, flags);
}

} // namespace offload
} // namespace ock
