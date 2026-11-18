/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of the License at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef MF_HYBRID_HOST_URMA_TRANSPORT_MANAGER_H
#define MF_HYBRID_HOST_URMA_TRANSPORT_MANAGER_H

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vendor/ascend/dl_hcomm_api.h"
#include "hcomm_api_wrapper.h"
#include "hybm_transport_manager.h"
#include "hybm_urma_common.h"

namespace ock {
namespace mf {
namespace transport {
namespace host {

using namespace ock::mf::transport::urma;

using device::HcommApiWrapper;
using device::HcommEndpointHandle;
using device::HcommMemHandleType;

class HostUrmaTransportManager final : public transport::TransportManager {
public:
    HostUrmaTransportManager() = default;
    ~HostUrmaTransportManager() override;

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
    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;

    const std::string &GetNic() const override;
    const TransportPrivateData GetPrivateData() const override;

    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;
    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;
    Result Synchronize(uint32_t rankId) override;

private:
    struct LocalRegistration {
        TransportMemoryRegion mr{};
        HcommMemHandleType handle{nullptr};
        UrmaMemTag memTag{0};
        uint64_t exportedGva{0};
        uint32_t refCount{0};
    };

    struct RemoteRankState {
        UrmaEndpointDesc endpointDesc{};
        HcommChannelDesc channelDesc{};
        ChannelHandle channel{0};
        std::string channelName;
    };

    Result BuildLocalHostEndpointDescLocked(UrmaEndpointDesc &endpoint) const;
    Result ResolvePhysicalDeviceIdLocked();
    Result InitHostTransferFlagLocked();

    Result ResolveExportedGvaLocked(const TransportMemoryRegion &mr, uint64_t &exportedGva) const;

    Result PreparePeerLocked(uint32_t peerRank, RemoteRankState &state);
    Result ValidateDevicePeerMemoryKeysLocked(uint32_t peerRank, const std::vector<TransportMemoryKey> &memKeys) const;
    Result DestroyRemoteChannelLocked(uint32_t peerRank, RemoteRankState &state);
    Result RemoveRankLocked(uint32_t peerRank);

    mutable std::mutex mutex_{};
    bool opened_{false};
    uint32_t rankId_{0};
    uint32_t phyDeviceId_{0};
    TransportOptions options_{};
    HcommApiWrapper hcommApi_{};
    HcommEndpointHandle localEndpoint_{nullptr};
    UrmaEndpointDesc localEndpointDesc_{};

    void *hostTransFlagPtr_{nullptr};
    uint64_t hostTransFlagSize_{0};
    HcommMemHandleType hostTransFlagHcommHandle_{nullptr};

    std::map<uint64_t, LocalRegistration> localRegistrations_{};
    std::unordered_map<uint32_t, RemoteRankState> remoteRanks_{};
};

} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HOST_URMA_TRANSPORT_MANAGER_H
