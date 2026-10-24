/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#include <gtest/gtest.h>
#include <array>
#include <sstream>
#include <mutex>
#include <map>
#include <unordered_map>
#define private public
#include "host_rdma_hcomm_transport_manager.h"
#include "device_rdma_hcomm_transport_manager.h"
#include "compose_transport_manager.h"
#undef private

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::device;
namespace {
constexpr uint64_t POOL = 0x10000;
constexpr uint64_t GVA = 0x90000;
constexpr uint64_t POOL_SIZE = 4096;
constexpr size_t DESC_LEN = sizeof(EndpointDesc) + 2 * sizeof(uint64_t);
int imports, unimports, updates, registrations, unregistrations, submissions;
int importFailure, updateResult, unregResult, fenceResult, submitResult;
std::vector<std::string> events;
EndpointDesc createdEndpoint{};
CommEngine createdEngine;
HcommChannelDesc createdChannel{};
HcommCommMem registeredMemory{};

EndpointDesc MakeEndpoint(EndpointLocType location = ENDPOINT_LOC_TYPE_HOST)
{
    EndpointDesc endpoint{};
    endpoint.protocol = COMM_PROTOCOL_ROCE;
    endpoint.commAddr.type = COMM_ADDR_TYPE_IP_V4;
    endpoint.commAddr.addr.s_addr = 0x0100007f;
    endpoint.loc.locType = location;
    endpoint.raws[3] = 42; // Nontrivial reserved bytes must survive import and channel creation.
    return endpoint;
}
RdmaHcommPeerPrivateInfo MakePrivate(const EndpointDesc &endpoint)
{
    RdmaHcommPeerPrivateInfo peer{};
    peer.addrType = endpoint.commAddr.type;
    std::memcpy(peer.addrBytes, endpoint.commAddr.raws, sizeof(peer.addrBytes));
    peer.devPhyId = endpoint.loc.locType == ENDPOINT_LOC_TYPE_DEVICE ? endpoint.loc.device.devPhyId : 0;
    peer.sdid = endpoint.loc.locType == ENDPOINT_LOC_TYPE_DEVICE ? endpoint.loc.device.superDevId : 0;
    peer.listenPort = 5000;
    return peer;
}
void MakeDesc(uint8_t *dst, const EndpointDesc &endpoint, uint64_t addr, uint64_t size)
{
    std::memcpy(dst, &addr, sizeof(addr));
    std::memcpy(dst + sizeof(addr), &size, sizeof(size));
    std::memcpy(dst + DESC_LEN - sizeof(endpoint), &endpoint, sizeof(endpoint));
}
TransportMemoryKey MakeKey(const EndpointDesc &endpoint = MakeEndpoint())
{
    TransportMemoryKey key;
    key.ResizeBytes(HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t) + DESC_LEN + 32); // Deliberate tail padding.
    MakeDesc(key.ByteData(), endpoint, POOL, POOL_SIZE);
    MakeDesc(key.ByteData() + HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t), endpoint, POOL + POOL_SIZE, 8);
    auto &meta = HcommKeyMetaAt(key);
    meta.addr = POOL;
    meta.size = POOL_SIZE;
    meta.gvaBase = GVA;
    meta.descLen = DESC_LEN;
    meta.flagLen = DESC_LEN;
    return key;
}
TransportPrivateData MakePrivateKey(const EndpointDesc &endpoint)
{
    auto peer = MakePrivate(endpoint);
    RdmaHcommPrivateData data{};
    data.protocol = COMM_PROTOCOL_ROCE;
    data.addrType = peer.addrType;
    std::memcpy(data.addrBytes, peer.addrBytes, sizeof(data.addrBytes));
    data.devPhyId = peer.devPhyId;
    data.sdid = peer.sdid;
    data.listenPort = peer.listenPort;
    TransportPrivateData result{};
    WriteStructToKey(data, result.key);
    return result;
}

class HcommHeterogeneousTest : public testing::Test {
    decltype(DlHcommApi::gHcommMemImport) oldImport = DlHcommApi::gHcommMemImport;
    decltype(DlHcommApi::gHcommMemUnimport) oldUnimport = DlHcommApi::gHcommMemUnimport;
    decltype(DlHcommApi::gHcommMemReg) oldReg = DlHcommApi::gHcommMemReg;
    decltype(DlHcommApi::gHcommMemUnreg) oldUnreg = DlHcommApi::gHcommMemUnreg;
    decltype(DlHcommApi::gHcommMemExport) oldExport = DlHcommApi::gHcommMemExport;
    decltype(DlHcommApi::gHcommChannelUpdateMemInfo) oldUpdate = DlHcommApi::gHcommChannelUpdateMemInfo;
    decltype(DlHcommApi::gHcommChannelFence) oldFence = DlHcommApi::gHcommChannelFence;
    decltype(DlHcommApi::gHcommReadNbi) oldRead = DlHcommApi::gHcommReadNbi;
    decltype(DlHcommApi::gHcommWriteNbi) oldWrite = DlHcommApi::gHcommWriteNbi;
    decltype(DlHcommApi::gHcommChannelCreate) oldCreate = DlHcommApi::gHcommChannelCreate;
    decltype(DlHcommApi::gHcommChannelDestroy) oldDestroy = DlHcommApi::gHcommChannelDestroy;
    decltype(DlHcommApi::gHcommChannelGetStatus) oldStatus = DlHcommApi::gHcommChannelGetStatus;
    decltype(DlHcommApi::gHcommThreadAlloc) oldAlloc = DlHcommApi::gHcommThreadAlloc;
    decltype(DlHcommApi::gHcommThreadFree) oldFree = DlHcommApi::gHcommThreadFree;
    decltype(DlHcommApi::gHcommEndpointDestroy) oldEndpointDestroy = DlHcommApi::gHcommEndpointDestroy;

protected:
    void SetUp() override
    {
        imports = unimports = updates = registrations = unregistrations = submissions = 0;
        importFailure = updateResult = unregResult = fenceResult = submitResult = 0;
        events.clear();
        DlHcommApi::gHcommMemImport = [](EndpointHandle, const void *desc, uint32_t, HcommCommMem *mem) -> int32_t {
            events.push_back("import");
            if (++imports == importFailure) {
                return BM_ERROR;
            }
            uint64_t addr;
            std::memcpy(&addr, desc, sizeof(addr));
            mem->addr = reinterpret_cast<void *>(addr);
            std::memcpy(&mem->size, static_cast<const uint8_t *>(desc) + sizeof(addr), sizeof(mem->size));
            mem->type = COMM_MEM_TYPE_HOST;
            return BM_OK;
        };
        DlHcommApi::gHcommMemUnimport = [](EndpointHandle, const void *, uint32_t) -> int32_t {
            events.push_back("unimport");
            ++unimports;
            return BM_OK;
        };
        DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *mem,
                                      HcommMemHandle *handle) -> int32_t {
            events.push_back("reg");
            ++registrations;
            registeredMemory = *mem;
            *handle = reinterpret_cast<void *>(0x1234);
            return BM_OK;
        };
        DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t {
            events.push_back("unreg");
            ++unregistrations;
            return unregResult;
        };
        DlHcommApi::gHcommMemExport = [](EndpointHandle, HcommMemHandle, void **desc, uint32_t *size) -> int32_t {
            static uint8_t bytes[DESC_LEN];
            MakeDesc(bytes, MakeEndpoint(), reinterpret_cast<uint64_t>(registeredMemory.addr), registeredMemory.size);
            *desc = bytes;
            *size = DESC_LEN;
            return BM_OK;
        };
        DlHcommApi::gHcommChannelUpdateMemInfo = [](HcommMemHandle *handles, uint32_t count, ChannelHandle) -> int32_t {
            EXPECT_EQ(handles, nullptr);
            EXPECT_EQ(count, 0U);
            events.push_back("update");
            ++updates;
            return updateResult;
        };
        DlHcommApi::gHcommChannelFence = [](ChannelHandle) -> int32_t {
            events.push_back("fence");
            return fenceResult;
        };
        DlHcommApi::gHcommReadNbi = [](ChannelHandle, void *, const void *, uint64_t) -> int32_t {
            ++submissions;
            return submitResult;
        };
        DlHcommApi::gHcommWriteNbi = DlHcommApi::gHcommReadNbi;
        DlHcommApi::gHcommChannelCreate = [](EndpointHandle, CommEngine engine, HcommChannelDesc *desc, uint32_t,
                                             ChannelHandle *channel) -> int32_t {
            events.push_back("channel");
            createdEndpoint = desc->remoteEndpoint;
            createdEngine = engine;
            createdChannel = *desc;
            *channel = 17;
            return BM_OK;
        };
        DlHcommApi::gHcommChannelDestroy = [](const ChannelHandle *, uint32_t) -> int32_t { return BM_OK; };
        DlHcommApi::gHcommChannelGetStatus = [](const ChannelHandle *, uint32_t, int32_t *status) -> int32_t {
            *status = 0; // HCOMM channel READY
            return BM_OK;
        };
        DlHcommApi::gHcommThreadAlloc = [](CommEngine, uint32_t, const uint32_t *, ThreadHandle *thread) -> int32_t {
            *thread = 19;
            return BM_OK;
        };
        DlHcommApi::gHcommThreadFree = [](const ThreadHandle *, uint32_t) -> int32_t { return BM_OK; };
        DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return BM_OK; };
    }
    void TearDown() override
    {
        DlHcommApi::gHcommMemImport = oldImport;
        DlHcommApi::gHcommMemUnimport = oldUnimport;
        DlHcommApi::gHcommMemReg = oldReg;
        DlHcommApi::gHcommMemUnreg = oldUnreg;
        DlHcommApi::gHcommMemExport = oldExport;
        DlHcommApi::gHcommChannelUpdateMemInfo = oldUpdate;
        DlHcommApi::gHcommChannelFence = oldFence;
        DlHcommApi::gHcommReadNbi = oldRead;
        DlHcommApi::gHcommWriteNbi = oldWrite;
        DlHcommApi::gHcommChannelCreate = oldCreate;
        DlHcommApi::gHcommChannelDestroy = oldDestroy;
        DlHcommApi::gHcommChannelGetStatus = oldStatus;
        DlHcommApi::gHcommThreadAlloc = oldAlloc;
        DlHcommApi::gHcommThreadFree = oldFree;
        DlHcommApi::gHcommEndpointDestroy = oldEndpointDestroy;
    }
};

TEST_F(HcommHeterogeneousTest, TrailerUsesActualDescriptorLengthAndPreservesAllEndpointBytes)
{
    auto endpoint = MakeEndpoint();
    auto key = MakeKey(endpoint);
    HcommKeyView view;
    ASSERT_GT(key.ByteSize(), DESC_LEN);
    ASSERT_EQ(ReadHcommKey(key, MakePrivate(endpoint), true, view), BM_OK);
    EXPECT_EQ(std::memcmp(&view.endpoint, &endpoint, sizeof(endpoint)), 0);
}

TEST_F(HcommHeterogeneousTest, RejectsTruncatedOverflowAndMetadataOverlappingDescriptors)
{
    auto key = MakeKey();
    auto peer = MakePrivate(MakeEndpoint());
    HcommKeyView view;
    for (uint64_t len : std::array<uint64_t, 4>{0, sizeof(EndpointDesc), 225, UINT64_MAX}) {
        HcommKeyMetaAt(key).descLen = len;
        EXPECT_NE(ReadHcommKey(key, peer, true, view), BM_OK);
    }
    key = MakeKey();
    HcommKeyMetaAt(key).flagLen = UINT64_MAX;
    EXPECT_NE(ReadHcommKey(key, peer, true, view), BM_OK);
    key.ResizeWords(3);
    EXPECT_NE(ReadHcommKey(key, peer, true, view), BM_OK);
}

TEST_F(HcommHeterogeneousTest, RejectsWrongProtocolLocationAddressAndFlag)
{
    const auto peer = MakePrivate(MakeEndpoint());
    HcommKeyView view;
    auto endpoint = MakeEndpoint();
    endpoint.protocol = COMM_PROTOCOL_RESERVED;
    EXPECT_NE(ReadHcommKey(MakeKey(endpoint), peer, true, view), BM_OK);
    endpoint = MakeEndpoint();
    endpoint.loc.locType = ENDPOINT_LOC_TYPE_RESERVED;
    EXPECT_NE(ReadHcommKey(MakeKey(endpoint), peer, true, view), BM_OK);
    endpoint = MakeEndpoint();
    endpoint.commAddr.addr.s_addr++;
    EXPECT_NE(ReadHcommKey(MakeKey(endpoint), peer, true, view), BM_OK);
    auto key = MakeKey();
    HcommKeyMetaAt(key).flagLen = 0;
    EXPECT_NE(ReadHcommKey(key, peer, true, view), BM_OK);
    key = MakeKey();
    endpoint = MakeEndpoint();
    endpoint.raws[2]++;
    MakeDesc(key.ByteData() + HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t), endpoint, POOL + POOL_SIZE, 8);
    EXPECT_NE(ReadHcommKey(key, peer, true, view), BM_OK);
}

TEST_F(HcommHeterogeneousTest, ExistingPrivateVersionsRemainCompatible)
{
    EXPECT_EQ(sizeof(RdmaHcommPrivateData), 64U);
    EXPECT_EQ(sizeof(RdmaHcommPrivateDataHost), 72U);
    RdmaHcommPeerPrivateInfo peer;
    auto key = MakePrivateKey(MakeEndpoint()).key;
    EXPECT_EQ(ReadHcommPrivateData(key, peer), BM_OK);
    RdmaHcommPrivateDataHost data;
    data.protocol = COMM_PROTOCOL_ROCE;
    data.addrType = COMM_ADDR_TYPE_IP_V4;
    data.flagAddr = 42;
    WriteStructToKey(data, key);
    EXPECT_EQ(ReadHcommPrivateData(key, peer), BM_OK);
    EXPECT_EQ(peer.flagAddr, 42U);
    data.payloadLen = 1;
    WriteStructToKey(data, key);
    EXPECT_NE(ReadHcommPrivateData(key, peer), BM_OK);
}

TEST_F(HcommHeterogeneousTest, ImportFailureRollsBackWithoutPublishingEndpoint)
{
    HcommPeerMemory state;
    state.peerData = MakePrivate(MakeEndpoint());
    importFailure = 2;
    EXPECT_NE(ImportHcommPeerKeys(nullptr, {MakeKey()}, true, state), BM_OK);
    EXPECT_FALSE(state.hasEndpointDesc);
    EXPECT_EQ(state.peerDva, 0U);
    EXPECT_TRUE(state.importedDescs.empty());
    EXPECT_EQ(unimports, 1);
}

TEST_F(HcommHeterogeneousTest, RepeatedAndMultipleKeysAreValidatedBeforeAnyImport)
{
    HcommPeerMemory state;
    state.peerData = MakePrivate(MakeEndpoint());
    auto key = MakeKey();
    ASSERT_EQ(ImportHcommPeerKeys(nullptr, {key, key}, true, state), BM_OK);
    EXPECT_EQ(imports, 2); // data and flag, each once
    EXPECT_EQ(ImportHcommPeerKeys(nullptr, {key}, true, state), BM_OK);
    EXPECT_EQ(imports, 2);
    auto changed = MakeEndpoint();
    changed.raws[0]++;
    EXPECT_NE(ImportHcommPeerKeys(nullptr, {MakeKey(changed)}, true, state), BM_OK);
    HcommKeyMetaAt(key).gvaBase++;
    EXPECT_NE(ImportHcommPeerKeys(nullptr, {key}, true, state), BM_OK);
    EXPECT_EQ(imports, 2);
    state = HcommPeerMemory{};
    state.peerData = MakePrivate(MakeEndpoint());
    EXPECT_NE(ImportHcommPeerKeys(nullptr, {MakeKey(), MakeKey(changed)}, true, state), BM_OK);
    EXPECT_EQ(imports, 2);
    EXPECT_FALSE(state.hasEndpointDesc);
}

TEST_F(HcommHeterogeneousTest, ComposeCrossRoutesBothDirectionsAndRejectsMalformedEnvelope)
{
    for (bool localHost : {false, true}) {
        auto tags = std::make_shared<HybmEntityTagInfo>();
        ASSERT_EQ(tags->AddRankTag(0, "local"), BM_OK);
        ASSERT_EQ(tags->AddRankTag(1, "peer"), BM_OK);
        ASSERT_EQ(tags->AddRankTag(2, "excluded"), BM_OK);
        ASSERT_EQ(tags->AddTagOpInfo(localHost ? "local:HOST_RDMA:peer" : "local:DEVICE_RDMA:peer"), BM_OK);
        ASSERT_EQ(tags->AddTagOpInfo(localHost ? "local:DEVICE_RDMA:excluded" : "local:HOST_RDMA:excluded"), BM_OK);
        ComposeTransportManager manager(tags);
        if (localHost) {
            manager.hostTransportManager_ = std::make_shared<host::HostRdmaHcommTransportManager>();
        } else {
            manager.deviceTransportManager_ = std::make_shared<DeviceRdmaHcommTransportManager>();
        }
        auto endpoint = MakeEndpoint(localHost ? ENDPOINT_LOC_TYPE_DEVICE : ENDPOINT_LOC_TYPE_HOST);
        TransportMemoryKey merged;
        if (localHost) {
            WriteDeviceMemoryKey(MakeKey(endpoint), merged);
        } else {
            WriteHostMemoryKey(MakeKey(endpoint), merged);
        }
        HybmTransPrepareOptions input, output;
        auto &info = input.options[1];
        info.memKeys = {merged};
        info.privateData = MakePrivateKey(endpoint);
        info.nic = localHost ? "device#tcp://127.0.0.1:5000;" : "host#tcp://127.0.0.1:5000;";
        auto route = [&] {
            return localHost ? manager.GetHostPrepareOptions(input, output)
                             : manager.GetDevicePrepareOptions(input, output);
        };
        ASSERT_EQ(route(), BM_OK);
        EXPECT_EQ(output.options.at(1).privateData.key, info.privateData.key);
        EXPECT_EQ(output.options.at(1).memKeys[0], MakeKey(endpoint));
        EXPECT_EQ(output.options.at(1).nic, "tcp://127.0.0.1:5000");
        input.options[2].privateData.key.keys = {UINT64_MAX}; // Must be filtered before HCOMM parsing.
        output.options.clear();
        ASSERT_EQ(route(), BM_OK);
        EXPECT_EQ(output.options.size(), 1U);
        EXPECT_EQ(output.options.count(2), 0U);
        info.memKeys[0].keys[0] = UINT64_MAX;
        EXPECT_NE(route(), BM_OK);
    }
}

TEST_F(HcommHeterogeneousTest, DeviceWaitsForMrThenUsesImportedHostEndpointAndOriginalFlag)
{
    DeviceRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.rankId_ = 1;
    manager.rankCount_ = 2;
    HybmTransPrepareOptions options;
    options.options[0].privateData = MakePrivateKey(MakeEndpoint());
    options.options[0].peerProtocol = HYBM_DOP_TYPE_HOST_RDMA;
    ASSERT_EQ(manager.Prepare(options), BM_OK);
    EXPECT_EQ(manager.remoteRanks_[0].channel, 0U);
    auto tags = std::make_shared<HybmEntityTagInfo>();
    ASSERT_EQ(tags->AddRankTag(1, "device"), BM_OK);
    ASSERT_EQ(tags->AddRankTag(0, "host"), BM_OK);
    ASSERT_EQ(tags->AddTagOpInfo("device:DEVICE_RDMA:host"), BM_OK);
    ComposeTransportManager compose(tags);
    compose.options_.rankId = 1;
    compose.deviceTransportManager_ = std::make_shared<DeviceRdmaHcommTransportManager>();
    TransportMemoryKey envelope;
    WriteHostMemoryKey(MakeKey(), envelope);
    options.options[0].memKeys = {envelope};
    options.options[0].nic = "host#tcp://127.0.0.1:5000;";
    HybmTransPrepareOptions routed;
    ASSERT_EQ(compose.GetDevicePrepareOptions(options, routed), BM_OK);
    ASSERT_EQ(routed.options.at(0).peerProtocol, HYBM_DOP_TYPE_HOST_RDMA);
    ASSERT_EQ(manager.UpdateRankOptions(routed), BM_OK);
    EXPECT_EQ(std::memcmp(&createdEndpoint, &manager.remoteRanks_[0].remoteEndpointDesc, sizeof(EndpointDesc)), 0);
    EXPECT_EQ(createdEndpoint.raws[3], 42);
    EXPECT_EQ(createdEngine, COMM_ENGINE_AICPU);
    EXPECT_EQ(createdChannel.notifyNum, 1U);
    EXPECT_EQ(createdChannel.port, 5000);
    EXPECT_EQ(manager.remoteRanks_[0].remoteFlagAddr, POOL + POOL_SIZE);
}

TEST_F(HcommHeterogeneousTest, ComposeDevicePeersKeepOpaqueMrAndOriginalEndpointConstruction)
{
    for (bool hostRoce : {false, true}) {
        auto tags = std::make_shared<HybmEntityTagInfo>();
        ASSERT_EQ(tags->AddRankTag(1, "local"), BM_OK);
        ASSERT_EQ(tags->AddRankTag(0, "peer"), BM_OK);
        ASSERT_EQ(tags->AddTagOpInfo("local:DEVICE_RDMA:peer"), BM_OK);
        ComposeTransportManager compose(tags);
        compose.options_.rankId = 1;
        auto device = std::make_shared<DeviceRdmaHcommTransportManager>();
        compose.deviceTransportManager_ = device;
        device->opened_ = true;
        device->rankId_ = 1;
        device->rankCount_ = 2;
        device->hostRocePath_ = hostRoce;
        device->serverId_ = 19;
        device->superPodId_ = 23;
        auto endpoint = MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE);
        endpoint.loc.device.devPhyId = 7;
        endpoint.loc.device.superDevId = 11;
        HybmTransPrepareOptions input, routed;
        auto &info = input.options[0];
        info.privateData = MakePrivateKey(endpoint);
        if (hostRoce) {
            RdmaHcommPrivateDataHost priv{};
            priv.protocol = COMM_PROTOCOL_ROCE;
            priv.addrType = endpoint.commAddr.type;
            std::memcpy(priv.addrBytes, endpoint.commAddr.raws, sizeof(priv.addrBytes));
            priv.listenPort = 5000;
            priv.flagAddr = POOL + POOL_SIZE;
            WriteStructToKey(priv, info.privateData.key);
        }
        info.nic = "device#tcp://127.0.0.1:5000;";
        // The mock HCOMM importer accepts this opaque descriptor without an EndpointDesc trailer.
        auto key = MakeKey(endpoint);
        HcommKeyMetaAt(key).descLen = 2 * sizeof(uint64_t);
        HcommKeyMetaAt(key).flagLen = 0;
        TransportMemoryKey envelope;
        WriteDeviceMemoryKey(key, envelope);
        info.memKeys = {envelope};
        ASSERT_EQ(compose.GetDevicePrepareOptions(input, routed), BM_OK);
        EXPECT_EQ(routed.options.at(0).peerProtocol, HYBM_DOP_TYPE_DEVICE_RDMA);
        ASSERT_EQ(device->Prepare(routed), BM_OK);
        EXPECT_EQ(createdEngine, hostRoce ? COMM_ENGINE_CPU : COMM_ENGINE_AICPU);
        EndpointDesc expected{};
        expected.protocol = COMM_PROTOCOL_ROCE;
        expected.commAddr = endpoint.commAddr;
        expected.loc.locType = hostRoce ? ENDPOINT_LOC_TYPE_HOST : ENDPOINT_LOC_TYPE_DEVICE;
        if (!hostRoce) {
            expected.loc.device.devPhyId = 7;
            expected.loc.device.superDevId = 11;
            expected.loc.device.serverIdx = 19;
            expected.loc.device.superPodIdx = 23;
        }
        EXPECT_EQ(std::memcmp(&createdEndpoint, &expected, sizeof(expected)), 0);
        EXPECT_EQ(createdChannel.notifyNum, 1U);
    }
}

TEST_F(HcommHeterogeneousTest, DeviceLegacyPrepareStillBuildsEndpointWithoutImportedMr)
{
    DeviceRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.rankId_ = 1;
    manager.rankCount_ = 2;
    HybmTransPrepareOptions options;
    options.options[0].privateData = MakePrivateKey(MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE));
    options.options[0].memKeys = {TransportMemoryKey{}}; // Legacy importer skips an empty key.
    ASSERT_EQ(manager.Prepare(options), BM_OK);
    EXPECT_EQ(imports, 0);
    EXPECT_EQ(createdEndpoint.loc.locType, ENDPOINT_LOC_TYPE_DEVICE);
    EXPECT_EQ(createdEngine, COMM_ENGINE_AICPU);
}

TEST_F(HcommHeterogeneousTest, DeviceHostOriginRejectsInvalidOrNonHostEndpointTrailer)
{
    for (bool truncated : {false, true}) {
        DeviceRdmaHcommTransportManager manager;
        manager.opened_ = true;
        manager.rankId_ = 1;
        manager.rankCount_ = 2;
        auto endpoint = MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE);
        auto key = MakeKey(endpoint);
        if (truncated) {
            HcommKeyMetaAt(key).descLen = 2 * sizeof(uint64_t);
        }
        HybmTransPrepareOptions options;
        options.options[0].privateData = MakePrivateKey(endpoint);
        options.options[0].peerProtocol = HYBM_DOP_TYPE_HOST_RDMA;
        options.options[0].memKeys = {key};
        EXPECT_EQ(manager.Prepare(options), BM_INVALID_PARAM);
        EXPECT_EQ(manager.remoteRanks_.at(0).channel, 0U);
    }
}

TEST_F(HcommHeterogeneousTest, DeviceRegistrationDoesNotRefreshExistingChannels)
{
    DeviceRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.remoteRanks_[0].channel = 17;
    updateResult = BM_ERROR;
    TransportMemoryRegion mr{POOL, POOL_SIZE, REG_MR_ACCESS_FLAG_BOTH_READ_WRITE, REG_MR_FLAG_HBM};
    ASSERT_EQ(manager.RegisterMemoryRegion(mr), BM_OK);
    EXPECT_TRUE(manager.QueryHasRegistered(POOL, POOL_SIZE));
    EXPECT_EQ(manager.UnregisterMemoryRegion(POOL), BM_OK);
    EXPECT_TRUE(manager.localRegistrations_.empty());
    EXPECT_EQ(registrations, 1);
    EXPECT_EQ(unregistrations, 1);
    EXPECT_EQ(updates, 0);
}

TEST_F(HcommHeterogeneousTest, HostFlagIsExportedAndRetainedWhileChannelExists)
{
    host::HostRdmaHcommTransportManager manager;
    manager.opened_ = true;
    ASSERT_EQ(manager.InitTransferFlagLocked(), BM_OK);
    EXPECT_EQ(registeredMemory.type, COMM_MEM_TYPE_HOST);
    EXPECT_NE(manager.localFlagHandle_, nullptr);
    TransportMemoryRegion mr{POOL, POOL_SIZE, REG_MR_ACCESS_FLAG_BOTH_READ_WRITE, REG_MR_FLAG_DRAM};
    ASSERT_EQ(manager.RegisterMemoryRegion(mr), BM_OK);
    TransportMemoryKey key;
    ASSERT_EQ(manager.QueryMemoryKey(POOL, key), BM_OK);
    HcommKeyView view;
    EXPECT_EQ(ReadHcommKey(key, MakePrivate(MakeEndpoint()), true, view), BM_OK);
    EXPECT_EQ(view.meta.flagLen, DESC_LEN);
    manager.remoteRanks_[1].channel = 17;
    fenceResult = BM_ERROR;
    EXPECT_NE(manager.CloseDevice(), BM_OK);
    EXPECT_NE(manager.localFlagHandle_, nullptr);
    EXPECT_NE(manager.localFlagPtr_, nullptr);
    fenceResult = BM_OK;
}

TEST_F(HcommHeterogeneousTest, HostNbiFenceAndFullRangeValidation)
{
    host::HostRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.localRegistrations_[POOL] = {reinterpret_cast<void *>(1), POOL_SIZE, 1};
    auto &state = manager.remoteRanks_[1];
    state.channel = 17;
    state.peerDva = POOL;
    state.peerDvaSize = POOL_SIZE;
    state.peerGva = GVA;
    EXPECT_EQ(manager.WriteRemote(1, POOL, GVA, 8), BM_OK);
    EXPECT_EQ(submissions, 1);
    EXPECT_EQ(events.back(), "fence");
    EXPECT_NE(manager.ReadRemoteAsync(1, POOL, GVA + POOL_SIZE - 4, 8), BM_OK);
    EXPECT_NE(manager.WriteRemoteAsync(1, UINT64_MAX - 2, GVA, 8), BM_OK);
    EXPECT_EQ(submissions, 1);
    EXPECT_EQ(manager.UnregisterMemoryRegion(POOL), BM_NOT_SUPPORTED);
    EXPECT_EQ(manager.RemoveRanks({1}), BM_OK);
    unregResult = BM_ERROR;
    EXPECT_NE(manager.UnregisterMemoryRegion(POOL), BM_OK);
    EXPECT_EQ(manager.localRegistrations_.size(), 1U);
    unregResult = BM_OK;
    EXPECT_EQ(manager.UnregisterMemoryRegion(POOL), BM_OK);
    EXPECT_EQ(events.back(), "unreg");
    EXPECT_EQ(updates, 0); // CPU plugin has no AICPU snapshot refresh capability.
}

TEST_F(HcommHeterogeneousTest, HvaDvaAndRemoteOffsetsUseSeparateOrigins)
{
    DeviceRdmaHcommTransportManager manager;
    manager.localRegistrations_[POOL] = {reinterpret_cast<void *>(1), 0x50000, POOL_SIZE, 1};
    RemoteRankState state;
    state.peerGva = GVA;
    state.peerDva = 0x70000;
    state.peerDvaSize = POOL_SIZE;
    state.remoteEndpointDesc = MakeEndpoint();
    state.hasEndpointDesc = true;
    EXPECT_EQ(manager.ConvertGvaToDva(POOL + 16), 0x50010U);
    EXPECT_EQ(manager.ConvertRemoteGvaToDva(state, GVA + 32), 0x70020U);
    EXPECT_EQ(manager.ConvertRemoteGvaToDva(state, GVA - 1), GVA - 1);
    EXPECT_EQ(manager.ConvertRemoteGvaToDva(state, GVA + POOL_SIZE), GVA + POOL_SIZE);
}
TEST_F(HcommHeterogeneousTest, HostWaitsForDeviceMrAndCreatesCpuServer)
{
    host::HostRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.rankId_ = 0;
    manager.rankCount_ = 2;
    manager.localListenPort_ = 6000;
    auto endpoint = MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE);
    HybmTransPrepareOptions options;
    options.options[1].privateData = MakePrivateKey(endpoint);
    ASSERT_EQ(manager.Prepare(options), BM_OK);
    EXPECT_EQ(manager.remoteRanks_[1].channel, 0U);
    options.options[1].memKeys = {MakeKey(endpoint)};
    ASSERT_EQ(manager.UpdateRankOptions(options), BM_OK);
    EXPECT_EQ(createdEngine, COMM_ENGINE_CPU);
    EXPECT_EQ(createdChannel.role, HCOMM_SOCKET_ROLE_SERVER);
    EXPECT_EQ(createdChannel.port, 6000U);
    EXPECT_EQ(createdChannel.notifyNum, 0U);
    EXPECT_EQ(std::memcmp(&createdEndpoint, &endpoint, sizeof(endpoint)), 0);
}

TEST_F(HcommHeterogeneousTest, DeviceImportAndUnimportDoNotRefreshOtherChannels)
{
    DeviceRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.rankId_ = 1;
    manager.rankCount_ = 3;
    manager.remoteRanks_[2].channel = 33;
    DlHcommApi::gHcommChannelUpdateMemInfo = nullptr;
    HybmTransPrepareOptions options;
    options.options[0].privateData = MakePrivateKey(MakeEndpoint());
    options.options[0].peerProtocol = HYBM_DOP_TYPE_HOST_RDMA;
    options.options[0].memKeys = {MakeKey()};
    ASSERT_EQ(manager.Prepare(options), BM_OK);
    EXPECT_EQ(updates, 0);
    ASSERT_EQ(manager.RemoveRanks({0}), BM_OK);
    EXPECT_EQ(unimports, 2);
    EXPECT_EQ(updates, 0);
    EXPECT_EQ(manager.remoteRanks_.count(0), 0U);
    EXPECT_EQ(manager.remoteRanks_.at(2).channel, 33U);
}

TEST_F(HcommHeterogeneousTest, DeviceUnregisterFailureRetainsRegistrationForRetry)
{
    DeviceRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.remoteRanks_[0].channel = 17;
    TransportMemoryRegion mr{POOL, POOL_SIZE, REG_MR_ACCESS_FLAG_BOTH_READ_WRITE, REG_MR_FLAG_HBM};
    ASSERT_EQ(manager.RegisterMemoryRegion(mr), BM_OK);
    unregResult = BM_ERROR;
    EXPECT_NE(manager.UnregisterMemoryRegion(POOL), BM_OK);
    EXPECT_NE(manager.localRegistrations_.at(POOL).handle, nullptr);
    EXPECT_TRUE(manager.QueryHasRegistered(POOL, POOL_SIZE));
    unregResult = BM_OK;
    EXPECT_EQ(manager.UnregisterMemoryRegion(POOL), BM_OK);
    EXPECT_TRUE(manager.localRegistrations_.empty());
    EXPECT_EQ(updates, 0);
}

TEST_F(HcommHeterogeneousTest, FailedHostChannelCleanupCannotBecomeReadyOnRetry)
{
    host::HostRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.rankId_ = 0;
    manager.rankCount_ = 2;
    manager.localListenPort_ = 5000;
    auto &state = manager.remoteRanks_[1];
    state.peerData = MakePrivate(MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE));
    state.hasPeerData = true;
    ASSERT_EQ(ImportHcommPeerKeys(nullptr, {MakeKey(MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE))}, true, state), BM_OK);
    DlHcommApi::gHcommChannelGetStatus = [](const ChannelHandle *, uint32_t, int32_t *) -> int32_t { return BM_ERROR; };
    DlHcommApi::gHcommChannelDestroy = [](const ChannelHandle *, uint32_t) -> int32_t { return BM_ERROR; };
    EXPECT_NE(manager.EnsureChannelLocked(1, state), BM_OK);
    EXPECT_TRUE(state.failed);
    EXPECT_NE(manager.ConnectRank(1), BM_OK);
    EXPECT_NE(manager.WriteRemoteAsync(1, POOL, GVA, 8), BM_OK);
    EXPECT_EQ(submissions, 0);
    DlHcommApi::gHcommChannelDestroy = [](const ChannelHandle *, uint32_t) -> int32_t { return BM_OK; };
    EXPECT_EQ(manager.CloseDevice(), BM_OK);
}

TEST_F(HcommHeterogeneousTest, ComposeRelocatesCrossTypeKeyWithoutDroppingRemotePayload)
{
    for (bool localHost : {false, true}) {
        ComposeTransportManager manager(nullptr);
        if (localHost) {
            manager.hostTransportManager_ = std::make_shared<host::HostRdmaHcommTransportManager>();
        } else {
            manager.deviceTransportManager_ = std::make_shared<DeviceRdmaHcommTransportManager>();
        }
        auto payload = MakeKey(MakeEndpoint(localHost ? ENDPOINT_LOC_TYPE_DEVICE : ENDPOINT_LOC_TYPE_HOST));
        TransportMemoryKey envelope;
        if (localHost) {
            WriteDeviceMemoryKey(payload, envelope);
        } else {
            WriteHostMemoryKey(payload, envelope);
        }
        manager.UpdateMemoryKey(envelope, reinterpret_cast<void *>(GVA + POOL_SIZE));
        TransportMemoryKey hostKey, deviceKey;
        ReadHostMemoryKey(envelope, hostKey);
        ReadDeviceMemoryKey(envelope, deviceKey);
        HcommKeyMetaAt(payload).gvaBase = GVA + POOL_SIZE;
        if (!localHost) {
            HcommKeyMetaAt(payload).addr = GVA + POOL_SIZE;
        }
        EXPECT_EQ(localHost ? deviceKey : hostKey, payload);
        EXPECT_TRUE(localHost ? hostKey.Empty() : deviceKey.Empty());
        EXPECT_EQ(HcommKeyMetaAt(payload).addr, localHost ? POOL : GVA + POOL_SIZE);
    }
}

TEST_F(HcommHeterogeneousTest, DevicePrepareRoutesHostAndDevicePeersUsingTheirOwnPayloads)
{
    auto tags = std::make_shared<HybmEntityTagInfo>();
    ASSERT_EQ(tags->AddRankTag(0, "local"), BM_OK);
    ASSERT_EQ(tags->AddRankTag(1, "host"), BM_OK);
    ASSERT_EQ(tags->AddRankTag(2, "device"), BM_OK);
    ASSERT_EQ(tags->AddTagOpInfo("local:DEVICE_RDMA:host,local:DEVICE_RDMA:device"), BM_OK);
    ComposeTransportManager manager(tags);
    manager.deviceTransportManager_ = std::make_shared<DeviceRdmaHcommTransportManager>();
    HybmTransPrepareOptions input, output;
    for (uint32_t rank : {1U, 2U}) {
        const bool remoteHost = rank == 1;
        auto endpoint = MakeEndpoint(remoteHost ? ENDPOINT_LOC_TYPE_HOST : ENDPOINT_LOC_TYPE_DEVICE);
        auto &info = input.options[rank];
        info.privateData = MakePrivateKey(endpoint);
        info.nic = remoteHost ? "host#tcp://127.0.0.1:5000;" : "device#tcp://127.0.0.1:5000;";
        TransportMemoryKey envelope;
        if (remoteHost) {
            WriteHostMemoryKey(MakeKey(endpoint), envelope);
        } else {
            WriteDeviceMemoryKey(MakeKey(endpoint), envelope);
        }
        info.memKeys = {envelope};
    }
    ASSERT_EQ(manager.GetDevicePrepareOptions(input, output), BM_OK);
    ASSERT_EQ(output.options.size(), 2U);
    EXPECT_EQ(output.options.at(1).memKeys.at(0), MakeKey(MakeEndpoint()));
    EXPECT_EQ(output.options.at(2).memKeys.at(0), MakeKey(MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE)));
    EXPECT_EQ(output.options.at(1).peerProtocol, HYBM_DOP_TYPE_HOST_RDMA);
    EXPECT_EQ(output.options.at(2).peerProtocol, HYBM_DOP_TYPE_DEVICE_RDMA);
    TransportMemoryKey invalidHost;
    WriteHostMemoryKey(MakeKey(MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE)), invalidHost);
    input.options.at(1).memKeys = {invalidHost};
    output.options.clear();
    EXPECT_EQ(manager.GetDevicePrepareOptions(input, output), BM_INVALID_PARAM);
}

} // namespace

TEST_F(HcommHeterogeneousTest, DeviceImportRetriesAgainAndPreservesRepeatedImportBehavior)
{
    DeviceRdmaHcommTransportManager manager;
    manager.opened_ = true;
    manager.rankId_ = 1;
    manager.rankCount_ = 2;
    DlHcommApi::gHcommMemImport = [](EndpointHandle, const void *desc, uint32_t, HcommCommMem *mem) -> int32_t {
        if (++imports <= 2) {
            return 20; // HCCL_E_AGAIN
        }
        uint64_t addr;
        std::memcpy(&addr, desc, sizeof(addr));
        mem->addr = reinterpret_cast<void *>(addr);
        std::memcpy(&mem->size, static_cast<const uint8_t *>(desc) + sizeof(addr), sizeof(mem->size));
        mem->type = COMM_MEM_TYPE_HOST;
        return BM_OK;
    };
    HybmTransPrepareOptions options;
    options.options[0].privateData = MakePrivateKey(MakeEndpoint());
    options.options[0].peerProtocol = HYBM_DOP_TYPE_HOST_RDMA;
    options.options[0].memKeys = {MakeKey()};
    ASSERT_EQ(manager.Prepare(options), BM_OK);
    EXPECT_EQ(imports, 4); // Two retries, data import, flag import.
    HcommKeyMetaAt(options.options[0].memKeys[0]).gvaBase += POOL_SIZE;
    EXPECT_EQ(manager.UpdateRankOptions(options), BM_OK);
    EXPECT_EQ(imports, 4); // Restore the original already-imported peer shortcut.
    EXPECT_EQ(manager.remoteRanks_.at(0).peerGva, GVA);
}

TEST_F(HcommHeterogeneousTest, DeviceImportRetryLimitAndNonRetryableFailure)
{
    DeviceRdmaHcommTransportManager manager;
    HcommCommMem memory{};
    DlHcommApi::gHcommMemImport = [](EndpointHandle, const void *, uint32_t, HcommCommMem *) -> int32_t {
        ++imports;
        return 20;
    };
    EXPECT_EQ(manager.RawMemImportWithRetry(nullptr, nullptr, 0, 0, memory), 20);
    EXPECT_EQ(imports, 10);
    imports = 0;
    DlHcommApi::gHcommMemImport = [](EndpointHandle, const void *, uint32_t, HcommCommMem *) -> int32_t {
        ++imports;
        return BM_ERROR;
    };
    EXPECT_EQ(manager.RawMemImportWithRetry(nullptr, nullptr, 0, 0, memory), BM_ERROR);
    EXPECT_EQ(imports, 1);
}

TEST_F(HcommHeterogeneousTest, ZeroCapacityControlKeyImportsFlagOnceAndRejectsMalformedShape)
{
    for (auto location : {ENDPOINT_LOC_TYPE_HOST, ENDPOINT_LOC_TYPE_DEVICE}) {
        auto endpoint = MakeEndpoint(location);
        std::array<uint8_t, DESC_LEN> descriptor{};
        MakeDesc(descriptor.data(), endpoint, POOL, sizeof(uint64_t));
        TransportMemoryKey key;
        ASSERT_EQ(BuildHcommControlKey(descriptor.data(), descriptor.size(), POOL, sizeof(uint64_t), key), BM_OK);
        HcommPeerMemory state{};
        state.peerData = MakePrivate(endpoint);
        const auto previous_imports = imports;
        ASSERT_EQ(ImportHcommPeerKeys(nullptr, {key}, true, state), BM_OK);
        EXPECT_EQ(imports, previous_imports + 1);
        EXPECT_EQ(state.importedDescs.size(), 1U);
        EXPECT_EQ(state.remoteFlagAddr, POOL);
        EXPECT_EQ(state.remoteFlagSize, sizeof(uint64_t));
        EXPECT_EQ(ImportHcommPeerKeys(nullptr, {key}, true, state), BM_OK);
        EXPECT_EQ(imports, previous_imports + 1);
        HcommKeyView view{};
        HcommKeyMetaAt(key).size = POOL_SIZE;
        EXPECT_EQ(ReadHcommKey(key, state.peerData, true, view), BM_INVALID_PARAM);
        EXPECT_EQ(BuildHcommControlKey(nullptr, descriptor.size(), POOL, 8, key), BM_INVALID_PARAM);
        EXPECT_EQ(BuildHcommControlKey(descriptor.data(), descriptor.size(), 0, 8, key), BM_INVALID_PARAM);
    }
}

TEST_F(HcommHeterogeneousTest, ComposeExportsZeroHostControlWithoutRegisteringDataPool)
{
    auto host_manager = std::make_shared<host::HostRdmaHcommTransportManager>();
    host_manager->opened_ = true;
    ASSERT_EQ(host_manager->InitTransferFlagLocked(), BM_OK);
    ComposeTransportManager compose(nullptr);
    compose.hostTransportManager_ = host_manager;
    TransportMemoryKey merged, key;
    ASSERT_EQ(compose.QueryControlKey(merged), BM_OK);
    ReadHostMemoryKey(merged, key);
    HcommKeyView view{};
    ASSERT_EQ(ReadHcommKey(key, MakePrivate(MakeEndpoint()), true, view), BM_OK);
    EXPECT_EQ(view.meta.unused, kHcommControlKey);
    EXPECT_EQ(view.meta.size, sizeof(uint64_t));
    EXPECT_EQ(view.meta.flagLen, 0U);
    EXPECT_TRUE(host_manager->localRegistrations_.empty());
    EXPECT_EQ(registrations, 1); // Only the existing control word, no KV pool.
}

TEST_F(HcommHeterogeneousTest, ComposeExportsZeroDeviceControlAndHostCanRouteIt)
{
    auto device = std::make_shared<DeviceRdmaHcommTransportManager>();
    uint64_t flag = 1;
    device->opened_ = true;
    device->localFlagPtr_ = &flag;
    device->localFlagHandle_ = reinterpret_cast<void *>(1);
    device->localFlagSize_ = sizeof(flag);
    registeredMemory = {COMM_MEM_TYPE_DEVICE, &flag, sizeof(flag)};
    DlHcommApi::gHcommMemExport = [](EndpointHandle, HcommMemHandle, void **desc, uint32_t *size) -> int32_t {
        static uint8_t bytes[DESC_LEN];
        MakeDesc(bytes, MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE), reinterpret_cast<uint64_t>(registeredMemory.addr),
                 registeredMemory.size);
        *desc = bytes;
        *size = DESC_LEN;
        return BM_OK;
    };
    ComposeTransportManager exporter(nullptr);
    exporter.deviceTransportManager_ = device;
    TransportMemoryKey merged;
    const auto result = exporter.QueryControlKey(merged);
    device->localFlagPtr_ = nullptr;
    device->localFlagHandle_ = nullptr;
    ASSERT_EQ(result, BM_OK);
    EXPECT_TRUE(device->localRegistrations_.empty());
    auto tags = std::make_shared<HybmEntityTagInfo>();
    ASSERT_EQ(tags->AddRankTag(0, "host"), BM_OK);
    ASSERT_EQ(tags->AddRankTag(1, "device"), BM_OK);
    ASSERT_EQ(tags->AddTagOpInfo("host:HOST_RDMA:device"), BM_OK);
    ComposeTransportManager receiver(tags);
    receiver.options_.rankId = 0;
    receiver.hostTransportManager_ = std::make_shared<host::HostRdmaHcommTransportManager>();
    HybmTransPrepareOptions input, routed;
    input.options[1].privateData = MakePrivateKey(MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE));
    input.options[1].nic = "device#tcp://127.0.0.1:5000;";
    input.options[1].memKeys = {merged};
    ASSERT_EQ(receiver.GetHostPrepareOptions(input, routed), BM_OK);
    ASSERT_EQ(routed.options.at(1).memKeys.size(), 1U);
    HcommPeerMemory state{};
    state.peerData = MakePrivate(MakeEndpoint(ENDPOINT_LOC_TYPE_DEVICE));
    EXPECT_EQ(ImportHcommPeerKeys(nullptr, routed.options.at(1).memKeys, true, state), BM_OK);
    EXPECT_EQ(imports, 1);
}

TEST_F(HcommHeterogeneousTest, DeviceConnectsZeroHostUsingSingleImportedControlWord)
{
    std::array<uint8_t, DESC_LEN> descriptor{};
    MakeDesc(descriptor.data(), MakeEndpoint(), POOL, sizeof(uint64_t));
    TransportMemoryKey key;
    ASSERT_EQ(BuildHcommControlKey(descriptor.data(), descriptor.size(), POOL, sizeof(uint64_t), key), BM_OK);
    DeviceRdmaHcommTransportManager device;
    device.opened_ = true;
    device.rankId_ = 1;
    device.rankCount_ = 2;
    HybmTransPrepareOptions options;
    options.options[0].privateData = MakePrivateKey(MakeEndpoint());
    options.options[0].peerProtocol = HYBM_DOP_TYPE_HOST_RDMA;
    options.options[0].memKeys = {key};
    ASSERT_EQ(device.Prepare(options), BM_OK);
    EXPECT_EQ(imports, 1);
    EXPECT_EQ(device.remoteRanks_[0].remoteFlagAddr, POOL);
    EXPECT_EQ(device.remoteRanks_[0].remoteFlagSize, sizeof(uint64_t));
    EXPECT_NE(device.remoteRanks_[0].channel, 0U);
    EXPECT_EQ(createdEngine, COMM_ENGINE_AICPU);
    EXPECT_EQ(device.remoteRanks_[0].importedDescs.size(), 1U);
}
