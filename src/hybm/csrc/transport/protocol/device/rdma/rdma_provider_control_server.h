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

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rdma_provider_device.h"
#include "provider_service.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

class RdmaProviderControlServer : public ProviderService {
public:
    RdmaProviderControlServer() = default;
    ~RdmaProviderControlServer() override;

    bool Init(const ProviderServiceOptions &options) override;
    bool RegisterMemory(void *hostAddr, uint64_t size, uint64_t gvaBase, uint32_t providerRank) override;
    bool Start() override;
    void Stop() override;
    bool CleanupComplete() override;
    const std::string &GetControlUrl() const override
    {
        return controlUrl_;
    }

    Result RemoveRanks(const std::vector<uint32_t> &ranks) override;
    std::vector<uint32_t> GetConnectedProtocols(uint32_t rankId) override;
    std::vector<uint32_t> GetPeerRanks() override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return {peerRanks_.begin(), peerRanks_.end()};
    }

private:
    void AcceptLoop();
    void HandleClient(int fd, uint64_t removalGeneration);
    RdmaProviderDevice *SelectDevice(uint32_t proxyPhyId, uint32_t proxyRank);
    void RemoveDisconnectedClients();

private:
    std::vector<std::unique_ptr<RdmaProviderDevice>> devices_;
    std::unordered_map<uint32_t, RdmaProviderDevice *> phyToDevice_;
    std::mutex mutex_;
    std::mutex stopMutex_;
    int listenFd_{-1};
    int handshakeFd_{-1};
    uint32_t handshakeRank_{UINT32_MAX};
    bool handshakeCancelled_{false};
    uint64_t removalGeneration_{0};
    std::unordered_map<uint32_t, uint64_t> rankRemovalGenerations_;
    std::unordered_set<uint32_t> peerRanks_;
    std::string controlUrl_;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> memoryReady_{false};
    uint64_t size_{0};
    uint64_t gvaBase_{0};
    uint32_t providerRank_{0};
    uint32_t rankCount_{0};
    struct PeerLink {
        RdmaProviderDevice *device;
        void *qp;
        int fd;
    };
    std::unordered_map<uint32_t, PeerLink> qps_;
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
