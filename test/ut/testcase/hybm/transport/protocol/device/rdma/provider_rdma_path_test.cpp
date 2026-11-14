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
#include <limits>
#include <thread>
#include <vector>

#include "rdma_provider_completion.h"
#include "dl_hal_api.h"
#include "dl_hccp_api.h"
#include "hybm_networks_common.h"
#include "mf_scope_guard.h"
#include "rdma_provider_link.h"

#define private public
#include "rdma_provider_control_server.h"
#include "rdma_proxy_transport_manager.h"
#undef private

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::device;

namespace {
constexpr uint64_t POOL_SIZE = 65536;
constexpr uint64_t PROVIDER_GVA = 0x100000;
constexpr uint64_t PROXY_GVA = 0x200000;
constexpr uint64_t LOCAL_HVA = 0x300000;
constexpr uint64_t LOCAL_DVA = 0x400000;
constexpr uint64_t REMOTE_DVA = 0x500000;
std::atomic<uint32_t> nextQp{0};
std::atomic<uint32_t> submitted{0};
std::atomic<uint32_t> waited{0};
std::atomic<bool> submitOk{true};
std::atomic<bool> waitOk{true};
std::atomic<bool> destroyOk{true};
uint64_t lastLocal = 0;
uint64_t lastRemote = 0;
bool lastWrite = false;

class ProviderRdmaPathTest : public testing::Test {
protected:
    template<class F, class S>
    void Hook(F function, S stub)
    {
        hooks_.push_back(std::make_unique<MOCKCPP_NS::ApiHook>(reinterpret_cast<const void *>(function),
                                                               reinterpret_cast<const void *>(stub)));
    }

    // Worker threads call these stubs directly; mockcpp invocation recording is not thread safe.
    std::vector<std::unique_ptr<MOCKCPP_NS::ApiHook>> hooks_;

    void SetUp() override
    {
        GlobalMockObject::reset();
        nextQp = 0;
        submitted = 0;
        waited = 0;
        submitOk = waitOk = destroyOk = true;
        Hook(
            reinterpret_cast<std::vector<uint32_t> (*)()>(&NetworkGetIpAddresses),
            +[]() { return std::vector<uint32_t>{}; });
        Hook(DlAclApi::AclrtSetDevice, +[](int32_t) { return BM_OK; });
        Hook(
            DlHccpApi::RaGetQpStatus, +[](void *, int &status) {
                status = 1;
                return BM_OK;
            });
        Hook(DlHccpApi::RaQpDestroy, +[](void *) { return destroyOk ? BM_OK : BM_ERROR; });
        Hook(DlHccpApi::RaDeregisterMR, +[](const void *, void *) { return BM_OK; });
        Hook(DlHalApi::HalHostUnregisterEx, +[](void *, uint32_t, uint32_t) { return BM_OK; });
        Hook(
            reinterpret_cast<bool (*)(RdmaProviderDevice *, uint32_t, uint32_t)>(&RdmaProviderDevice::Open),
            +[](RdmaProviderDevice *device, uint32_t id, uint32_t) {
                device->userId = device->phyId = id;
                device->context = std::make_shared<RdmaProviderDeviceContext>();
                return true;
            });
        Hook(
            reinterpret_cast<bool (*)(RdmaProviderDevice *, void *, uint64_t)>(&RdmaProviderDevice::RegisterMemory),
            +[](RdmaProviderDevice *device, void *, uint64_t size) {
                device->dvaBase = reinterpret_cast<void *>(REMOTE_DVA);
                device->mrSize = size;
                device->mrInfo.rkey = 23;
                return true;
            });
        Hook(
            reinterpret_cast<bool (*)(RdmaProviderDevice *, void *, uint64_t, bool, HccpMrInfo &, void *&)>(
                &RdmaProviderDevice::RegisterMr),
            +[](RdmaProviderDevice *, void *, uint64_t size, bool, HccpMrInfo &info, void *&handle) {
                info.addr = reinterpret_cast<void *>(LOCAL_DVA);
                info.size = size;
                info.lkey = 17;
                info.rkey = 19;
                handle = reinterpret_cast<void *>(0x1234UL);
                return true;
            });
        Hook(
            reinterpret_cast<bool (*)(RdmaProviderDevice *, void *&, TypicalQp &)>(&RdmaProviderDevice::CreateQp),
            +[](RdmaProviderDevice *, void *&handle, TypicalQp &qp) {
                qp.qpn = ++nextQp;
                handle = reinterpret_cast<void *>(static_cast<uintptr_t>(qp.qpn));
                return true;
            });
        Hook(
            reinterpret_cast<bool (*)(RdmaProviderDevice *, void *, TypicalQp &, TypicalQp &)>(
                &RdmaProviderDevice::ModifyQp),
            +[](RdmaProviderDevice *, void *, TypicalQp &, TypicalQp &) { return true; });
        Hook(
            reinterpret_cast<bool (*)(RdmaProviderDevice *, uint64_t &, uint32_t &)>(
                &RdmaProviderDevice::GetNotifySource),
            +[](RdmaProviderDevice *, uint64_t &addr, uint32_t &key) {
                addr = 0x600000;
                key = 29;
                return true;
            });
        Hook(
            reinterpret_cast<bool (*)(RdmaProviderDevice *, void *, uint64_t, uint32_t, uint64_t, uint32_t, uint64_t,
                                      bool)>(&RdmaProviderDevice::SubmitTransfer),
            +[](RdmaProviderDevice *, void *, uint64_t local, uint32_t lkey, uint64_t remote, uint32_t rkey, uint64_t,
                bool write) {
                ++submitted;
                lastLocal = local;
                lastRemote = remote;
                lastWrite = write;
                return submitOk && lkey == 17 && rkey == 23;
            });
        Hook(
            reinterpret_cast<bool (*)(RdmaProviderDevice *, void *, uint64_t, uint32_t)>(
                &RdmaProviderDevice::WaitTransfer),
            +[](RdmaProviderDevice *, void *, uint64_t addr, uint32_t key) {
                ++waited;
                return waitOk && addr == 0x600000 && key == 29;
            });
    }

    void TearDown() override
    {
        hooks_.clear();
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    void StartProvider(RdmaProviderControlServer &server)
    {
        ProviderServiceOptions options{};
        options.userDeviceIds = {0, 1};
        options.transport.rankCount = 4;
        ASSERT_TRUE(server.Init(options));
        ASSERT_TRUE(server.RegisterMemory(reinterpret_cast<void *>(0x800000UL), POOL_SIZE, PROVIDER_GVA, 0));
        ASSERT_TRUE(server.Start());
    }

    void ConnectProxy(RdmaProviderControlServer &server, RdmaProxyTransportManager &proxy, uint32_t rank = 1)
    {
        TransportOptions options{};
        options.rankId = rank;
        options.rankCount = 4;
        options.deviceId = rank;
        ASSERT_EQ(proxy.OpenDevice(options), BM_OK);
        HybmTransPrepareOptions prepare;
        auto &peer = prepare.options[0];
        peer.isProvider = true;
        peer.nic = server.GetControlUrl();
        peer.providerGvaBase = PROXY_GVA;
        ASSERT_EQ(proxy.Prepare(prepare), BM_OK);
        ASSERT_EQ(proxy.ConnectRank(0), BM_OK);
        TransportMemoryRegion region{};
        region.addr = LOCAL_HVA;
        region.size = POOL_SIZE;
        region.flags = REG_MR_FLAG_DRAM;
        ASSERT_EQ(proxy.RegisterMemoryRegion(region), BM_OK);
    }
};

TEST_F(ProviderRdmaPathTest, HandshakeUsesProxyGvaAndReconnectsSameRankTwice)
{
    RdmaProviderControlServer server;
    StartProvider(server);
    RdmaProxyTransportManager survivor;
    ConnectProxy(server, survivor, 2);
    for (int attempt = 0; attempt != 3; ++attempt) {
        RdmaProxyTransportManager proxy;
        ConnectProxy(server, proxy);
        EXPECT_EQ(proxy.links_.at(0).providerGvaBase, PROXY_GVA);
        EXPECT_EQ(proxy.links_.at(0).npuAddrBase, REMOTE_DVA);
        EXPECT_EQ(proxy.ConnectRank(0), BM_OK);
        EXPECT_EQ(proxy.WriteRemote(0, LOCAL_HVA + 16, PROXY_GVA + 32, 64), BM_OK);
        EXPECT_EQ(lastLocal, LOCAL_DVA + 16);
        EXPECT_EQ(lastRemote, REMOTE_DVA + 32);
        EXPECT_TRUE(lastWrite);
        EXPECT_EQ(proxy.ReadRemote(0, LOCAL_HVA + 24, PROXY_GVA + 40, 32), BM_OK);
        EXPECT_FALSE(lastWrite);
        EXPECT_EQ(proxy.RemoveRanks({0}), BM_OK);
        EXPECT_EQ(server.RemoveRanks({1}), BM_OK);
        EXPECT_TRUE(proxy.GetConnectedProtocols(0).empty());
        EXPECT_EQ(survivor.GetConnectedProtocols(0), (std::vector<uint32_t>{HYBM_DOP_TYPE_DEVICE_RDMA}));
        EXPECT_EQ(survivor.WriteRemote(0, LOCAL_HVA, PROXY_GVA, 8), BM_OK);
        EXPECT_EQ(server.GetConnectedProtocols(2), (std::vector<uint32_t>{HYBM_DOP_TYPE_DEVICE_RDMA}));
    }
    EXPECT_EQ(nextQp.load(), 8U);
    EXPECT_EQ(survivor.CloseDevice(), BM_OK);
    server.Stop();
    EXPECT_TRUE(server.CleanupComplete());
}

TEST_F(ProviderRdmaPathTest, AsyncAndBatchTransfersWaitOncePerSubmissionGroup)
{
    RdmaProviderControlServer server;
    StartProvider(server);
    RdmaProxyTransportManager proxy;
    ConnectProxy(server, proxy);
    EXPECT_EQ(proxy.WriteRemoteAsync(0, LOCAL_HVA, PROXY_GVA, 8), BM_OK);
    EXPECT_EQ(proxy.ReadRemoteAsync(0, LOCAL_HVA + 8, PROXY_GVA + 8, 8), BM_OK);
    EXPECT_EQ(waited.load(), 0U);
    EXPECT_EQ(proxy.Synchronize(0), BM_OK);
    EXPECT_EQ(proxy.Synchronize(0), BM_OK);
    EXPECT_EQ(waited.load(), 1U);
    CopyDescriptor descriptor{};
    descriptor.localAddrs.assign(1025, reinterpret_cast<void *>(LOCAL_HVA));
    descriptor.globalAddrs.assign(1025, reinterpret_cast<void *>(PROXY_GVA));
    descriptor.counts.assign(1025, 8);
    EXPECT_EQ(proxy.WriteRemoteBatchAsync(0, descriptor), BM_OK);
    EXPECT_EQ(submitted.load(), 1027U);
    EXPECT_EQ(waited.load(), 3U);
    descriptor.localAddrs.resize(1);
    descriptor.globalAddrs.resize(1);
    descriptor.counts.resize(1);
    EXPECT_EQ(proxy.ReadRemoteBatchAsync(0, descriptor), BM_OK);
    EXPECT_FALSE(lastWrite);
    EXPECT_FALSE(proxy.links_.at(0).pendingWait);
}

TEST_F(ProviderRdmaPathTest, InvalidRangesAreRejectedBeforeSubmittingAnyWr)
{
    RdmaProviderControlServer server;
    StartProvider(server);
    RdmaProxyTransportManager proxy;
    ConnectProxy(server, proxy);
    EXPECT_EQ(proxy.WriteRemote(0, LOCAL_HVA - 1, PROXY_GVA, 8), BM_INVALID_PARAM);
    EXPECT_EQ(proxy.ReadRemote(0, LOCAL_HVA, PROXY_GVA + POOL_SIZE - 4, 8), BM_INVALID_PARAM);
    EXPECT_EQ(proxy.WriteRemote(3, LOCAL_HVA, PROXY_GVA, 8), BM_ERROR);
    CopyDescriptor descriptor{};
    descriptor.localAddrs = {reinterpret_cast<void *>(LOCAL_HVA)};
    EXPECT_EQ(proxy.ReadRemoteBatchAsync(0, descriptor), BM_INVALID_PARAM);
    descriptor.globalAddrs = {reinterpret_cast<void *>(PROXY_GVA)};
    descriptor.counts = {POOL_SIZE + 1};
    EXPECT_EQ(proxy.WriteRemoteBatchAsync(0, descriptor), BM_INVALID_PARAM);
    EXPECT_EQ(submitted.load(), 0U);
    EXPECT_TRUE(proxy.QueryHasRegistered(LOCAL_HVA + 8, POOL_SIZE - 8));
    EXPECT_FALSE(proxy.QueryHasRegistered(LOCAL_HVA + 8, POOL_SIZE));
    TransportMemoryKey key;
    EXPECT_EQ(proxy.QueryMemoryKey(LOCAL_HVA + 8, key), BM_OK);
    EXPECT_EQ(key.keys, (std::vector<uint64_t>{17, 19}));
    EXPECT_EQ(proxy.QueryMemoryKey(LOCAL_HVA - 1, key), BM_ERROR);
    EXPECT_EQ(proxy.UnregisterMemoryRegion(LOCAL_HVA), BM_OK);
    EXPECT_FALSE(proxy.QueryHasRegistered(LOCAL_HVA, 1));
}

TEST_F(ProviderRdmaPathTest, TransferFailureInvalidatesOnlyFailedLink)
{
    for (bool failSubmit : {true, false}) {
        RdmaProviderControlServer server;
        StartProvider(server);
        RdmaProxyTransportManager proxy;
        ConnectProxy(server, proxy);
        submitOk = !failSubmit;
        waitOk = failSubmit;
        EXPECT_EQ(proxy.WriteRemote(0, LOCAL_HVA, PROXY_GVA, 8), BM_ERROR);
        EXPECT_TRUE(proxy.links_.empty());
        EXPECT_TRUE(proxy.GetConnectedProtocols(0).empty());
        submitOk = waitOk = true;
        EXPECT_EQ(proxy.ConnectRank(0), BM_OK);
        EXPECT_EQ(proxy.WriteRemote(0, LOCAL_HVA, PROXY_GVA, 8), BM_OK);
    }
}

TEST_F(ProviderRdmaPathTest, FailedQpRemovalRetainsResourcesAndSuppressesLinkReadiness)
{
    RdmaProviderControlServer server;
    StartProvider(server);
    RdmaProxyTransportManager proxy;
    ConnectProxy(server, proxy);
    destroyOk = false;
    EXPECT_EQ(proxy.RemoveRanks({0}), BM_ERROR);
    EXPECT_EQ(proxy.links_.size(), 1U);
    EXPECT_EQ(proxy.links_.at(0).totalSize, 0U);
    EXPECT_TRUE(proxy.GetConnectedProtocols(0).empty());
    destroyOk = true;
    EXPECT_EQ(proxy.RemoveRanks({0}), BM_OK);
    EXPECT_TRUE(proxy.links_.empty());
    EXPECT_EQ(server.RemoveRanks({1, 3}), BM_OK);
}

TEST_F(ProviderRdmaPathTest, ChangedProviderDescriptionInvalidatesCachedLink)
{
    RdmaProviderControlServer server;
    StartProvider(server);
    RdmaProxyTransportManager proxy;
    EXPECT_EQ(proxy.ConnectRank(0), BM_NOT_INITIALIZED);
    EXPECT_EQ(proxy.Prepare({}), BM_NOT_INITIALIZED);
    ConnectProxy(server, proxy);
    HybmTransPrepareOptions prepare;
    prepare.options[0] = proxy.preparedProviders_.at(0);
    prepare.options[0].providerGvaBase += POOL_SIZE;
    EXPECT_EQ(proxy.UpdateRankOptions(prepare), BM_OK);
    EXPECT_TRUE(proxy.GetConnectedProtocols(0).empty());
    EXPECT_EQ(proxy.ConnectRank(0), BM_OK);
    EXPECT_EQ(proxy.links_.at(0).providerGvaBase, PROXY_GVA + POOL_SIZE);
    prepare.options[0].providerControlUrl = "invalid";
    EXPECT_EQ(proxy.Prepare(prepare), BM_INVALID_PARAM);
    EXPECT_EQ(proxy.OpenDevice({}), BM_ERROR);
}

TEST_F(ProviderRdmaPathTest, ProviderRejectsUnregisteredPoolAndStaleHandshake)
{
    RdmaProviderControlServer server;
    EXPECT_FALSE(server.Start());
    EXPECT_FALSE(server.RegisterMemory(nullptr, 0, 0, 0));
    StartProvider(server);
    EXPECT_TRUE(server.Start());
    EXPECT_FALSE(server.RegisterMemory(nullptr, 0, 0, 0));
    EXPECT_EQ(server.RemoveRanks({1}), BM_OK);
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    auto guard = make_scope_guard(sockets[0], [](int fd) { close(fd); });
    RdmaProviderLinkHello hello{RDMA_PROVIDER_LINK_MAGIC, RDMA_PROVIDER_LINK_VERSION, 1, 1, 1, 0};
    ASSERT_TRUE(SendStruct(sockets[0], hello, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    server.HandleClient(sockets[1], 0);
    RdmaProviderLinkHelloAck ack{};
    EXPECT_FALSE(RecvStruct(sockets[0], ack, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(nextQp.load(), 0U);
    EXPECT_TRUE(server.GetPeerRanks().empty());
}
} // namespace
