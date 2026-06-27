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

#ifndef MF_HYBRID_DEVICE_URMA_TRANSPORT_MANAGER_H
#define MF_HYBRID_DEVICE_URMA_TRANSPORT_MANAGER_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dl_hcomm_api.h"
#include "dl_rt_api.h"
#include "hcomm_transport_manager.h"
#include "hybm_transport_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

class DeviceUrmaTransportManager final : public transport::TransportManager {
public:
    DeviceUrmaTransportManager() = default;

    ~DeviceUrmaTransportManager() override;

    // Initialization/open/close lifecycle
    Result OpenDevice(const TransportOptions &options) override;

    Result CloseDevice() override;

    // Local/remote registration and key export/import
    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;

    Result UnregisterMemoryRegion(uint64_t addr) override;

    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;

    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;

    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;

    // Prepare/rank/connection/private data
    Result Prepare(const HybmTransPrepareOptions &options) override;

    Result RemoveRanks(const std::vector<uint32_t> &removedRanks) override;

    Result Connect() override;

    Result AsyncConnect() override;

    Result WaitForConnected(int64_t timeoutNs) override;

    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;

    const std::string &GetNic() const override;

    const TransportPrivateData GetPrivateData() const override;

    // Data transfer/copy
    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    // Sync stream
    Result Synchronize(uint32_t rankId) override;

private:
    struct LocalRegistration {
        TransportMemoryRegion mr{};
        HcommMemHandle handle{nullptr};
        UrmaMemTag memTag{0};
        uint32_t refCount{0};
        uint64_t deviceVa{0};
        std::unordered_map<uint32_t, HcommMemHandle> peerHandles{};
    };

    struct RemoteRegistration {
        uint64_t addr{0};
        uint64_t size{0};
        uint64_t memTag{0};
        std::vector<uint8_t> descBytes{};
        UrmaCommMem view{};
    };

    struct RemoteRankState {
        // 本 rank 为了和该 peer 通信而创建的本地 endpoint，按 peer 存放在 remoteRanks_[peerRank] 里
        UrmaEndpointHandle localEndpoint{nullptr};
        UrmaEndpointDesc localEndpointDesc{};
        UrmaEndpointDesc remoteEndpointDesc{};
        bool hasEndpointDesc{false};
        // 本 rank 侧用于与该 peer 通信的 channel 描述符
        HcommChannelDesc channelDesc{};
        // 本 rank 侧用于与该 peer 通信的 channel handle
        HcommChannelHandle channel{0};
        // 本 rank 侧用于与该 peer 通信的 thread
        HcommThreadHandle thread{0};
        uint32_t pendingOps{0};
        std::vector<RemoteRegistration> imports{};
        // Remote peer's notify record address (from TransportMemoryKey header during
        // ImportRemoteMemKeysLocked). Used as remote_flag_addr in kernel launch args.
        uint64_t remoteFlagAddr{0};
        uint64_t remoteFlagSize{0};
        // Raw hcommDesc bytes for the remote flag, used for HcommMemUnimport during cleanup.
        std::vector<uint8_t> remoteFlagDescBytes{};
    };

    struct RemoteMemKey {
        uint64_t addr;
        uint64_t size;
    };

    // Device kernel launch helpers (moved from removed HcomUrmaTransportAdapter)
    struct DeviceTransferBuffers {
        void *dstList{nullptr};
        void *srcList{nullptr};
        void *lenList{nullptr};
    };

    // Initialization/open/close helpers and lifecycle
    Result InitLocalDeviceInfoLocked(const TransportOptions &options);
    Result InitDeviceKernelNotifyLocked();
    Result InitDeviceTransferFlagLocked();
    void RollbackOpenDeviceLocked();
    Result EnsureDeviceKernelLoadedLocked();
    static Result DestroyRankChannelsAndThread(RemoteRankState &state);
    Result UnimportPeerImportsAndFlag(RemoteRankState &state, uint32_t peerRank);
    Result UnregisterPeerHandlesAndDestroyEndpoint(RemoteRankState &state, uint32_t peerRank);
    Result CleanupPeerRankState(RemoteRankState &state, uint32_t peerRank);
    Result CleanupLocalRegistrationsLocked();

    // Local/remote registration and key export/import
    Result FindLocalRegistrationLocked(uint64_t addr, uint64_t size, LocalRegistration *registration) const;
    Result FindRemoteRegistrationLocked(uint32_t rankId, uint64_t addr, uint64_t size,
                                        RemoteRegistration *registration) const;
    Result ImportRemoteMemKeysLocked(uint32_t peerRank, RemoteRankState &state,
                                     const std::vector<TransportMemoryKey> &memKeys);

    // Prepare/rank/connection/private data
    Result RemoveRankLocked(uint32_t rankId);

    // Data transfer/copy
    Result RemoteIo(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write);
    Result RemoteIoBatch(uint32_t rankId, const CopyDescriptor &descriptor, bool write);

    // Device kernel launch/buffer helpers and sync stream
    aclrtFuncHandle GetDeviceKernelFunc(bool isRead) const;
    void ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers);
    Result AddBatchPendingDeviceBuffers(DeviceTransferBuffers &buffers);
    void FreeBatchPendingDeviceBuffers();
    Result LaunchDeviceKernelBatch(HcommThreadHandle thread, bool isRead, HcommChannelHandle channel,
                                   const std::vector<uint64_t> &localAddrs, const std::vector<uint64_t> &remoteAddrs,
                                   const std::vector<uint64_t> &sizes, uint64_t remoteFlagAddr);
    Result SynchronizeDeviceKernelStream();

    mutable std::mutex mutex_{};
    bool opened_{false};
    uint32_t rankId_{0};
    uint32_t rankCount_{0};
    uint32_t userDeviceId_{0};
    uint32_t logicDeviceId_{0};
    uint32_t phyDeviceId_{0};
    uint32_t sdid_{0};
    uint32_t serverId_{0};
    uint32_t superPodId_{0};
    TransportOptions options_{};
    HcommTransportManager manager_;
    UrmaEndpointHandle localEndpoint_{nullptr};
    UrmaEndpointDesc localEndpointDesc_{};
    std::map<uint64_t, LocalRegistration> localRegistrations_{};
    // Device kernel launch state (moved from removed HcomUrmaTransportAdapter)
    std::mutex deviceKernelMutex_{};
    bool deviceKernelLoaded_{false};
    aclrtBinHandle deviceKernelHandle_{nullptr};
    DeviceFuncHandles deviceFuncHandles_{};
    void *notify_{nullptr};
    uint32_t notifyId_{0};
    // notify record address obtained via rtGetDevResAddress (non-ROCE path).
    // Used as local_flag_addr / flag_size in kernel launch args.
    // Registered with Hcomm so ReadOnThread can use it as dst buffer.
    uint64_t notifyAddr_{0};
    uint64_t notifyLen_{0};
    HcommMemHandle notifyHcommHandle_{nullptr};
    // Local flag buffer allocated via AclrtMalloc in OpenDevice, initialised to 1.
    // Registered with Hcomm for remote peer to use as remote_flag_addr in kernel launch args.
    // NOT inserted into localRegistrations_ and NOT exported/imported as a regular MR.
    void *devTransFlagPtr_{nullptr};
    uint64_t devTransFlagSize_{0};
    HcommMemHandle devTransFlagHcommHandle_{nullptr};
    // Separate tracking for batch kernel device buffers (variable-size, not pooled).
    std::mutex pendingBatchMutex_{};
    std::vector<DeviceTransferBuffers> pendingBatchBuffers_{};
    std::unordered_map<uint32_t, RemoteRankState> remoteRanks_{};
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_DEVICE_URMA_TRANSPORT_MANAGER_H
