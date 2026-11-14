/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at http://license.coscl.org.cn/MulanPSL2
 */
#ifndef MF_HYBM_PROVIDER_SERVICE_H
#define MF_HYBM_PROVIDER_SERVICE_H

#include <string>
#include <vector>
#include "hybm_transport_manager.h"

namespace ock::mf::transport::device {
struct ProviderServiceOptions {
    std::vector<uint32_t> userDeviceIds;
    TransportOptions transport;
    uint16_t listenPort{0};
};

class ProviderService {
public:
    virtual ~ProviderService() = default;
    virtual bool Init(const ProviderServiceOptions &options) = 0;
    virtual bool RegisterMemory(void *hostAddr, uint64_t size, uint64_t gvaBase, uint32_t providerRank) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool CleanupComplete() = 0;
    virtual const std::string &GetControlUrl() const = 0;
    virtual Result RemoveRanks(const std::vector<uint32_t> &ranks) = 0;
    virtual std::vector<uint32_t> GetConnectedProtocols(uint32_t rankId) = 0;
    virtual std::vector<uint32_t> GetPeerRanks() = 0;
};
} // namespace ock::mf::transport::device
#endif
