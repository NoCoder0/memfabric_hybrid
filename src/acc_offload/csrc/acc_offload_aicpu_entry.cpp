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
#include "hybm_device_copy.h"
#include "acc_offload_launch.h"
#include "acc_offload_aicpu_entry.h"

namespace ock::offload {

int32_t AccOffloadAicpuEntry::Initialize(const offload_config_t &config)
{
    constexpr uint64_t gibMask = (1ULL << 30) - 1;
    if (config.reserveSize == 0 || config.allocSize == 0) {
        OFFLOAD_LOG_ERROR("invalid A3 AICPU config, device=" << config.deviceId << " reserve=" << config.reserveSize
                                                             << " alloc=" << config.allocSize
                                                             << " flags=" << config.flags);
        return OFFLOAD_ERROR;
    }
    const auto size = (config.reserveSize + gibMask) & ~gibMask;
    if (size != ((config.allocSize + gibMask) & ~gibMask)) {
        OFFLOAD_LOG_ERROR("A3 AICPU pool sizes differ after GiB alignment, reserve=" << config.reserveSize
                                                                                     << " alloc=" << config.allocSize);
        return OFFLOAD_ERROR;
    }

    deviceId_ = static_cast<uint8_t>(config.deviceId);
    auto ret = hybm_init(deviceId_, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("initialize HYBM for A3 AICPU failed, device=" << config.deviceId << " ret=" << ret);
        return ret;
    }
    hybmInitialized_ = true;
    ret = AccOffloadLaunchApi::TryLoadLibrary();
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("failed to load AICPU offload launch extension, ret=" << ret);
        return ret;
    }
    launchLoaded_ = true;
    ret = CreatePool(size);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = hybm_prepare_device_copy(entity_, 0, poolBase_, poolSize_);
    if (ret == OFFLOAD_OK) {
        allocator_ = std::make_unique<AccOffloadMemManager>(reinterpret_cast<void *>(poolBase_), poolSize_);
    }
    return ret;
}

int32_t AccOffloadAicpuEntry::CreatePool(uint64_t size)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_AICPU_INITIATE;
    options.memType = HYBM_MEM_TYPE_HOST;
    options.bmDataOpType = HYBM_DOP_TYPE_AIV_SDMA;
    options.scene = HYBM_SCENE_OFFLOAD;
    options.rankCount = 1;
    options.devId = deviceId_;
    options.maxDRAMSize = size;
    options.hostVASpace = size;
    options.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
    options.dramShmFd = -1;

    entity_ = hybm_create_entity(entityId_, &options, 0);
    if (entity_ == nullptr) {
        OFFLOAD_LOG_ERROR("create A3 SDMA entity failed, entityId=" << entityId_);
        return OFFLOAD_ERROR;
    }
    auto ret = hybm_reserve_mem_space(entity_, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("reserve A3 SDMA pool failed, size=" << size << " ret=" << ret);
        return ret;
    }
    poolSize_ = size;
    slice_ = hybm_alloc_local_memory(entity_, HYBM_MEM_TYPE_HOST, size, 0);
    if (slice_ == nullptr) {
        OFFLOAD_LOG_ERROR("allocate A3 SDMA host slice failed, size=" << size);
        return OFFLOAD_ERROR;
    }
    poolBase_ = reinterpret_cast<uint64_t>(hybm_get_slice_va(entity_, slice_, 0));
    if (poolBase_ == 0) {
        OFFLOAD_LOG_ERROR("get A3 SDMA host slice address failed, entityId=" << entityId_);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

void AccOffloadAicpuEntry::UnInitialize()
{
    if (!hybmInitialized_) {
        return;
    }
    allocator_.reset();
    if (slice_ != nullptr) {
        hybm_free_local_memory(entity_, slice_, 1, 0);
        slice_ = nullptr;
    }
    if (poolSize_ != 0) {
        hybm_unreserve_mem_space(entity_, 0);
        poolSize_ = 0;
        poolBase_ = 0;
    }
    if (entity_ != nullptr) {
        hybm_destroy_entity(entity_, 0);
        entity_ = nullptr;
    }
    if (launchLoaded_) {
        AccOffloadLaunchApi::CleanupLibrary();
        launchLoaded_ = false;
    }
    hybm_uninit();
    hybmInitialized_ = false;
}

void *AccOffloadAicpuEntry::MallocHost(size_t size)
{
    return allocator_->Allocate(size);
}

void AccOffloadAicpuEntry::FreeHost(void *ptr)
{
    (void)allocator_->Release(ptr);
}

int32_t AccOffloadAicpuEntry::HostFree(uint64_t hostPtr, uint64_t)
{
    return allocator_->Release(reinterpret_cast<void *>(hostPtr));
}

int32_t AccOffloadAicpuEntry::HostRegister(uint64_t hostPtr)
{
    if (hostPtr < poolBase_ || hostPtr - poolBase_ >= poolSize_) {
        OFFLOAD_LOG_ERROR("host address outside A3 AICPU pool, address=" << hostPtr << " entityId=" << entityId_);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadAicpuEntry::GetDva(uint64_t hostPtr, uint64_t *)
{
    OFFLOAD_LOG_ERROR("A3 AICPU sparse copy uses the original host address; get_dva is unsupported, ptr=" << hostPtr);
    return OFFLOAD_FUNCTION_FAILED;
}

int32_t AccOffloadAicpuEntry::SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr,
                                         uint8_t devIdx)
{
    AccOffloadAicpuParam param{entity_, srcPtrs, dstPtrs, lenPtrs, sizePtr};
    auto callback = [](const AccOffloadAicpuParam &input, void *stream) {
        hybm_device_copy_params params{input.srcPtrs, input.dstPtrs, input.lenPtrs, input.sizePtr, 0, stream, {}};
        return hybm_data_device_copy(input.entity, &params);
    };
    return AccOffloadLaunchApi::AccOffloadAicpuLaunch(callback, param, devIdx);
}
} // namespace ock::offload
