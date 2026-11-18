/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include <mockcpp/ApiHook.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#define private   public
#define protected public
#include "vendor/ascend/dl_hal_api.h"
#include "hybm_networks_common.h"
#include "mf_scope_guard.h"
#include "provider_device_context.h"
#include "vendor/vendor_api.h"
#include "urma_provider_control_server.h"
#include "urma_proxy_transport_manager.h"
#undef private
#undef protected

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::device;

static AscendApiImpl &AscendRtApi()
{
    return *static_cast<AscendApiImpl *>(VendorApi::Api());
}

namespace {
std::atomic<bool> compatible{true};
std::atomic<bool> validKey{true};
std::atomic<bool> transportReady{true};
std::atomic<int> prepareResult{BM_OK};
std::atomic<int> cleanupResult{BM_OK};
std::atomic<uint16_t> preparedPort{UINT16_MAX};
std::atomic<uint64_t> importedBase{0};
std::atomic<uint32_t> reads{0};
std::atomic<uint32_t> writes{0};
constexpr uint64_t URMA_POOL_SIZE = 65536;
constexpr uint64_t URMA_POOL_GVA = 0x1000000;
constexpr uint64_t URMA_PROXY_GVA = 0x2000000;

bool Await(const std::function<bool()> &condition)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

class ProviderUrmaPathTest : public testing::Test {
protected:
    template<class F, class S>
    void Hook(F function, S stub)
    {
        hooks_.push_back(std::make_unique<MOCKCPP_NS::ApiHook>(reinterpret_cast<const void *>(function),
                                                               reinterpret_cast<const void *>(stub)));
    }

    // Worker threads call these stubs directly; mockcpp invocation recording is not thread safe.
    std::vector<std::unique_ptr<MOCKCPP_NS::ApiHook>> hooks_;
    aclrtGetCurrentContextFunc oldGetCurrentContext_{AscendRtApi().pAclrtGetCurrentContext_};
    aclrtSetCurrentContextFunc oldSetCurrentContext_{AscendRtApi().pAclrtSetCurrentContext_};

    void SetUp() override
    {
        GlobalMockObject::reset();
        compatible = validKey = transportReady = true;
        prepareResult = cleanupResult = BM_OK;
        preparedPort = UINT16_MAX;
        importedBase = reads = writes = 0;
        Hook(
            reinterpret_cast<std::vector<uint32_t> (*)()>(&NetworkGetIpAddresses),
            +[]() { return std::vector<uint32_t>{}; });
        AscendRtApi().pAclrtGetCurrentContext_ = +[](void **context) {
            *context = nullptr;
            return BM_OK;
        };
        AscendRtApi().pAclrtSetCurrentContext_ = +[](void *) { return BM_OK; };
        Hook(DlHcommApi::RefreshCurrentDevice, +[]() { return BM_OK; });
        Hook(
            DlHalApi::HalHostRegister, +[](void *addr, uint64_t, uint32_t, uint32_t, void **mapped) {
                *mapped = addr;
                return BM_OK;
            });
        Hook(DlHalApi::HalHostUnregisterEx, +[](void *, uint32_t, uint32_t) { return BM_OK; });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(UrmaProviderDevice *, uint32_t, const TransportOptions &, bool)>(
                &UrmaProviderDevice::Open),
            +[](UrmaProviderDevice *card, uint32_t id, const TransportOptions &, bool) {
                card->opened_ = true;
                card->phyId_ = id;
                card->manager_->localEndpoint_.push_back(reinterpret_cast<HcommEndpointHandle>(0x1234UL));
                card->manager_->localEndpointDesc_.resize(1);
                return BM_OK;
            });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *)>(
                &DeviceUrmaTransportManager::CloseDevice),
            +[](DeviceUrmaTransportManager *manager) {
                manager->localEndpoint_.clear();
                manager->localEndpointDesc_.clear();
                return BM_OK;
            });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *, const TransportMemoryRegion &,
                                                 uint64_t)>(&DeviceUrmaTransportManager::RegisterMappedHostMemory),
            +[](DeviceUrmaTransportManager *, const TransportMemoryRegion &, uint64_t) { return BM_OK; });
        Hook(
            reinterpret_cast<bool (*)(const DeviceUrmaTransportManager *, const TransportPrivateData &)>(
                &DeviceUrmaTransportManager::CanConnectPeer),
            +[](const DeviceUrmaTransportManager *, const TransportPrivateData &) { return compatible.load(); });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *, uint64_t, TransportMemoryKey &)>(
                &DeviceUrmaTransportManager::QueryMemoryKey),
            +[](DeviceUrmaTransportManager *, uint64_t, TransportMemoryKey &key) {
                key.ResizeWords(16);
                key.keys[0] = URMA_EXPORT_DESC_MAGIC;
                key.keys[1] = URMA_POOL_GVA;
                UrmaExportDesc desc{};
                desc.headerSize = sizeof(desc);
                desc.memoryType = UrmaMemoryType::HOST_DRAM;
                desc.size = validKey ? URMA_POOL_SIZE : URMA_POOL_SIZE + 1;
                desc.hcommDescLen = desc.deviceMarkerDescLen = 1;
                auto data = reinterpret_cast<uint8_t *>(&key.keys[DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS]);
                std::memcpy(data, &desc, sizeof(desc));
                data[sizeof(desc)] = data[sizeof(desc) + 1] = 1;
                return BM_OK;
            });
        Hook(
            reinterpret_cast<void (*)(DeviceUrmaTransportManager *, TransportMemoryKey &, void *)>(
                &DeviceUrmaTransportManager::UpdateMemoryKey),
            +[](DeviceUrmaTransportManager *, TransportMemoryKey &, void *base) {
                importedBase = reinterpret_cast<uint64_t>(base);
            });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *, const HybmTransPrepareOptions &,
                                                 uint16_t)>(
                static_cast<ock::mf::Result (DeviceUrmaTransportManager::*)(const HybmTransPrepareOptions &, uint16_t)>(
                    &DeviceUrmaTransportManager::Prepare)),
            +[](DeviceUrmaTransportManager *, const HybmTransPrepareOptions &options, uint16_t port) {
                preparedPort = port;
                return options.options.begin()->second.memKeys.empty() ? BM_OK : prepareResult.load();
            });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *, uint32_t, std::chrono::milliseconds)>(
                &DeviceUrmaTransportManager::WaitRankReady),
            +[](DeviceUrmaTransportManager *, uint32_t, std::chrono::milliseconds) { return BM_OK; });
        Hook(
            reinterpret_cast<std::vector<uint32_t> (*)(const DeviceUrmaTransportManager *, uint32_t)>(
                &DeviceUrmaTransportManager::GetConnectedProtocols),
            +[](const DeviceUrmaTransportManager *, uint32_t) {
                return transportReady ? std::vector<uint32_t>{HYBM_DOP_TYPE_DEVICE_URMA} : std::vector<uint32_t>{};
            });
        Hook(
            reinterpret_cast<void (*)(DeviceUrmaTransportManager *, uint32_t)>(
                &DeviceUrmaTransportManager::BeginRankDrain),
            +[](DeviceUrmaTransportManager *, uint32_t) {});
        Hook(
            reinterpret_cast<bool (*)(const DeviceUrmaTransportManager *, uint32_t)>(
                &DeviceUrmaTransportManager::IsRankDrained),
            +[](const DeviceUrmaTransportManager *, uint32_t) { return true; });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *, const std::vector<uint32_t> &)>(
                &DeviceUrmaTransportManager::RemoveRanks),
            +[](DeviceUrmaTransportManager *, const std::vector<uint32_t> &) { return cleanupResult.load(); });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *, uint32_t, uint64_t, uint64_t, uint64_t)>(
                &DeviceUrmaTransportManager::ReadRemoteAsync),
            +[](DeviceUrmaTransportManager *, uint32_t, uint64_t, uint64_t, uint64_t) {
                ++reads;
                return BM_OK;
            });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *, uint32_t, uint64_t, uint64_t, uint64_t)>(
                &DeviceUrmaTransportManager::WriteRemoteAsync),
            +[](DeviceUrmaTransportManager *, uint32_t, uint64_t, uint64_t, uint64_t) {
                ++writes;
                return BM_OK;
            });
        Hook(
            reinterpret_cast<ock::mf::Result (*)(DeviceUrmaTransportManager *, uint32_t)>(
                &DeviceUrmaTransportManager::Synchronize),
            +[](DeviceUrmaTransportManager *, uint32_t) { return BM_OK; });
    }

    void TearDown() override
    {
        AscendRtApi().pAclrtGetCurrentContext_ = oldGetCurrentContext_;
        AscendRtApi().pAclrtSetCurrentContext_ = oldSetCurrentContext_;
        hooks_.clear();
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    void StartProvider(UrmaProviderControlServer &server)
    {
        ProviderServiceOptions options{};
        options.userDeviceIds = {0, 1};
        options.transport.rankCount = 4;
        ASSERT_TRUE(server.Init(options));
        ASSERT_TRUE(server.RegisterMemory(reinterpret_cast<void *>(0x3000000UL), URMA_POOL_SIZE, URMA_POOL_GVA, 0));
        ASSERT_TRUE(server.Start());
    }

    void PrepareProxy(UrmaProviderControlServer &server, UrmaProxyTransportManager &proxy, uint32_t rank = 1)
    {
        TransportOptions options{};
        options.rankId = rank;
        options.rankCount = 4;
        ASSERT_EQ(proxy.OpenDevice(options), BM_OK);
        HybmTransPrepareOptions prepare;
        prepare.options[0].isProvider = true;
        prepare.options[0].nic = server.GetControlUrl();
        prepare.options[0].providerGvaBase = URMA_PROXY_GVA;
        ASSERT_EQ(proxy.Prepare(prepare), BM_OK);
    }
};

TEST_F(ProviderUrmaPathTest, RealControlHandshakeKeepsDeviceChannelPortIndependentOfTcpPort)
{
    UrmaProviderControlServer server;
    StartProvider(server);
    UrmaProxyTransportManager proxy;
    PrepareProxy(server, proxy);
    ASSERT_EQ(proxy.ConnectRank(0), BM_OK);
    EXPECT_EQ(importedBase.load(), URMA_PROXY_GVA);
    EXPECT_EQ(preparedPort.load(), 0U);
    EXPECT_EQ(proxy.ConnectRank(0), BM_OK);
    ASSERT_TRUE(Await([&] { return !server.GetConnectedProtocols(1).empty(); }));
    EXPECT_EQ(server.GetPeerRanks(), (std::vector<uint32_t>{1}));
    EXPECT_EQ(proxy.ReadRemote(0, 1, 2, 8), BM_OK);
    EXPECT_EQ(proxy.WriteRemote(0, 1, 2, 8), BM_OK);
    EXPECT_EQ(reads.load(), 1U);
    EXPECT_EQ(writes.load(), 1U);
    transportReady = false;
    EXPECT_TRUE(proxy.GetConnectedProtocols(0).empty());
    EXPECT_TRUE(server.GetConnectedProtocols(1).empty());
    EXPECT_EQ(proxy.ReadRemoteAsync(0, 1, 2, 8), BM_NOT_CONNECTED);
    EXPECT_EQ(reads.load(), 1U);
    transportReady = true;
    EXPECT_EQ(server.RemoveRanks({1}), BM_OK);
    EXPECT_TRUE(server.GetPeerRanks().empty());
    EXPECT_TRUE(proxy.GetConnectedProtocols(0).empty());
    EXPECT_EQ(proxy.CloseDevice(), BM_OK);
    server.Stop();
    EXPECT_TRUE(server.CleanupComplete());
}

TEST_F(ProviderUrmaPathTest, SameRankRejoinsTwiceWithoutDrainingSurvivingRank)
{
    UrmaProviderControlServer server;
    StartProvider(server);
    UrmaProxyTransportManager survivor;
    PrepareProxy(server, survivor, 2);
    ASSERT_EQ(survivor.ConnectRank(0), BM_OK);
    for (int attempt = 0; attempt != 3; ++attempt) {
        UrmaProxyTransportManager proxy;
        PrepareProxy(server, proxy);
        ASSERT_EQ(proxy.ConnectRank(0), BM_OK);
        ASSERT_TRUE(Await([&] { return !server.GetConnectedProtocols(1).empty(); }));
        EXPECT_EQ(proxy.RemoveRanks({0}), BM_OK);
        EXPECT_EQ(server.RemoveRanks({1}), BM_OK);
        EXPECT_EQ(survivor.WriteRemote(0, 1, 2, 8), BM_OK);
        EXPECT_FALSE(server.GetConnectedProtocols(2).empty());
    }
}

TEST_F(ProviderUrmaPathTest, IncompatibleEndpointAndMalformedPoolOfferNeverExposeLink)
{
    for (bool mismatch : {true, false}) {
        compatible = !mismatch;
        validKey = mismatch;
        UrmaProviderControlServer server;
        StartProvider(server);
        UrmaProxyTransportManager proxy;
        PrepareProxy(server, proxy);
        EXPECT_EQ(proxy.ConnectRank(0), BM_INVALID_PARAM);
        EXPECT_TRUE(proxy.GetConnectedProtocols(0).empty());
        EXPECT_EQ(proxy.WriteRemoteAsync(0, 1, 2, 8), BM_NOT_CONNECTED);
        EXPECT_EQ(writes.load(), 0U);
        EXPECT_EQ(proxy.CloseDevice(), BM_OK);
    }
}

TEST_F(ProviderUrmaPathTest, FailedPrepareCleansUpAndAllowsFreshHandshake)
{
    UrmaProviderControlServer server;
    StartProvider(server);
    UrmaProxyTransportManager proxy;
    PrepareProxy(server, proxy);
    prepareResult = BM_ERROR;
    EXPECT_EQ(proxy.ConnectRank(0), BM_ERROR);
    EXPECT_TRUE(proxy.links_.empty());
    EXPECT_TRUE(Await([&] { return server.GetPeerRanks().empty(); }));
    prepareResult = BM_OK;
    EXPECT_EQ(proxy.ConnectRank(0), BM_OK);
    EXPECT_EQ(proxy.WriteRemote(0, 1, 2, 8), BM_OK);
}

TEST_F(ProviderUrmaPathTest, FailedRankCleanupIsRetriedBeforeClosingDevice)
{
    UrmaProviderControlServer server;
    StartProvider(server);
    UrmaProxyTransportManager proxy;
    PrepareProxy(server, proxy);
    ASSERT_EQ(proxy.ConnectRank(0), BM_OK);
    cleanupResult = BM_ERROR;
    EXPECT_EQ(proxy.RemoveRanks({0}), BM_ERROR);
    EXPECT_FALSE(proxy.links_.empty());
    EXPECT_TRUE(proxy.GetConnectedProtocols(0).empty());
    cleanupResult = BM_OK;
    EXPECT_EQ(proxy.CloseDevice(), BM_OK);
    EXPECT_TRUE(proxy.links_.empty());
    EXPECT_EQ(proxy.CloseDevice(), BM_OK);
    EXPECT_EQ(proxy.UnregisterMemoryRegion(1), BM_OK);
    EXPECT_EQ(server.RemoveRanks({1}), BM_OK);
}

TEST_F(ProviderUrmaPathTest, ProviderRegistrationAndPrepareRejectInvalidState)
{
    UrmaProviderControlServer server;
    EXPECT_FALSE(server.Start());
    EXPECT_FALSE(server.RegisterMemory(nullptr, 1, 0, 0));
    EXPECT_FALSE(server.RegisterMemory(reinterpret_cast<void *>(1UL), 2, UINT64_MAX, 0));
    ProviderServiceOptions options{};
    EXPECT_FALSE(server.Init(options));
    EXPECT_TRUE(server.CleanupComplete());
    StartProvider(server);
    EXPECT_FALSE(server.Start());
    EXPECT_FALSE(server.RegisterMemory(reinterpret_cast<void *>(1UL), 1, 0, 0));
    UrmaProxyTransportManager proxy;
    PrepareProxy(server, proxy);
    EXPECT_EQ(proxy.ConnectRank(3), BM_NOT_INITIALIZED);
    EXPECT_EQ(proxy.OpenDevice({}), BM_ERROR);
    HybmTransPrepareOptions prepare;
    prepare.options[0].isProvider = true;
    prepare.options[0].providerControlUrl = "invalid";
    EXPECT_EQ(proxy.UpdateRankOptions(prepare), BM_INVALID_PARAM);
}

TEST_F(ProviderUrmaPathTest, RemovalGenerationRejectsAnEarlierAcceptedHandshake)
{
    UrmaProviderControlServer server;
    StartProvider(server);
    EXPECT_EQ(server.RemoveRanks({1}), BM_OK);
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    auto guard = make_scope_guard(sockets[0], [](int fd) { close(fd); });
    auto session = std::make_shared<UrmaProviderControlServer::Session>(sockets[1]);
    UrmaProviderMessage hello;
    hello.rank = 1;
    hello.privateData.key.ResizeWords(1);
    hello.privateData.key.keys[0] = 1;
    auto deadline = UrmaProviderDeadline();
    ASSERT_EQ(SendUrmaProviderMessage(sockets[0], hello, deadline), BM_OK);
    server.HandleClient(session, 0);
    UrmaProviderMessage offer;
    ASSERT_EQ(ReceiveUrmaProviderMessage(sockets[0], offer, deadline), BM_OK);
    EXPECT_EQ(offer.type, UrmaProviderMessageType::OFFER);
    EXPECT_EQ(offer.status, BM_NOT_CONNECTED);
    EXPECT_TRUE(session->ended);
    EXPECT_TRUE(server.GetPeerRanks().empty());
}
} // namespace
