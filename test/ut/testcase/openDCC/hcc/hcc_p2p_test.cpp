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
#include <array>
#include <cstring>
#include <deque>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "host_rdma_opendcc_transport_manager.h"
#include "hcc.h"
#include "hcc_p2p.h"
#include "genetty.h"
#include "mf_opendcc_config.h"
#include "compose_transport_manager.h"
#include "device_rdma_opendcc_transport_manager.h"

namespace {
struct MockQueue {
    std::deque<genetty_cq_wc_t> completions;
};
int posted = 0;
int failAfter = -1;
int stopped = 0;
bool stall = false;
bool stopFailure = false;
int Init(PROTOCOL, const char *, uint32_t)
{
    return GEN_OK;
}
void Uninit(uint32_t) {}
genetty_context_t Context(genetty_type_t, char *, uint32_t)
{
    return reinterpret_cast<void *>(1);
}
void ReturnContext(genetty_context_t, uint32_t) {}
genetty_cq_t CreateCq(genetty_cq_options_t *)
{
    return new MockQueue;
}
void DestroyCq(genetty_cq_t cq, uint32_t)
{
    delete static_cast<MockQueue *>(cq);
}
genetty_t CreateQp(genetty_options_t *options)
{
    return options->cq_handle;
}
void DestroyQp(genetty_t, uint32_t) {}
int Import(genetty_t, genetty_link_info_t *, uint32_t)
{
    return GEN_OK;
}
int Export(genetty_t, uint32_t, genetty_link_info_t *info)
{
    info->data = static_cast<char *>(std::malloc(4));
    std::memcpy(info->data, "link", 4);
    info->length = 4;
    return GEN_OK;
}
int Register(genetty_context_t, genetty_memory_segment_t *segment, uint32_t)
{
    segment->lkey = 11;
    segment->rkey = 12;
    return GEN_OK;
}
int Unregister(genetty_context_t, genetty_memory_segment_t *, uint32_t)
{
    return GEN_OK;
}
int Stop(genetty_t)
{
    ++stopped;
    return stopFailure ? GEN_DEVICE_ERROR : GEN_OK;
}
int Post(genetty_t qp, genetty_batch_op_data_t *data, uint32_t count, genetty_batch_op_options_t *options)
{
    EXPECT_EQ(count, 1U);
    if (posted++ == failAfter) {
        return GEN_POST_FAILED;
    }
    if (options->op_type == OP_WRITE) {
        std::memcpy(data->raddress, data->laddress, data->lsize);
    } else {
        std::memcpy(data->laddress, data->raddress, data->lsize);
    }
    static_cast<MockQueue *>(qp)->completions.push_back({OPS_SUCCESS, options->op_type, 1, options->usr_ctx});
    return GEN_OK;
}
int Poll(genetty_cq_t cq, int32_t, genetty_cq_wc_t *wc, uint32_t)
{
    auto &queue = static_cast<MockQueue *>(cq)->completions;
    if (stall || queue.empty()) {
        return 0;
    }
    *wc = queue.front();
    queue.pop_front();
    return 1;
}

class HccP2pTest : public testing::Test {
protected:
    void SetUp() override
    {
        posted = stopped = 0;
        failAfter = -1;
        stall = stopFailure = false;
        MOCKER(genetty_init).stubs().will(invoke(Init));
        MOCKER(genetty_uninit).stubs().will(invoke(Uninit));
        MOCKER(genetty_context_get_by_ip).stubs().will(invoke(Context));
        MOCKER(genetty_context_return).stubs().will(invoke(ReturnContext));
        MOCKER(genetty_cq_create).stubs().will(invoke(CreateCq));
        MOCKER(genetty_cq_destroy).stubs().will(invoke(DestroyCq));
        MOCKER(genetty_create).stubs().will(invoke(CreateQp));
        MOCKER(genetty_destroy).stubs().will(invoke(DestroyQp));
        MOCKER(genetty_import).stubs().will(invoke(Import));
        MOCKER(genetty_export).stubs().will(invoke(Export));
        MOCKER(genetty_memory_register).stubs().will(invoke(Register));
        MOCKER(genetty_memory_unregister).stubs().will(invoke(Unregister));
        MOCKER(genetty_batch_post).stubs().will(invoke(Post));
        MOCKER(genetty_cq_poll).stubs().will(invoke(Poll));
        MOCKER(genetty_quiesce).stubs().will(invoke(Stop));
        ASSERT_EQ(hcc_init(GEN_PROT_HOST_RDMA, "kunpeng", 0), HCC_OK);
        hcc_options_t options{};
        hcc_options_init(&options);
        comm = hcc_create(&options);
        ASSERT_NE(comm, nullptr);
        hcc_tunnel_options_t tunnelOptions{128, 0};
        tunnel = hcc_tunnel_create(comm, &tunnelOptions);
        ASSERT_NE(tunnel, nullptr);
        char link[] = "peer";
        hcc_link_info_t info{link, sizeof(link)};
        ASSERT_EQ(hcc_tunnel_import(&tunnel, 0, &info), HCC_OK);
        local.fill(0x19);
        remote.fill(0x73);
        memory = {MT_HOST_DRAM, reinterpret_cast<uint64_t>(local.data()), local.size(), 0, 0};
        ASSERT_EQ(hcc_memory_register(&comm, &memory, 0), HCC_OK);
    }
    void TearDown() override
    {
        stopFailure = false;
        if (request != nullptr) {
            EXPECT_EQ(hcc_tunnel_quiesce(tunnel), HCC_OK);
            EXPECT_EQ(hcc_request_release(&request), HCC_OK);
        }
        EXPECT_EQ(hcc_tunnel_destroy(&tunnel, 0), HCC_OK);
        EXPECT_EQ(hcc_destroy(&comm, 0), HCC_OK);
        hcc_uninit(0);
        GlobalMockObject::verify();
    }
    hcc_copy_desc_t Descriptor(uint64_t offset, uint64_t size)
    {
        return {reinterpret_cast<uint64_t>(local.data()) + offset, reinterpret_cast<uint64_t>(remote.data()) + offset,
                size, memory.lkey, memory.rkey};
    }
    hcc_t comm = nullptr;
    hcc_tunnel_t tunnel = nullptr;
    hcc_request_t request = nullptr;
    hcc_mem_segment_t memory{};
    std::array<uint8_t, 128> local{}, remote{};
};

TEST_F(HccP2pTest, LateRegistrationOffsetWriteReadAndGuards)
{
    auto desc = Descriptor(8, 32);
    ASSERT_EQ(hcc_tunnel_submit(tunnel, OP_WRITE, &desc, 1, &request), HCC_OK);
    ASSERT_EQ(hcc_request_wait(request, 100), HCC_OK);
    ASSERT_EQ(hcc_request_release(&request), HCC_OK);
    EXPECT_EQ(remote[7], 0x73);
    EXPECT_EQ(remote[8], 0x19);
    EXPECT_EQ(remote[39], 0x19);
    EXPECT_EQ(remote[40], 0x73);
    remote.fill(0x41);
    desc.local_address += 64;
    ASSERT_EQ(hcc_tunnel_submit(tunnel, OP_READ, &desc, 1, &request), HCC_OK);
    ASSERT_EQ(hcc_request_wait(request, 100), HCC_OK);
    ASSERT_EQ(hcc_request_release(&request), HCC_OK);
    EXPECT_EQ(local[71], 0x19);
    EXPECT_EQ(local[72], 0x41);
    EXPECT_EQ(local[103], 0x41);
    EXPECT_EQ(local[104], 0x19);
}

TEST_F(HccP2pTest, PartialSubmissionStopsBeforeRelease)
{
    std::array<hcc_copy_desc_t, 2> batch{Descriptor(8, 16), Descriptor(40, 16)};
    failAfter = 1;
    ASSERT_EQ(hcc_tunnel_submit(tunnel, OP_WRITE, batch.data(), batch.size(), &request), HCC_OK);
    EXPECT_EQ(hcc_request_wait(request, 100), HCC_POST_FAILED);
    EXPECT_EQ(posted, 2);
    EXPECT_EQ(stopped, 1);
    EXPECT_EQ(remote[8], 0x19);
    EXPECT_EQ(remote[40], 0x73);
    EXPECT_EQ(hcc_request_release(&request), HCC_OK);
    EXPECT_EQ(hcc_memory_unregister(&comm, &memory, 0), HCC_OK);
}

TEST_F(HccP2pTest, PendingRequestPinsMemory)
{
    auto desc = Descriptor(8, 16);
    ASSERT_EQ(hcc_tunnel_submit(tunnel, OP_READ, &desc, 1, &request), HCC_OK);
    EXPECT_EQ(hcc_memory_unregister(&comm, &memory, 0), HCC_BUSY);
    EXPECT_EQ(hcc_request_release(&request), HCC_BUSY);
    EXPECT_EQ(hcc_tunnel_destroy(&tunnel, 0), HCC_BUSY);
    EXPECT_EQ(hcc_destroy(&comm, 0), HCC_BUSY);
    EXPECT_EQ(hcc_request_wait(request, 100), HCC_OK);
    EXPECT_EQ(hcc_request_release(&request), HCC_OK);
    EXPECT_EQ(hcc_memory_unregister(&comm, &memory, 0), HCC_OK);
}

TEST_F(HccP2pTest, TimeoutQuiescesDma)
{
    auto desc = Descriptor(8, 16);
    stall = true;
    ASSERT_EQ(hcc_tunnel_submit(tunnel, OP_WRITE, &desc, 1, &request), HCC_OK);
    EXPECT_EQ(hcc_request_wait(request, 0), HCC_TIMEOUT);
    EXPECT_EQ(stopped, 1);
    EXPECT_EQ(hcc_request_release(&request), HCC_OK);
}

TEST_F(HccP2pTest, StopFailureRetainsRequestAndMemory)
{
    auto desc = Descriptor(8, 16);
    stall = stopFailure = true;
    ASSERT_EQ(hcc_tunnel_submit(tunnel, OP_WRITE, &desc, 1, &request), HCC_OK);
    EXPECT_EQ(hcc_request_wait(request, 0), HCC_DEVICE_ERROR);
    EXPECT_EQ(hcc_request_release(&request), HCC_BUSY);
    EXPECT_EQ(hcc_memory_unregister(&comm, &memory, 0), HCC_BUSY);
    stopFailure = false;
    EXPECT_EQ(hcc_tunnel_quiesce(tunnel), HCC_OK);
    EXPECT_EQ(hcc_request_release(&request), HCC_OK);
}

TEST_F(HccP2pTest, BatchRejectsOutOfRangeBeforeDma)
{
    std::array<hcc_copy_desc_t, 2> batch{Descriptor(8, 16), Descriptor(120, 16)};
    EXPECT_EQ(hcc_tunnel_submit(tunnel, OP_WRITE, batch.data(), batch.size(), &request), HCC_INVALID_PARAM);
    EXPECT_EQ(request, nullptr);
    EXPECT_EQ(posted, 0);
}

TEST_F(HccP2pTest, MultiplePeersHaveIndependentRequests)
{
    hcc_tunnel_options_t options{128, 0};
    auto second = hcc_tunnel_create(comm, &options);
    char data[] = "peer2";
    hcc_link_info_t info{data, sizeof(data)};
    ASSERT_EQ(hcc_tunnel_import(&second, 0, &info), HCC_OK);
    auto firstDesc = Descriptor(8, 16);
    auto secondDesc = Descriptor(48, 16);
    local[48] = 0x45;
    hcc_request_t secondRequest = nullptr;
    ASSERT_EQ(hcc_tunnel_submit(tunnel, OP_WRITE, &firstDesc, 1, &request), HCC_OK);
    ASSERT_EQ(hcc_tunnel_submit(second, OP_WRITE, &secondDesc, 1, &secondRequest), HCC_OK);
    EXPECT_EQ(hcc_request_wait(secondRequest, 100), HCC_OK);
    EXPECT_EQ(hcc_request_wait(request, 100), HCC_OK);
    EXPECT_EQ(remote[8], 0x19);
    EXPECT_EQ(remote[48], 0x45);
    EXPECT_EQ(hcc_request_release(&secondRequest), HCC_OK);
    EXPECT_EQ(hcc_request_release(&request), HCC_OK);
    EXPECT_EQ(hcc_tunnel_destroy(&second, 0), HCC_OK);
}

using namespace ock::mf;
using namespace ock::mf::transport;

TEST(OpenDccSelectionTest, ExplicitEnvironmentSelectsBackend)
{
    const char *value = std::getenv("MF_HYBM_RDMA_USE_OPENDCC");
    bool enabled = value != nullptr && std::strcmp(value, "1") == 0;
    ASSERT_EQ(OpenDccEnabled(), enabled);
    if (!enabled) {
        EXPECT_FALSE(OpenDccHostEnabled());
        return;
    }
    auto open = reinterpret_cast<int32_t (*)(OpenDccTransportManager *, const TransportOptions &)>(
        &OpenDccTransportManager::OpenDevice);
    MOCKER(open).stubs().will(returnValue(BM_OK));
    ComposeTransportManager manager(nullptr);
    TransportOptions options{};
    options.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    EXPECT_EQ(manager.OpenDevice(options), BM_OK);
    auto selected = manager.GetTransport(HYBM_DOP_TYPE_DEVICE_RDMA);
#ifdef ASCEND_NPU
    EXPECT_NE(dynamic_cast<device::DeviceRdmaOpenDccTransportManager *>(selected.get()), nullptr);
#else
    EXPECT_NE(dynamic_cast<host::HostRdmaOpenDccTransportManager *>(selected.get()), nullptr);
    EXPECT_TRUE(OpenDccHostEnabled());
#endif
    EXPECT_EQ(manager.CloseDevice(), BM_OK);
    GlobalMockObject::verify();
}

TEST_F(HccP2pTest, RepeatedRegistrationKeepsMemoryUntilLastReference)
{
    ASSERT_EQ(hcc_memory_register(&comm, &memory, 0), HCC_OK);
    ASSERT_EQ(hcc_memory_unregister(&comm, &memory, 0), HCC_OK);
    auto desc = Descriptor(8, 16);
    ASSERT_EQ(hcc_tunnel_submit(tunnel, OP_WRITE, &desc, 1, &request), HCC_OK);
    EXPECT_EQ(hcc_request_wait(request, 100), HCC_OK);
    EXPECT_EQ(hcc_request_release(&request), HCC_OK);
    EXPECT_EQ(hcc_memory_unregister(&comm, &memory, 0), HCC_OK);
    EXPECT_EQ(hcc_tunnel_submit(tunnel, OP_WRITE, &desc, 1, &request), HCC_INVALID_PARAM);
}

uint16_t FreePort()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    socklen_t size = sizeof(address);
    EXPECT_EQ(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size), 0);
    close(fd);
    return ntohs(address.sin_port);
}

TEST_F(HccP2pTest, MemFabricManagersHandshakeKeysBatchAndReadback)
{
    MOCKER(DlOpenDccApi::Load).stubs().will(returnValue(BM_OK));
    DlOpenDccApi::hcc_init = &::hcc_init;
    DlOpenDccApi::hcc_uninit = &::hcc_uninit;
    DlOpenDccApi::hcc_options_init = &::hcc_options_init;
    DlOpenDccApi::hcc_create = &::hcc_create;
    DlOpenDccApi::hcc_destroy = &::hcc_destroy;
    DlOpenDccApi::hcc_memory_register = &::hcc_memory_register;
    DlOpenDccApi::hcc_memory_unregister = &::hcc_memory_unregister;
    DlOpenDccApi::hcc_memory_info_encode = &::hcc_memory_info_encode;
    DlOpenDccApi::hcc_memory_info_decode = &::hcc_memory_info_decode;
    DlOpenDccApi::hcc_memory_free_info = &::hcc_memory_free_info;
    DlOpenDccApi::hcc_tunnel_create = &::hcc_tunnel_create;
    DlOpenDccApi::hcc_tunnel_destroy = &::hcc_tunnel_destroy;
    DlOpenDccApi::hcc_tunnel_export = &::hcc_tunnel_export;
    DlOpenDccApi::hcc_tunnel_import = &::hcc_tunnel_import;
    DlOpenDccApi::hcc_tunnel_free_link_info = &::hcc_tunnel_free_link_info;
    DlOpenDccApi::hcc_tunnel_submit = &::hcc_tunnel_submit;
    DlOpenDccApi::hcc_request_wait = &::hcc_request_wait;
    DlOpenDccApi::hcc_request_release = &::hcc_request_release;
    DlOpenDccApi::hcc_tunnel_quiesce = &::hcc_tunnel_quiesce;

    setenv("MF_OPENDCC_RDMA_IP", "127.0.0.1", 1);
    std::array<host::HostRdmaOpenDccTransportManager, 3> managers;
    std::array<std::array<uint8_t, 128>, 3> buffers{};
    std::array<TransportMemoryKey, 3> keys;
    for (uint32_t rank = 0; rank < managers.size(); ++rank) {
        TransportOptions options{};
        options.rankId = rank;
        options.rankCount = managers.size();
        options.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
        options.initialType = HYBM_TYPE_HOST_INITIATE;
        auto port = FreePort();
        options.nic = "tcp://127.0.0.1:" + std::to_string(port - rank);
        ASSERT_EQ(managers[rank].OpenDevice(options), BM_OK);
        EXPECT_EQ(managers[rank].GetNic(), "tcp://127.0.0.1:" + std::to_string(port));
        buffers[rank].fill(0x70 + rank);
        TransportMemoryRegion mr{};
        mr.addr = reinterpret_cast<uint64_t>(buffers[rank].data());
        mr.size = buffers[rank].size();
        mr.flags = REG_MR_FLAG_DRAM;
        ASSERT_EQ(managers[rank].RegisterMemoryRegion(mr), BM_OK);
        ASSERT_EQ(managers[rank].QueryMemoryKey(mr.addr, keys[rank]), BM_OK);
        managers[rank].UpdateMemoryKey(keys[rank], reinterpret_cast<void *>(0x1000000ULL * (rank + 1)));
        EXPECT_TRUE(managers[rank].QueryHasRegistered(mr.addr + 8, 16));
        EXPECT_FALSE(managers[rank].QueryHasRegistered(mr.addr + 120, 16));
    }
    HybmTransPrepareOptions peers;
    for (uint32_t rank = 0; rank < managers.size(); ++rank) {
        TransportRankPrepareInfo info;
        info.nic = managers[rank].GetNic();
        info.privateData = managers[rank].GetPrivateData();
        info.memKeys.push_back(keys[rank]);
        peers.options[rank] = info;
    }
    for (auto &manager : managers) {
        ASSERT_EQ(manager.Prepare(peers), BM_OK);
    }
    for (uint32_t rank = 1; rank < managers.size(); ++rank) {
        int accepted = BM_ERROR;
        std::thread accept([&]() { accepted = managers[rank].ConnectRank(0); });
        auto ret = managers[0].ConnectRank(rank);
        accept.join();
        ASSERT_EQ(ret, BM_OK);
        ASSERT_EQ(accepted, BM_OK);
        EXPECT_EQ(managers[rank].GetConnectedProtocols(0), std::vector<uint32_t>{HYBM_DOP_TYPE_DEVICE_RDMA});
        buffers[0][8] = 0x20 + rank;
        auto local = reinterpret_cast<uint64_t>(buffers[0].data());
        auto remote = 0x1000000ULL * (rank + 1);
        ASSERT_EQ(managers[0].WriteRemote(rank, local + 8, remote + 16, 16), BM_OK);
        EXPECT_EQ(buffers[rank][15], 0x70 + rank);
        EXPECT_EQ(buffers[rank][16], 0x20 + rank);
        EXPECT_EQ(buffers[rank][32], 0x70 + rank);
        buffers[rank][16] = 0x40 + rank;
        ASSERT_EQ(managers[0].ReadRemote(rank, local + 64, remote + 16, 16), BM_OK);
        EXPECT_EQ(buffers[0][64], 0x40 + rank);
        EXPECT_EQ(buffers[0][63], 0x70);
        EXPECT_EQ(buffers[0][80], 0x70);
    }
    auto local = reinterpret_cast<uint64_t>(buffers[0].data());
    CopyDescriptor batch{{reinterpret_cast<void *>(local + 8), reinterpret_cast<void *>(local + 32)},
                         {reinterpret_cast<void *>(0x2000010), reinterpret_cast<void *>(0x2000030)},
                         {16, 16}};
    failAfter = posted + 1;
    auto previousStops = stopped;
    EXPECT_NE(managers[0].WriteRemoteBatchAsync(1, batch), BM_OK);
    EXPECT_GT(stopped, previousStops);
    EXPECT_EQ(managers[0].UnregisterMemoryRegion(local), BM_OK);
    for (auto &manager : managers) {
        EXPECT_EQ(manager.CloseDevice(), BM_OK);
    }
}
} // namespace
