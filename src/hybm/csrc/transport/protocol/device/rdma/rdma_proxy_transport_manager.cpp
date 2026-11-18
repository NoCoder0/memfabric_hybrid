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

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hybm_logger.h"
#include "hybm_va_manager.h"
#include "mf_scope_guard.h"
#include "provider_control_io.h"
#include "rdma_provider_device.h"
#include "rdma_provider_link.h"
#include "rdma_proxy_transport_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

RdmaProxyTransportManager::~RdmaProxyTransportManager()
{
    if (CloseDevice() != BM_OK) {
        for (auto &item : links_) {
            if (item.second.fd >= 0) {
                close(item.second.fd);
            }
        }
    }
}

Result RdmaProxyTransportManager::OpenDevice(const TransportOptions &options)
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (device_.context != nullptr) {
        BM_LOG_ERROR("proxy transport device is already open, deviceId=" << deviceId_);
        return BM_ERROR;
    }
    rankId_ = options.rankId;
    deviceId_ = options.deviceId;
    if (!device_.Open(deviceId_, options.rankCount)) {
        return BM_ERROR;
    }
    nic_.clear();
    return BM_OK;
}

Result RdmaProxyTransportManager::CloseDevice()
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    std::unique_lock<std::shared_mutex> gate(ioGate_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (closeFailed_) {
        return BM_ERROR;
    }
    preparedProviders_.clear();
    for (auto it = links_.begin(); it != links_.end();) {
        auto &item = *it;
        if (!device_.TryDestroyQp(item.second.qpHandle)) {
            closeFailed_ = true;
            device_.cleanupBlocked_ = true;
            return BM_ERROR;
        }
        if (item.second.fd >= 0) {
            close(item.second.fd);
        }
        it = links_.erase(it);
    }
    Result result = BM_OK;
    for (auto it = registers_.begin(); it != registers_.end();) {
        if (ReleaseRegister(*it) == BM_OK) {
            it = registers_.erase(it);
        } else {
            result = BM_ERROR;
            ++it;
        }
    }
    if (result != BM_OK) {
        closeFailed_ = true;
        return result;
    }
    device_.Close();
    closeFailed_ = device_.context != nullptr;
    return closeFailed_ ? BM_ERROR : BM_OK;
}

Result RdmaProxyTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    const bool isHbm = (mr.flags & REG_MR_FLAG_HBM) != 0U;
    const bool isDram = (mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0U;
    const auto mappedAddr =
        isDram ? HybmVaManager::GetInstance().TransformVa(mr.addr, HVM_HVA, HVM_DVA) : HVM_INVALID_VA;
    const bool hasRegisteredMapping = mappedAddr != 0U && mappedAddr != HVM_INVALID_VA;
    // The segment or ACL owns an existing host/device mapping; this transport owns only the MR.
    const bool borrowedMapping = (mr.flags & REG_MR_FLAG_ACL_DRAM) != 0U || hasRegisteredMapping;
    uint64_t deviceAddr = mr.addr;
    if (borrowedMapping) {
        deviceAddr = mappedAddr;
        if (deviceAddr == 0U || deviceAddr == HVM_INVALID_VA) {
            BM_LOG_ERROR("ACL DRAM has no registered device mapping, addr:" << mr.addr << " size:" << mr.size);
            return BM_INVALID_PARAM;
        }
    }
    HccpMrInfo info{};
    void *mrHandle = nullptr;
    const bool ownsHostMapping = !isHbm && !borrowedMapping;
    if (!device_.RegisterMr(reinterpret_cast<void *>(deviceAddr), mr.size, !ownsHostMapping, info, mrHandle)) {
        return BM_ERROR;
    }
    RegisterEntry entry{};
    entry.hostAddr = mr.addr;
    entry.size = mr.size;
    entry.dva = reinterpret_cast<uint64_t>(info.addr);
    entry.lkey = info.lkey;
    entry.rkey = info.rkey;
    entry.mrHandle = mrHandle;
    entry.ownsHostMapping = ownsHostMapping;
    std::lock_guard<std::mutex> lock(mutex_);
    registers_.push_back(entry);
    return BM_OK;
}

Result RdmaProxyTransportManager::ReleaseRegister(RegisterEntry &entry)
{
    entry.active = false;
    return ReleaseRegisteredMr(device_, entry.mrHandle, reinterpret_cast<void *>(entry.hostAddr),
                               !entry.ownsHostMapping)
               ? BM_OK
               : BM_ERROR;
}

Result RdmaProxyTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = registers_.begin(); it != registers_.end(); ++it) {
        if (it->hostAddr == addr) {
            if (ReleaseRegister(*it) != BM_OK) {
                return BM_ERROR;
            }
            registers_.erase(it);
            break;
        }
    }
    return BM_OK;
}

bool RdmaProxyTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    RegisterEntry entry{};
    return FindRegister(addr, entry) && size <= entry.size && addr - entry.hostAddr <= entry.size - size;
}

Result RdmaProxyTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    RegisterEntry entry{};
    if (!FindRegister(addr, entry)) {
        BM_LOG_ERROR("provider registered memory key not found, deviceId=" << deviceId_ << " addr=" << addr);
        return BM_ERROR;
    }
    key.keys = {entry.lkey, entry.rkey};
    return BM_OK;
}

void RdmaProxyTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    (void)key;
    (void)addr;
}

std::vector<uint32_t> RdmaProxyTransportManager::GetConnectedProtocols(uint32_t rankId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = links_.find(rankId);
    if (preparedProviders_.count(rankId) == 0 || it == links_.end() || it->second.totalSize == 0 ||
        it->second.qpHandle == nullptr) {
        return {};
    }
    if (!RdmaProviderLinkReady(device_, it->second.qpHandle, it->second.fd)) {
        return {};
    }
    return {HYBM_DOP_TYPE_DEVICE_RDMA};
}

Result RdmaProxyTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_.context == nullptr) {
        BM_LOG_ERROR("proxy transport prepare requires an open device, deviceId=" << deviceId_);
        return BM_NOT_INITIALIZED;
    }
    for (const auto &item : options.options) {
        if (!item.second.isProvider) {
            continue;
        }
        auto info = item.second;
        if (info.providerControlUrl.empty()) {
            info.providerControlUrl = info.nic;
        }
        std::string ip;
        uint16_t port = 0;
        if (!provider_control::ParseUrl(info.providerControlUrl, ip, port)) {
            BM_LOG_ERROR("RDMA proxy invalid provider URL, providerRank=" << item.first
                                                                          << " url=" << info.providerControlUrl);
            return BM_INVALID_PARAM;
        }
        auto link = links_.find(item.first);
        if (link != links_.end() && (link->second.controlUrl != info.providerControlUrl ||
                                     link->second.requestedGvaBase != info.providerGvaBase)) {
            link->second.totalSize = 0;
        }
        preparedProviders_[item.first] = std::move(info);
    }
    return BM_OK;
}

Result RdmaProxyTransportManager::ConnectRank(uint32_t rankId)
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    TransportRankPrepareInfo info;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto peer = preparedProviders_.find(rankId);
        if (peer == preparedProviders_.end()) {
            BM_LOG_ERROR("RDMA proxy rank is not prepared, providerRank=" << rankId);
            return BM_NOT_INITIALIZED;
        }
        info = peer->second;
    }
    const auto ret = ConnectProvider(rankId, info);
    if (ret != BM_OK) {
        return ret;
    }
    if (GetConnectedProtocols(rankId).empty()) {
        BM_LOG_ERROR("RDMA proxy link is not ready, providerRank=" << rankId);
        return BM_NOT_INITIALIZED;
    }
    return BM_OK;
}

Result RdmaProxyTransportManager::ConnectProvider(uint32_t rankId, const TransportRankPrepareInfo &info)
{
    std::unique_lock<std::shared_mutex> gate(ioGate_);
    const auto &url = info.providerControlUrl;
    std::unique_lock<std::mutex> linkLock(mutex_);
    auto existing = links_.find(rankId);
    if (existing != links_.end()) {
        auto &link = existing->second;
        const bool hasMatchingRegion =
            link.totalSize != 0 && link.controlUrl == url && link.requestedGvaBase == info.providerGvaBase;
        if (hasMatchingRegion && RdmaProviderLinkReady(device_, link.qpHandle, link.fd)) {
            return BM_OK;
        }
        if (!device_.TryDestroyQp(link.qpHandle)) {
            return BM_ERROR;
        }
        close(link.fd);
        links_.erase(existing);
    }
    linkLock.unlock();
    const auto deadline = std::chrono::steady_clock::now() + HANDSHAKE_TIMEOUT;
    std::string ip;
    uint16_t port = 0;
    if (!provider_control::ParseUrl(url, ip, port)) {
        BM_LOG_ERROR("RDMA proxy invalid provider URL, providerRank=" << rankId << " url=" << url);
        return BM_INVALID_PARAM;
    }
    int fd = provider_control::ConnectToHost(ip, port, deadline);
    if (fd < 0) {
        BM_LOG_ERROR("RDMA proxy connection failed, stage=TCP connect providerRank=" << rankId << " url=" << url
                                                                                     << " ret=" << BM_ERROR);
        return BM_ERROR;
    }
    auto socketGuard = make_scope_guard(fd, [](int value) { close(value); });
    RdmaProviderLinkHello hello{};
    hello.magic = RDMA_PROVIDER_LINK_MAGIC;
    hello.version = RDMA_PROVIDER_LINK_VERSION;
    hello.proxyRank = rankId_;
    hello.proxyPhyId = device_.phyId;
    hello.proxyDeviceId = deviceId_;
    hello.reserved = device_.UsesNotifyCompletion() ? RDMA_PROVIDER_LINK_NOTIFY_COMPLETION : 0;
    if (!SendStruct(fd, hello, deadline)) {
        BM_LOG_ERROR("RDMA proxy HELLO send failed, providerRank=" << rankId << " errno=" << errno);
        return BM_ERROR;
    }
    RdmaProviderLinkHelloAck ack{};
    if (!RecvStruct(fd, ack, deadline) || ack.magic != RDMA_PROVIDER_LINK_MAGIC ||
        ack.version != RDMA_PROVIDER_LINK_VERSION || ack.status != 0 || ack.providerRank != rankId) {
        BM_LOG_ERROR("RDMA proxy connection failed, stage=handshake providerRank=" << rankId << " url=" << url
                                                                                   << " ret=" << BM_ERROR);
        return BM_ERROR;
    }
    void *qpHandle = nullptr;
    TypicalQp localQp{};
    if (!device_.CreateQp(qpHandle, localQp)) {
        return BM_ERROR;
    }
    auto qpGuard = make_scope_guard(qpHandle, [this](void *value) { device_.DestroyQp(value); });
    auto localInfo = ToLinkQp(localQp, RDMA_PROVIDER_LINK_MAGIC);
    if (!SendStruct(fd, localInfo, deadline)) {
        BM_LOG_ERROR("RDMA proxy QP send failed, providerRank=" << rankId << " errno=" << errno);
        return BM_ERROR;
    }
    RdmaProviderLinkQpInfo remoteInfo{};
    if (!RecvStruct(fd, remoteInfo, deadline) || remoteInfo.magic != RDMA_PROVIDER_LINK_MAGIC) {
        BM_LOG_ERROR("RDMA proxy QP reply invalid or incomplete, providerRank=" << rankId << " errno=" << errno);
        return BM_ERROR;
    }
    TypicalQp remoteQp{};
    FromLinkQp(remoteInfo, remoteQp);
    if (!device_.ModifyQp(qpHandle, localQp, remoteQp)) {
        return BM_ERROR;
    }
    RdmaProviderLinkMrInfo mr{};
    if (!RecvStruct(fd, mr, deadline) || mr.magic != RDMA_PROVIDER_LINK_MAGIC || mr.status != 0) {
        BM_LOG_ERROR("RDMA proxy MR reply invalid or incomplete, providerRank=" << rankId << " peerStatus=" << mr.status
                                                                                << " errno=" << errno);
        return BM_ERROR;
    }
    if (device_.UsesNotifyCompletion() && (mr.notifySourceAddr == 0 || mr.notifySourceRkey == 0)) {
        BM_LOG_ERROR("RDMA proxy MR reply has no completion notification source, providerRank=" << rankId);
        return BM_INVALID_PARAM;
    }
    const uint64_t localGvaBase = info.providerGvaBase != 0 ? info.providerGvaBase : mr.providerGvaBase;
    const auto lastBase = std::numeric_limits<uint64_t>::max() - mr.totalSize;
    const bool matchesAck = mr.totalSize != 0 && mr.totalSize == ack.totalSize &&
                            mr.providerGvaBase == ack.providerGvaBase && mr.selectedPhyId == ack.selectedPhyId;
    const bool validAddressRanges =
        matchesAck && mr.npuAddrBase <= lastBase && mr.providerGvaBase <= lastBase && localGvaBase <= lastBase;
    if (!validAddressRanges) {
        BM_LOG_ERROR("RDMA proxy MR range invalid or inconsistent with HELLO, providerRank=" << rankId);
        return BM_INVALID_PARAM;
    }
    RdmaProviderLinkDone done{};
    done.magic = RDMA_PROVIDER_LINK_MAGIC;
    done.status = 0;
    if (!SendStruct(fd, done, deadline)) {
        BM_LOG_ERROR("RDMA proxy completion acknowledgement failed, providerRank=" << rankId << " errno=" << errno);
        return BM_ERROR;
    }

    DirectLink link{};
    link.fd = fd;
    link.qpHandle = qpHandle;
    link.npuAddrBase = mr.npuAddrBase;
    // Incoming addresses use the proxy segment's GVA space, not the provider process's space.
    link.providerGvaBase = localGvaBase;
    link.controlUrl = url;
    link.requestedGvaBase = info.providerGvaBase;
    link.totalSize = mr.totalSize;
    link.rkey = mr.rkey;
    link.notifySourceAddr = mr.notifySourceAddr;
    link.notifySourceRkey = mr.notifySourceRkey;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        links_[rankId] = link;
    }
    qpGuard.release();
    socketGuard.release();
    BM_LOG_INFO("RDMA proxy link established, providerRank=" << rankId << " proxyRank=" << hello.proxyRank
                                                             << " selectedPhy=" << mr.selectedPhyId
                                                             << " npuAddr=" << mr.npuAddrBase << " rkey=" << mr.rkey);
    return BM_OK;
}

Result RdmaProxyTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    std::unique_lock<std::shared_mutex> gate(ioGate_);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto rank : removedRanks) {
        preparedProviders_.erase(rank);
        auto it = links_.find(rank);
        if (it != links_.end()) {
            it->second.totalSize = 0;
            if (!device_.TryDestroyQp(it->second.qpHandle)) {
                return BM_ERROR;
            }
            if (it->second.fd >= 0) {
                close(it->second.fd);
            }
            links_.erase(it);
        }
    }
    return BM_OK;
}

Result RdmaProxyTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    return Prepare(options);
}

const std::string &RdmaProxyTransportManager::GetNic() const
{
    return nic_;
}

const TransportPrivateData RdmaProxyTransportManager::GetPrivateData() const
{
    return TransportPrivateData{};
}

bool RdmaProxyTransportManager::FindRegister(uint64_t addr, RegisterEntry &entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &item : registers_) {
        if (item.active && addr >= item.hostAddr && addr - item.hostAddr < item.size) {
            entry = item;
            return true;
        }
    }
    return false;
}

Result RdmaProxyTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIO(rankId, lAddr, rAddr, size, true, true);
}

Result RdmaProxyTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIO(rankId, lAddr, rAddr, size, false, true);
}

std::shared_ptr<std::mutex> RdmaProxyTransportManager::LinkMutexFor(uint32_t rankId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &slot = linkMutexes_[rankId];
    if (slot == nullptr) {
        slot = std::make_shared<std::mutex>();
    }
    return slot;
}

void RdmaProxyTransportManager::InvalidateLinkLocked(uint32_t rankId)
{
    auto it = links_.find(rankId);
    if (it == links_.end()) {
        return;
    }
    // 走失败即弃用该链路：后续请求会重新握手，避免在半死连接上继续提交。
    if (!device_.TryDestroyQp(it->second.qpHandle)) {
        it->second.totalSize = 0;
        return;
    }
    if (it->second.fd >= 0) {
        close(it->second.fd);
    }
    links_.erase(it);
}

// 解析并提交一笔（或多笔）WR；wait=false 时只提交，由 Synchronize() 统一等待。
Result RdmaProxyTransportManager::RemoteIO(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write,
                                           bool wait)
{
    std::shared_lock<std::shared_mutex> gate(ioGate_);
    auto linkLock = LinkMutexFor(rankId);
    std::lock_guard<std::mutex> linkGuard(*linkLock);

    void *qpHandle = nullptr;
    uint64_t remoteDva = 0;
    uint32_t rkey = 0;
    uint64_t notifySourceAddr = 0;
    uint32_t notifySourceRkey = 0;
    uint64_t localDva = 0;
    uint32_t lkey = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(rankId);
        if (it == links_.end()) {
            BM_LOG_ERROR("no direct provider link for rank=" << rankId);
            return BM_ERROR;
        }
        const auto &link = it->second;
        auto reg = std::find_if(registers_.begin(), registers_.end(), [lAddr, size](const RegisterEntry &entry) {
            return entry.active && ContainsAddressRange(entry.hostAddr, entry.size, lAddr, size);
        });
        if (link.totalSize == 0 || reg == registers_.end() ||
            !ContainsAddressRange(link.providerGvaBase, link.totalSize, rAddr, size) ||
            size > std::numeric_limits<uint32_t>::max()) {
            BM_LOG_ERROR("provider transfer outside registered range, rank=" << rankId << " size=" << size);
            return BM_INVALID_PARAM;
        }
        const auto remoteOffset = rAddr - link.providerGvaBase;
        const auto localOffset = lAddr - reg->hostAddr;
        if (localOffset > std::numeric_limits<uint64_t>::max() - reg->dva) {
            BM_LOG_ERROR("provider device address overflow, rank=" << rankId);
            return BM_INVALID_PARAM;
        }
        qpHandle = link.qpHandle;
        remoteDva = link.npuAddrBase + remoteOffset;
        rkey = link.rkey;
        notifySourceAddr = link.notifySourceAddr;
        notifySourceRkey = link.notifySourceRkey;
        localDva = reg->dva + localOffset;
        lkey = reg->lkey;
    }
    if (!device_.SubmitTransfer(qpHandle, localDva, lkey, remoteDva, rkey, size, write)) {
        std::lock_guard<std::mutex> lock(mutex_);
        InvalidateLinkLocked(rankId);
        return BM_ERROR;
    }
    if (!wait) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(rankId);
        if (it != links_.end()) {
            it->second.pendingWait = true;
        }
        return BM_OK;
    }
    if (!device_.WaitTransfer(qpHandle, notifySourceAddr, notifySourceRkey)) {
        std::lock_guard<std::mutex> lock(mutex_);
        InvalidateLinkLocked(rankId);
        return BM_ERROR;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = links_.find(rankId);
    if (it != links_.end()) {
        it->second.pendingWait = false;
    }
    return BM_OK;
}

Result RdmaProxyTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIO(rankId, lAddr, rAddr, size, false, false);
}

Result RdmaProxyTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIO(rankId, lAddr, rAddr, size, true, false);
}

Result RdmaProxyTransportManager::Synchronize(uint32_t rankId)
{
    std::shared_lock<std::shared_mutex> gate(ioGate_);
    auto linkLock = LinkMutexFor(rankId);
    std::lock_guard<std::mutex> linkGuard(*linkLock);

    void *qpHandle = nullptr;
    uint64_t notifySourceAddr = 0;
    uint32_t notifySourceRkey = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(rankId);
        if (it == links_.end()) {
            BM_LOG_ERROR("no direct provider link for rank=" << rankId);
            return BM_ERROR;
        }
        if (!it->second.pendingWait) {
            return BM_OK;
        }
        qpHandle = it->second.qpHandle;
        notifySourceAddr = it->second.notifySourceAddr;
        notifySourceRkey = it->second.notifySourceRkey;
    }
    if (!device_.WaitTransfer(qpHandle, notifySourceAddr, notifySourceRkey)) {
        std::lock_guard<std::mutex> lock(mutex_);
        InvalidateLinkLocked(rankId);
        return BM_ERROR;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = links_.find(rankId);
    if (it != links_.end()) {
        it->second.pendingWait = false;
    }
    return BM_OK;
}

Result RdmaProxyTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    return RemoteBatchIO(rankId, descriptor, true);
}

Result RdmaProxyTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    return RemoteBatchIO(rankId, descriptor, false);
}

// 整批只做一次提交 + 一次完成等待（超过单个 QP 的提交上限时按批切分），
// 与融合侧 device RDMA 的"一批一次 fence"对齐。
Result RdmaProxyTransportManager::RemoteBatchIO(uint32_t rankId, const CopyDescriptor &descriptor, bool write)
{
    const size_t count = descriptor.localAddrs.size();
    if (count != descriptor.globalAddrs.size() || count != descriptor.counts.size()) {
        BM_LOG_ERROR("provider batch arrays have different lengths, rank=" << rankId);
        return BM_INVALID_PARAM;
    }
    constexpr size_t kMaxWrPerSubmit = 1024;
    std::vector<uint64_t> localDvas(count, 0);
    std::vector<uint64_t> remoteAddrs(count, 0);
    std::vector<uint32_t> lkeys(count, 0);
    void *qpHandle = nullptr;
    uint32_t rkey = 0;
    uint64_t notifySourceAddr = 0;
    uint32_t notifySourceRkey = 0;

    std::shared_lock<std::shared_mutex> gate(ioGate_);
    auto linkLock = LinkMutexFor(rankId);
    std::lock_guard<std::mutex> linkGuard(*linkLock);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(rankId);
        if (it == links_.end() || it->second.totalSize == 0) {
            BM_LOG_ERROR("no usable direct provider link for rank=" << rankId);
            return BM_ERROR;
        }
        const auto &link = it->second;
        qpHandle = link.qpHandle;
        rkey = link.rkey;
        notifySourceAddr = link.notifySourceAddr;
        notifySourceRkey = link.notifySourceRkey;
        for (size_t i = 0; i < count; ++i) {
            const auto lAddr = reinterpret_cast<uint64_t>(descriptor.localAddrs[i]);
            const auto rAddr = reinterpret_cast<uint64_t>(descriptor.globalAddrs[i]);
            const auto size = descriptor.counts[i];
            auto reg = std::find_if(registers_.begin(), registers_.end(), [lAddr, size](const RegisterEntry &entry) {
                return entry.active && ContainsAddressRange(entry.hostAddr, entry.size, lAddr, size);
            });
            if (reg == registers_.end() || !ContainsAddressRange(link.providerGvaBase, link.totalSize, rAddr, size) ||
                size > std::numeric_limits<uint32_t>::max()) {
                BM_LOG_ERROR("provider batch entry outside registered range, rank=" << rankId << " index=" << i
                                                                                    << " size=" << size);
                return BM_INVALID_PARAM;
            }
            localDvas[i] = reg->dva + (lAddr - reg->hostAddr);
            lkeys[i] = reg->lkey;
            remoteAddrs[i] = link.npuAddrBase + (rAddr - link.providerGvaBase);
        }
    }

    for (size_t begin = 0; begin < count; begin += kMaxWrPerSubmit) {
        const size_t end = std::min(begin + kMaxWrPerSubmit, count);
        for (size_t i = begin; i < end; ++i) {
            if (!device_.SubmitTransfer(qpHandle, localDvas[i], lkeys[i], remoteAddrs[i], rkey, descriptor.counts[i],
                                        write)) {
                BM_LOG_ERROR("provider batch submit failed, rank=" << rankId << " index=" << i);
                std::lock_guard<std::mutex> lock(mutex_);
                InvalidateLinkLocked(rankId);
                return BM_ERROR;
            }
        }
        if (!device_.WaitTransfer(qpHandle, notifySourceAddr, notifySourceRkey)) {
            BM_LOG_ERROR("provider batch wait failed, rank=" << rankId << " range=[" << begin << "," << end << ")");
            std::lock_guard<std::mutex> lock(mutex_);
            InvalidateLinkLocked(rankId);
            return BM_ERROR;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = links_.find(rankId);
    if (it != links_.end()) {
        it->second.pendingWait = false;
    }
    return BM_OK;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
