/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#ifndef MF_HOST_RDMA_HCOMM_TRANSPORT_MANAGER_H
#define MF_HOST_RDMA_HCOMM_TRANSPORT_MANAGER_H
#include <mutex>
#include <unordered_map>
#include "hybm_transport_manager.h"
#include "hcomm_transport_layout.h"
namespace ock {
namespace mf {
namespace transport {
namespace host {
class HostRdmaHcommTransportManager final : public TransportManager {
public:
    ~HostRdmaHcommTransportManager() override;
    bool IsHcomm() const override
    {
        return true;
    }

    Result OpenDevice(const TransportOptions &options) override;
    Result CloseDevice() override;
    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;
    Result UnregisterMemoryRegion(uint64_t addr) override;
    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;
    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;
    Result QueryControlKey(TransportMemoryKey &key) override;
    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;
    Result Prepare(const HybmTransPrepareOptions &options) override;
    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;
    Result RemoveRanks(const std::vector<uint32_t> &ranks) override;
    Result ConnectRank(uint32_t rankId) override;
    const std::string &GetNic() const override;
    const TransportPrivateData GetPrivateData() const override;
    Result ReadRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result WriteRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result ReadRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result WriteRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result ReadRemoteBatchAsync(uint32_t rank, const CopyDescriptor &desc) override;
    Result WriteRemoteBatchAsync(uint32_t rank, const CopyDescriptor &desc) override;
    Result Synchronize(uint32_t rank) override;

private:
    struct LocalMemEntry {
        HcommMemHandle handle{nullptr};
        uint64_t size{0};
        uint32_t refCount{1};
    };
    struct RemoteRankState : device::HcommPeerMemory {
        ChannelHandle channel{0};
        std::string channelName;
    };
    Result InitTransferFlagLocked();
    Result EnsureChannelLocked(uint32_t rank, RemoteRankState &state);
    Result CleanupRankLocked(uint32_t rank);
    Result FenceLocked(uint32_t rank);
    Result QuiesceLocked();
    Result CheckLocalMemoryMutableLocked() const;
    Result ResolveAddressesLocked(const RemoteRankState &state, uint64_t local, uint64_t remote, uint64_t size,
                                  uint64_t &localAddr, uint64_t &remoteAddr) const;
    Result RemoteIo(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size, bool write);
    Result RemoteIoBatch(uint32_t rank, const CopyDescriptor &desc, bool write);
    Result SubmitOneLocked(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size, bool write);
    mutable std::mutex mutex_;
    bool opened_{false};
    uint32_t rankId_{0};
    uint32_t rankCount_{0};
    std::string localNic_;
    uint16_t localListenPort_{0};
    EndpointDesc localEndpointDesc_{};
    EndpointHandle endpoint_{nullptr};
    void *localFlagPtr_{nullptr};
    HcommMemHandle localFlagHandle_{nullptr};
    std::vector<uint8_t> localFlagExportDesc_;
    std::unordered_map<uint64_t, LocalMemEntry> localRegistrations_;
    std::unordered_map<uint32_t, RemoteRankState> remoteRanks_;
    device::HcommApiWrapper api_;
};
} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock
#endif
