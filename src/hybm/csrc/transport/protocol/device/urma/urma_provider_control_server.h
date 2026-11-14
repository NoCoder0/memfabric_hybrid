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

#pragma once
#include "provider_service.h"
#include "urma_provider_device.h"
#include "urma_provider_protocol.h"
#include <thread>
#include <mutex>
#include <condition_variable>
namespace ock::mf::transport::device {
class UrmaProviderControlServer final : public ProviderService {
public:
    ~UrmaProviderControlServer() override;
    bool Init(const ProviderServiceOptions &options) override;
    bool RegisterMemory(void *hostAddr, uint64_t size, uint64_t gvaBase, uint32_t providerRank) override;
    bool Start() override;
    void Stop() override;
    bool CleanupComplete() override;
    const std::string &GetControlUrl() const override
    {
        return controlUrl_;
    }
    Result RemoveRanks(const std::vector<uint32_t> &ranks) override;
    std::vector<uint32_t> GetConnectedProtocols(uint32_t rankId) override;
    std::vector<uint32_t> GetPeerRanks() override;

private:
    struct Session {
        explicit Session(int socket) : fd(socket) {}
        ~Session();
        int fd;
        uint32_t rank{UINT32_MAX};
        std::shared_ptr<UrmaProviderDevice> card;
        std::atomic<bool> ready{false};
        std::atomic<bool> closing{false};
        std::atomic<bool> exposed{false};
        std::atomic<bool> quiesced{false};
        std::atomic<bool> ended{false};
        std::mutex sendMutex;
    };
    void AcceptLoop();
    void HandleClient(std::shared_ptr<Session> session, uint64_t removalGeneration);
    Result Send(const std::shared_ptr<Session> &session, const UrmaProviderMessage &message, ProviderDeadline deadline);
    bool CleanupCards();
    ProviderServiceOptions options_{};
    std::vector<std::shared_ptr<UrmaProviderDevice>> cards_;
    std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions_;
    std::vector<std::shared_ptr<Session>> accepted_;
    uint64_t removalGeneration_{0};
    std::unordered_map<uint32_t, uint64_t> rankRemovalGenerations_;
    std::mutex mutex_;
    std::mutex stopMutex_;
    std::mutex removeMutex_;
    std::condition_variable changed_;
    std::vector<std::thread> workers_;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> accepting_{false};
    int listenFd_{-1};
    std::string controlUrl_;
    void *hostAddr_{nullptr};
    uint64_t size_{0};
    uint64_t gvaBase_{0};
    uint32_t rank_{0};
};
} // namespace ock::mf::transport::device
