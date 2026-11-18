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

#include "urma_provider_control_server.h"
#include "vendor/vendor_api.h"
#include "provider_device_context.h"
#include "hybm_networks_common.h"
#include "hybm_common_include.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <limits>
namespace ock::mf::transport::device {
UrmaProviderControlServer::Session::~Session()
{
    if (fd >= 0)
        close(fd);
}
UrmaProviderControlServer::~UrmaProviderControlServer()
{
    Stop();
}
bool UrmaProviderControlServer::Init(const ProviderServiceOptions &options)
{
    UrmaProviderContext restore;
    Stop();
    if (!cards_.empty()) {
        BM_LOG_ERROR("URMA provider initialization has retained resources");
        return false;
    }
    uint16_t port = options.listenPort;
    if (!provider_control::ResolveListenPort(port)) {
        BM_LOG_ERROR("URMA provider control port must be an integer in [1, 65535]");
        return false;
    }
    options_ = options;
    options_.transport.protocol = HYBM_DOP_TYPE_DEVICE_URMA;
    for (auto userId : options.userDeviceIds) {
        auto device = std::make_shared<UrmaProviderDevice>();
        cards_.push_back(device);
        if (device->Open(userId, options_.transport, true) != BM_OK) {
            BM_LOG_ERROR("URMA provider device initialization failed, userId=" << userId);
            Stop();
            return false;
        }
    }
    if (cards_.empty()) {
        BM_LOG_ERROR("URMA provider has no devices");
        return false;
    }
    listenFd_ = provider_control::CreateListenSocket(port);
    if (listenFd_ < 0) {
        BM_LOG_ERROR("URMA provider listen failed, port=" << port);
        Stop();
        return false;
    }
    auto ips = NetworkGetIpAddresses();
    in_addr address{};
    address.s_addr = ips.empty() ? htonl(INADDR_LOOPBACK) : htonl(ips.front());
    controlUrl_ = std::string(inet_ntoa(address)) + ":" + std::to_string(port);
    return true;
}
bool UrmaProviderControlServer::RegisterMemory(void *hostAddr, uint64_t size, uint64_t gvaBase, uint32_t providerRank)
{
    UrmaProviderContext restore;
    if (hostAddr_ != nullptr || hostAddr == nullptr || size == 0 || gvaBase > UINT64_MAX - size || running_) {
        BM_LOG_ERROR("URMA provider invalid pool registration, size=" << size);
        return false;
    }
    hostAddr_ = hostAddr;
    size_ = size;
    gvaBase_ = gvaBase;
    rank_ = providerRank;
    for (auto &device : cards_) {
        if (!device->RegisterMemory(hostAddr, size)) {
            BM_LOG_ERROR("URMA provider pool registration failed, phyId=" << device->GetPhysicalId());
            return false;
        }
    }
    return true;
}
bool UrmaProviderControlServer::Start()
{
    if (listenFd_ < 0 || hostAddr_ == nullptr || running_)
        return false;
    for (const auto &card : cards_)
        if (!card->IsMemoryRegistered())
            return false;
    running_ = true;
    accepting_ = true;
    acceptThread_ = std::thread(&UrmaProviderControlServer::AcceptLoop, this);
    return true;
}
Result UrmaProviderControlServer::Send(const std::shared_ptr<Session> &session, const UrmaProviderMessage &message,
                                       ProviderDeadline deadline)
{
    std::lock_guard<std::mutex> lock(session->sendMutex);
    return SendUrmaProviderMessage(session->fd, message, deadline);
}
void UrmaProviderControlServer::AcceptLoop()
{
    while (accepting_) {
        pollfd item{listenFd_, POLLIN, 0};
        if (poll(&item, 1, 100) <= 0 || (item.revents & POLLIN) == 0)
            continue;
        uint64_t removalGeneration;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            removalGeneration = removalGeneration_;
        }
        int fd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        auto session = std::make_shared<Session>(fd);
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < accepted_.size();) {
            if (!accepted_[i]->ended) {
                ++i;
                continue;
            }
            workers_[i].join();
            accepted_.erase(accepted_.begin() + i);
            workers_.erase(workers_.begin() + i);
        }
        if (accepted_.size() >= static_cast<size_t>(options_.transport.rankCount) + 16U) {
            BM_LOG_ERROR("URMA provider pending connection limit reached");
            continue;
        }
        accepted_.push_back(session);
        workers_.emplace_back(&UrmaProviderControlServer::HandleClient, this, session, removalGeneration);
    }
}
void UrmaProviderControlServer::HandleClient(std::shared_ptr<Session> session, uint64_t removalGeneration)
{
    auto deadline = UrmaProviderDeadline();
    UrmaProviderMessage hello;
    Result ret = ReceiveUrmaProviderMessage(session->fd, hello, deadline);
    UrmaProviderMessage offer;
    offer.type = UrmaProviderMessageType::OFFER;
    if (ret == BM_OK && hello.type == UrmaProviderMessageType::HELLO && hello.status == BM_OK &&
        hello.protocols == HYBM_DOP_TYPE_DEVICE_URMA && hello.rank < options_.transport.rankCount &&
        hello.rank != rank_) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto removed = rankRemovalGenerations_.find(hello.rank);
        const bool expired = removed != rankRemovalGenerations_.end() && removalGeneration < removed->second;
        if (expired) {
            BM_LOG_ERROR("URMA provider stale handshake, rank=" << hello.rank
                                                                << " handshakeGeneration=" << removalGeneration
                                                                << " removedGeneration=" << removed->second);
        }
        if (accepting_ && !expired && sessions_.count(hello.rank) == 0) {
            std::vector<std::shared_ptr<UrmaProviderDevice>> candidates;
            for (auto &card : cards_) {
                if (card->Transport().CanConnectPeer(hello.privateData))
                    candidates.push_back(card);
            }
            if (!candidates.empty()) {
                session->rank = hello.rank;
                for (auto &card : candidates)
                    if (card->GetPhysicalId() == hello.phyId)
                        session->card = card;
                if (session->card == nullptr) {
                    session->card = candidates[hello.rank % candidates.size()];
                }
                sessions_[hello.rank] = session;
            }
        }
    }
    if (session->card == nullptr) {
        offer.status = BM_NOT_CONNECTED;
        Send(session, offer, deadline);
    } else {
        auto &manager = session->card->Transport();
        ret = UrmaProviderContext::SetCurrentContext(session->card->GetContext());
        offer.rank = rank_;
        offer.phyId = session->card->GetPhysicalId();
        offer.gvaBase = gvaBase_;
        offer.poolSize = size_;
        offer.privateData = manager.GetPrivateData();
        if (ret == BM_OK)
            ret = manager.QueryMemoryKey(reinterpret_cast<uint64_t>(hostAddr_), offer.memoryKey);
        HybmTransPrepareOptions prepare;
        prepare.options[hello.rank].privateData = hello.privateData;
        if (ret == BM_OK)
            ret = manager.Prepare(prepare);
        offer.status = ret;
        if (Send(session, offer, deadline) != BM_OK)
            ret = BM_ERROR;
        if (ret == BM_OK) {
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            ret = manager.WaitRankReady(hello.rank, remaining);
        }
        UrmaProviderMessage ready;
        if (ret == BM_OK)
            ret = ReceiveUrmaProviderMessage(session->fd, ready, deadline);
        if (ret == BM_OK && (ready.type != UrmaProviderMessageType::READY || ready.status != BM_OK))
            ret = BM_ERROR;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session->closing || !running_)
                ret = BM_NOT_CONNECTED;
            if (ret == BM_OK)
                session->exposed = true;
        }
        UrmaProviderMessage ack;
        ack.type = UrmaProviderMessageType::ACK;
        ack.status = ret;
        auto ackRet = Send(session, ack, deadline);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session->ready = ret == BM_OK && ackRet == BM_OK && !session->closing;
        }
        if (ret == BM_OK && ackRet == BM_OK) {
            while (running_) {
                pollfd item{session->fd, POLLIN, 0};
                if (poll(&item, 1, 100) <= 0)
                    continue;
                UrmaProviderMessage message;
                auto receiveRet = ReceiveUrmaProviderMessage(session->fd, message, UrmaProviderDeadline());
                if (receiveRet != BM_OK)
                    break;
                if (message.type != UrmaProviderMessageType::QUIESCED || message.status != BM_OK) {
                    BM_LOG_ERROR("URMA provider unexpected active control frame, rank=" << session->rank);
                    break;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                session->ready = false;
                session->closing = true;
                session->quiesced = true;
                changed_.notify_all();
                // QUIESCED is the final peer frame; RemoveRanks owns resource cleanup.
                break;
            }
        }
        if (!session->exposed) {
            manager.BeginRankDrain(hello.rank);
            auto cleanup = manager.RemoveRanks({hello.rank});
            std::lock_guard<std::mutex> lock(mutex_);
            session->quiesced = cleanup == BM_OK;
            auto current = sessions_.find(hello.rank);
            if (cleanup == BM_OK && !session->closing && current != sessions_.end() && current->second == session)
                sessions_.erase(current);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    session->ready = false;
    session->ended = true;
    changed_.notify_all();
}
Result UrmaProviderControlServer::RemoveRanks(const std::vector<uint32_t> &ranks)
{
    UrmaProviderContext restore;
    std::lock_guard<std::mutex> removeLock(removeMutex_);
    Result result = BM_OK;
    if (!ranks.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++removalGeneration_;
        for (auto rank : ranks) {
            rankRemovalGenerations_[rank] = removalGeneration_;
        }
    }
    for (auto rank : ranks) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(rank);
            if (it == sessions_.end())
                continue;
            session = it->second;
            session->ready = false;
            session->closing = true;
        }
        auto deadline = UrmaProviderDeadline();
        UrmaProviderMessage drain;
        drain.type = UrmaProviderMessageType::DRAIN;
        if (session->exposed && !session->quiesced && !session->ended)
            Send(session, drain, deadline);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait_until(lock, deadline, [&] { return session->quiesced || session->ended; });
            if (!session->quiesced && !session->ended) {
                BM_LOG_ERROR("URMA provider cannot prove peer drained, rank=" << rank);
                result = BM_NOT_CONNECTED;
                continue;
            }
        }
        // A disconnected peer cannot acknowledge DRAIN; successful transport cleanup must release its rank.
        if (UrmaProviderContext::SetCurrentContext(session->card->GetContext()) != BM_OK ||
            session->card->Transport().RemoveRanks({rank}) != BM_OK) {
            result = BM_ERROR;
            continue;
        }
        shutdown(session->fd, SHUT_RDWR);
        std::lock_guard<std::mutex> lock(mutex_);
        auto current = sessions_.find(rank);
        if (current != sessions_.end() && current->second == session)
            sessions_.erase(current);
    }
    return result;
}
std::vector<uint32_t> UrmaProviderControlServer::GetConnectedProtocols(uint32_t rankId)
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(rankId);
        if (it == sessions_.end() || !it->second->ready || it->second->closing ||
            !provider_control::SocketAlive(it->second->fd))
            return {};
        session = it->second;
    }
    UrmaProviderContext context(session->card->GetContext());
    if (!context.Valid())
        return {};
    auto protocols = session->card->Transport().GetConnectedProtocols(rankId);
    std::lock_guard<std::mutex> lock(mutex_);
    return session->ready && !session->closing ? protocols : std::vector<uint32_t>{};
}
std::vector<uint32_t> UrmaProviderControlServer::GetPeerRanks()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> result;
    for (const auto &entry : sessions_)
        result.push_back(entry.first);
    return result;
}
bool UrmaProviderControlServer::CleanupCards()
{
    if (!sessions_.empty())
        return false;
    for (auto &device : cards_) {
        if (device->Close() != BM_OK || !device->CleanupComplete()) {
            return false;
        }
    }
    cards_.clear();
    hostAddr_ = nullptr;
    size_ = 0;
    return true;
}
void UrmaProviderControlServer::Stop()
{
    UrmaProviderContext restore;
    std::lock_guard<std::mutex> stopLock(stopMutex_);
    accepting_ = false;
    if (listenFd_ >= 0)
        shutdown(listenFd_, SHUT_RDWR);
    if (acceptThread_.joinable())
        acceptThread_.join();
    auto ranks = GetPeerRanks();
    RemoveRanks(ranks);
    running_ = false;
    for (const auto &session : accepted_)
        shutdown(session->fd, SHUT_RDWR);
    for (auto &thread : workers_)
        if (thread.joinable())
            thread.join();
    workers_.clear();
    if (listenFd_ >= 0)
        close(listenFd_);
    listenFd_ = -1;
    CleanupCards();
    accepted_.clear();
}
bool UrmaProviderControlServer::CleanupComplete()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return cards_.empty() && sessions_.empty();
}
} // namespace ock::mf::transport::device
