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

#pragma once

#include "device_urma_transport_manager.h"

namespace ock::mf::transport::device {
class UrmaProviderDevice {
public:
    UrmaProviderDevice();
    ~UrmaProviderDevice();
    UrmaProviderDevice(const UrmaProviderDevice &) = delete;
    UrmaProviderDevice &operator=(const UrmaProviderDevice &) = delete;
    Result Open(uint32_t userDeviceId, const TransportOptions &options, bool passive);
    bool RegisterMemory(void *hostAddr, uint64_t size);
    Result Close();
    bool CleanupComplete() const;
    DeviceUrmaTransportManager &Transport()
    {
        return *manager_;
    }
    const DeviceUrmaTransportManager &Transport() const
    {
        return *manager_;
    }
    uint32_t GetPhysicalId() const
    {
        return phyId_;
    }
    void *GetContext() const
    {
        return context_;
    }
    bool IsMemoryRegistered() const
    {
        return registered_;
    }

private:
    std::shared_ptr<DeviceUrmaTransportManager> manager_;
    void *context_{nullptr};
    uint32_t logicId_{0};
    uint32_t phyId_{0};
    void *mapped_{nullptr};
    void *hostAddr_{nullptr};
    bool opened_{false};
    bool registered_{false};
    bool closeFailed_{false};
};
} // namespace ock::mf::transport::device
