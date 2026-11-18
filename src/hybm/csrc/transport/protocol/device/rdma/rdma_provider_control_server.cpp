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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hybm_logger.h"
#include "hybm_networks_common.h"
#include "mf_scope_guard.h"
#include "provider_control_io.h"
#include "rdma_provider_control_server.h"
#include "rdma_provider_device.h"
#include "rdma_provider_link.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

RdmaProviderControlServer::~RdmaProviderControlServer()
{
    Stop();
    for (auto &item : qps_) {
        close(item.second.fd);
    }
}

bool RdmaProviderControlServer::Init(const ProviderServiceOptions &options)
{
    Stop();
    if (!devices_.empty()) {
        BM_LOG_ERROR("provider cannot initialize while memory cleanup is pending");
        return false;
    }
    stopping_.store(false);
    rankCount_ = options.transport.rankCount;
    auto listenPort = options.listenPort;
    if (!provider_control::ResolveListenPort(listenPort)) {
        BM_LOG_ERROR("provider control port must be an integer in [1, 65535]");
        return false;
    }
    auto rollback = make_scope_guard(this, [](RdmaProviderControlServer *server) { server->Stop(); });
    for (auto id : options.userDeviceIds) {
        auto dev = std::make_unique<RdmaProviderDevice>();
        if (!dev->Open(id, options.transport.rankCount)) {
            BM_LOG_WARN("provider open device " << id << " failed, skip");
            continue;
        }
        if (phyToDevice_.count(dev->phyId) != 0) {
            continue;
        }
        phyToDevice_[dev->phyId] = dev.get();
        devices_.push_back(std::move(dev));
    }
    if (devices_.empty()) {
        BM_LOG_ERROR("provider has no usable NPU");
        return false;
    }
    listenFd_ = provider_control::CreateListenSocket(listenPort);
    if (listenFd_ < 0) {
        BM_LOG_ERROR("RDMA provider listen failed, port=" << listenPort << " errno=" << errno);
        return false;
    }
    auto ips = NetworkGetIpAddresses();
    std::string ip = "127.0.0.1";
    if (!ips.empty()) {
        in_addr addr{};
        addr.s_addr = htonl(ips.front());
        ip = inet_ntoa(addr);
    }
    controlUrl_ = ip + ":" + std::to_string(listenPort);
    BM_LOG_INFO("RDMA provider control server listening at " << controlUrl_);
    rollback.release();
    return true;
}

bool RdmaProviderControlServer::RegisterMemory(void *hostAddr, uint64_t size, uint64_t gvaBase, uint32_t providerRank)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (memoryReady_.load() || devices_.empty()) {
        BM_LOG_ERROR("provider memory registration requires an initialized, unregistered server");
        return false;
    }
    for (auto &dev : devices_) {
        if (!dev->RegisterMemory(hostAddr, size)) {
            for (auto &opened : devices_) {
                opened->UnregisterMemory();
            }
            return false;
        }
    }
    size_ = size;
    gvaBase_ = gvaBase;
    providerRank_ = providerRank;
    memoryReady_.store(true, std::memory_order_release);
    return true;
}

bool RdmaProviderControlServer::Start()
{
    std::lock_guard<std::mutex> lock(stopMutex_);
    if (listenFd_ < 0) {
        BM_LOG_ERROR("provider control server has no listening socket");
        return false;
    }
    if (running_.exchange(true)) {
        return true;
    }
    stopping_.store(false);
    acceptThread_ = std::thread([this]() { AcceptLoop(); });
    return true;
}

bool RdmaProviderControlServer::CleanupComplete()
{
    std::lock_guard<std::mutex> stopLock(stopMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    return qps_.empty() && devices_.empty();
}

Result RdmaProviderControlServer::RemoveRanks(const std::vector<uint32_t> &ranks)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ranks.empty()) {
        ++removalGeneration_;
    }
    Result result = BM_OK;
    for (auto rank : ranks) {
        rankRemovalGenerations_[rank] = removalGeneration_;
        peerRanks_.erase(rank);
        if (handshakeFd_ >= 0 && handshakeRank_ == rank) {
            handshakeCancelled_ = true;
            (void)shutdown(handshakeFd_, SHUT_RDWR);
        }
        auto it = qps_.find(rank);
        if (it == qps_.end()) {
            continue;
        }
        (void)shutdown(it->second.fd, SHUT_RDWR);
        if (!it->second.device->TryDestroyQp(it->second.qp)) {
            BM_LOG_ERROR("provider remove QP failed, proxyRank=" << rank);
            result = BM_ERROR;
            continue;
        }
        close(it->second.fd);
        qps_.erase(it);
    }
    return result;
}

std::vector<uint32_t> RdmaProviderControlServer::GetConnectedProtocols(uint32_t rankId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = qps_.find(rankId);
    if (!memoryReady_.load() || it == qps_.end()) {
        return {};
    }
    const auto &link = it->second;
    if (link.qp == nullptr || !RdmaProviderLinkReady(*link.device, link.qp, link.fd)) {
        return {};
    }
    return {HYBM_DOP_TYPE_DEVICE_RDMA};
}

void RdmaProviderControlServer::Stop()
{
    std::lock_guard<std::mutex> stopLock(stopMutex_);
    if (stopping_.exchange(true)) {
        return;
    }
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (listenFd_ >= 0) {
            (void)shutdown(listenFd_, SHUT_RDWR);
        }
        if (handshakeFd_ >= 0) {
            (void)shutdown(handshakeFd_, SHUT_RDWR);
        }
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    for (auto it = qps_.begin(); it != qps_.end();) {
        (void)shutdown(it->second.fd, SHUT_RDWR);
        if (!it->second.device->TryDestroyQp(it->second.qp)) {
            it->second.device->cleanupBlocked_ = true;
            ++it;
            continue;
        }
        close(it->second.fd);
        it = qps_.erase(it);
    }
    if (!qps_.empty()) {
        memoryReady_.store(false);
        return;
    }
    for (auto it = devices_.begin(); it != devices_.end();) {
        (*it)->Close();
        if ((*it)->context != nullptr) {
            // Keep backing registered when device teardown could not complete.
            ++it;
        } else {
            it = devices_.erase(it);
        }
    }
    phyToDevice_.clear();
    memoryReady_.store(false);
    size_ = 0;
    controlUrl_.clear();
    peerRanks_.clear();
}

void RdmaProviderControlServer::RemoveDisconnectedClients()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = qps_.begin(); it != qps_.end();) {
        if (!provider_control::SocketIdle(it->second.fd)) {
            BM_LOG_INFO("RDMA provider peer control session ended, proxyRank=" << it->first);
            if (!it->second.device->TryDestroyQp(it->second.qp)) {
                ++it;
                continue;
            }
            close(it->second.fd);
            it = qps_.erase(it);
        } else {
            ++it;
        }
    }
}

void RdmaProviderControlServer::AcceptLoop()
{
    while (running_.load()) {
        RemoveDisconnectedClients();
        pollfd listener{listenFd_, POLLIN, 0};
        auto ret = poll(&listener, 1, 100);
        if (!running_.load()) {
            break;
        }
        if (ret <= 0 || (listener.revents & POLLIN) == 0) {
            continue;
        }
        uint64_t removalGeneration;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            removalGeneration = removalGeneration_;
        }
        int fd = accept(listenFd_, nullptr, nullptr);
        if (fd >= 0) {
            HandleClient(fd, removalGeneration);
        }
    }
}

RdmaProviderDevice *RdmaProviderControlServer::SelectDevice(uint32_t proxyPhyId, uint32_t proxyRank)
{
    auto it = phyToDevice_.find(proxyPhyId);
    if (it != phyToDevice_.end()) {
        return it->second;
    }
    if (devices_.empty()) {
        return nullptr;
    }
    return devices_[proxyRank % devices_.size()].get();
}

void RdmaProviderControlServer::HandleClient(int fd, uint64_t removalGeneration)
{
    const auto deadline = std::chrono::steady_clock::now() + HANDSHAKE_TIMEOUT;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load()) {
            close(fd);
            return;
        }
        handshakeFd_ = fd;
        handshakeRank_ = UINT32_MAX;
        handshakeCancelled_ = false;
    }
    auto socketGuard = make_scope_guard(fd, [this](int value) {
        std::lock_guard<std::mutex> lock(mutex_);
        handshakeFd_ = -1;
        handshakeRank_ = UINT32_MAX;
        close(value);
    });
    RdmaProviderLinkHello hello{};
    if (!RecvStruct(fd, hello, deadline) || hello.magic != RDMA_PROVIDER_LINK_MAGIC ||
        hello.version != RDMA_PROVIDER_LINK_VERSION || hello.reserved > RDMA_PROVIDER_LINK_NOTIFY_COMPLETION) {
        BM_LOG_ERROR("RDMA provider invalid or incomplete HELLO, fd=" << fd << " errno=" << errno);
        return;
    }
    RdmaProviderLinkHelloAck ack{};
    RdmaProviderDevice *dev;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handshakeCancelled_) {
            return;
        }
        // 成员准入：只拒绝被移除过的 rank 是不够的，越界/自身 rank 必须在交换 QP/MR 之前挡掉。
        if (rankCount_ == 0 || hello.proxyRank >= rankCount_ || hello.proxyRank == providerRank_) {
            BM_LOG_ERROR("RDMA provider invalid HELLO, providerRank="
                         << providerRank_ << " proxyRank=" << hello.proxyRank << " rankCount=" << rankCount_);
            return;
        }
        auto removed = rankRemovalGenerations_.find(hello.proxyRank);
        if (removed != rankRemovalGenerations_.end() && removalGeneration < removed->second) {
            BM_LOG_ERROR("RDMA provider stale handshake, proxyRank=" << hello.proxyRank
                                                                     << " handshakeGeneration=" << removalGeneration
                                                                     << " removedGeneration=" << removed->second);
            return;
        }
        handshakeRank_ = hello.proxyRank;
        peerRanks_.insert(hello.proxyRank);
        dev = SelectDevice(hello.proxyPhyId, hello.proxyRank);
        ack = {RDMA_PROVIDER_LINK_MAGIC,
               RDMA_PROVIDER_LINK_VERSION,
               dev != nullptr && memoryReady_.load(std::memory_order_acquire) ? 0 : -1,
               dev != nullptr ? dev->phyId : 0,
               providerRank_,
               gvaBase_,
               size_};
    }
    if (!SendStruct(fd, ack, deadline) || ack.status != 0) {
        BM_LOG_ERROR("RDMA provider HELLO acknowledgement failed, proxyRank=" << hello.proxyRank
                                                                              << " status=" << ack.status);
        return;
    }
    void *qpHandle = nullptr;
    TypicalQp localQp{};
    if (!dev->CreateQp(qpHandle, localQp)) {
        return;
    }
    auto qpGuard = make_scope_guard(qpHandle, [dev](void *value) { dev->DestroyQp(value); });
    auto localInfo = ToLinkQp(localQp, RDMA_PROVIDER_LINK_MAGIC);
    RdmaProviderLinkQpInfo remoteInfo{};
    if (!SendStruct(fd, localInfo, deadline) || !RecvStruct(fd, remoteInfo, deadline) ||
        remoteInfo.magic != RDMA_PROVIDER_LINK_MAGIC) {
        BM_LOG_ERROR("RDMA provider QP exchange failed, proxyRank=" << hello.proxyRank << " errno=" << errno);
        return;
    }
    TypicalQp remoteQp{};
    FromLinkQp(remoteInfo, remoteQp);
    if (!dev->ModifyQp(qpHandle, localQp, remoteQp)) {
        return;
    }
    RdmaProviderLinkMrInfo mr{RDMA_PROVIDER_LINK_MAGIC,
                              0,
                              reinterpret_cast<uint64_t>(dev->dvaBase),
                              dev->mrSize,
                              gvaBase_,
                              dev->mrInfo.rkey,
                              dev->phyId,
                              0,
                              0,
                              0};
    if (hello.reserved == RDMA_PROVIDER_LINK_NOTIFY_COMPLETION &&
        !dev->GetNotifySource(mr.notifySourceAddr, mr.notifySourceRkey)) {
        return;
    }
    RdmaProviderLinkDone done{};
    if (!SendStruct(fd, mr, deadline) || !RecvStruct(fd, done, deadline) || done.magic != RDMA_PROVIDER_LINK_MAGIC ||
        done.status != 0) {
        BM_LOG_ERROR("RDMA provider MR acknowledgement failed, proxyRank=" << hello.proxyRank << " errno=" << errno);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load() || handshakeCancelled_) {
            return;
        }
        auto old = qps_.find(hello.proxyRank);
        if (old != qps_.end()) {
            if (!old->second.device->TryDestroyQp(old->second.qp)) {
                return;
            }
            close(old->second.fd);
        }
        qps_[hello.proxyRank] = {dev, qpHandle, fd};
        handshakeFd_ = -1;
    }
    qpGuard.release();
    socketGuard.release();
    BM_LOG_INFO("RDMA provider link established, providerRank="
                << ack.providerRank << " proxyRank=" << hello.proxyRank << " selectedPhy=" << dev->phyId
                << " remotePhy=" << hello.proxyPhyId << " tc=" << localQp.tc << " sl=" << localQp.sl);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
