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

#include "urma_provider_device.h"
#include "provider_device_context.h"
#include "vendor/ascend/dl_hal_api.h"

namespace ock::mf::transport::device {
UrmaProviderDevice::UrmaProviderDevice() : manager_(std::make_shared<DeviceUrmaTransportManager>()) {}

UrmaProviderDevice::~UrmaProviderDevice()
{
    if (Close() == BM_OK) {
        return;
    }
    static auto *retained = new std::vector<std::shared_ptr<DeviceUrmaTransportManager>>();
    static auto *retentionMutex = new std::mutex();
    std::lock_guard<std::mutex> lock(*retentionMutex);
    retained->push_back(std::move(manager_));
    BM_LOG_ERROR("URMA provider device retained resources after cleanup failure, phyId=" << phyId_);
}

Result UrmaProviderDevice::Open(uint32_t userDeviceId, const TransportOptions &options, bool passive)
{
    if (opened_ || !CleanupComplete()) {
        BM_LOG_ERROR("URMA provider device cannot reopen before cleanup, userId=" << userDeviceId);
        return BM_ERROR;
    }
    UrmaProviderContext restore;
    auto deviceOptions = options;
    deviceOptions.protocol = HYBM_DOP_TYPE_DEVICE_URMA;
    manager_->EnableStandaloneLifecycle(passive);
    auto ret = manager_->OpenDeviceOnUserDevice(deviceOptions, userDeviceId);
    auto contextRet = VendorApi::Api()->GetCurrentContext(&context_);
    if (ret != BM_OK) {
        return ret;
    }
    int32_t logic = -1;
    if (contextRet != BM_OK ||
        VendorApi::Api()->GetLogicDevIdByUserDevId(static_cast<int32_t>(userDeviceId), &logic) != BM_OK || logic < 0) {
        BM_LOG_ERROR("URMA provider device cannot capture context or logical device, userId=" << userDeviceId);
        return BM_ERROR;
    }
    logicId_ = static_cast<uint32_t>(logic);
    phyId_ = manager_->GetPhysicalDeviceId();
    opened_ = true;
    return BM_OK;
}

bool UrmaProviderDevice::RegisterMemory(void *hostAddr, uint64_t size)
{
    if (!opened_ || hostAddr == nullptr || size == 0 || hostAddr_ != nullptr) {
        BM_LOG_ERROR("URMA provider device invalid pool registration, phyId=" << phyId_ << " size=" << size);
        return false;
    }
    UrmaProviderContext context(context_);
    if (!context.Valid()) {
        return false;
    }
    hostAddr_ = hostAddr;
    if (DlHalApi::HalHostRegister(hostAddr, size, HOST_MEM_MAP_DEV, logicId_, &mapped_) != BM_OK) {
        BM_LOG_ERROR("URMA provider device mapping failed, phyId=" << phyId_);
        return false;
    }
    TransportMemoryRegion mr{};
    mr.addr = reinterpret_cast<uint64_t>(hostAddr);
    mr.size = size;
    mr.flags = REG_MR_FLAG_DRAM;
    if (manager_->RegisterMappedHostMemory(mr, reinterpret_cast<uint64_t>(mapped_)) != BM_OK) {
        return false;
    }
    registered_ = true;
    return true;
}

Result UrmaProviderDevice::Close()
{
    if (closeFailed_) {
        return BM_ERROR;
    }
    if (!manager_->CleanupComplete()) {
        UrmaProviderContext context(context_);
        if (!context.Valid()) {
            return BM_ERROR;
        }
        auto ret = manager_->CloseDevice();
        if (ret != BM_OK || !manager_->CleanupComplete()) {
            return ret != BM_OK ? ret : BM_ERROR;
        }
    }
    registered_ = false;
    if (mapped_ != nullptr) {
        UrmaProviderContext context(context_);
        if (!context.Valid()) {
            return BM_ERROR;
        }
        auto ret = DlHalApi::HalHostUnregisterEx(hostAddr_, logicId_, HOST_MEM_MAP_DEV);
        if (ret != BM_OK) {
            closeFailed_ = true;
            BM_LOG_ERROR("URMA provider device unmap failed, logicId=" << logicId_ << " ret=" << ret);
            return ret;
        }
        mapped_ = nullptr;
    }
    hostAddr_ = nullptr;
    opened_ = false;
    return BM_OK;
}

bool UrmaProviderDevice::CleanupComplete() const
{
    return mapped_ == nullptr && manager_->CleanupComplete();
}
} // namespace ock::mf::transport::device
