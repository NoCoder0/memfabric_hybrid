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

#ifndef MF_HYBRID_COMPOSE_TRANSPORT_MANAGER_H
#define MF_HYBRID_COMPOSE_TRANSPORT_MANAGER_H

#include "hybm_transport_manager.h"
#include "hybm_entity_tag_info.h"

#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ock {
namespace mf {
namespace transport {

struct ComposeMemoryRegion {
    uint64_t addr = 0;
    uint64_t size = 0;
    uint32_t flags = 0;
    TransportType type = TT_BUTT;

    ComposeMemoryRegion(const uint64_t addr, const uint64_t size, TransportType type, uint32_t flags = 0)
        : addr{addr}, size{size}, flags{flags}, type{type}
    {}
};

class ComposeTransportManager : public TransportManager {
public:
    explicit ComposeTransportManager(HybmEntityTagInfoPtr tag) noexcept : tagManager_{std::move(tag)} {};

    Result OpenDevice(const TransportOptions &options) override;

    Result CloseDevice() override;

    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;

    Result UnregisterMemoryRegion(uint64_t addr) override;

    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;

    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;

    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;

    Result Prepare(const HybmTransPrepareOptions &options) override;

    Result RemoveRanks(const std::vector<uint32_t> &removedRanks) override;

    Result ConnectRank(uint32_t rankId) override;
    std::vector<uint32_t> GetConnectedProtocols(uint32_t rankId) const override;
    std::shared_ptr<TransportManager> GetTransport(uint32_t opTypes) const;

    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;
    const TransportRankPrepareInfo *GetRankPrepareInfo(uint32_t rankId) const override;

    const std::string &GetNic() const override;

    const TransportPrivateData GetPrivateData() const override;

    const void *GetQpInfo() const override;

    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result Synchronize(uint32_t rankId) override;

    Result SynchronizeRanks(const std::set<uint32_t> &rankIds) override;

    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result TransferRemoteBatchAsync(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                    const RankGroupMap &groupMap, std::vector<uint32_t> &localIndices,
                                    RankGroupMap &unregisteredGroups, std::set<uint32_t> &batchRanks) override;

private:
    Result OpenHostTransport(const TransportOptions &options);

    Result OpenDeviceTransport(const TransportOptions &options);
    Result OpenTransport(const TransportOptions &options, uint32_t opTypes);
    std::shared_ptr<TransportManager> FindTransport(uint32_t opTypes) const;
    uint32_t FindTransportProtocol(uint32_t opTypes) const;
    std::vector<std::pair<uint32_t, std::shared_ptr<TransportManager>>> GetRankTransports(uint32_t rankId) const;
    void GetTransportPrepareOptions(const HybmTransPrepareOptions &param, uint32_t protocolMask,
                                    HybmTransPrepareOptions &transportOptions);
    bool IsDeviceUrma() const;
    std::vector<uint32_t> BuildRankProtocols(uint32_t rankId) const;
    uint32_t FilterUnreachableLayers(uint32_t rankId, uint32_t opTypes) const;

private:
    std::map<uint32_t, std::shared_ptr<TransportManager>> transports_;
    mutable std::shared_mutex rankPrepareInfosMutex_;
    std::unordered_map<uint32_t, transport::TransportRankPrepareInfo> rankPrepareInfos_;
    mutable std::mutex connectedProtocolsMutex_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> connectedProtocols_;

    std::string nicInfo_;
    std::mutex mrsMutex_;
    std::map<uint64_t, ComposeMemoryRegion, std::greater<uint64_t>> mrs_;
    TransportOptions options_{};
    HybmEntityTagInfoPtr tagManager_{};
};
} // namespace transport
} // namespace mf
} // namespace ock
#endif // MF_HYBRID_COMPOSE_TRANSPORT_MANAGER_H
