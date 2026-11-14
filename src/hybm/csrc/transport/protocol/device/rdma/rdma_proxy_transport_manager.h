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
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hybm_transport_manager.h"
#include "rdma_provider_device.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

class RdmaProxyTransportManager : public TransportManager {
public:
    explicit RdmaProxyTransportManager() = default;
    ~RdmaProxyTransportManager() override;

    Result OpenDevice(const TransportOptions &options) override;
    Result CloseDevice() override;
    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;
    Result UnregisterMemoryRegion(uint64_t addr) override;
    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;
    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;
    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;
    std::vector<uint32_t> GetConnectedProtocols(uint32_t rankId) const override;
    Result Prepare(const HybmTransPrepareOptions &options) override;
    Result ConnectRank(uint32_t rankId) override;
    Result RemoveRanks(const std::vector<uint32_t> &removedRanks) override;
    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;
    const std::string &GetNic() const override;
    const TransportPrivateData GetPrivateData() const override;
    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result Synchronize(uint32_t rankId) override;
    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;
    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

private:
    struct RegisterEntry {
        uint64_t hostAddr{0};
        uint64_t size{0};
        uint64_t dva{0};
        uint32_t lkey{0};
        uint32_t rkey{0};
        void *mrHandle{nullptr};
        bool ownsHostMapping{false};
        bool active{true};
    };
    struct DirectLink {
        std::string controlUrl;
        uint64_t requestedGvaBase{0};
        int fd{-1};
        void *qpHandle{nullptr};
        uint64_t npuAddrBase{0};
        uint64_t providerGvaBase{0};
        uint64_t totalSize{0};
        uint32_t rkey{0};
        uint64_t notifySourceAddr{0};
        uint32_t notifySourceRkey{0};
        bool pendingWait{false};
    };
    Result ConnectProvider(uint32_t rankId, const TransportRankPrepareInfo &info);
    bool FindRegister(uint64_t addr, RegisterEntry &entry);
    Result ReleaseRegister(RegisterEntry &entry);
    Result RemoteIO(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write, bool wait = true);
    Result RemoteBatchIO(uint32_t rankId, const CopyDescriptor &descriptor, bool write);
    std::shared_ptr<std::mutex> LinkMutexFor(uint32_t rankId);
    void InvalidateLinkLocked(uint32_t rankId);

private:
    bool closeFailed_{false};
    RdmaProviderDevice device_;
    std::string nic_;
    uint32_t rankId_{0};
    uint32_t deviceId_{0};
    mutable std::mutex mutex_;
    // 结构变更（建链/拆链/注册）与数据面互斥；数据面之间按 rank 并发。
    std::shared_mutex ioGate_;
    std::mutex operationMutex_;
    std::unordered_map<uint32_t, std::shared_ptr<std::mutex>> linkMutexes_;
    std::vector<RegisterEntry> registers_;
    std::unordered_map<uint32_t, DirectLink> links_;
    std::unordered_map<uint32_t, TransportRankPrepareInfo> preparedProviders_;
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
