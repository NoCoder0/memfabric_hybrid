#ifndef MF_HYBRID_DEVICE_RDMA_HCOMM_TRANSPORT_MANAGER_H
#define MF_HYBRID_DEVICE_RDMA_HCOMM_TRANSPORT_MANAGER_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dl_acl_api.h"
#include "dl_rt_api.h"
#include "device_kernel_helper.h"
#include "hybm_transport_manager.h"
#include "hybm_types.h"
#include "hybm_kernel_hepler.h"
#include "hcomm_api_wrapper.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

constexpr uint32_t DEVICE_RDMA_HCOMM_DEFAULT_TC = 132;
constexpr uint32_t DEVICE_RDMA_HCOMM_DEFAULT_SL = 4;
constexpr uint32_t ACL_NOTIFY_DEVICE_USE_ONLY = 1;
constexpr uint32_t DEFAULT_NOTIFY_NUM = 0;
constexpr uint32_t HYBM_DEVICE_KERNEL_BLOCK_DIM = 1;
constexpr uint32_t HYBM_NOTIFY_DEFAULT_WAIT_TIME_S = 600;
constexpr uint32_t HCOMM_KERNEL_BATCH_TIMEOUT_US = 600000000;
constexpr uint64_t DEVICE_RDMA_HCOMM_FLAG_SIZE = 8;

constexpr uint32_t RDMA_HCOMM_PRIVATE_DATA_MAGIC = 0xA5FAD001U;
constexpr uint16_t RDMA_HCOMM_PRIVATE_DATA_VERSION = 1U;

// TransportMemoryKey slot 2 metadata layout (like device_urma's UrmaExportDesc):
// keys[TRANS_KEY_DEV_SLOT2 .. TRANS_KEY_DEV_SLOT3-1] accessed via HcommKeyMeta struct
// Writing: auto &meta = HcommKeyMetaAt(key); meta.addr = ...;
// Reading: const auto &meta = HcommKeyMetaAt(key); ... = meta.addr;
struct HcommKeyMeta {
    uint64_t unused{0};  // +0 (reserved)
    uint64_t addr{0};    // +1 peer DVA
    uint64_t size{0};    // +2 peer size
    uint64_t descLen{0}; // +3 HCOMM desc length
    uint64_t flagLen{0}; // +4 flag desc length
    uint64_t gvaBase{0}; // +5 GVA base
};
static_assert(sizeof(HcommKeyMeta) <= KEY_SIZE * sizeof(uint64_t), "HcommKeyMeta must fit in one slot");

inline HcommKeyMeta &HcommKeyMetaAt(TransportMemoryKey &key)
{
    return *reinterpret_cast<HcommKeyMeta *>(&key.keys[TRANS_KEY_DEV_SLOT2]);
}
inline const HcommKeyMeta &HcommKeyMetaAt(const TransportMemoryKey &key)
{
    return *reinterpret_cast<const HcommKeyMeta *>(&key.keys[TRANS_KEY_DEV_SLOT2]);
}

// Slot 3: flag descriptor data
constexpr uint32_t HCOMM_KEY_FLAG_SLOT = TRANS_KEY_DEV_SLOT3;

// Fallback positions in other slots (backward compat with older transport layouts)
constexpr uint32_t HCOMM_KEY_LEGACY_ADDR_IDX = TRANS_KEY_DEV_SLOT1 + (KEY_SIZE - 3);
constexpr uint32_t HCOMM_KEY_LEGACY_SIZE_IDX = TRANS_KEY_DEV_SLOT1 + (KEY_SIZE - 2);
constexpr uint32_t HCOMM_KEY_HOST_ADDR_IDX = TRANS_KEY_HOST_SLOT;
constexpr uint32_t HCOMM_KEY_HOST_SIZE_IDX = TRANS_KEY_HOST_SLOT + 1;

struct RdmaHcommPrivateData {
    uint32_t magic{RDMA_HCOMM_PRIVATE_DATA_MAGIC};
    uint16_t version{RDMA_HCOMM_PRIVATE_DATA_VERSION};
    uint16_t payloadLen{0};
    CommProtocol protocol{COMM_PROTOCOL_RESERVED};
    CommAddrType addrType{COMM_ADDR_TYPE_RESERVED};
    uint8_t addrBytes[36]{};
    uint32_t devPhyId{0};
    uint32_t sdid{0};
    uint32_t listenPort{0}; // HCOMM endpoint listen port for channel creation
};
static_assert(sizeof(RdmaHcommPrivateData) <= sizeof(TransportPrivateData{}.key.keys),
              "RdmaHcommPrivateData must fit in TransportPrivateData.key.keys");

struct RemoteRankState {
    ChannelHandle channel{0};
    ThreadHandle thread{0};
    EndpointDesc remoteEndpointDesc{};
    bool hasEndpointDesc{false};
    uint64_t remoteFlagAddr{0};
    uint64_t remoteFlagSize{0};

    // Peer's registered DVA and its corresponding GVA base
    uint64_t peerDva{0};
    uint64_t peerDvaSize{0};
    uint64_t peerGva{0};
    // Imported HCOMM descriptors (memDesc + flagDesc copies) for Unimport on RemoveRanks,
    // keeping the HCOMM endpoint table in sync so a later re-import (after join retry)
    // does not hit "memDesc already imported" (HCCL_E_AGAIN).
    std::vector<std::vector<uint8_t>> importedDescs{};
    std::mutex rankMutex{}; // serialize per-rank WQE submission (multi-thread safety)
};

struct LocalMemEntry {
    HcommMemHandle handle{nullptr};
    uint64_t addr{0};
    uint64_t size{0};
    uint32_t refCount{0};
};

class DeviceRdmaHcommTransportManager final : public TransportManager {
public:
    DeviceRdmaHcommTransportManager() = default;
    ~DeviceRdmaHcommTransportManager() override;

    Result OpenDevice(const TransportOptions &options) override;
    Result CloseDevice() override;

    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;
    Result UnregisterMemoryRegion(uint64_t addr) override;
    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;
    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;
    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;

    Result Prepare(const HybmTransPrepareOptions &options) override;
    Result RemoveRanks(const std::vector<uint32_t> &removedRanks) override;
    Result Connect() override;
    Result AsyncConnect() override;
    Result WaitForConnected(int64_t timeoutNs) override;
    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;
    const std::string &GetNic() const override;
    const TransportPrivateData GetPrivateData() const override;

    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result Synchronize(uint32_t rankId) override;
    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;
    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

private:
    // ── CompletionContext (per-thread async context, matched to device_urma) ──
    struct PendingTransfer {
        uint32_t rankId{0};
        DeviceTransferBuffers buffers{};
        bool inFlight{false};
    };

    struct CompletionContext {
        void *stream{nullptr}; // per-thread stream
        void *notify{nullptr}; // per-thread notify
        uint32_t notifyId{0};
        uint64_t notifyAddr{0}; // notify record address (for kernel launch args)
        uint64_t notifyLen{0};
        HcommMemHandle notifyHcommHandle{nullptr};
        bool initialized{false};
        std::vector<PendingTransfer> pendingTransfers{};
    };

    struct OpenGeneration {
        uint64_t id{0};
    };

    struct ContextBinding {
        std::weak_ptr<OpenGeneration> owner;
        std::weak_ptr<CompletionContext> ctx;
    };

    // Endpoint/peer lifecycle
    Result BuildLocalEndpointDesc(EndpointDesc &desc) const;
    Result CreatePeerResources(uint32_t peerRank, const RdmaHcommPrivateData &peerData);
    Result DestroyPeerResources(uint32_t peerRank);
    Result ImportPeerMems(uint32_t peerRank, const std::vector<TransportMemoryKey> &memKeys);
    // 并发 join 时多进程对同一对端 HcommMemImport 会返回 HCCL_E_AGAIN(20)（HCOMM 内部
    // memDescMap/remoteRmaBufferMgr 无锁并发冲突），需重试；其他错误码直接失败。
    int32_t RawMemImportWithRetry(HcommEndpointHandle endpoint, const void *desc, uint32_t descLen, uint32_t peerRank,
                                  HcommCommMem &outMem) const;

    Result InitTransferFlagLocked();
    void DestroyTransferFlagLocked();

    // Data transfer
    Result RemoteIo(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool isRead);
    Result RemoteIoBatch(uint32_t rankId, const CopyDescriptor &descriptor, bool isRead);
    uint64_t ConvertGvaToDva(uint64_t gva) const;
    uint64_t ConvertRemoteGvaToDva(const RemoteRankState &state, uint64_t rGva) const;

    // Kernel loading / launch
    Result LoadDeviceKernel();
    void ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers);

    // ── TLS binding & per-thread CompletionContext ──
    static std::vector<ContextBinding> &GetTlsBindings();
    CompletionContext *LookupOrCreateContextLocked();
    CompletionContext *FindCurrentContextLocked() const;
    Result CreateAndPublishContextLocked(std::shared_ptr<CompletionContext> &ctx);
    Result EnsureContextInitLocked(CompletionContext &ctx);

    // ── Transfer staging & per-rank pending management ──
    Result StageAndLaunchTransfer(CompletionContext &ctx, RemoteRankState &state, bool isRead,
                                  const std::vector<uint64_t> &localVec, const std::vector<uint64_t> &remoteVec,
                                  const std::vector<uint64_t> &sizeVec, uint32_t rankId);
    static void ExtractRankPending(std::vector<PendingTransfer> &src, uint32_t rankId,
                                   std::vector<PendingTransfer> &dst);
    static void RestoreRankPending(std::vector<PendingTransfer> &src, std::vector<PendingTransfer> &dst);
    static Result SynchronizeContextLocked(void *notify, void *stream, std::vector<PendingTransfer> &pendingTransfers);
    Result SynchronizeRankPendingLocked(CompletionContext &ctx, RemoteRankState &state, uint32_t rankId,
                                        bool hasInFlight);

    mutable std::mutex mutex_;
    bool opened_{false};
    bool connected_{false};

    uint32_t rankId_{0};
    uint32_t rankCount_{0};
    uint32_t phyDeviceId_{0};
    uint32_t sdid_{0};
    uint32_t serverId_{0};
    uint32_t superPodId_{0};
    EndpointHandle localEndpoint_{nullptr};
    EndpointDesc localEndpointDesc_{};
    std::string localNic_{};
    std::string channelNameStore_;

    void *localFlagPtr_{nullptr};
    uint64_t localFlagSize_{DEVICE_RDMA_HCOMM_FLAG_SIZE};
    HcommMemHandle localFlagHandle_{nullptr};
    std::vector<uint8_t> localFlagExportDesc_;
    uint32_t localFlagExportLen_{0};

    std::unordered_map<uint32_t, RemoteRankState> remoteRanks_;
    std::unordered_map<uint64_t, LocalMemEntry> localRegistrations_;
    HcommApiWrapper hcommApi_;

    aclrtBinHandle deviceKernelHandle_{nullptr};
    DeviceFuncHandles deviceFuncHandles_{};
    std::mutex deviceKernelMutex_{};

    std::vector<std::shared_ptr<CompletionContext>> registry_{};
    std::shared_ptr<OpenGeneration> openGeneration_{};
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif
