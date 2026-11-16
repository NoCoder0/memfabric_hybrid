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
#include "acc_offload_local_dram_entry.h"

namespace ock {
namespace offload {

using namespace ock::mf;

constexpr uint64_t KB = 1024ULL;
constexpr uint64_t MB = KB * 1024ULL;
constexpr uint64_t GB = MB * 1024ULL;

static uint64_t AlignUp(uint64_t value, uint64_t align) noexcept
{
    return (value + align - 1) & ~(align - 1);
}

int32_t AccOffloadLocalDramEntry::Initialize(const offload_config_t &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        return OFFLOAD_OK;
    }
    const auto reserveSize = AlignUp(config.reserveSize, GB);
    const auto allocSize = AlignUp(config.allocSize, GB);
    if (reserveSize != allocSize) {
        OFFLOAD_LOG_ERROR("local dram requires equal aligned pool sizes, reserveSize: "
                          << config.reserveSize << ", allocSize: " << config.allocSize);
        return OFFLOAD_ERROR;
    }
    auto ret = hybm_init(config.deviceId, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("initialize local HYBM failed, deviceId: " << config.deviceId << ", ret: " << ret);
        return ret;
    }
    hybmInitialized_ = true;
    deviceId_ = config.deviceId;
    size_ = reserveSize;
    ret = AccOffloadLaunchApi::TryLoadLibrary();
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("load local offload launch library failed, deviceId: " << config.deviceId
                                                                                 << ", ret: " << ret);
        Cleanup();
        return ret;
    }
    launchLoaded_ = true;
    ret = InitializePool(config);
    if (ret != OFFLOAD_OK) {
        Cleanup();
        return ret;
    }
    memMng_ = std::make_shared<AccOffloadMemManager>(base_, size_);
    inited_ = true;
    return OFFLOAD_OK;
}

int32_t AccOffloadLocalDramEntry::CreateHostPool(const hybm_options &options)
{
    const auto size = options.maxDRAMSize;
    entity_ = hybm_create_entity(entityId_, &options, 0);
    if (entity_ == nullptr) {
        OFFLOAD_LOG_ERROR("create local host entity failed, entityId=" << entityId_);
        return OFFLOAD_ERROR;
    }
    auto ret = hybm_reserve_mem_space(entity_, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("reserve local pool failed, size=" << size << " ret=" << ret);
        return ret;
    }
    poolReserved_ = true;
    slice_ = hybm_alloc_local_memory(entity_, HYBM_MEM_TYPE_HOST, size, 0);
    if (slice_ == nullptr) {
        OFFLOAD_LOG_ERROR("allocate local host slice failed, size=" << size);
        return OFFLOAD_ERROR;
    }
    base_ = reinterpret_cast<uint8_t *>(hybm_get_slice_va(entity_, slice_, 0));
    if (base_ == nullptr) {
        OFFLOAD_LOG_ERROR("get local host slice address failed, entityId=" << entityId_);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

void AccOffloadLocalDramEntry::UnInitialize()
{
    std::lock_guard<std::mutex> lock(mutex_);
    Cleanup();
}

void AccOffloadLocalDramEntry::Cleanup()
{
    memMng_.reset();
    ReleaseBackendResources();
    if (slice_ != nullptr) {
        hybm_free_local_memory(entity_, slice_, 1, 0);
        slice_ = nullptr;
    }
    if (poolReserved_) {
        hybm_unreserve_mem_space(entity_, 0);
        poolReserved_ = false;
    }
    if (entity_ != nullptr) {
        hybm_destroy_entity(entity_, 0);
        entity_ = nullptr;
    }
    if (launchLoaded_) {
        AccOffloadLaunchApi::CleanupLibrary();
        launchLoaded_ = false;
    }
    if (hybmInitialized_) {
        hybm_uninit();
        hybmInitialized_ = false;
    }
    base_ = nullptr;
    size_ = 0;
    inited_ = false;
}

void *AccOffloadLocalDramEntry::MallocHost(size_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_ || memMng_ == nullptr) {
        OFFLOAD_LOG_ERROR("pool not initialized, malloc failed, size (bytes): " << size << ", entityId: " << entityId_);
        return nullptr;
    }
    return memMng_->Allocate(size);
}

void AccOffloadLocalDramEntry::FreeHost(void *ptr)
{
    (void)HostFree(reinterpret_cast<uint64_t>(ptr), 0);
}

int32_t AccOffloadLocalDramEntry::HostFree(uint64_t hostPtr, uint64_t flags)
{
    (void)flags;
    std::lock_guard<std::mutex> lock(mutex_);
    if (ValidateHostPointer(hostPtr) != OFFLOAD_OK) {
        return OFFLOAD_ERROR;
    }
    auto ret = memMng_->Release(reinterpret_cast<void *>(hostPtr));
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("release host block failed, hostPtr: " << hostPtr << ", entityId: " << entityId_
                                                                 << ", ret: " << ret);
    }
    return ret;
}

int32_t AccOffloadLocalDramEntry::ValidateHostPointer(uint64_t hostPtr) const
{
    if (!inited_ || memMng_ == nullptr) {
        OFFLOAD_LOG_ERROR("pool not initialized, hostPtr: " << hostPtr << ", entityId: " << entityId_);
        return OFFLOAD_ERROR;
    }
    const auto base = reinterpret_cast<uint64_t>(base_);
    if (base_ == nullptr || hostPtr < base || hostPtr - base >= size_) {
        OFFLOAD_LOG_ERROR("host pointer outside pool, hostPtr: " << hostPtr << ", poolBase: " << base
                                                                 << ", poolSize (bytes): " << size_
                                                                 << ", entityId: " << entityId_);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadLocalDramEntry::HostRegister(uint64_t hostPtr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // The whole pool is already mapped; no per-block registration is needed.
    return ValidateHostPointer(hostPtr);
}

} // namespace offload
} // namespace ock
