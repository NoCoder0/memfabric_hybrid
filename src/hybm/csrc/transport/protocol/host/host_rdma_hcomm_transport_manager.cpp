/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#include "host_rdma_hcomm_transport_manager.h"
#include <arpa/inet.h>
#include <cstdlib>
#include "hybm_va_manager.h"
#include "mf_ipv4_validator.h"
namespace ock {
namespace mf {
namespace transport {
namespace host {
using namespace device;

HostRdmaHcommTransportManager::~HostRdmaHcommTransportManager()
{
    (void)CloseDevice();
}

Result HostRdmaHcommTransportManager::OpenDevice(const TransportOptions &options)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
        return BM_OK;
    }
    BM_VALIDATE_RETURN(options.rankCount != 0 && options.rankId < options.rankCount,
                       "Invalid Host HCOMM rank: " << options.rankId, BM_INVALID_PARAM);
    std::string protocol, ip, mask, port;
    BM_VALIDATE_RETURN(NetValidator::ParseNicUrl(options.nic, protocol, ip, mask, port) && protocol == "tcp://",
                       "Invalid Host HCOMM NIC: " << options.nic, BM_INVALID_PARAM);
    char *end = nullptr;
    const auto portValue = std::strtoul(port.c_str(), &end, 10);
    BM_VALIDATE_RETURN(end != port.c_str() && *end == '\0' && portValue > 0 && portValue <= UINT16_MAX,
                       "Invalid Host HCOMM listen port: " << port, BM_INVALID_PARAM);
    EndpointDescInit(&localEndpointDesc_, 1);
    localEndpointDesc_.protocol = COMM_PROTOCOL_ROCE;
    localEndpointDesc_.commAddr.type = COMM_ADDR_TYPE_IP_V4;
    BM_VALIDATE_RETURN(inet_pton(AF_INET, ip.c_str(), &localEndpointDesc_.commAddr.addr) == 1,
                       "Invalid Host HCOMM IPv4 address: " << ip, BM_INVALID_PARAM);
    localEndpointDesc_.loc.locType = ENDPOINT_LOC_TYPE_HOST;
    localEndpointDesc_.loc.host.id = 0;
    auto ret = HcommApiWrapper::LoadHcomm();
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "LoadHcomm failed, ret: " << ret);
    ret = api_.CreateEndpoint(localEndpointDesc_, endpoint_);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommEndpointCreate failed, ret: " << ret);
    ret = InitTransferFlagLocked();
    if (ret != BM_OK) {
        (void)api_.DestroyEndpoint(endpoint_);
        endpoint_ = nullptr;
        return ret;
    }
    rankId_ = options.rankId;
    rankCount_ = options.rankCount;
    localNic_ = options.nic;
    // C's CPU server binds the explicitly configured channel port.
    localListenPort_ = static_cast<uint16_t>(portValue);
    opened_ = true;
    return BM_OK;
}

Result HostRdmaHcommTransportManager::InitTransferFlagLocked()
{
    // The NPU reads this registered word after Fence; it does not write a completion notification here.
    localFlagPtr_ = std::malloc(sizeof(uint64_t));
    BM_VALIDATE_RETURN(localFlagPtr_ != nullptr, "Host HCOMM flag allocation failed", BM_MALLOC_FAILED);
    *static_cast<uint64_t *>(localFlagPtr_) = 1;
    HcommCommMem mem{COMM_MEM_TYPE_HOST, localFlagPtr_, sizeof(uint64_t)};
    auto ret = api_.RawMemReg(endpoint_, "__hybm_transfer_flag__", mem, localFlagHandle_);
    void *desc = nullptr;
    uint32_t length = 0;
    if (ret == BM_OK) {
        ret = api_.RawMemExport(endpoint_, localFlagHandle_, desc, length);
    }
    if (ret != BM_OK || desc == nullptr || length <= sizeof(EndpointDesc) ||
        length > TRANS_KEY_MAX_WORDS * sizeof(uint64_t) - HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t)) {
        BM_LOG_ERROR("Host HCOMM flag registration/export failed, ret: " << ret << ", bytes: " << length);
        if (localFlagHandle_ != nullptr) {
            (void)api_.RawMemUnreg(endpoint_, localFlagHandle_);
            localFlagHandle_ = nullptr;
        }
        std::free(localFlagPtr_);
        localFlagPtr_ = nullptr;
        return BM_ERROR;
    }
    localFlagExportDesc_.assign(static_cast<uint8_t *>(desc), static_cast<uint8_t *>(desc) + length);
    return BM_OK;
}

Result HostRdmaHcommTransportManager::FenceLocked(uint32_t rank)
{
    auto &state = remoteRanks_.at(rank);
    if (state.channel == 0) {
        return BM_OK;
    }
    auto ret = DlHcommApi::HcommChannelFence(state.channel);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HcommChannelFence failed, rank: " << rank << ", ret: " << ret);
        state.failed = true;
    }
    return ret;
}

Result HostRdmaHcommTransportManager::QuiesceLocked()
{
    for (const auto &item : remoteRanks_) {
        auto ret = FenceLocked(item.first);
        if (ret != BM_OK) {
            return ret;
        }
    }
    return BM_OK;
}

Result HostRdmaHcommTransportManager::CheckLocalMemoryMutableLocked() const
{
    for (const auto &item : remoteRanks_) {
        BM_VALIDATE_RETURN(
            item.second.channel == 0,
            "Host HCOMM uses a fixed pool; remove channels before changing local MRs, peer: " << item.first,
            BM_NOT_SUPPORTED);
    }
    return BM_OK;
}

Result HostRdmaHcommTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    BM_VALIDATE_RETURN(opened_, "Host HCOMM transport is unavailable", BM_NOT_INITIALIZED);
    BM_VALIDATE_RETURN(HcommRangeContains(mr.addr, mr.size, mr.addr, mr.size) &&
                           (mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) && !(mr.flags & REG_MR_FLAG_HBM),
                       "Invalid Host HCOMM DRAM region, bytes: " << mr.size, BM_INVALID_PARAM);
    auto it = localRegistrations_.find(mr.addr);
    if (it != localRegistrations_.end()) {
        BM_VALIDATE_RETURN(it->second.size == mr.size, "Host HCOMM registration size changed", BM_INVALID_PARAM);
        ++it->second.refCount;
        return BM_OK;
    }
    for (const auto &entry : localRegistrations_) {
        BM_VALIDATE_RETURN(mr.addr + mr.size <= entry.first || entry.first + entry.second.size <= mr.addr,
                           "Overlapping Host HCOMM registration", BM_INVALID_PARAM);
    }
    auto ret = CheckLocalMemoryMutableLocked();
    if (ret != BM_OK) {
        return ret;
    }
    HcommCommMem mem{COMM_MEM_TYPE_HOST, reinterpret_cast<void *>(mr.addr), mr.size};
    HcommMemHandle handle = nullptr;
    ret = api_.RawMemReg(endpoint_, std::to_string(mr.addr).c_str(), mem, handle);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommMemReg failed, ret: " << ret << ", bytes: " << mr.size);
    localRegistrations_.emplace(mr.addr, LocalMemEntry{handle, mr.size, 1});
    return BM_OK;
}

Result HostRdmaHcommTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = localRegistrations_.find(addr);
    if (it == localRegistrations_.end()) {
        return BM_OK;
    }
    if (it->second.refCount > 1) {
        --it->second.refCount;
        return BM_OK;
    }
    auto ret = CheckLocalMemoryMutableLocked();
    if (ret != BM_OK) {
        return ret;
    }
    ret = api_.RawMemUnreg(endpoint_, it->second.handle);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommMemUnreg failed, ret: " << ret);
    localRegistrations_.erase(it);
    return BM_OK;
}

bool HostRdmaHcommTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &entry : localRegistrations_) {
        if (entry.second.handle != nullptr && HcommRangeContains(entry.first, entry.second.size, addr, size)) {
            return true;
        }
    }
    return false;
}

Result HostRdmaHcommTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = localRegistrations_.find(addr);
    BM_VALIDATE_RETURN(it != localRegistrations_.end() && it->second.handle != nullptr,
                       "Host HCOMM memory is not registered", BM_INVALID_PARAM);
    void *desc = nullptr;
    uint32_t length = 0;
    auto ret = api_.RawMemExport(endpoint_, it->second.handle, desc, length);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommMemExport failed, ret: " << ret);
    BM_VALIDATE_RETURN(desc != nullptr && length > sizeof(EndpointDesc) &&
                           length <= TRANS_KEY_DEV_SLOT2 * sizeof(uint64_t) && !localFlagExportDesc_.empty(),
                       "Invalid Host HCOMM export length, bytes: " << length, BM_INVALID_PARAM);
    key.keys.clear();
    key.ResizeBytes(HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t) + localFlagExportDesc_.size());
    std::memcpy(key.ByteData(), desc, length);
    auto &meta = HcommKeyMetaAt(key);
    meta.addr = addr;
    meta.size = it->second.size;
    meta.descLen = length;
    meta.flagLen = localFlagExportDesc_.size();
    const uint64_t gva = HybmVaManager::GetInstance().TransformVa(addr, HVM_HVA, HVM_GVA);
    meta.gvaBase = gva != 0 ? gva : addr;
    std::memcpy(key.ByteData() + HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t), localFlagExportDesc_.data(), meta.flagLen);
    return BM_OK;
}

Result HostRdmaHcommTransportManager::QueryControlKey(TransportMemoryKey &key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    BM_VALIDATE_RETURN(opened_ && localFlagHandle_ != nullptr, "HCOMM control buffer is unavailable, rank: " << rankId_,
                       BM_NOT_INITIALIZED);
    void *desc = nullptr;
    uint32_t length = 0;
    auto ret = api_.RawMemExport(endpoint_, localFlagHandle_, desc, length);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret,
                                     "HcommMemExport for control buffer failed, rank: " << rankId_ << ", ret: " << ret);
    return BuildHcommControlKey(desc, length, reinterpret_cast<uint64_t>(localFlagPtr_), sizeof(uint64_t), key);
}

void HostRdmaHcommTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    if (addr != nullptr) {
        EnsureHcommKeyMeta(key);
        // Keep the MR's exported DMA address; only the advertised GVA origin changes.
        HcommKeyMetaAt(key).gvaBase = reinterpret_cast<uint64_t>(addr);
    }
}

Result HostRdmaHcommTransportManager::EnsureChannelLocked(uint32_t rank, RemoteRankState &state)
{
    BM_VALIDATE_RETURN(!state.failed, "Host HCOMM peer failed, rank: " << rank, BM_ERROR);
    if (state.channel != 0 || !state.hasEndpointDesc) {
        return BM_OK;
    }
    BM_VALIDATE_RETURN(state.hasPeerData, "Missing HCOMM peer private data, rank: " << rank, BM_INVALID_PARAM);
    const bool server = rankId_ < rank;
    BM_VALIDATE_RETURN(state.remoteEndpointDesc.loc.locType != ENDPOINT_LOC_TYPE_DEVICE || server,
                       "Host HCOMM rank must be lower than NPU rank: " << rank, BM_INVALID_PARAM);
    BM_VALIDATE_RETURN(server || state.peerData.listenPort != 0,
                       "HCOMM client requires peer listen port, rank: " << rank, BM_INVALID_PARAM);
    state.channelName =
        "hybm_" + std::to_string(std::min(rankId_, rank)) + "_" + std::to_string(std::max(rankId_, rank));
    HcommChannelDesc desc{};
    HcommChannelDescInit(&desc, 1);
    desc.role = server ? HCOMM_SOCKET_ROLE_SERVER : HCOMM_SOCKET_ROLE_CLIENT;
    desc.remoteEndpoint = state.remoteEndpointDesc;
    desc.exchangeAllMems = true;
    desc.notifyNum = 0;
    desc.port = server ? localListenPort_ : static_cast<uint16_t>(state.peerData.listenPort);
    desc.channelName = state.channelName.c_str();
    desc.roceAttr.tc = 132;
    desc.roceAttr.sl = 4;
    desc.roceAttr.queueNum = 1;
    auto ret = HcommApiWrapper::CreateChannel(endpoint_, COMM_ENGINE_CPU, desc, state.channel);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommChannelCreate failed, rank: " << rank << ", ret: " << ret);
    ret = HcommApiWrapper::WaitForChannelsReady({{state.channel, rank}}, std::chrono::seconds(60));
    if (ret != BM_OK) {
        if (HcommApiWrapper::DestroyChannel(state.channel) != BM_OK) {
            state.failed = true;
        }
    }
    return ret;
}

Result HostRdmaHcommTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    std::lock_guard<std::mutex> lock(mutex_);
    BM_VALIDATE_RETURN(opened_, "Host HCOMM transport is unavailable", BM_NOT_INITIALIZED);
    for (const auto &item : options.options) {
        const auto rank = item.first;
        BM_VALIDATE_RETURN(rank < rankCount_, "Invalid HCOMM peer rank: " << rank, BM_INVALID_PARAM);
        if (rank == rankId_) {
            continue;
        }
        auto &state = remoteRanks_[rank];
        BM_VALIDATE_RETURN(!state.failed, "HCOMM peer failed, rank: " << rank, BM_ERROR);
        if (!item.second.privateData.key.Empty()) {
            RdmaHcommPeerPrivateInfo peer{};
            auto ret = ReadHcommPrivateData(item.second.privateData.key, peer);
            if (ret != BM_OK) {
                return ret;
            }
            BM_VALIDATE_RETURN(!state.hasPeerData || SameHcommPrivateData(peer, state.peerData),
                               "HCOMM peer private data changed, rank: " << rank, BM_INVALID_PARAM);
            state.peerData = peer;
            state.hasPeerData = true;
        }
        BM_VALIDATE_RETURN(state.hasPeerData, "Missing HCOMM private data, rank: " << rank, BM_INVALID_PARAM);
        auto ret = QuiesceLocked();
        if (ret != BM_OK) {
            return ret;
        }
        ret = ImportHcommPeerKeys(endpoint_, item.second.memKeys, state.peerData.flagAddr == 0, state);
        if (ret != BM_OK) {
            return ret;
        }
        ret = EnsureChannelLocked(rank, state);
        if (ret != BM_OK) {
            return ret;
        }
    }
    return BM_OK;
}

Result HostRdmaHcommTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    return Prepare(options);
}

Result HostRdmaHcommTransportManager::ConnectRank(uint32_t rank)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = remoteRanks_.find(rank);
    return it == remoteRanks_.end() ? BM_OK : EnsureChannelLocked(rank, it->second);
}

Result HostRdmaHcommTransportManager::CleanupRankLocked(uint32_t rank)
{
    auto &state = remoteRanks_.at(rank);
    auto ret = FenceLocked(rank);
    if (ret != BM_OK) {
        return ret;
    }
    if (state.channel != 0) {
        ret = HcommApiWrapper::DestroyChannel(state.channel);
        BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommChannelDestroy failed, rank: " << rank << ", ret: " << ret);
    }
    while (!state.importedDescs.empty()) {
        const auto &desc = state.importedDescs.back();
        ret = api_.RawMemUnimport(endpoint_, desc.data(), static_cast<uint32_t>(desc.size()));
        if (ret != BM_OK) {
            BM_LOG_ERROR("HcommMemUnimport failed, rank: " << rank << ", ret: " << ret);
            state.failed = true;
            return ret;
        }
        state.importedDescs.pop_back();
    }
    return BM_OK;
}

Result HostRdmaHcommTransportManager::RemoveRanks(const std::vector<uint32_t> &ranks)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto ret = QuiesceLocked();
    if (ret != BM_OK) {
        return ret;
    }
    for (auto rank : ranks) {
        if (remoteRanks_.count(rank) == 0) {
            continue;
        }
        ret = CleanupRankLocked(rank);
        if (ret != BM_OK) {
            return ret;
        }
        remoteRanks_.erase(rank);
    }
    return BM_OK;
}

Result HostRdmaHcommTransportManager::CloseDevice()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        return BM_OK;
    }
    auto ret = QuiesceLocked();
    if (ret != BM_OK) {
        return ret;
    }
    while (!remoteRanks_.empty()) {
        auto it = remoteRanks_.begin();
        ret = CleanupRankLocked(it->first);
        if (ret != BM_OK) {
            return ret;
        }
        remoteRanks_.erase(it);
    }
    while (!localRegistrations_.empty()) {
        auto it = localRegistrations_.begin();
        if (it->second.handle != nullptr) {
            ret = api_.RawMemUnreg(endpoint_, it->second.handle);
            BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommMemUnreg during close failed, ret: " << ret);
        }
        localRegistrations_.erase(it);
    }
    if (localFlagHandle_ != nullptr) {
        ret = api_.RawMemUnreg(endpoint_, localFlagHandle_);
        BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommMemUnreg flag failed, ret: " << ret);
        localFlagHandle_ = nullptr;
    }
    std::free(localFlagPtr_);
    localFlagPtr_ = nullptr;
    localFlagExportDesc_.clear();
    ret = api_.DestroyEndpoint(endpoint_);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "HcommEndpointDestroy failed, ret: " << ret);
    endpoint_ = nullptr;
    opened_ = false;
    return BM_OK;
}

const std::string &HostRdmaHcommTransportManager::GetNic() const
{
    return localNic_;
}

const TransportPrivateData HostRdmaHcommTransportManager::GetPrivateData() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    TransportPrivateData result{};
    if (opened_) {
        RdmaHcommPrivateData data{};
        data.protocol = COMM_PROTOCOL_ROCE;
        data.addrType = localEndpointDesc_.commAddr.type;
        std::memcpy(data.addrBytes, localEndpointDesc_.commAddr.raws, sizeof(data.addrBytes));
        data.listenPort = localListenPort_;
        WriteStructToKey(data, result.key);
    }
    return result;
}

Result HostRdmaHcommTransportManager::ResolveAddressesLocked(const RemoteRankState &state, uint64_t local,
                                                             uint64_t remote, uint64_t size, uint64_t &localAddr,
                                                             uint64_t &remoteAddr) const
{
    const auto hva = HybmVaManager::GetInstance().TransformVa(local, HVM_GVA, HVM_HVA);
    localAddr = hva != 0 ? hva : local;
    bool registered = false;
    for (const auto &entry : localRegistrations_) {
        if (entry.second.handle != nullptr && HcommRangeContains(entry.first, entry.second.size, localAddr, size)) {
            registered = true;
            break;
        }
    }
    BM_VALIDATE_RETURN(registered && HcommRangeContains(state.peerGva, state.peerDvaSize, remote, size),
                       "Host HCOMM transfer exceeds registered memory, bytes: " << size, BM_INVALID_PARAM);
    remoteAddr = state.peerDva + (remote - state.peerGva);
    return BM_OK;
}

Result HostRdmaHcommTransportManager::SubmitOneLocked(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size,
                                                      bool write)
{
    auto &state = remoteRanks_.at(rank);
    auto ret = write ? DlHcommApi::HcommWriteNbi(state.channel, reinterpret_cast<void *>(remote),
                                                 reinterpret_cast<const void *>(local), size)
                     : DlHcommApi::HcommReadNbi(state.channel, reinterpret_cast<void *>(local),
                                                reinterpret_cast<const void *>(remote), size);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Hcomm Nbi submission failed, rank: " << rank << ", bytes: " << size << ", ret: " << ret);
        state.failed = true;
        (void)FenceLocked(rank);
    }
    return ret;
}

Result HostRdmaHcommTransportManager::RemoteIo(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size,
                                               bool write)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = remoteRanks_.find(rank);
    BM_VALIDATE_RETURN(opened_ && it != remoteRanks_.end() && it->second.channel != 0 && !it->second.failed,
                       "Host HCOMM peer is not ready, rank: " << rank, BM_NOT_CONNECTED);
    uint64_t lAddr = 0, rAddr = 0;
    auto ret = ResolveAddressesLocked(it->second, local, remote, size, lAddr, rAddr);
    return ret == BM_OK ? SubmitOneLocked(rank, lAddr, rAddr, size, write) : ret;
}

Result HostRdmaHcommTransportManager::RemoteIoBatch(uint32_t rank, const CopyDescriptor &desc, bool write)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = remoteRanks_.find(rank);
    BM_VALIDATE_RETURN(opened_ && it != remoteRanks_.end() && it->second.channel != 0 && !it->second.failed,
                       "Host HCOMM peer is not ready, rank: " << rank, BM_NOT_CONNECTED);
    BM_VALIDATE_RETURN(desc.localAddrs.size() == desc.counts.size() && desc.globalAddrs.size() == desc.counts.size(),
                       "Host HCOMM batch descriptor lengths differ", BM_INVALID_PARAM);
    std::vector<uint64_t> local(desc.counts.size()), remote(desc.counts.size());
    // Host data operators reverse the source/destination lists for reads.
    for (size_t i = 0; i < desc.counts.size(); ++i) {
        auto ret = ResolveAddressesLocked(it->second,
                                          reinterpret_cast<uint64_t>(write ? desc.localAddrs[i] : desc.globalAddrs[i]),
                                          reinterpret_cast<uint64_t>(write ? desc.globalAddrs[i] : desc.localAddrs[i]),
                                          desc.counts[i], local[i], remote[i]);
        if (ret != BM_OK) {
            return ret;
        }
    }
    for (size_t i = 0; i < desc.counts.size(); ++i) {
        auto ret = SubmitOneLocked(rank, local[i], remote[i], desc.counts[i], write);
        if (ret != BM_OK) {
            return ret;
        }
    }
    return BM_OK;
}

Result HostRdmaHcommTransportManager::ReadRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    return RemoteIo(rank, local, remote, size, false);
}
Result HostRdmaHcommTransportManager::WriteRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    return RemoteIo(rank, local, remote, size, true);
}
Result HostRdmaHcommTransportManager::ReadRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    auto ret = ReadRemoteAsync(rank, local, remote, size);
    return ret == BM_OK ? Synchronize(rank) : ret;
}
Result HostRdmaHcommTransportManager::WriteRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    auto ret = WriteRemoteAsync(rank, local, remote, size);
    return ret == BM_OK ? Synchronize(rank) : ret;
}
Result HostRdmaHcommTransportManager::ReadRemoteBatchAsync(uint32_t rank, const CopyDescriptor &desc)
{
    return RemoteIoBatch(rank, desc, false);
}
Result HostRdmaHcommTransportManager::WriteRemoteBatchAsync(uint32_t rank, const CopyDescriptor &desc)
{
    return RemoteIoBatch(rank, desc, true);
}
Result HostRdmaHcommTransportManager::Synchronize(uint32_t rank)
{
    std::lock_guard<std::mutex> lock(mutex_);
    BM_VALIDATE_RETURN(remoteRanks_.count(rank) != 0, "Unknown HCOMM peer rank: " << rank, BM_NOT_CONNECTED);
    auto ret = FenceLocked(rank);
    BM_VALIDATE_RETURN(!remoteRanks_.at(rank).failed, "Host HCOMM peer has failed, rank: " << rank, BM_ERROR);
    return ret;
}
} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock
