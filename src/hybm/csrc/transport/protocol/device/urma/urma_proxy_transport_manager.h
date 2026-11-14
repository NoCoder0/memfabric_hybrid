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
#include "urma_provider_device.h"
#include "urma_provider_protocol.h"
#include <thread>
#include <mutex>
namespace ock::mf::transport::device {
class UrmaProviderContext;

class UrmaProxyTransportManager final : public TransportManager {
public:
    ~UrmaProxyTransportManager() override;
    Result OpenDevice(const TransportOptions &options) override;
    Result CloseDevice() override;
    size_t CurrentThreadPendingCount() const
    {
        return device_->Transport().CurrentThreadPendingCount();
    }
    bool CleanupComplete() const
    {
        return closed_ && device_->CleanupComplete();
    }
    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;
    Result UnregisterMemoryRegion(uint64_t addr) override;
    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;
    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;
    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;
    Result Prepare(const HybmTransPrepareOptions &options) override;
    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;
    Result ConnectRank(uint32_t rank) override;
    Result RemoveRanks(const std::vector<uint32_t> &ranks) override;
    std::vector<uint32_t> GetConnectedProtocols(uint32_t rank) const override;
    const std::string &GetNic() const override;
    const TransportPrivateData GetPrivateData() const override;
    Result ReadRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result WriteRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result ReadRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result WriteRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result ReadRemoteBatchAsync(uint32_t rank, const CopyDescriptor &descriptor) override;
    Result WriteRemoteBatchAsync(uint32_t rank, const CopyDescriptor &descriptor) override;
    Result Synchronize(uint32_t rank) override;
    Result SynchronizeRanks(const std::set<uint32_t> &ranks) override;
    Result TransferRemoteBatchAsync(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                    const RankGroupMap &groups, std::vector<uint32_t> &local,
                                    RankGroupMap &unregistered, std::set<uint32_t> &batchRanks) override;

private:
    struct Link {
        ~Link();
        int fd{-1};
        std::atomic<bool> ready{false};
        std::atomic<bool> draining{false};
        std::atomic<bool> quiesced{false};
        std::atomic<bool> failed{false};
    };
    Result Drain(uint32_t rank, const std::shared_ptr<Link> &link, ProviderDeadline deadline);
    std::vector<uint32_t> QueryConnectedProtocols(uint32_t rank, const UrmaProviderContext *activeContext) const;
    void ControlLoop();
    std::shared_ptr<UrmaProviderDevice> device_{std::make_shared<UrmaProviderDevice>()};
    TransportOptions options_{};
    mutable std::mutex mutex_;
    std::mutex operationMutex_;
    std::unordered_map<uint32_t, TransportRankPrepareInfo> providers_;
    std::unordered_map<uint32_t, std::shared_ptr<Link>> links_;
    std::atomic<bool> running_{false};
    std::thread controlThread_;
    bool closed_{false};
};
} // namespace ock::mf::transport::device
