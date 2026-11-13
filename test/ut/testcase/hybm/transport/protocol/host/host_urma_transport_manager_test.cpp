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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#include "hybm_common_include.h"

#define private public
#include "dl_acl_api.h"
#include "dl_hcomm_api.h"
#undef private

#include "host_urma_transport_manager.h"
#include "hybm_urma_eid_query.h"
#include "hybm_va_manager.h"

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::urma;
using ock::mf::transport::host::HostUrmaTransportManager;

namespace {
constexpr int32_t LOGICAL_DEVICE = 2;
constexpr int32_t PHYSICAL_DEVICE = 7;
constexpr uint32_t PEER_RANK = 1U;
constexpr ChannelHandle FIRST_CHANNEL = 0xA501U;
const EndpointHandle ENDPOINT = reinterpret_cast<EndpointHandle>(0xA500U);
constexpr char HOST_EID[] = "00112233445566778899aabbccddeeff";
constexpr std::array<uint8_t, COMM_ADDR_EID_LEN> HOST_EID_BYTES{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                                                0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
std::array<uint8_t, 5> gMemoryDescriptor{0x10, 0x20, 0x30, 0x40, 0x50};
std::array<uint8_t, 3> gFlagDescriptor{0xa1, 0xb2, 0xc3};

struct RegisteredMemory {
    std::string tag;
    HcommCommMem memory;
    HcommMemHandle handle;
};

struct CreatedChannel {
    CommEngine engine;
    HcommChannelDesc descriptor;
    std::string name;
    ChannelHandle handle;
};

struct HostUrmaCalls {
    std::vector<EndpointDesc> endpoints;
    std::vector<EndpointHandle> destroyedEndpoints;
    std::vector<RegisteredMemory> registrations;
    std::vector<HcommMemHandle> exports;
    std::vector<HcommMemHandle> unregistrations;
    std::vector<CreatedChannel> channels;
    std::vector<ChannelHandle> polledChannels;
    std::vector<ChannelHandle> destroyedChannels;
    std::vector<char> releaseOrder;
    uint32_t pendingPolls{0U};
    uint32_t importCalls{0U};
};

HostUrmaCalls gCalls;

struct HostUrmaApiGuard {
    aclrtGetDeviceFunc getDevice{DlAclApi::pAclrtGetDevice};
    aclrtGetPhyDevIdByLogicDevIdFunc getPhysicalDevice{DlAclApi::pAclrtGetPhyDevIdByLogicDevId};
    hcommEndpointCreateFunc createEndpoint{DlHcommApi::gHcommEndpointCreate};
    hcommEndpointDestroyFunc destroyEndpoint{DlHcommApi::gHcommEndpointDestroy};
    hcommMemRegFunc registerMemory{DlHcommApi::gHcommMemReg};
    hcommMemUnregFunc unregisterMemory{DlHcommApi::gHcommMemUnreg};
    hcommMemExportFunc exportMemory{DlHcommApi::gHcommMemExport};
    hcommMemImportFunc importMemory{DlHcommApi::gHcommMemImport};
    hcommChannelCreateFunc createChannel{DlHcommApi::gHcommChannelCreate};
    hcommChannelDestroyFunc destroyChannel{DlHcommApi::gHcommChannelDestroy};
    hcommChannelGetStatusFunc channelStatus{DlHcommApi::gHcommChannelGetStatus};

    ~HostUrmaApiGuard()
    {
        DlAclApi::pAclrtGetDevice = getDevice;
        DlAclApi::pAclrtGetPhyDevIdByLogicDevId = getPhysicalDevice;
        DlHcommApi::gHcommEndpointCreate = createEndpoint;
        DlHcommApi::gHcommEndpointDestroy = destroyEndpoint;
        DlHcommApi::gHcommMemReg = registerMemory;
        DlHcommApi::gHcommMemUnreg = unregisterMemory;
        DlHcommApi::gHcommMemExport = exportMemory;
        DlHcommApi::gHcommMemImport = importMemory;
        DlHcommApi::gHcommChannelCreate = createChannel;
        DlHcommApi::gHcommChannelDestroy = destroyChannel;
        DlHcommApi::gHcommChannelGetStatus = channelStatus;
    }
};

HybmTransPrepareOptions MakePeerOptions(uint32_t peerRank = PEER_RANK)
{
    UrmaEndpointDesc endpoint{};
    endpoint.devPhyId = peerRank + 3U;
    endpoint.superDevId = 10U;
    endpoint.serverIdx = 20U;
    endpoint.superPodIdx = 30U;
    endpoint.protocol = UrmaProtocol::UBC_CTP;
    endpoint.type = COMM_ADDR_TYPE_EID;
    endpoint.raws[0] = 0x12;
    endpoint.raws[COMM_ADDR_EID_LEN - 1] = static_cast<uint8_t>(peerRank);
    endpoint.processId = 100U;
    endpoint.instanceId = peerRank;
    UrmaPrivateDataDesc header{};
    header.payloadLen = sizeof(endpoint);
    HybmTransPrepareOptions options{};
    auto &peer = options.options[peerRank];
    peer.role = HYBM_ROLE_PEER;
    peer.privateData.key.ResizeBytes(sizeof(header) + sizeof(endpoint));
    std::memcpy(peer.privateData.key.ByteData(), &header, sizeof(header));
    std::memcpy(peer.privateData.key.ByteData() + sizeof(header), &endpoint, sizeof(endpoint));

    UrmaExportDesc memory{};
    memory.headerSize = sizeof(memory);
    memory.memoryType = UrmaMemoryType::DEVICE_HBM;
    memory.memTag = peerRank;
    memory.addr = 0x800000U + peerRank * 0x10000U;
    memory.size = 0x1000U;
    memory.hcommDescLen = gMemoryDescriptor.size();
    TransportMemoryKey key{};
    key.ResizeWords(DEVICE_URMA_MAX_EXPORT_KEY_LENGTH);
    key.keys[0] = URMA_EXPORT_DESC_MAGIC;
    key.keys[1] = memory.addr;
    auto *payload = key.ByteData() + DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS * sizeof(uint64_t);
    std::memcpy(payload, &memory, sizeof(memory));
    std::memcpy(payload + sizeof(memory), gMemoryDescriptor.data(), gMemoryDescriptor.size());
    peer.memKeys.push_back(std::move(key));
    return options;
}
} // namespace

class HostUrmaTransportManagerTest : public testing::Test {
protected:
    void SetUp() override
    {
        gCalls = {};
        DlAclApi::pAclrtGetDevice = [](int32_t *device) {
            *device = LOGICAL_DEVICE;
            return BM_OK;
        };
        DlAclApi::pAclrtGetPhyDevIdByLogicDevId = [](int32_t logical, int32_t *physical) {
            EXPECT_EQ(logical, LOGICAL_DEVICE);
            *physical = PHYSICAL_DEVICE;
            return BM_OK;
        };
        MOCKER(QueryHostUrmaEid).stubs().will(invoke(+[](uint32_t physical, std::string &eid) {
            EXPECT_EQ(physical, static_cast<uint32_t>(PHYSICAL_DEVICE));
            eid = HOST_EID;
            return BM_OK;
        }));
        DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *desc, EndpointHandle *endpoint) {
            gCalls.endpoints.push_back(*desc);
            *endpoint = ENDPOINT;
            return BM_OK;
        };
        DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle endpoint) {
            gCalls.destroyedEndpoints.push_back(endpoint);
            gCalls.releaseOrder.push_back('E');
            return BM_OK;
        };
        DlHcommApi::gHcommMemReg = [](EndpointHandle endpoint, const char *tag, const HcommCommMem *memory,
                                      HcommMemHandle *handle) {
            EXPECT_EQ(endpoint, ENDPOINT);
            *handle = memory->addr;
            gCalls.registrations.push_back({tag, *memory, *handle});
            return BM_OK;
        };
        DlHcommApi::gHcommMemUnreg = [](EndpointHandle endpoint, HcommMemHandle handle) {
            EXPECT_EQ(endpoint, ENDPOINT);
            gCalls.unregistrations.push_back(handle);
            gCalls.releaseOrder.push_back(handle == gCalls.registrations.front().handle ? 'F' : 'M');
            return BM_OK;
        };
        DlHcommApi::gHcommMemExport = [](EndpointHandle endpoint, HcommMemHandle handle, void **desc, uint32_t *size) {
            EXPECT_EQ(endpoint, ENDPOINT);
            gCalls.exports.push_back(handle);
            if (handle == gCalls.registrations.front().handle) {
                *desc = gFlagDescriptor.data();
                *size = gFlagDescriptor.size();
            } else {
                *desc = gMemoryDescriptor.data();
                *size = gMemoryDescriptor.size();
            }
            return BM_OK;
        };
        DlHcommApi::gHcommMemImport = [](EndpointHandle, const void *, uint32_t, HcommCommMem *) {
            ++gCalls.importCalls;
            return BM_OK;
        };
        DlHcommApi::gHcommChannelCreate = [](EndpointHandle endpoint, CommEngine engine, HcommChannelDesc *descs,
                                             uint32_t count, ChannelHandle *channels) {
            EXPECT_EQ(endpoint, ENDPOINT);
            EXPECT_EQ(count, 1U);
            *channels = FIRST_CHANNEL + gCalls.channels.size();
            gCalls.channels.push_back(
                {engine, *descs, descs->channelName == nullptr ? "" : descs->channelName, *channels});
            return BM_OK;
        };
        DlHcommApi::gHcommChannelGetStatus = [](const ChannelHandle *channels, uint32_t count, int32_t *status) {
            EXPECT_EQ(count, 1U);
            gCalls.polledChannels.push_back(*channels);
            *status = gCalls.pendingPolls == 0U ? 0 : 1;
            if (gCalls.pendingPolls != 0U) {
                --gCalls.pendingPolls;
            }
            return BM_OK;
        };
        DlHcommApi::gHcommChannelDestroy = [](const ChannelHandle *channels, uint32_t count) {
            EXPECT_EQ(count, 1U);
            gCalls.destroyedChannels.push_back(*channels);
            gCalls.releaseOrder.push_back('C');
            return BM_OK;
        };
        options_.rankId = 0U;
        options_.rankCount = 3U;
        options_.protocol = HYBM_DOP_TYPE_HOST_URMA;
        options_.initialType = HYBM_TYPE_HOST_INITIATE;
        options_.scene = HYBM_SCENE_OFFLOAD;
        options_.nic = "host-urma";
        BaseAllocatedGvaInfo allocation{};
        allocation.va[HVM_GVA] = reinterpret_cast<uint64_t>(pool_.data());
        allocation.size = pool_.size();
        allocation.memType = HYBM_MEM_TYPE_HOST;
        ASSERT_EQ(HybmVaManager::GetInstance().AddVaInfo(allocation, true), BM_OK);
        mapped_ = true;
        region_.addr = allocation.va[HVM_GVA] + 64U;
        region_.size = 128U;
        region_.flags = REG_MR_FLAG_DRAM;
        manager_ = std::make_unique<HostUrmaTransportManager>();
    }

    void TearDown() override
    {
        manager_.reset();
        if (mapped_) {
            EXPECT_TRUE(HybmVaManager::GetInstance().RemoveOneVaInfo(reinterpret_cast<uint64_t>(pool_.data())));
        }
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    HostUrmaApiGuard apiGuard_;
    std::array<uint8_t, 512> pool_{};
    bool mapped_{false};
    TransportOptions options_{};
    TransportMemoryRegion region_{};
    std::unique_ptr<HostUrmaTransportManager> manager_;
};

TEST_F(HostUrmaTransportManagerTest, OpenDevicePublishesHostEndpointAndReleasesTransferFlag)
{
    ASSERT_EQ(manager_->OpenDevice(options_), BM_OK);
    ASSERT_EQ(gCalls.endpoints.size(), 1U);
    const auto &endpoint = gCalls.endpoints.front();
    EXPECT_EQ(endpoint.protocol, COMM_PROTOCOL_UBC_CTP);
    EXPECT_EQ(endpoint.loc.locType, ENDPOINT_LOC_TYPE_HOST);
    EXPECT_EQ(endpoint.loc.host.id, options_.rankId);
    EXPECT_EQ(endpoint.commAddr.type, COMM_ADDR_TYPE_EID);
    EXPECT_EQ(std::memcmp(endpoint.commAddr.raws, HOST_EID_BYTES.data(), HOST_EID_BYTES.size()), 0);
    EXPECT_EQ(manager_->GetNic(), options_.nic);
    ASSERT_EQ(gCalls.registrations.size(), 1U);
    const auto &flag = gCalls.registrations.front();
    EXPECT_EQ(flag.memory.type, COMM_MEM_TYPE_HOST);
    EXPECT_EQ(flag.memory.size, sizeof(uint64_t));
    ASSERT_NE(flag.memory.addr, nullptr);
    EXPECT_EQ(*static_cast<const uint64_t *>(flag.memory.addr), 1U);
    EXPECT_EQ(flag.tag, std::to_string(reinterpret_cast<uint64_t>(flag.memory.addr)));

    const auto data = manager_->GetPrivateData();
    ASSERT_GE(data.key.ByteSize(), sizeof(UrmaPrivateDataDesc) + sizeof(UrmaHostEndpointDesc));
    UrmaPrivateDataDesc header{};
    UrmaHostEndpointDesc host{};
    std::memcpy(&header, data.key.ByteData(), sizeof(header));
    std::memcpy(&host, data.key.ByteData() + sizeof(header), sizeof(host));
    EXPECT_EQ(header.magic, URMA_PRIVATE_DATA_MAGIC);
    EXPECT_EQ(header.version, URMA_HOST_PRIVATE_DATA_VERSION);
    EXPECT_EQ(header.payloadLen, sizeof(host));
    EXPECT_EQ(host.hostId, options_.rankId);
    EXPECT_EQ(host.endpoint.devPhyId, static_cast<uint32_t>(PHYSICAL_DEVICE));
    EXPECT_EQ(host.endpoint.superDevId, UINT32_MAX);
    EXPECT_EQ(host.endpoint.processId, static_cast<uint32_t>(getpid()));
    EXPECT_GT(host.endpoint.instanceId, 0U);
    EXPECT_EQ(host.endpoint.protocol, UrmaProtocol::UBC_CTP);
    EXPECT_EQ(host.endpoint.type, COMM_ADDR_TYPE_EID);
    EXPECT_EQ(std::memcmp(host.endpoint.raws, HOST_EID_BYTES.data(), HOST_EID_BYTES.size()), 0);

    EXPECT_EQ(manager_->OpenDevice(options_), BM_OK);
    EXPECT_EQ(gCalls.endpoints.size(), 1U);
    EXPECT_EQ(gCalls.registrations.size(), 1U);
    EXPECT_EQ(manager_->CloseDevice(), BM_OK);
    EXPECT_EQ(gCalls.unregistrations, (std::vector<HcommMemHandle>{flag.handle}));
    EXPECT_EQ(gCalls.destroyedEndpoints, (std::vector<EndpointHandle>{ENDPOINT}));
    EXPECT_EQ(gCalls.releaseOrder, (std::vector<char>{'F', 'E'}));
}

TEST_F(HostUrmaTransportManagerTest, RegisterPoolExportsMemoryAndFlagAndRetainsSharedRegistration)
{
    ASSERT_EQ(manager_->OpenDevice(options_), BM_OK);
    ASSERT_EQ(manager_->RegisterMemoryRegion(region_), BM_OK);
    ASSERT_EQ(manager_->RegisterMemoryRegion(region_), BM_OK);
    ASSERT_EQ(gCalls.registrations.size(), 2U);
    const auto &registration = gCalls.registrations.back();
    EXPECT_EQ(registration.memory.type, COMM_MEM_TYPE_HOST);
    EXPECT_EQ(registration.memory.addr, reinterpret_cast<void *>(region_.addr));
    EXPECT_EQ(registration.memory.size, region_.size);
    EXPECT_EQ(registration.tag, std::to_string(region_.addr));
    EXPECT_TRUE(manager_->QueryHasRegistered(region_.addr, region_.size));

    TransportMemoryKey key{};
    ASSERT_EQ(manager_->QueryMemoryKey(region_.addr, key), BM_OK);
    ASSERT_EQ(key.keys.size(), DEVICE_URMA_MAX_EXPORT_KEY_LENGTH);
    EXPECT_EQ(key.keys[0], URMA_EXPORT_DESC_MAGIC);
    EXPECT_EQ(key.keys[1], region_.addr);
    UrmaExportDesc desc{};
    const auto *payload = key.ByteData() + DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS * sizeof(uint64_t);
    std::memcpy(&desc, payload, sizeof(desc));
    EXPECT_EQ(desc.magic, URMA_EXPORT_DESC_MAGIC);
    EXPECT_EQ(desc.version, URMA_EXPORT_DESC_VERSION);
    EXPECT_EQ(desc.headerSize, sizeof(desc));
    EXPECT_EQ(desc.memoryType, UrmaMemoryType::HOST_DRAM);
    EXPECT_EQ(desc.memTag, region_.addr);
    EXPECT_EQ(desc.addr, region_.addr);
    EXPECT_EQ(desc.size, region_.size);
    EXPECT_EQ(desc.hcommDescLen, gMemoryDescriptor.size());
    EXPECT_EQ(desc.deviceMarkerDescLen, gFlagDescriptor.size());
    EXPECT_EQ(std::memcmp(payload + sizeof(desc), gMemoryDescriptor.data(), gMemoryDescriptor.size()), 0);
    EXPECT_EQ(
        std::memcmp(payload + sizeof(desc) + gMemoryDescriptor.size(), gFlagDescriptor.data(), gFlagDescriptor.size()),
        0);

    ASSERT_EQ(manager_->UnregisterMemoryRegion(region_.addr), BM_OK);
    EXPECT_TRUE(gCalls.unregistrations.empty());
    EXPECT_TRUE(manager_->QueryHasRegistered(region_.addr, region_.size));
    TransportMemoryKey secondKey{};
    ASSERT_EQ(manager_->QueryMemoryKey(region_.addr, secondKey), BM_OK);
    ASSERT_GE(secondKey.keys.size(), DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS);
    EXPECT_EQ(secondKey.keys[0], URMA_EXPORT_DESC_MAGIC);
    EXPECT_EQ(secondKey.keys[1], region_.addr);
    const auto flagHandle = gCalls.registrations.front().handle;
    EXPECT_EQ(gCalls.exports, (std::vector<HcommMemHandle>{registration.handle, flagHandle, flagHandle}));
    auto expectedKey = key;
    auto *mappedAddress = reinterpret_cast<void *>(0x700000U);
    expectedKey.keys[1] = reinterpret_cast<uint64_t>(mappedAddress);
    manager_->UpdateMemoryKey(key, mappedAddress);
    EXPECT_EQ(key.keys, expectedKey.keys);

    EXPECT_EQ(manager_->UnregisterMemoryRegion(region_.addr), BM_OK);
    EXPECT_EQ(gCalls.unregistrations, (std::vector<HcommMemHandle>{registration.handle}));
    EXPECT_FALSE(manager_->QueryHasRegistered(region_.addr, region_.size));
}

TEST_F(HostUrmaTransportManagerTest, PrepareCreatesCpuChannelAndConnectPollsUntilReady)
{
    ASSERT_EQ(manager_->OpenDevice(options_), BM_OK);
    const auto peerOptions = MakePeerOptions();
    ASSERT_EQ(manager_->Prepare(peerOptions), BM_OK);
    ASSERT_EQ(gCalls.channels.size(), 1U);
    const auto &channel = gCalls.channels.front();
    const auto &remote = channel.descriptor.remoteEndpoint;
    EXPECT_EQ(channel.engine, COMM_ENGINE_CPU);
    EXPECT_EQ(channel.descriptor.header.version, HCOMM_CHANNEL_VERSION);
    EXPECT_EQ(channel.descriptor.header.magicWord, HCOMM_CHANNEL_MAGIC_WORD);
    EXPECT_EQ(channel.descriptor.header.size, sizeof(HcommChannelDesc));
    EXPECT_EQ(channel.descriptor.role, HCOMM_SOCKET_ROLE_SERVER);
    EXPECT_TRUE(channel.descriptor.exchangeAllMems);
    EXPECT_EQ(channel.descriptor.port, 0U);
    EXPECT_EQ(remote.protocol, COMM_PROTOCOL_UBC_CTP);
    EXPECT_EQ(remote.loc.locType, ENDPOINT_LOC_TYPE_DEVICE);
    EXPECT_EQ(remote.loc.device.devPhyId, PEER_RANK + 3U);
    EXPECT_EQ(remote.loc.device.superDevId, 10U);
    EXPECT_EQ(remote.loc.device.serverIdx, 20U);
    EXPECT_EQ(remote.loc.device.superPodIdx, 30U);
    EXPECT_EQ(remote.commAddr.type, COMM_ADDR_TYPE_EID);
    EXPECT_EQ(remote.commAddr.raws[0], 0x12);
    EXPECT_EQ(remote.commAddr.raws[COMM_ADDR_EID_LEN - 1], PEER_RANK);
    const auto localData = manager_->GetPrivateData();
    UrmaHostEndpointDesc host{};
    UrmaEndpointDesc peer{};
    ASSERT_GE(localData.key.ByteSize(), sizeof(UrmaPrivateDataDesc) + sizeof(host));
    std::memcpy(&host, localData.key.ByteData() + sizeof(UrmaPrivateDataDesc), sizeof(host));
    std::memcpy(&peer, peerOptions.options.at(PEER_RANK).privateData.key.ByteData() + sizeof(UrmaPrivateDataDesc),
                sizeof(peer));
    EXPECT_EQ(channel.name, UrmaChannelSessionName(host.endpoint, peer));
    EXPECT_EQ(channel.name, UrmaChannelSessionName(peer, host.endpoint));
    EXPECT_EQ(gCalls.importCalls, 0U);
    EXPECT_TRUE(gCalls.polledChannels.empty());

    gCalls.pendingPolls = 2U;
    EXPECT_EQ(manager_->ConnectRank(PEER_RANK), BM_OK);
    EXPECT_EQ(gCalls.polledChannels, (std::vector<ChannelHandle>{FIRST_CHANNEL, FIRST_CHANNEL, FIRST_CHANNEL}));
    EXPECT_TRUE(gCalls.destroyedChannels.empty());
}

TEST_F(HostUrmaTransportManagerTest, UpdateRankOptionsCreatesPeerThenReusesChannelForMemoryUpdates)
{
    ASSERT_EQ(manager_->OpenDevice(options_), BM_OK);
    const auto peerOptions = MakePeerOptions();
    auto endpointOnly = peerOptions;
    endpointOnly.options.at(PEER_RANK).memKeys.clear();
    ASSERT_EQ(manager_->UpdateRankOptions(endpointOnly), BM_OK);
    ASSERT_EQ(gCalls.channels.size(), 1U);
    const auto name = gCalls.channels.front().name;

    EXPECT_EQ(manager_->Prepare(endpointOnly), BM_OK);
    EXPECT_EQ(manager_->UpdateRankOptions(endpointOnly), BM_OK);
    EXPECT_EQ(manager_->UpdateRankOptions(peerOptions), BM_OK);
    EXPECT_EQ(gCalls.channels.size(), 1U);
    EXPECT_EQ(gCalls.channels.front().name, name);
    EXPECT_EQ(gCalls.importCalls, 0U);
    EXPECT_EQ(manager_->ConnectRank(PEER_RANK), BM_OK);
    EXPECT_EQ(gCalls.polledChannels, (std::vector<ChannelHandle>{FIRST_CHANNEL}));
    EXPECT_TRUE(gCalls.destroyedChannels.empty());
}

TEST_F(HostUrmaTransportManagerTest, RemoveRankPreservesOtherPeerAndSupportsReconnect)
{
    ASSERT_EQ(manager_->OpenDevice(options_), BM_OK);
    ASSERT_EQ(manager_->Prepare(MakePeerOptions(1U)), BM_OK);
    ASSERT_EQ(manager_->Prepare(MakePeerOptions(2U)), BM_OK);
    ASSERT_EQ(gCalls.channels.size(), 2U);
    const auto firstName = gCalls.channels.front().name;

    EXPECT_EQ(manager_->RemoveRanks({1U}), BM_OK);
    EXPECT_EQ(gCalls.destroyedChannels, (std::vector<ChannelHandle>{FIRST_CHANNEL}));
    EXPECT_EQ(manager_->ConnectRank(2U), BM_OK);
    EXPECT_EQ(gCalls.polledChannels, (std::vector<ChannelHandle>{FIRST_CHANNEL + 1U}));
    ASSERT_EQ(manager_->Prepare(MakePeerOptions(1U)), BM_OK);
    ASSERT_EQ(gCalls.channels.size(), 3U);
    EXPECT_EQ(gCalls.channels.back().name, firstName);
    EXPECT_EQ(manager_->ConnectRank(1U), BM_OK);
    EXPECT_EQ(gCalls.polledChannels.back(), FIRST_CHANNEL + 2U);
    EXPECT_EQ(manager_->CloseDevice(), BM_OK);
    auto destroyed = gCalls.destroyedChannels;
    std::sort(destroyed.begin(), destroyed.end());
    EXPECT_EQ(destroyed, (std::vector<ChannelHandle>{FIRST_CHANNEL, FIRST_CHANNEL + 1U, FIRST_CHANNEL + 2U}));
    EXPECT_EQ(gCalls.destroyedEndpoints, (std::vector<EndpointHandle>{ENDPOINT}));
}

TEST_F(HostUrmaTransportManagerTest, DestructorReleasesLiveChannelAndMemoryBeforeEndpoint)
{
    ASSERT_EQ(manager_->OpenDevice(options_), BM_OK);
    ASSERT_EQ(manager_->RegisterMemoryRegion(region_), BM_OK);
    ASSERT_EQ(manager_->Prepare(MakePeerOptions()), BM_OK);
    ASSERT_EQ(manager_->ConnectRank(PEER_RANK), BM_OK);
    ASSERT_EQ(gCalls.registrations.size(), 2U);
    const auto flagHandle = gCalls.registrations.front().handle;
    const auto poolHandle = gCalls.registrations.back().handle;

    manager_.reset();

    EXPECT_EQ(gCalls.destroyedChannels, (std::vector<ChannelHandle>{FIRST_CHANNEL}));
    EXPECT_EQ(gCalls.unregistrations, (std::vector<HcommMemHandle>{poolHandle, flagHandle}));
    EXPECT_EQ(gCalls.destroyedEndpoints, (std::vector<EndpointHandle>{ENDPOINT}));
    EXPECT_EQ(gCalls.releaseOrder, (std::vector<char>{'C', 'M', 'F', 'E'}));
}
