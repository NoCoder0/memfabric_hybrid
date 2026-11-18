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

#include "urma_proxy_transport_manager.h"
#include "hybm_common_include.h"
#include "vendor/vendor_api.h"
#include "provider_device_context.h"
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <limits>
#include <optional>
#include <cstring>
#include <algorithm>
namespace ock::mf::transport::device {
namespace {
bool ValidPoolOffer(const UrmaProviderMessage &offer)
{
    const auto &key = offer.memoryKey;
    if (key.WordCount() <= DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS || key.keys[0] != URMA_EXPORT_DESC_MAGIC ||
        key.keys[1] == 0)
        return false;
    size_t offset = DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS * sizeof(uint64_t);
    bool found = false;
    while (offset < key.ByteSize()) {
        const auto *bytes = key.ByteData() + offset;
        size_t remaining = key.ByteSize() - offset;
        if (std::all_of(bytes, bytes + remaining, [](uint8_t byte) { return byte == 0; }))
            break;
        if (remaining < sizeof(UrmaExportDesc))
            return false;
        UrmaExportDesc desc;
        std::memcpy(&desc, bytes, sizeof(desc));
        if (desc.magic != URMA_EXPORT_DESC_MAGIC || desc.version != URMA_EXPORT_DESC_VERSION ||
            desc.headerSize != sizeof(desc) || desc.size != offer.poolSize ||
            desc.memoryType != UrmaMemoryType::HOST_DRAM || desc.hcommDescLen == 0 || desc.deviceMarkerDescLen == 0)
            return false;
        remaining -= sizeof(desc);
        if (desc.hcommDescLen > remaining)
            return false;
        remaining -= desc.hcommDescLen;
        if (desc.deviceMarkerDescLen > remaining)
            return false;
        offset += sizeof(desc) + static_cast<size_t>(desc.hcommDescLen) + desc.deviceMarkerDescLen;
        found = true;
    }
    return found;
}
} // namespace

UrmaProxyTransportManager::Link::~Link()
{
    if (fd >= 0)
        close(fd);
}
UrmaProxyTransportManager::~UrmaProxyTransportManager()
{
    if (closed_)
        return;
    auto ret = CloseDevice();
    running_ = false;
    if (controlThread_.joinable())
        controlThread_.join();
    if (ret != BM_OK) {
        static auto *retained = new std::vector<std::shared_ptr<UrmaProviderDevice>>;
        static auto *retainedMutex = new std::mutex;
        std::lock_guard<std::mutex> lock(*retainedMutex);
        retained->push_back(device_);
        BM_LOG_ERROR("URMA proxy retained incomplete transport cleanup");
    }
}
Result UrmaProxyTransportManager::OpenDevice(const TransportOptions &options)
{
    if (running_) {
        BM_LOG_ERROR("URMA proxy is already open");
        return BM_ERROR;
    }
    if (closed_) {
        device_ = std::make_shared<UrmaProviderDevice>();
        closed_ = false;
    }
    options_ = options;
    auto ret = device_->Open(options.deviceId, options, false);
    if (ret != BM_OK)
        return ret;
    running_ = true;
    controlThread_ = std::thread(&UrmaProxyTransportManager::ControlLoop, this);
    return BM_OK;
}
Result UrmaProxyTransportManager::CloseDevice()
{
    if (closed_)
        return BM_OK;
    UrmaProviderContext context(device_->GetContext());
    if (!context.Valid())
        return BM_ERROR;
    std::vector<uint32_t> ranks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : providers_)
            ranks.push_back(entry.first);
    }
    auto ret = RemoveRanks(ranks);
    if (ret != BM_OK)
        return ret;
    running_ = false;
    if (controlThread_.joinable())
        controlThread_.join();
    ret = device_->Close();
    if (ret == BM_OK && device_->CleanupComplete())
        closed_ = true;
    return ret;
}
Result UrmaProxyTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    UrmaProviderContext context(device_->GetContext());
    return context.Valid() ? device_->Transport().RegisterMemoryRegion(mr) : BM_ERROR;
}
Result UrmaProxyTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    UrmaProviderContext context(device_->GetContext());
    return closed_ ? BM_OK : (context.Valid() ? device_->Transport().UnregisterMemoryRegion(addr) : BM_ERROR);
}
bool UrmaProxyTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    return !closed_ && device_->Transport().QueryHasRegistered(addr, size);
}
Result UrmaProxyTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    return device_->Transport().QueryMemoryKey(addr, key);
}
void UrmaProxyTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    device_->Transport().UpdateMemoryKey(key, addr);
}
const std::string &UrmaProxyTransportManager::GetNic() const
{
    return device_->Transport().GetNic();
}
const TransportPrivateData UrmaProxyTransportManager::GetPrivateData() const
{
    return device_->Transport().GetPrivateData();
}
Result UrmaProxyTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    for (const auto &entry : options.options) {
        if (!entry.second.isProvider)
            continue;
        auto info = entry.second;
        if (info.providerControlUrl.empty())
            info.providerControlUrl = info.nic;
        std::string ip;
        uint16_t port;
        if (!provider_control::ParseUrl(info.providerControlUrl, ip, port)) {
            BM_LOG_ERROR("URMA proxy invalid provider URL, rank=" << entry.first);
            return BM_INVALID_PARAM;
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto previous = providers_.find(entry.first);
            changed = previous != providers_.end() && (previous->second.providerControlUrl != info.providerControlUrl ||
                                                       previous->second.providerGvaBase != info.providerGvaBase);
        }
        if (changed) {
            auto ret = RemoveRanks({entry.first});
            if (ret != BM_OK)
                return ret;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        providers_[entry.first] = std::move(info);
    }
    return BM_OK;
}
Result UrmaProxyTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    return Prepare(options);
}
Result UrmaProxyTransportManager::ConnectRank(uint32_t rank)
{
    UrmaProviderContext context(device_->GetContext());
    if (!context.Valid())
        return BM_ERROR;
    std::lock_guard<std::mutex> operation(operationMutex_);
    TransportRankPrepareInfo info;
    std::shared_ptr<Link> link;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = providers_.find(rank);
        if (it == providers_.end() || closed_) {
            BM_LOG_ERROR("URMA proxy rank is not prepared, rank=" << rank);
            return BM_NOT_INITIALIZED;
        }
        if (links_.count(rank) != 0) {
            lock.unlock();
            return QueryConnectedProtocols(rank, &context).empty() ? BM_NOT_CONNECTED : BM_OK;
        }
        info = it->second;
        link = std::make_shared<Link>();
        links_[rank] = link;
    }
    auto deadline = UrmaProviderDeadline();
    std::string ip;
    uint16_t port = 0;
    provider_control::ParseUrl(info.providerControlUrl, ip, port);
    link->fd = provider_control::ConnectToHost(ip, port, deadline);
    Result ret = BM_ERROR;
    bool prepared = false;
    if (link->fd >= 0) {
        UrmaProviderMessage hello;
        hello.rank = options_.rankId;
        hello.phyId = device_->GetPhysicalId();
        hello.privateData = device_->Transport().GetPrivateData();
        ret = SendUrmaProviderMessage(link->fd, hello, deadline);
        UrmaProviderMessage offer;
        if (ret == BM_OK)
            ret = ReceiveUrmaProviderMessage(link->fd, offer, deadline);
        auto localBase = info.providerGvaBase;
        if (ret == BM_OK) {
            const bool matchesRequest = offer.type == UrmaProviderMessageType::OFFER && offer.status == BM_OK &&
                                        offer.rank == rank && offer.protocols == HYBM_DOP_TYPE_DEVICE_URMA;
            const bool hasUsablePool = matchesRequest && offer.poolSize != 0 &&
                                       offer.gvaBase <= UINT64_MAX - offer.poolSize &&
                                       localBase <= UINT64_MAX - offer.poolSize;
            if (!hasUsablePool || !ValidPoolOffer(offer)) {
                BM_LOG_ERROR("URMA proxy invalid provider offer, rank=" << rank);
                ret = BM_INVALID_PARAM;
            }
        }
        if (ret == BM_OK) {
            device_->Transport().UpdateMemoryKey(offer.memoryKey, reinterpret_cast<void *>(localBase));
            HybmTransPrepareOptions prepare;
            prepare.options[rank].privateData = offer.privateData;
            prepare.options[rank].memKeys.push_back(offer.memoryKey);
            prepared = true;
            ret = device_->Transport().Prepare(prepare, offer.channelPort);
        }
        if (ret == BM_OK)
            ret = device_->Transport().WaitRankReady(rank, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                               deadline - std::chrono::steady_clock::now()));
        UrmaProviderMessage ready;
        ready.type = UrmaProviderMessageType::READY;
        ready.status = ret;
        auto sent = SendUrmaProviderMessage(link->fd, ready, deadline);
        if (ret == BM_OK)
            ret = sent;
        UrmaProviderMessage ack;
        if (ret == BM_OK)
            ret = ReceiveUrmaProviderMessage(link->fd, ack, deadline);
        if (ret == BM_OK && (ack.type != UrmaProviderMessageType::ACK || ack.status != BM_OK))
            ret = BM_ERROR;
    }
    if (ret == BM_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        link->ready = true;
        return BM_OK;
    }
    BM_LOG_ERROR("URMA proxy connection failed, rank=" << rank << " ret=" << ret);
    if (prepared) {
        device_->Transport().BeginRankDrain(rank);
        if (device_->Transport().IsRankDrained(rank) && link->fd >= 0) {
            UrmaProviderMessage quiesced;
            quiesced.type = UrmaProviderMessageType::QUIESCED;
            SendUrmaProviderMessage(link->fd, quiesced, std::chrono::steady_clock::now() + std::chrono::seconds(1));
        }
        if (device_->Transport().RemoveRanks({rank}) != BM_OK)
            return ret;
    }
    if (link->fd >= 0)
        shutdown(link->fd, SHUT_RDWR);
    std::lock_guard<std::mutex> lock(mutex_);
    links_.erase(rank);
    return ret;
}
Result UrmaProxyTransportManager::Drain(uint32_t rank, const std::shared_ptr<Link> &link, ProviderDeadline deadline)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        link->draining = true;
        link->ready = false;
        device_->Transport().BeginRankDrain(rank);
    }
    if (!device_->Transport().IsRankDrained(rank))
        return BM_TIMEOUT;
    if (!link->quiesced && !link->failed) {
        UrmaProviderMessage message;
        message.type = UrmaProviderMessageType::QUIESCED;
        if (SendUrmaProviderMessage(link->fd, message, deadline) == BM_OK)
            link->quiesced = true;
        else
            link->failed = true;
    }
    // Local IO is drained. The provider requires its own proof before releasing the pool.
    return BM_OK;
}
Result UrmaProxyTransportManager::RemoveRanks(const std::vector<uint32_t> &ranks)
{
    UrmaProviderContext context(device_->GetContext());
    if (!context.Valid())
        return BM_ERROR;
    std::lock_guard<std::mutex> operation(operationMutex_);
    Result result = BM_OK;
    for (auto rank : ranks) {
        std::shared_ptr<Link> link;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = links_.find(rank);
            if (it == links_.end()) {
                providers_.erase(rank);
                continue;
            }
            link = it->second;
        }
        auto deadline = UrmaProviderDeadline();
        auto ret = Drain(rank, link, deadline);
        while (ret == BM_TIMEOUT && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ret = Drain(rank, link, deadline);
        }
        if (ret == BM_OK)
            ret = device_->Transport().RemoveRanks({rank});
        if (ret != BM_OK) {
            result = ret;
            continue;
        }
        shutdown(link->fd, SHUT_RDWR);
        std::lock_guard<std::mutex> lock(mutex_);
        links_.erase(rank);
        providers_.erase(rank);
    }
    return result;
}
void UrmaProxyTransportManager::ControlLoop()
{
    if (VendorApi::Api()->SetCurrentContext(device_->GetContext()) != BM_OK) {
        BM_LOG_ERROR("URMA proxy control thread cannot restore ACL context");
        return;
    }
    while (running_) {
        std::vector<std::pair<uint32_t, std::shared_ptr<Link>>> links;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto &entry : links_)
                if (entry.second->ready || (entry.second->draining && !entry.second->quiesced && !entry.second->failed))
                    links.push_back(entry);
        }
        for (const auto &entry : links) {
            auto link = entry.second;
            pollfd item{link->fd, POLLIN, 0};
            if (poll(&item, 1, 0) > 0) {
                UrmaProviderMessage message;
                auto ret = ReceiveUrmaProviderMessage(link->fd, message,
                                                      std::chrono::steady_clock::now() + std::chrono::seconds(1));
                std::lock_guard<std::mutex> lock(mutex_);
                if (!link->draining) {
                    device_->Transport().BeginRankDrain(entry.first);
                    link->draining = true;
                    link->ready = false;
                }
                if (ret != BM_OK || message.type != UrmaProviderMessageType::DRAIN || message.status != BM_OK)
                    link->failed = true;
            }
            if (!link->draining || link->failed || link->quiesced)
                continue;
            std::unique_lock<std::mutex> operation(operationMutex_, std::try_to_lock);
            if (!operation.owns_lock())
                continue;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto current = links_.find(entry.first);
                if (current == links_.end() || current->second != link)
                    continue;
            }
            Drain(entry.first, link, UrmaProviderDeadline());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
std::vector<uint32_t> UrmaProxyTransportManager::GetConnectedProtocols(uint32_t rank) const
{
    return QueryConnectedProtocols(rank, nullptr);
}
std::vector<uint32_t> UrmaProxyTransportManager::QueryConnectedProtocols(uint32_t rank,
                                                                         const UrmaProviderContext *activeContext) const
{
    std::shared_ptr<Link> link;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(rank);
        if (it == links_.end() || !it->second->ready || it->second->draining ||
            !provider_control::SocketAlive(it->second->fd))
            return {};
        link = it->second;
    }
    std::optional<UrmaProviderContext> localContext;
    if (activeContext == nullptr) {
        localContext.emplace(device_->GetContext());
        if (!localContext->Valid())
            return {};
    }
    auto protocols = device_->Transport().GetConnectedProtocols(rank);
    std::lock_guard<std::mutex> lock(mutex_);
    return link->ready && !link->draining ? protocols : std::vector<uint32_t>{};
}
Result UrmaProxyTransportManager::ReadRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    auto ret = ReadRemoteAsync(rank, local, remote, size);
    return ret == BM_OK ? Synchronize(rank) : ret;
}
Result UrmaProxyTransportManager::WriteRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    auto ret = WriteRemoteAsync(rank, local, remote, size);
    return ret == BM_OK ? Synchronize(rank) : ret;
}
Result UrmaProxyTransportManager::ReadRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    UrmaProviderContext context(device_->GetContext());
    return !context.Valid() || QueryConnectedProtocols(rank, &context).empty()
               ? BM_NOT_CONNECTED
               : device_->Transport().ReadRemoteAsync(rank, local, remote, size);
}
Result UrmaProxyTransportManager::WriteRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    UrmaProviderContext context(device_->GetContext());
    return !context.Valid() || QueryConnectedProtocols(rank, &context).empty()
               ? BM_NOT_CONNECTED
               : device_->Transport().WriteRemoteAsync(rank, local, remote, size);
}
Result UrmaProxyTransportManager::ReadRemoteBatchAsync(uint32_t rank, const CopyDescriptor &descriptor)
{
    UrmaProviderContext context(device_->GetContext());
    return !context.Valid() || QueryConnectedProtocols(rank, &context).empty()
               ? BM_NOT_CONNECTED
               : device_->Transport().ReadRemoteBatchAsync(rank, descriptor);
}
Result UrmaProxyTransportManager::WriteRemoteBatchAsync(uint32_t rank, const CopyDescriptor &descriptor)
{
    UrmaProviderContext context(device_->GetContext());
    return !context.Valid() || QueryConnectedProtocols(rank, &context).empty()
               ? BM_NOT_CONNECTED
               : device_->Transport().WriteRemoteBatchAsync(rank, descriptor);
}
Result UrmaProxyTransportManager::Synchronize(uint32_t rank)
{
    UrmaProviderContext context(device_->GetContext());
    return context.Valid() ? device_->Transport().Synchronize(rank) : BM_ERROR;
}
Result UrmaProxyTransportManager::SynchronizeRanks(const std::set<uint32_t> &ranks)
{
    UrmaProviderContext context(device_->GetContext());
    return context.Valid() ? device_->Transport().SynchronizeRanks(ranks) : BM_ERROR;
}
Result UrmaProxyTransportManager::TransferRemoteBatchAsync(const hybm_batch_copy_params &params,
                                                           hybm_data_copy_direction direction,
                                                           const RankGroupMap &groups, std::vector<uint32_t> &local,
                                                           RankGroupMap &unregistered, std::set<uint32_t> &batchRanks)
{
    UrmaProviderContext context(device_->GetContext());
    if (!context.Valid())
        return BM_ERROR;
    for (const auto &entry : groups) {
        for (auto rank : {entry.first.first, entry.first.second}) {
            if (rank != options_.rankId && QueryConnectedProtocols(rank, &context).empty())
                return BM_NOT_CONNECTED;
        }
    }
    return device_->Transport().TransferRemoteBatchAsync(params, direction, groups, local, unregistered, batchRanks);
}
} // namespace ock::mf::transport::device
