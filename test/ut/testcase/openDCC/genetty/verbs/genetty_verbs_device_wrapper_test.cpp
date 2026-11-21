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

#include <arpa/inet.h>
#include <memory>
#include <vector>
#include <thread>
#include <array>
#include <sys/socket.h>

#define protected public
#include "mf_ref.h"
#undef protected

#include "genetty.h"
#include "mock_device_ts.h"
#include "verbs/genetty_verbs_types.h"
#include "genetty_info_types.h"
#include "under_api/genetty_dl_api.h"

#define private   public
#define protected public
#include "pingpong_genetty/odcc_pp_genetty.h"
#include "verbs/genetty_verbs_backend.h"
#include "verbs/genetty_verbs_device_cq_qp.h"
#include "verbs/genetty_verbs_host_cq_qp.h"
#include "verbs/genetty_verbs_device_wrapper.h"
#include "verbs/genetty_verbs_device_ctx_pool.h"
#include "under_api/acl/genetty_dl_acl_api.h"
#include "under_api/hccp/genetty_dl_hccp_api.h"
#include "under_api/tsd/genetty_dl_tsd_api.h"
#undef private
#undef protected

namespace ock {
namespace openDCC {
namespace verbs {
namespace {
constexpr int MOCK_FAILURE = 123;
constexpr int32_t ACL_REPEAT_INITIALIZE = 100002;
constexpr int32_t USER_DEVICE_ID = 0;
constexpr int32_t LOGIC_DEVICE_ID = 2;
constexpr int32_t PHYSICAL_DEVICE_ID = 3;
constexpr uint32_t TSD_RANK_SIZE = 2;
constexpr int HCCP_NETWORK_OFFLINE = 1;
constexpr int HCCP_HDC_TYPE_RDMA = 6;
constexpr uint32_t HCCP_NOTIFY = 1;
constexpr const char *DEVICE_IP = "192.0.2.1";
constexpr uint32_t MULTI_DEVICE_COUNT = 2;
constexpr uint32_t INTERFACE_COUNT = 3;
constexpr uint32_t DEVICE_INTERFACE_INDEX = 2;
constexpr uint16_t HOST_PORT_INDEX = 1;
constexpr int ACQUIRED_REFERENCES = 2;
constexpr uint64_t MR_ADDRESS = 0x100000;
constexpr uint64_t MR_SIZE = 4096;
constexpr uint32_t MR_LKEY = 0x1234;
constexpr uint32_t MR_RKEY = 0x5678;
constexpr size_t DISTINCT_MR_COUNT = 4;
constexpr int MR_OPERATION_ATTEMPTS = 2;

enum class CleanupStep { MR, RDEV, RA, TSD };

struct MockState {
    int deregisterMrResult = 0;
    int deregisterMrCalls = 0;
    int failedDeregisterCall = -1;
    const void *deregisteredRdev = nullptr;
    void *deregisteredMr = nullptr;
    int registerMrResult = 0;
    int registerMrCalls = 0;
    bool nullMrHandle = false;
    const void *registeredRdev = nullptr;
    HccpMrInfo registeredInfo{};
    int aclInitResult = 0;
    int finalizeResult = 0;
    int rdevInitResult = 0;
    int rdevCloseResult = 0;
    int raCloseResult = 0;
    uint32_t tsdCloseResult = 0;
    int aclInitCalls = 0;
    int finalizeCalls = 0;
    int finalizeReferenceCalls = 0;
    int liveHandles = 0;
    int liveRa = 0;
    int liveTsd = 0;
    uint32_t deviceCount = MULTI_DEVICE_COUNT;
    int deviceCountResult = 0;
    int deviceCountCalls = 0;
    int32_t currentDeviceId = USER_DEVICE_ID;
    int32_t failedDeviceId = -1;
    int interfaceResult = 0;
    uint32_t interfaceCount = INTERFACE_COUNT;
    uint32_t returnedInterfaceCount = INTERFACE_COUNT;
    bool wrongInterfaceName = false;
    bool failRestore = false;
    std::vector<int32_t> selectedDevices;
    std::vector<CleanupStep> cleanup;
};

MockState state;

int DeregisterMr(const void *rdmaHandle, void *mrHandle)
{
    EXPECT_NE(rdmaHandle, nullptr);
    EXPECT_EQ(*static_cast<const int *>(rdmaHandle), 0);
    ++state.deregisterMrCalls;
    state.deregisteredRdev = rdmaHandle;
    state.deregisteredMr = mrHandle;
    state.cleanup.push_back(CleanupStep::MR);
    return state.deregisterMrCalls == state.failedDeregisterCall ? MOCK_FAILURE : state.deregisterMrResult;
}

int CloseRdev(void *handle, uint32_t notifyType)
{
    EXPECT_EQ(notifyType, HCCP_NOTIFY);
    /* Match RA: an error can be reported after the host handle has been freed. */
    delete static_cast<int *>(handle);
    --state.liveHandles;
    state.cleanup.push_back(CleanupStep::RDEV);
    return state.rdevCloseResult;
}

void InstallAclMocks()
{
    AclApi::LOADED = true;
    AclApi::INITIALIZED = false;
    AclApi::OWNS_INITIALIZATION = false;
    AclApi::aclInit_ptr = [](const char *) {
        ++state.aclInitCalls;
        return state.aclInitResult;
    };
    AclApi::aclFinalize_ptr = []() {
        ++state.finalizeCalls;
        return state.finalizeResult;
    };
    AclApi::aclFinalizeReference_ptr = nullptr;
    AclApi::aclrtGetDeviceCount_ptr = [](uint32_t *count) {
        ++state.deviceCountCalls;
        if (state.deviceCountResult == 0) {
            *count = state.deviceCount;
        }
        return state.deviceCountResult;
    };
    AclApi::aclrtSetDevice_ptr = [](int32_t id) {
        EXPECT_EQ(id, USER_DEVICE_ID);
        return 0;
    };
    AclApi::aclrtGetDevice_ptr = [](int32_t *id) {
        *id = USER_DEVICE_ID;
        return 0;
    };
    AclApi::aclrtGetLogicDevIdByUserDevId_ptr = [](int32_t userId, int32_t *id) {
        EXPECT_EQ(userId, USER_DEVICE_ID);
        *id = LOGIC_DEVICE_ID;
        return 0;
    };
    AclApi::aclrtGetPhyDevIdByUserDevId_ptr = [](int32_t userId, int32_t *id) {
        EXPECT_EQ(userId, USER_DEVICE_ID);
        *id = PHYSICAL_DEVICE_ID;
        return 0;
    };
}

void InstallDeviceMocks()
{
    TsdApi::LOADED = true;
    HccpApi::LOADED = true;
    TsdApi::TsdOpen_ptr = [](uint32_t id, uint32_t rankSize) -> uint32_t {
        EXPECT_EQ(id, USER_DEVICE_ID);
        EXPECT_EQ(rankSize, TSD_RANK_SIZE);
        ++state.liveTsd;
        return 0;
    };
    TsdApi::TsdClose_ptr = [](uint32_t id) {
        EXPECT_EQ(id, USER_DEVICE_ID);
        state.cleanup.push_back(CleanupStep::TSD);
        if (state.tsdCloseResult == 0) {
            --state.liveTsd;
        }
        return state.tsdCloseResult;
    };
    HccpApi::RaInit_ptr = [](HccpRaInitConfig *config) {
        EXPECT_EQ(config->phyId, PHYSICAL_DEVICE_ID);
        EXPECT_EQ(config->nicPosition, HCCP_NETWORK_OFFLINE);
        EXPECT_EQ(config->hdcType, HCCP_HDC_TYPE_RDMA);
        EXPECT_FALSE(config->enableHdcAsync);
        ++state.liveRa;
        return 0;
    };
    HccpApi::RaDeinit_ptr = [](HccpRaInitConfig *config) {
        EXPECT_EQ(config->phyId, PHYSICAL_DEVICE_ID);
        state.cleanup.push_back(CleanupStep::RA);
        if (state.raCloseResult == 0) {
            --state.liveRa;
        }
        return state.raCloseResult;
    };
    HccpApi::RaRdevInitV2_ptr = [](HccpRdevInitInfo, HccpRdev, void **handle) {
        if (state.rdevInitResult == 0) {
            *handle = new int(0);
            ++state.liveHandles;
        }
        return state.rdevInitResult;
    };
    HccpApi::RaRdevDeinit_ptr = CloseRdev;
}

class DeviceRdmaContextTest : public testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_EQ(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
        state = MockState{};
        InstallAclMocks();
        InstallDeviceMocks();
    }

    void TearDown() override
    {
        state.raCloseResult = 0;
        state.tsdCloseResult = 0;
        state.rdevCloseResult = 0;
        state.finalizeResult = 0;
        HccpApi::RaRdevDeinit_ptr = CloseRdev;
        EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_OK);
        EXPECT_EQ(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
        EXPECT_EQ(state.liveHandles, 0);
        EXPECT_EQ(state.liveRa, 0);
        EXPECT_EQ(state.liveTsd, 0);
        EXPECT_EQ(AclApi::AclFinalize(), 0);
        AclApi::UnLoad();
        TsdApi::UnLoad();
        HccpApi::UnLoad();
    }
};

void InstallInterfaceMocks()
{
    HccpApi::RaGetIfNum_ptr = [](HccpRaGetIfAttr *config, uint32_t *count) {
        EXPECT_GT(state.liveRa, 0);
        EXPECT_EQ(config->phyId, static_cast<uint32_t>(state.currentDeviceId + PHYSICAL_DEVICE_ID));
        EXPECT_EQ(config->nicPosition, HCCP_NETWORK_OFFLINE);
        EXPECT_TRUE(config->isAll);
        *count = state.interfaceCount;
        return state.interfaceResult;
    };
    HccpApi::RaGetIfAddrs_ptr = [](HccpRaGetIfAttr *config, HccpInterfaceInfo infos[], uint32_t *count) {
        EXPECT_EQ(*count, state.interfaceCount);
        for (uint32_t i = 0; i < *count; ++i) {
            infos[i].family = i == DEVICE_INTERFACE_INDEX ? AF_INET : AF_INET6;
            auto name = state.wrongInterfaceName ? "lo" : "eth" + std::to_string(config->phyId);
            std::strncpy(infos[i].ifname, name.c_str(), sizeof(infos[i].ifname) - 1);
            auto ip = "192.0.2." + std::to_string(state.currentDeviceId + 1);
            EXPECT_EQ(inet_pton(AF_INET, ip.c_str(), &infos[i].ifaddr.ip.addr), 1);
        }
        *count = state.returnedInterfaceCount;
        return 0;
    };
}

void InstallEnumerationMocks()
{
    AclApi::aclrtSetDevice_ptr = [](int32_t id) {
        if (id == state.failedDeviceId || (state.failRestore && state.selectedDevices.size() >= state.deviceCount)) {
            return MOCK_FAILURE;
        }
        state.currentDeviceId = id;
        state.selectedDevices.push_back(id);
        return 0;
    };
    AclApi::aclrtGetDevice_ptr = [](int32_t *id) {
        *id = state.currentDeviceId;
        return *id < 0 ? MOCK_FAILURE : 0;
    };
    AclApi::aclrtGetLogicDevIdByUserDevId_ptr = [](int32_t id, int32_t *logicId) {
        *logicId = id + LOGIC_DEVICE_ID;
        return 0;
    };
    AclApi::aclrtGetPhyDevIdByUserDevId_ptr = [](int32_t id, int32_t *physicalId) {
        *physicalId = id + PHYSICAL_DEVICE_ID;
        return 0;
    };
    TsdApi::TsdOpen_ptr = [](uint32_t, uint32_t rankSize) -> uint32_t {
        EXPECT_EQ(rankSize, TSD_RANK_SIZE);
        ++state.liveTsd;
        return 0;
    };
    TsdApi::TsdClose_ptr = [](uint32_t) -> uint32_t {
        state.cleanup.push_back(CleanupStep::TSD);
        --state.liveTsd;
        return 0;
    };
    HccpApi::RaInit_ptr = [](HccpRaInitConfig *) {
        ++state.liveRa;
        return 0;
    };
    HccpApi::RaDeinit_ptr = [](HccpRaInitConfig *) {
        state.cleanup.push_back(CleanupStep::RA);
        --state.liveRa;
        return 0;
    };
    InstallInterfaceMocks();
}

class DeviceContextApiTest : public DeviceRdmaContextTest {
protected:
    void SetUp() override
    {
        DeviceRdmaContextTest::SetUp();
        InstallEnumerationMocks();
        ASSERT_EQ(genetty_init(GEN_PROT_DEVICE_RDMA, "ascend", 0), GEN_OK);
    }

    void TearDown() override
    {
        genetty_uninit(0);
        DeviceRdmaContextTest::TearDown();
    }
};

class DeviceMemoryRegistrationTest : public DeviceContextApiTest {
protected:
    void SetUp() override
    {
        DeviceContextApiTest::SetUp();
        context_ = genetty_context_get_by_id(DEVICE_RDMA_GENETTY, LOGIC_DEVICE_ID, 0);
        ASSERT_NE(context_, nullptr);
        HccpApi::RaRegisterMr_ptr = [](const void *rdmaHandle, HccpMrInfo *info, void **mrHandle) {
            ++state.registerMrCalls;
            state.registeredRdev = rdmaHandle;
            state.registeredInfo = *info;
            info->lkey = MR_LKEY;
            info->rkey = MR_RKEY;
            if (state.registerMrResult == 0 && !state.nullMrHandle) {
                *mrHandle = &state.registerMrCalls;
            }
            return state.registerMrResult;
        };
        HccpApi::RaDeregisterMr_ptr = DeregisterMr;
    }

    void TearDown() override
    {
        state.deregisterMrResult = 0;
        state.failedDeregisterCall = -1;
        HccpApi::RaDeregisterMr_ptr = DeregisterMr;
        if (context_ != nullptr) {
            genetty_context_return(context_, 0);
        }
        DeviceContextApiTest::TearDown();
    }

    genetty_context_t context_ = nullptr;
    genetty_memory_segment_t segment_{MT_DEVICE_HBM, MR_ADDRESS, MR_SIZE, 0, 0};
};

TEST_F(DeviceMemoryRegistrationTest, RegistersAndReusesKeysForTheSameMemory)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(state.registeredRdev, ctx->wrapCtx_->Handle());
    EXPECT_EQ(state.registeredInfo.addr, reinterpret_cast<void *>(MR_ADDRESS));
    EXPECT_EQ(state.registeredInfo.size, MR_SIZE);
    EXPECT_EQ(state.registeredInfo.access, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    EXPECT_EQ(segment_.lkey, MR_LKEY);
    EXPECT_EQ(segment_.rkey, MR_RKEY);
    ASSERT_EQ(ctx->mrs_.size(), 1);
    EXPECT_EQ(ctx->mrs_.begin()->second.handle, &state.registerMrCalls);
    segment_.lkey = 0;
    segment_.rkey = 0;
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(state.registerMrCalls, 1);
    EXPECT_EQ(segment_.lkey, MR_LKEY);
    EXPECT_EQ(segment_.rkey, MR_RKEY);
}

TEST_F(DeviceMemoryRegistrationTest, DistinguishesMemoryTypeAddressAndSize)
{
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    auto other = segment_;
    other.type = MT_HOST_DRAM;
    ASSERT_EQ(genetty_memory_register(context_, &other, 0), GEN_OK);
    other = segment_;
    other.address += MR_SIZE;
    ASSERT_EQ(genetty_memory_register(context_, &other, 0), GEN_OK);
    other = segment_;
    other.size += MR_SIZE;
    ASSERT_EQ(genetty_memory_register(context_, &other, 0), GEN_OK);
    EXPECT_EQ(state.registerMrCalls, DISTINCT_MR_COUNT);
    EXPECT_EQ(reinterpret_cast<DeviceVerbsContext *>(context_)->mrs_.size(), DISTINCT_MR_COUNT);
}

TEST_F(DeviceMemoryRegistrationTest, FailureDoesNotCacheOrPublishKeysAndCanBeRetried)
{
    state.registerMrResult = MOCK_FAILURE;
    EXPECT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_ERROR);
    EXPECT_TRUE(reinterpret_cast<DeviceVerbsContext *>(context_)->mrs_.empty());
    EXPECT_EQ(segment_.lkey, 0);
    EXPECT_EQ(segment_.rkey, 0);
    state.registerMrResult = 0;
    EXPECT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(segment_.lkey, MR_LKEY);
    EXPECT_EQ(segment_.rkey, MR_RKEY);
}

TEST_F(DeviceMemoryRegistrationTest, RejectsNullHandleAndMissingApi)
{
    state.nullMrHandle = true;
    EXPECT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_ERROR);
    EXPECT_TRUE(reinterpret_cast<DeviceVerbsContext *>(context_)->mrs_.empty());
    HccpApi::RaRegisterMr_ptr = nullptr;
    EXPECT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_ERROR);
    EXPECT_EQ(state.registerMrCalls, 1);
    EXPECT_TRUE(reinterpret_cast<DeviceVerbsContext *>(context_)->mrs_.empty());
    EXPECT_EQ(segment_.lkey, 0);
    EXPECT_EQ(segment_.rkey, 0);
}

TEST_F(DeviceMemoryRegistrationTest, RejectsNullSegmentAndUnavailableContext)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    EXPECT_EQ(ctx->RegisterMemory(nullptr, 0), GEN_INVALID_PARAM);
    DeviceVerbsContext emptyContext(DEVICE_RDMA_GENETTY, nullptr);
    EXPECT_EQ(emptyContext.RegisterMemory(&segment_, 0), GEN_DEVICE_ERROR);
    ctx->wrapCtx_->Destroy();
    EXPECT_EQ(ctx->RegisterMemory(&segment_, 0), GEN_DEVICE_ERROR);
    EXPECT_TRUE(ctx->mrs_.empty());
    EXPECT_EQ(state.registerMrCalls, 0);
}

TEST_F(DeviceMemoryRegistrationTest, RejectsInvalidMemoryAtTheCApi)
{
    EXPECT_EQ(genetty_memory_register(context_, nullptr, 0), GEN_INVALID_PARAM);
    auto invalid = segment_;
    invalid.address = 0;
    EXPECT_EQ(genetty_memory_register(context_, &invalid, 0), GEN_INVALID_PARAM);
    invalid = segment_;
    invalid.size = 0;
    EXPECT_EQ(genetty_memory_register(context_, &invalid, 0), GEN_INVALID_PARAM);
    invalid = segment_;
    invalid.type = MT_INVALID;
    EXPECT_EQ(genetty_memory_register(context_, &invalid, 0), GEN_INVALID_PARAM);
    EXPECT_EQ(state.registerMrCalls, 0);
}

TEST_F(DeviceMemoryRegistrationTest, UnregistersOnceAndAllowsNewRegistration)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(state.deregisteredRdev, ctx->wrapCtx_->Handle());
    EXPECT_EQ(state.deregisteredMr, &state.registerMrCalls);
    EXPECT_TRUE(ctx->mrs_.empty());
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_NOT_FOUND);
    EXPECT_EQ(state.deregisterMrCalls, 1);
    EXPECT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(state.registerMrCalls, MR_OPERATION_ATTEMPTS);
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(state.deregisterMrCalls, MR_OPERATION_ATTEMPTS);
}

TEST_F(DeviceMemoryRegistrationTest, FailedUnregisterKeepsTheRecordForRetry)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    state.deregisterMrResult = MOCK_FAILURE;
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_ERROR);
    ASSERT_EQ(ctx->mrs_.size(), 1);
    const auto &mr = ctx->mrs_.begin()->second;
    EXPECT_EQ(mr.handle, &state.registerMrCalls);
    EXPECT_EQ(mr.address, MR_ADDRESS);
    EXPECT_EQ(mr.size, MR_SIZE);
    EXPECT_EQ(mr.lkey, MR_LKEY);
    EXPECT_EQ(mr.rkey, MR_RKEY);
    state.deregisterMrResult = 0;
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(state.deregisterMrCalls, MR_OPERATION_ATTEMPTS);
    EXPECT_TRUE(ctx->mrs_.empty());
}

TEST_F(DeviceMemoryRegistrationTest, MissingUnregisterApiKeepsTheRecord)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    auto deregister = HccpApi::RaDeregisterMr_ptr;
    HccpApi::RaDeregisterMr_ptr = nullptr;
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_ERROR);
    EXPECT_EQ(ctx->mrs_.size(), 1);
    EXPECT_EQ(state.deregisterMrCalls, 0);
    HccpApi::RaDeregisterMr_ptr = deregister;
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_OK);
    EXPECT_TRUE(ctx->mrs_.empty());
}

TEST_F(DeviceMemoryRegistrationTest, UnregisterRequiresMatchingMemory)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_NOT_FOUND);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    auto other = segment_;
    other.type = MT_HOST_DRAM;
    EXPECT_EQ(genetty_memory_unregister(context_, &other, 0), GEN_NOT_FOUND);
    other = segment_;
    other.address += MR_SIZE;
    EXPECT_EQ(genetty_memory_unregister(context_, &other, 0), GEN_NOT_FOUND);
    other = segment_;
    other.size += MR_SIZE;
    EXPECT_EQ(genetty_memory_unregister(context_, &other, 0), GEN_NOT_FOUND);
    EXPECT_EQ(ctx->mrs_.size(), 1);
    EXPECT_EQ(state.deregisterMrCalls, 0);
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_OK);
}

TEST_F(DeviceMemoryRegistrationTest, UnregisterChecksSegmentAndHandles)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    EXPECT_EQ(genetty_memory_unregister(context_, nullptr, 0), GEN_INVALID_PARAM);
    EXPECT_EQ(ctx->UnRegisterMemory(nullptr, 0), GEN_INVALID_PARAM);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    ctx->mrs_.begin()->second.handle = nullptr;
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_ERROR);
    ASSERT_EQ(ctx->mrs_.size(), 1);
    ctx->mrs_.begin()->second.handle = &state.registerMrCalls;
    DeviceVerbsContext emptyContext(DEVICE_RDMA_GENETTY, nullptr);
    emptyContext.mrs_ = ctx->mrs_;
    EXPECT_EQ(emptyContext.UnRegisterMemory(&segment_, 0), GEN_DEVICE_ERROR);
    EXPECT_EQ(emptyContext.mrs_.size(), 1);
    ctx->wrapCtx_->Destroy();
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_DEVICE_ERROR);
    EXPECT_EQ(ctx->mrs_.size(), 1);
    EXPECT_EQ(state.deregisterMrCalls, 0);
    /* These injected records no longer have a live owning device. */
    ctx->mrs_.clear();
    emptyContext.mrs_.clear();
}

TEST_F(DeviceMemoryRegistrationTest, DestroyUnregistersMemoryBeforeClosingDevice)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    segment_.address += MR_SIZE;
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    state.cleanup.clear();
    ctx->Destroy();
    EXPECT_TRUE(ctx->mrs_.empty());
    EXPECT_EQ(ctx->wrapCtx_->Handle(), nullptr);
    EXPECT_EQ(state.cleanup, (std::vector<CleanupStep>{CleanupStep::MR, CleanupStep::MR, CleanupStep::RDEV,
                                                       CleanupStep::RA, CleanupStep::TSD}));
    auto cleanupCount = state.cleanup.size();
    ctx->Destroy();
    EXPECT_EQ(state.cleanup.size(), cleanupCount);
    EXPECT_EQ(ctx->RegisterMemory(&segment_, 0), GEN_DEVICE_ERROR);
}

TEST_F(DeviceMemoryRegistrationTest, DestroyRetainsOnlyFailedMemoryForRetry)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    segment_.address += MR_SIZE;
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    auto handle = ctx->wrapCtx_->Handle();
    state.failedDeregisterCall = 1;
    state.cleanup.clear();
    ctx->Destroy();
    ASSERT_NE(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    auto pending = DeviceRdmaContext::PENDING_CLEANUP;
    ASSERT_EQ(pending->pendingMrs_.size(), 1);
    EXPECT_EQ(pending->pendingMrs_.begin()->second.address, MR_ADDRESS);
    EXPECT_EQ(pending->Handle(), handle);
    EXPECT_TRUE(ctx->mrs_.empty());
    EXPECT_EQ(state.cleanup, (std::vector<CleanupStep>{CleanupStep::MR, CleanupStep::MR}));
    EXPECT_EQ(state.liveHandles, MULTI_DEVICE_COUNT);
    state.cleanup.clear();
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_OK);
    EXPECT_EQ(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(state.cleanup,
              (std::vector<CleanupStep>{CleanupStep::MR, CleanupStep::RDEV, CleanupStep::RA, CleanupStep::TSD}));
}

TEST_F(DeviceMemoryRegistrationTest, DestroyKeepsResourcesWhenMemoryApiIsUnavailable)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    HccpApi::RaDeregisterMr_ptr = nullptr;
    state.cleanup.clear();
    ctx->Destroy();
    ASSERT_NE(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_ERROR);
    EXPECT_EQ(state.liveHandles, MULTI_DEVICE_COUNT);
    EXPECT_TRUE(state.cleanup.empty());
    HccpApi::RaDeregisterMr_ptr = DeregisterMr;
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_OK);
    EXPECT_EQ(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
}

TEST_F(DeviceMemoryRegistrationTest, PoolReleasePreservesFailedMemoryUntilRetry)
{
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    state.deregisterMrResult = MOCK_FAILURE;
    genetty_context_return(context_, 0);
    context_ = nullptr;
    genetty_uninit(0);
    ASSERT_NE(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(DeviceRdmaContext::PENDING_CLEANUP->pendingMrs_.size(), 1);
    EXPECT_EQ(state.liveHandles, 1);
    EXPECT_EQ(state.liveRa, 1);
    EXPECT_EQ(state.liveTsd, 1);
    state.deregisterMrResult = 0;
    state.cleanup.clear();
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_OK);
    EXPECT_EQ(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(state.cleanup,
              (std::vector<CleanupStep>{CleanupStep::MR, CleanupStep::RDEV, CleanupStep::RA, CleanupStep::TSD}));
}

TEST_F(DeviceMemoryRegistrationTest, DestructorCleansMemoryWithoutExplicitDestroy)
{
    DeviceRdmaContextPtr wrap;
    ASSERT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, DEVICE_IP, wrap), GEN_OK);
    auto ctx = mf::MakeRef<DeviceVerbsContext>(DEVICE_RDMA_GENETTY, wrap);
    ASSERT_EQ(ctx->RegisterMemory(&segment_, 0), GEN_OK);
    state.cleanup.clear();
    ctx = nullptr;
    EXPECT_EQ(wrap->Handle(), nullptr);
    EXPECT_EQ(state.cleanup,
              (std::vector<CleanupStep>{CleanupStep::MR, CleanupStep::RDEV, CleanupStep::RA, CleanupStep::TSD}));
}

TEST_F(DeviceMemoryRegistrationTest, DeviceCloseRetryDoesNotRepeatMemoryCleanup)
{
    auto ctx = reinterpret_cast<DeviceVerbsContext *>(context_);
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    HccpApi::RaRdevDeinit_ptr = nullptr;
    state.cleanup.clear();
    ctx->Destroy();
    ASSERT_NE(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_TRUE(DeviceRdmaContext::PENDING_CLEANUP->pendingMrs_.empty());
    EXPECT_EQ(state.cleanup, (std::vector<CleanupStep>{CleanupStep::MR}));
    HccpApi::RaRdevDeinit_ptr = CloseRdev;
    state.cleanup.clear();
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_OK);
    EXPECT_EQ(state.cleanup, (std::vector<CleanupStep>{CleanupStep::RDEV, CleanupStep::RA, CleanupStep::TSD}));
}

TEST_F(DeviceRdmaContextTest, UnloadClearsMemoryApis)
{
    HccpApi::RaDeregisterMr_ptr = [](const void *, void *) { return 0; };
    HccpApi::UnLoad();
    EXPECT_EQ(HccpApi::RaRegisterMr_ptr, nullptr);
    EXPECT_EQ(HccpApi::RaDeregisterMr_ptr, nullptr);
    EXPECT_EQ(HccpApi::RaDeregisterMr(&state, &state.registerMrCalls), GEN_NOT_INITIALIZED);
}

TEST_F(DeviceContextApiTest, GetsByLogicalIdAndReturnsEachReference)
{
    auto &pool = VerbsBackend::Instance().deviceCtxPool_;
    ASSERT_NE(pool, nullptr);
    for (uint32_t i = 0; i < MULTI_DEVICE_COUNT; ++i) {
        const auto &ctx = pool->contexts_[i];
        auto references = ctx->ref_count_;
        auto first = genetty_context_get_by_id(DEVICE_RDMA_GENETTY, i + LOGIC_DEVICE_ID, 0);
        auto second = genetty_context_get_by_id(DEVICE_RDMA_GENETTY, i + LOGIC_DEVICE_ID, 0);
        EXPECT_EQ(first, reinterpret_cast<genetty_context_t>(ctx.Get()));
        EXPECT_EQ(second, first);
        EXPECT_EQ(ctx->ref_count_, references + ACQUIRED_REFERENCES);
        genetty_context_return(first, 0);
        genetty_context_return(second, 0);
        EXPECT_EQ(ctx->ref_count_, references);
        EXPECT_NE(ctx->wrapCtx_->Handle(), nullptr);
    }
    EXPECT_EQ(genetty_context_get_by_id(DEVICE_RDMA_GENETTY, USER_DEVICE_ID, 0), nullptr);
    EXPECT_EQ(genetty_context_get_by_id(DEVICE_RDMA_GENETTY, UINT16_MAX, 0), nullptr);
}

TEST_F(DeviceContextApiTest, GetsByIpAndAutoPolicy)
{
    auto &pool = VerbsBackend::Instance().deviceCtxPool_;
    char ip[] = "192.0.2.2/24";
    auto byIp = genetty_context_get_by_ip(DEVICE_RDMA_GENETTY, ip, 0);
    EXPECT_EQ(byIp, reinterpret_cast<genetty_context_t>(pool->contexts_.back().Get()));
    genetty_context_return(byIp, 0);
    char anyIp[] = "";
    auto any = genetty_context_get_by_ip(DEVICE_RDMA_GENETTY, anyIp, 0);
    EXPECT_EQ(any, reinterpret_cast<genetty_context_t>(pool->contexts_.front().Get()));
    genetty_context_return(any, 0);
    auto automatic = genetty_context_get_by_policy(DEVICE_RDMA_GENETTY,
                                                   GEN_CTX_POLICY_NUMA_AFFINITY | GEN_CTX_POLICY_DEVICE_BALANCED, 0);
    EXPECT_EQ(automatic, reinterpret_cast<genetty_context_t>(pool->contexts_.front().Get()));
    genetty_context_return(automatic, 0);
    char missingIp[] = "192.0.2.99";
    EXPECT_EQ(genetty_context_get_by_ip(DEVICE_RDMA_GENETTY, missingIp, 0), nullptr);
    EXPECT_EQ(genetty_context_get_by_ip(DEVICE_RDMA_GENETTY, nullptr, 0), nullptr);
    EXPECT_EQ(genetty_context_get_by_id(HOST_RDMA_GENETTY, LOGIC_DEVICE_ID, 0), nullptr);
}

TEST_F(DeviceContextApiTest, RoutesHostAndDeviceWithTheSameId)
{
    auto &backend = VerbsBackend::Instance();
    ibv_port_attr portAttr{};
    ibv_device_attr deviceAttr{};
    auto device =
        mf::MakeRef<VerbsDevice>("mock_cx7", "", HOST_PORT_INDEX, LOGIC_DEVICE_ID, portAttr, deviceAttr, nullptr);
    ibv_context rawContext{};
    ibv_pd rawPd{};
    auto wrap = mf::MakeRef<VerbsContext>(&rawContext, &rawPd, device);
    /* Only device metadata is needed for context selection. */
    wrap->rawContext_ = nullptr;
    wrap->rawPd_ = nullptr;
    auto host = mf::MakeRef<HostVerbsContext>(HOST_RDMA_GENETTY, wrap);
    backend.hostCtxPool_ = mf::MakeRef<HostVerbsContextPool>();
    backend.hostCtxPool_->contexts_.push_back(host);
    backend.hostCtxPool_->inited_ = true;
    auto references = host->ref_count_;
    auto hostHandle = genetty_context_get_by_id(HOST_RDMA_GENETTY, LOGIC_DEVICE_ID, 0);
    auto deviceHandle = genetty_context_get_by_id(DEVICE_RDMA_GENETTY, LOGIC_DEVICE_ID, 0);
    EXPECT_EQ(hostHandle, reinterpret_cast<genetty_context_t>(host.Get()));
    EXPECT_NE(deviceHandle, nullptr);
    EXPECT_NE(hostHandle, deviceHandle);
    EXPECT_EQ(host->ref_count_, references + 1);
    genetty_context_return(hostHandle, 0);
    genetty_context_return(deviceHandle, 0);
    EXPECT_EQ(host->ref_count_, references);
}

TEST_F(DeviceContextApiTest, RejectsForeignContextAndInvalidOptions)
{
    auto &backend = VerbsBackend::Instance();
    auto wrap = backend.deviceCtxPool_->contexts_.front()->wrapCtx_;
    auto foreign = mf::MakeRef<DeviceVerbsContext>(DEVICE_RDMA_GENETTY, wrap);
    auto references = foreign->ref_count_;
    genetty_context_return(reinterpret_cast<genetty_context_t>(foreign.Get()), 0);
    EXPECT_EQ(foreign->ref_count_, references);
    genetty_context_return(nullptr, 0);
    GenettyCtxGetOptions options;
    options.genettyType = DEVICE_RDMA_GENETTY;
    genetty_context_t context = reinterpret_cast<genetty_context_t>(foreign.Get());
    EXPECT_EQ(backend.GetContext(options, &context), GEN_INVALID_PARAM);
    EXPECT_EQ(context, nullptr);
    options.type = GCCP_AUTO;
    EXPECT_EQ(backend.GetContext(options, nullptr), GEN_INVALID_PARAM);
    options.genettyType = HOST_URMA_GENETTY;
    EXPECT_EQ(backend.GetContext(options, &context), GEN_INVALID_PARAM);
    EXPECT_EQ(context, nullptr);
}

TEST_F(DeviceContextApiTest, UninitClosesDevicesAndAllowsReinit)
{
    genetty_uninit(0);
    EXPECT_EQ(state.liveHandles, 0);
    EXPECT_EQ(state.liveRa, 0);
    EXPECT_EQ(state.liveTsd, 0);
    EXPECT_EQ(VerbsBackend::Instance().deviceCtxPool_, nullptr);
    EXPECT_EQ(genetty_context_get_by_id(DEVICE_RDMA_GENETTY, LOGIC_DEVICE_ID, 0), nullptr);
    ASSERT_EQ(genetty_init(GEN_PROT_DEVICE_RDMA, "ascend", 0), GEN_OK);
    auto context = genetty_context_get_by_id(DEVICE_RDMA_GENETTY, LOGIC_DEVICE_ID, 0);
    EXPECT_NE(context, nullptr);
    genetty_context_return(context, 0);
}

TEST_F(DeviceRdmaContextTest, BackendInitFailureCanBeRetried)
{
    InstallEnumerationMocks();
    state.deviceCount = 0;
    EXPECT_EQ(genetty_init(GEN_PROT_DEVICE_RDMA, "ascend", 0), GEN_DEVICE_ERROR);
    EXPECT_FALSE(VerbsBackend::Instance().inited_);
    EXPECT_EQ(VerbsBackend::Instance().deviceCtxPool_, nullptr);
    state.deviceCount = MULTI_DEVICE_COUNT;
    EXPECT_EQ(genetty_init(GEN_PROT_DEVICE_RDMA, "ascend", 0), GEN_OK);
    genetty_uninit(0);
}

TEST_F(DeviceRdmaContextTest, PoolSelectSkipsClosedContextsAndChecksInitialization)
{
    InstallEnumerationMocks();
    DeviceVerbsContextPool pool;
    DeviceVerbsContextPtr selected;
    EXPECT_EQ(pool.SelectContext(UINT16_MAX, selected), GEN_ERROR);
    EXPECT_EQ(pool.SelectContext(std::string(DEVICE_IP), selected), GEN_ERROR);
    ASSERT_EQ(pool.AddAllDevices(), GEN_OK);
    pool.contexts_.front()->Destroy();
    pool.contexts_.insert(pool.contexts_.begin(), nullptr);
    EXPECT_EQ(pool.SelectContext(UINT16_MAX, selected), GEN_OK);
    EXPECT_EQ(selected.Get(), pool.contexts_.back().Get());
    EXPECT_EQ(pool.SelectContext(std::string(DEVICE_IP), selected), GEN_INVALID_PARAM);
    pool.CloseAllDevices();
    EXPECT_EQ(pool.SelectContext(UINT16_MAX, selected), GEN_ERROR);
}

TEST_F(DeviceRdmaContextTest, GetDeviceCountChecksArgumentsAndPropagatesErrors)
{
    EXPECT_EQ(AclApi::AclrtGetDeviceCount(nullptr), GEN_INVALID_PARAM);
    EXPECT_EQ(state.deviceCountCalls, 0);
    uint32_t count = 0;
    EXPECT_EQ(AclApi::AclrtGetDeviceCount(&count), 0);
    EXPECT_EQ(count, MULTI_DEVICE_COUNT);
    state.deviceCountResult = MOCK_FAILURE;
    EXPECT_EQ(AclApi::AclrtGetDeviceCount(&count), MOCK_FAILURE);
    AclApi::UnLoad();
    EXPECT_EQ(AclApi::aclrtGetDeviceCount_ptr, nullptr);
    EXPECT_EQ(AclApi::AclrtGetDeviceCount(&count), GEN_NOT_INITIALIZED);
}

TEST_F(DeviceRdmaContextTest, PoolDiscoversDevicesAndPreservesCallerDevice)
{
    InstallEnumerationMocks();
    DeviceVerbsContextPool pool;
    ASSERT_EQ(pool.AddAllDevices(), GEN_OK);
    ASSERT_EQ(pool.contexts_.size(), MULTI_DEVICE_COUNT);
    EXPECT_EQ(state.currentDeviceId, USER_DEVICE_ID);
    for (uint32_t i = 0; i < MULTI_DEVICE_COUNT; ++i) {
        const auto &ctx = pool.contexts_[i];
        EXPECT_EQ(ctx->Type(), DEVICE_RDMA_GENETTY);
        EXPECT_EQ(ctx->wrapCtx_->UserDeviceId(), i);
        EXPECT_EQ(ctx->wrapCtx_->LogicDeviceId(), i + LOGIC_DEVICE_ID);
        EXPECT_EQ(ctx->wrapCtx_->PhysicalDeviceId(), i + PHYSICAL_DEVICE_ID);
        EXPECT_EQ(ctx->wrapCtx_->Ip(), "192.0.2." + std::to_string(i + 1));
        DeviceVerbsContextPtr found;
        EXPECT_EQ(pool.Lookup(reinterpret_cast<genetty_context_t>(ctx.Get()), found), GEN_OK);
        EXPECT_EQ(found.Get(), ctx.Get());
    }
    EXPECT_EQ(pool.AddAllDevices(), GEN_OK);
    EXPECT_EQ(state.deviceCountCalls, 1);
    EXPECT_EQ(state.liveHandles, MULTI_DEVICE_COUNT);
    pool.CloseAllDevices();
    EXPECT_FALSE(pool.inited_);
    EXPECT_TRUE(pool.contexts_.empty());
    EXPECT_EQ(pool.AddAllDevices(), GEN_OK);
}

TEST_F(DeviceRdmaContextTest, LazyPoolOpensOnlyRequestedLogicalDevice)
{
    InstallEnumerationMocks();
    ASSERT_EQ(genetty_init(GEN_PROT_DEVICE_RDMA, "ascend", GEN_INIT_LAZY_DEVICE), GEN_OK);
    EXPECT_EQ(state.liveHandles, 0);
    auto context = genetty_context_get_by_id(DEVICE_RDMA_GENETTY, LOGIC_DEVICE_ID + 1, 0);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(state.liveHandles, 1);
    EXPECT_EQ(state.currentDeviceId, USER_DEVICE_ID);
    auto repeated = genetty_context_get_by_id(DEVICE_RDMA_GENETTY, LOGIC_DEVICE_ID + 1, 0);
    EXPECT_EQ(repeated, context);
    EXPECT_EQ(state.liveHandles, 1);
    genetty_context_return(repeated, 0);
    genetty_context_return(context, 0);
    genetty_uninit(0);
    EXPECT_EQ(state.liveHandles, 0);
}

TEST_F(DeviceRdmaContextTest, PoolSkipsUnavailableDevice)
{
    InstallEnumerationMocks();
    state.failedDeviceId = MULTI_DEVICE_COUNT - 1;
    DeviceVerbsContextPool pool;
    ASSERT_EQ(pool.AddAllDevices(), GEN_OK);
    ASSERT_EQ(pool.contexts_.size(), 1);
    EXPECT_EQ(pool.contexts_.front()->wrapCtx_->UserDeviceId(), USER_DEVICE_ID);
    EXPECT_EQ(state.liveHandles, 1);
    EXPECT_EQ(state.liveRa, 1);
    EXPECT_EQ(state.liveTsd, 1);
}

TEST_F(DeviceRdmaContextTest, PoolRejectsEmptyOrFailedEnumeration)
{
    InstallEnumerationMocks();
    DeviceVerbsContextPool pool;
    state.deviceCount = 0;
    EXPECT_EQ(pool.AddAllDevices(), GEN_DEVICE_ERROR);
    state.deviceCountResult = MOCK_FAILURE;
    EXPECT_EQ(pool.AddAllDevices(), GEN_DEVICE_ERROR);
    EXPECT_FALSE(pool.inited_);
    EXPECT_TRUE(pool.contexts_.empty());
    EXPECT_TRUE(state.selectedDevices.empty());
}

TEST_F(DeviceRdmaContextTest, PoolRollsBackFailedIpDiscovery)
{
    InstallEnumerationMocks();
    DeviceVerbsContextPool pool;
    state.interfaceResult = MOCK_FAILURE;
    EXPECT_EQ(pool.AddAllDevices(), GEN_DEVICE_ERROR);
    EXPECT_FALSE(pool.inited_);
    EXPECT_TRUE(pool.contexts_.empty());
    EXPECT_EQ(state.liveRa, 0);
    EXPECT_EQ(state.liveTsd, 0);
    EXPECT_EQ(state.currentDeviceId, USER_DEVICE_ID);
    state.interfaceResult = 0;
    EXPECT_EQ(pool.AddAllDevices(), GEN_OK);
}

TEST_F(DeviceRdmaContextTest, IpDiscoveryChecksInterfaceNameAndReturnedCount)
{
    InstallEnumerationMocks();
    DeviceRdmaContextPtr context;
    state.wrongInterfaceName = true;
    EXPECT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, "", context), GEN_NOT_FOUND);
    state.wrongInterfaceName = false;
    state.returnedInterfaceCount = INTERFACE_COUNT + 1;
    EXPECT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, "", context), GEN_DEVICE_ERROR);
    state.returnedInterfaceCount = DEVICE_INTERFACE_INDEX;
    EXPECT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, "", context), GEN_NOT_FOUND);
    EXPECT_EQ(context, nullptr);
    EXPECT_EQ(state.liveRa, 0);
    EXPECT_EQ(state.liveTsd, 0);
}

TEST_F(DeviceRdmaContextTest, PoolRollsBackIfCallerDeviceCannotBeRestored)
{
    InstallEnumerationMocks();
    state.failRestore = true;
    DeviceVerbsContextPool pool;
    EXPECT_EQ(pool.AddAllDevices(), GEN_DEVICE_ERROR);
    EXPECT_FALSE(pool.inited_);
    EXPECT_TRUE(pool.contexts_.empty());
    EXPECT_EQ(state.liveHandles, 0);
    EXPECT_EQ(state.liveRa, 0);
    EXPECT_EQ(state.liveTsd, 0);
}

TEST_F(DeviceRdmaContextTest, PoolCanInitializeWithoutPreviouslySelectedDevice)
{
    InstallEnumerationMocks();
    state.currentDeviceId = -1;
    DeviceVerbsContextPool pool;
    EXPECT_EQ(pool.AddAllDevices(), GEN_OK);
    EXPECT_EQ(pool.contexts_.size(), MULTI_DEVICE_COUNT);
}

TEST_F(DeviceRdmaContextTest, CloseErrorDoesNotReuseConsumedHandle)
{
    DeviceRdmaContextPtr context;
    ASSERT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, DEVICE_IP, context), GEN_OK);
    state.rdevCloseResult = MOCK_FAILURE;
    context->Destroy();
    context->Destroy();
    context = nullptr;
    EXPECT_EQ(state.cleanup, (std::vector<CleanupStep>{CleanupStep::RDEV, CleanupStep::RA, CleanupStep::TSD}));
    EXPECT_EQ(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
}

TEST_F(DeviceRdmaContextTest, UnloadedCloseApiPreservesHandleForRetry)
{
    DeviceRdmaContextPtr context;
    ASSERT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, DEVICE_IP, context), GEN_OK);
    HccpApi::RaRdevDeinit_ptr = nullptr;
    context = nullptr;
    EXPECT_EQ(state.liveHandles, 1);
    EXPECT_TRUE(state.cleanup.empty());
    ASSERT_NE(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    HccpApi::RaRdevDeinit_ptr = CloseRdev;
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_OK);
    EXPECT_EQ(state.cleanup, (std::vector<CleanupStep>{CleanupStep::RDEV, CleanupStep::RA, CleanupStep::TSD}));
}

TEST_F(DeviceRdmaContextTest, FailedCreationRetainsUnfinishedRollback)
{
    auto previous = mf::MakeRef<DeviceRdmaContext>();
    auto output = previous;
    state.rdevInitResult = MOCK_FAILURE;
    state.raCloseResult = MOCK_FAILURE;
    EXPECT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, DEVICE_IP, output), GEN_DEVICE_ERROR);
    EXPECT_EQ(output.Get(), previous.Get());
    ASSERT_NE(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(state.liveRa, 1);
    EXPECT_EQ(state.liveTsd, 1);
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_DEVICE_ERROR);
    EXPECT_EQ(state.cleanup, (std::vector<CleanupStep>{CleanupStep::RA, CleanupStep::RA}));
    state.raCloseResult = 0;
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_OK);
    EXPECT_EQ(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(state.liveRa, 0);
    EXPECT_EQ(state.liveTsd, 0);
}

TEST_F(DeviceRdmaContextTest, NewCreationFinishesPendingCleanupFirst)
{
    DeviceRdmaContextPtr context;
    ASSERT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, DEVICE_IP, context), GEN_OK);
    state.raCloseResult = MOCK_FAILURE;
    context = nullptr;
    ASSERT_NE(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, DEVICE_IP, context), GEN_DEVICE_ERROR);
    EXPECT_EQ(context, nullptr);
    EXPECT_EQ(state.liveTsd, 1);
    state.raCloseResult = 0;
    ASSERT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, DEVICE_IP, context), GEN_OK);
    EXPECT_EQ(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(state.liveTsd, 1);
    EXPECT_EQ(state.liveRa, 1);
}

TEST_F(DeviceRdmaContextTest, TsdRetryDoesNotRepeatEarlierCleanup)
{
    DeviceRdmaContextPtr context;
    ASSERT_EQ(DeviceRdmaContext::CreateContext(USER_DEVICE_ID, DEVICE_IP, context), GEN_OK);
    state.tsdCloseResult = MOCK_FAILURE;
    context = nullptr;
    ASSERT_NE(DeviceRdmaContext::PENDING_CLEANUP, nullptr);
    EXPECT_EQ(state.liveRa, 0);
    EXPECT_EQ(state.liveTsd, 1);
    state.cleanup.clear();
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_DEVICE_ERROR);
    state.tsdCloseResult = 0;
    EXPECT_EQ(DeviceRdmaContext::RetryPendingCleanup(), GEN_OK);
    EXPECT_EQ(state.cleanup, (std::vector<CleanupStep>{CleanupStep::TSD, CleanupStep::TSD}));
}

TEST_F(DeviceRdmaContextTest, BorrowedLegacyAclIsNotFinalized)
{
    state.aclInitResult = ACL_REPEAT_INITIALIZE;
    EXPECT_EQ(AclApi::Init(), GEN_OK);
    EXPECT_EQ(AclApi::Init(), GEN_OK);
    EXPECT_EQ(state.aclInitCalls, 1);
    EXPECT_EQ(AclApi::AclFinalize(), 0);
    EXPECT_EQ(state.finalizeCalls, 0);
    EXPECT_FALSE(AclApi::INITIALIZED);
}

TEST_F(DeviceRdmaContextTest, OwnedAclFinalizeFailureCanBeRetried)
{
    ASSERT_EQ(AclApi::Init(), GEN_OK);
    state.finalizeResult = MOCK_FAILURE;
    EXPECT_EQ(AclApi::AclFinalize(), MOCK_FAILURE);
    EXPECT_TRUE(AclApi::INITIALIZED);
    EXPECT_TRUE(AclApi::OWNS_INITIALIZATION);
    state.finalizeResult = 0;
    EXPECT_EQ(AclApi::AclFinalize(), 0);
    EXPECT_EQ(AclApi::AclFinalize(), 0);
    EXPECT_EQ(state.finalizeCalls, 2);
}

TEST_F(DeviceRdmaContextTest, ModernAclReleasesOnlyItsOwnReference)
{
    AclApi::aclFinalizeReference_ptr = [](uint64_t *refCount) {
        EXPECT_EQ(refCount, nullptr);
        ++state.finalizeReferenceCalls;
        return 0;
    };
    for (auto initResult : {0, ACL_REPEAT_INITIALIZE}) {
        state.aclInitResult = initResult;
        EXPECT_EQ(AclApi::Init(), GEN_OK);
        EXPECT_EQ(AclApi::AclFinalize(), 0);
    }
    EXPECT_EQ(state.finalizeReferenceCalls, 2);
    EXPECT_EQ(state.finalizeCalls, 0);
}

TEST_F(DeviceRdmaContextTest, AclInitFailureDoesNotAcquireOwnership)
{
    state.aclInitResult = MOCK_FAILURE;
    EXPECT_EQ(AclApi::Init(), GEN_DEVICE_ERROR);
    EXPECT_FALSE(AclApi::INITIALIZED);
    EXPECT_FALSE(AclApi::OWNS_INITIALIZATION);
    EXPECT_EQ(AclApi::AclFinalize(), 0);
    EXPECT_EQ(state.finalizeCalls, 0);
}

constexpr uint32_t TEST_QPN = 123;
constexpr uint32_t TEST_PSN = 456;
constexpr uint32_t TEST_MTU = 4;
constexpr uint32_t TEST_DEPTH = 128;
constexpr uint64_t TEST_DB = 0x123456789abcdef0ULL;
struct QueueMockState {
    HccpQpExtAttrs attrs{};
    HccpSendWrV2 wr{};
    HccpSgList sge{};
    HccpTypicalQp local{};
    HccpTypicalQp remote{};
    int createResult = 0;
    int queryResult = 0;
    int modifyResult = 0;
    int sendResult = 0;
    int pollResult = 1;
    int completionStatus = 0;
    bool wrongId = false;
    int destroyCalls = 0;
    int live = 0;
    bool copyData = false;
    bool corruptRead = false;
} queueState;

void InstallQueueMocks()
{
    HccpApi::RaQpCreateWithAttrs_ptr = [](void *, HccpQpExtAttrs *attrs, void **handle) {
        queueState.attrs = *attrs;
        if (queueState.createResult == 0) {
            *handle = new int(0);
            ++queueState.live;
        }
        return queueState.createResult;
    };
    HccpApi::RaQpDestroy_ptr = [](void *handle) {
        delete static_cast<int *>(handle);
        --queueState.live;
        ++queueState.destroyCalls;
        return 0;
    };
    HccpApi::RaGetQpAttr_ptr = [](void *, HccpQpAttr *attr) {
        attr->qpn = TEST_QPN;
        attr->psn = TEST_PSN;
        attr->pathMtu = TEST_MTU;
        return queueState.queryResult;
    };
    HccpApi::RaTypicalQpModify_ptr = [](void *, HccpTypicalQp *local, HccpTypicalQp *remote) {
        queueState.local = *local;
        queueState.remote = *remote;
        return queueState.modifyResult;
    };
    HccpApi::RaSendWrV2_ptr = [](void *, HccpSendWrV2 *wr, HccpSendWrRsp *response) {
        queueState.wr = *wr;
        queueState.sge = *wr->buf_list;
        if (queueState.copyData) {
            auto local = reinterpret_cast<void *>(wr->buf_list->addr);
            auto remote = reinterpret_cast<void *>(wr->dst_addr);
            if (wr->op == 0) {
                memcpy(remote, local, wr->buf_list->len);
            } else {
                memcpy(local, remote, wr->buf_list->len);
                if (queueState.corruptRead) {
                    *static_cast<uint8_t *>(local) ^= 1;
                }
            }
        }
        response->db.db_info = TEST_DB;
        return queueState.sendResult;
    };
    HccpApi::RaPollCq_ptr = [](void *, bool send, unsigned int count, void *out) {
        EXPECT_TRUE(send);
        EXPECT_EQ(count, 1U);
        auto wc = static_cast<HccpLiteWcV2 *>(out);
        wc->wc.wr_id = queueState.wr.wr_id + (queueState.wrongId ? 1 : 0);
        wc->wc.opcode = queueState.wr.op == 0 ? 1 : 2;
        wc->wc.status = queueState.completionStatus;
        return queueState.pollResult;
    };
}

class DeviceQueueTest : public DeviceMemoryRegistrationTest {
protected:
    void SetUp() override
    {
        DeviceMemoryRegistrationTest::SetUp();
        queueState = {};
        MockTsReset();
        InstallQueueMocks();
        genetty_cq_options_t options{};
        genetty_cq_options_init(&options);
        options.context = context_;
        options.queue_depth = TEST_DEPTH;
        cq_ = genetty_cq_create(&options);
        ASSERT_NE(cq_, nullptr);
    }
    void TearDown() override
    {
        MockTsStall(false);
        MockTsFailFree(false);
        if (qp_ != nullptr) {
            genetty_destroy(qp_, 0);
        }
        genetty_cq_destroy(cq_, 0);
        DeviceMemoryRegistrationTest::TearDown();
        EXPECT_EQ(queueState.live, 0);
        EXPECT_EQ(MockTsLiveResources(), 0);
    }
    void CreateQp()
    {
        genetty_options_t options{};
        genetty_options_init(&options);
        options.cq_handle = cq_;
        options.sq_depth = TEST_DEPTH;
        options.rq_depth = TEST_DEPTH;
        options.traffic_class = 132;
        qp_ = genetty_create(&options);
    }
    void ConnectQp()
    {
        CreateQp();
        ASSERT_NE(qp_, nullptr);
        VerbsLinkInfo peer;
        peer.Type(HOST_RDMA_GENETTY);
        peer.Vendor(VT_KUNPENG_CPU);
        peer.Qpn(TEST_QPN + 1);
        peer.Psn(TEST_PSN + 1);
        peer.Mtu(TEST_MTU);
        genetty_link_info_t info{};
        ASSERT_EQ(peer.Export(&info, 0), GEN_OK);
        EXPECT_EQ(genetty_import(qp_, &info, 0), GEN_OK);
        genetty_link_info_free(&info);
    }
    genetty_cq_t cq_ = nullptr;
    genetty_t qp_ = nullptr;
    genetty_batch_op_data_t data_{reinterpret_cast<void *>(MR_ADDRESS),           MR_SIZE, MR_LKEY,
                                  reinterpret_cast<void *>(MR_ADDRESS + MR_SIZE), MR_SIZE, MR_RKEY};
    genetty_batch_op_options_t options_{TEST_DB, 0, OP_WRITE, 1, 1, 0};
};

TEST_F(DeviceQueueTest, CreatesAndExportsExtendedQpThroughPublicApi)
{
    CreateQp();
    ASSERT_NE(qp_, nullptr);
    EXPECT_EQ(queueState.attrs.qpMode, HCCP_QP_MODE_OP_EXT);
    EXPECT_EQ(queueState.attrs.qpAttr.cap.max_send_wr, TEST_DEPTH);
    EXPECT_EQ(queueState.attrs.cqAttr.sendCqDepth, TEST_DEPTH);
    genetty_link_info_t info{};
    ASSERT_EQ(genetty_export(qp_, 0, &info), GEN_OK);
    VerbsLinkInfo link;
    EXPECT_EQ(link.Import(&info, 0), GEN_OK);
    genetty_link_info_free(&info);
    EXPECT_EQ(link.Type(), DEVICE_RDMA_GENETTY);
    EXPECT_EQ(link.Qpn(), TEST_QPN);
    EXPECT_EQ(link.Psn(), TEST_PSN);
    EXPECT_EQ(link.Mtu(), TEST_MTU);
}

TEST_F(DeviceQueueTest, QueryFailureDestroysPartiallyCreatedQp)
{
    queueState.queryResult = MOCK_FAILURE;
    CreateQp();
    EXPECT_EQ(qp_, nullptr);
    EXPECT_EQ(queueState.destroyCalls, 1);
    EXPECT_EQ(queueState.live, 0);
}

TEST_F(DeviceQueueTest, CreateFailureDoesNotPublishQp)
{
    queueState.createResult = MOCK_FAILURE;
    CreateQp();
    EXPECT_EQ(qp_, nullptr);
    EXPECT_EQ(queueState.destroyCalls, 0);
}

TEST_F(DeviceQueueTest, LogicalCqRejectsSecondQp)
{
    CreateQp();
    ASSERT_NE(qp_, nullptr);
    auto first = qp_;
    CreateQp();
    EXPECT_EQ(qp_, nullptr);
    qp_ = first;
}

TEST_F(DeviceQueueTest, ConnectUsesActualPsnAndRoceParameters)
{
    ConnectQp();
    EXPECT_EQ(queueState.local.psn, TEST_PSN);
    EXPECT_EQ(queueState.remote.psn, TEST_PSN + 1);
    EXPECT_EQ(queueState.remote.qpn, TEST_QPN + 1);
    EXPECT_EQ(queueState.local.tc, 132U);
    EXPECT_EQ(queueState.local.sl, 4U);
}

TEST_F(DeviceQueueTest, WriteAndReadRingTsDoorbellAndValidateCompletion)
{
    ConnectQp();
    for (auto op : {OP_WRITE, OP_READ}) {
        options_.op_type = op;
        ASSERT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_OK);
        EXPECT_EQ(queueState.sge.addr, MR_ADDRESS);
        EXPECT_EQ(queueState.sge.lkey, MR_LKEY);
        EXPECT_EQ(queueState.wr.rkey, MR_RKEY);
        EXPECT_EQ(queueState.wr.dst_addr, MR_ADDRESS + MR_SIZE);
        EXPECT_EQ(MockTsLastDoorbell(), TEST_DB);
        genetty_cq_wc_t wc{};
        EXPECT_EQ(genetty_cq_poll(cq_, 1, &wc, 0), 1);
        EXPECT_EQ(wc.status, OPS_SUCCESS);
        EXPECT_EQ(wc.type, op);
        EXPECT_EQ(wc.usr_ctx, options_.usr_ctx);
        EXPECT_NE(wc.genetty_id, 0U);
        ++options_.usr_ctx;
    }
    EXPECT_EQ(MockTsSendCount(), 2);
}

TEST_F(DeviceQueueTest, RequiresCompletionBeforeNextPost)
{
    ConnectQp();
    ASSERT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_OK);
    EXPECT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_POST_FAILED);
    queueState.pollResult = 0;
    genetty_cq_wc_t wc{};
    EXPECT_EQ(genetty_cq_poll(cq_, 1, &wc, 0), 0);
    EXPECT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_POST_FAILED);
    queueState.pollResult = 1;
    EXPECT_EQ(genetty_cq_poll(cq_, 1, &wc, 0), 1);
    EXPECT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_OK);
}

TEST_F(DeviceQueueTest, BadCompletionPoisonsQueue)
{
    ConnectQp();
    ASSERT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_OK);
    queueState.wrongId = true;
    genetty_cq_wc_t wc{};
    EXPECT_EQ(genetty_cq_poll(cq_, 1, &wc, 0), 1);
    EXPECT_EQ(wc.status, OPS_ERROR);
    EXPECT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_POST_FAILED);
}

TEST_F(DeviceQueueTest, CompletionErrorIsNotReportedAsSuccess)
{
    ConnectQp();
    ASSERT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_OK);
    queueState.completionStatus = MOCK_FAILURE;
    genetty_cq_wc_t wc{};
    EXPECT_EQ(genetty_cq_poll(cq_, 1, &wc, 0), 1);
    EXPECT_EQ(wc.status, OPS_ERROR);
}

TEST_F(DeviceQueueTest, FailedSendDoesNotRingDoorbell)
{
    ConnectQp();
    queueState.sendResult = MOCK_FAILURE;
    EXPECT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_POST_FAILED);
    EXPECT_EQ(MockTsSendCount(), 0);
}

TEST_F(DeviceQueueTest, RejectsUnsignaledBatchAndWriteImmediate)
{
    ConnectQp();
    EXPECT_EQ(genetty_batch_post(qp_, &data_, 2, &options_), GEN_INVALID_PARAM);
    options_.signal = 0;
    EXPECT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_INVALID_PARAM);
    options_.signal = 1;
    options_.op_type = OP_WRITE_IMM;
    EXPECT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_INVALID_PARAM);
    EXPECT_EQ(MockTsSendCount(), 0);
}

TEST_F(DeviceQueueTest, FailedTsCleanupKeepsQpAndMrUntilRetry)
{
    ConnectQp();
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    ASSERT_EQ(genetty_batch_post(qp_, &data_, 1, &options_), GEN_OK);
    MockTsFailFree(true);
    genetty_destroy(qp_, 0);
    qp_ = nullptr;
    EXPECT_EQ(queueState.destroyCalls, 0);
    EXPECT_NE(genetty_memory_unregister(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(state.deregisterMrCalls, 0);
    MockTsFailFree(false);
    genetty_cq_destroy(cq_, 0);
    cq_ = nullptr;
    EXPECT_EQ(queueState.destroyCalls, 1);
    EXPECT_EQ(genetty_memory_unregister(context_, &segment_, 0), GEN_OK);
}

TEST_F(DeviceRdmaContextTest, TsHeadMustCatchUpAndTimedOutResourcesRemainOwned)
{
    MockTsReset();
    DeviceTsWriter ts;
    ASSERT_EQ(ts.Initialize(USER_DEVICE_ID, LOGIC_DEVICE_ID), GEN_OK);
    MockTsStall(true);
    EXPECT_NE(ts.Submit(TEST_DB, 1), GEN_OK);
    EXPECT_NE(ts.Destroy(), GEN_OK);
    EXPECT_GT(MockTsLiveResources(), 0);
    MockTsStall(false);
    EXPECT_EQ(ts.Destroy(), GEN_OK);
    EXPECT_EQ(MockTsLiveResources(), 0);
}

TEST_F(DeviceRdmaContextTest, AclMemoryWrappersPassArgumentsAndErrors)
{
    AclApi::aclrtMalloc_ptr = [](void **address, size_t size, int policy) -> int32_t {
        EXPECT_EQ(size, MR_SIZE);
        EXPECT_EQ(policy, ACL_MEM_MALLOC_HUGE_FIRST);
        *address = reinterpret_cast<void *>(MR_ADDRESS);
        return 0;
    };
    AclApi::aclrtMemcpy_ptr = [](void *dst, size_t capacity, const void *src, size_t size, int kind) -> int32_t {
        EXPECT_EQ(dst, reinterpret_cast<void *>(MR_ADDRESS));
        EXPECT_EQ(src, reinterpret_cast<void *>(MR_ADDRESS + MR_SIZE));
        EXPECT_EQ(capacity, MR_SIZE);
        EXPECT_EQ(size, MR_SIZE);
        EXPECT_EQ(kind, ACL_MEMCPY_HOST_TO_DEVICE);
        return MOCK_FAILURE;
    };
    AclApi::aclrtFree_ptr = [](void *address) -> int32_t {
        EXPECT_EQ(address, reinterpret_cast<void *>(MR_ADDRESS));
        return 0;
    };
    void *address = nullptr;
    EXPECT_EQ(AclApi::AclrtMalloc(&address, MR_SIZE, ACL_MEM_MALLOC_HUGE_FIRST), 0);
    EXPECT_EQ(AclApi::AclrtMemcpy(address, MR_SIZE, reinterpret_cast<void *>(MR_ADDRESS + MR_SIZE), MR_SIZE,
                                  ACL_MEMCPY_HOST_TO_DEVICE),
              MOCK_FAILURE);
    EXPECT_EQ(AclApi::AclrtFree(address), 0);
}

class DevicePingPongTest : public DeviceQueueTest {
protected:
    void RunPingPong(bool success)
    {
        ConnectQp();
        queueState.copyData = true;
        AclApi::aclrtMemcpy_ptr = [](void *dst, size_t, const void *src, size_t size, int) -> int32_t {
            memcpy(dst, src, size);
            return 0;
        };
        constexpr uint32_t DATA_SIZE = 1024;
        constexpr uint32_t ROUNDS = 3;
        std::array<uint8_t, DATA_SIZE * 2> clientMemory{};
        std::array<uint8_t, DATA_SIZE * 2> serverMemory{};
        int sockets[2]{};
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
        SubCommandPp client("pp", {});
        SubCommandPp server("pp", {});
        client.endpoint_ = PP_ENDPOINT_DEVICE;
        client.context_ = context_;
        client.genetty_ = qp_;
        client.cq_ = cq_;
        client.localAddress_ = clientMemory.data();
        client.local_.lkey = MR_LKEY;
        client.remote_.address = reinterpret_cast<uint64_t>(serverMemory.data());
        client.remote_.rkey = MR_RKEY;
        client.memorySize_ = DATA_SIZE;
        client.iterations_ = ROUNDS;
        client.timeoutMs_ = 100;
        client.client_ = new SocketClient;
        client.client_->fd_ = sockets[0];
        client.client_->timeoutMs_ = client.timeoutMs_;
        server.isServer_ = true;
        server.localAddress_ = serverMemory.data();
        server.memorySize_ = DATA_SIZE;
        server.iterations_ = ROUNDS;
        server.server_ = new SocketServer;
        server.server_->acceptedFd_ = sockets[1];
        server.server_->timeoutMs_ = client.timeoutMs_;
        int serverResult = 0;
        std::thread peer([&] {
            serverResult = server.PingPong();
            server.DestroySocket();
        });
        auto clientResult = client.PingPong();
        client.DestroySocket();
        peer.join();
        EXPECT_EQ(clientResult == 0, success);
        EXPECT_EQ(serverResult == 0, success);
        if (success) {
            EXPECT_EQ(MockTsSendCount(), ROUNDS * 2);
        }
    }
};

TEST_F(DevicePingPongTest, MultipleRoundsVerifyWriteAndDistinctReadData)
{
    RunPingPong(true);
}

TEST_F(DevicePingPongTest, CorruptedReadFailsBothEndpoints)
{
    queueState.corruptRead = true;
    RunPingPong(false);
}

TEST_F(DevicePingPongTest, MissingCompletionTimesOutAndFailsBothEndpoints)
{
    queueState.pollResult = 0;
    RunPingPong(false);
}

TEST(OdccSocketTest, ReceiveTimesOutAndPeerClosureIsReported)
{
    constexpr uint32_t TIMEOUT_MS = 20;
    int sockets[2]{};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    SocketClient socket;
    socket.fd_ = sockets[0];
    socket.timeoutMs_ = TIMEOUT_MS;
    char value = 0;
    EXPECT_EQ(socket.Receive(&value, sizeof(value)), 0U);
    EXPECT_EQ(errno, ETIMEDOUT);
    close(sockets[1]);
    EXPECT_EQ(socket.Receive(&value, sizeof(value)), 0U);
    EXPECT_EQ(errno, ECONNRESET);
    EXPECT_EQ(socket.Send(&value, sizeof(value)), 0U);
    socket.Close();
}

TEST(OdccSocketTest, OversizedFrameFailsBeforeAllocation)
{
    int sockets[2]{};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    SubCommandPp client("pp", {});
    client.client_ = new SocketClient;
    client.client_->fd_ = sockets[0];
    uint32_t length = htonl(UINT32_MAX);
    ASSERT_EQ(send(sockets[1], &length, sizeof(length), MSG_NOSIGNAL), sizeof(length));
    std::vector<char> data;
    EXPECT_NE(client.ReceiveFrame(data), 0);
    EXPECT_TRUE(data.empty());
    client.DestroySocket();
    close(sockets[1]);
}

TEST(OdccPingPongOptionsTest, RejectsUnsupportedRoleAndConflictingDeviceSelection)
{
    SubCommandPp server("pp", {{"endpoint", "device"}, {"server", "true"}});
    EXPECT_NE(server.Execute(), 0);
    SubCommandPp selection("pp", {{"rdma-ip", "192.0.2.1"}, {"device-id", "0"}});
    EXPECT_NE(selection.Execute(), 0);
    SubCommandPp mtu("pp", {{"path-mtu", "2000"}});
    EXPECT_NE(mtu.Execute(), 0);
}

TEST_F(DeviceQueueTest, ConsumedFailedQpHandleIsNeverDestroyedTwice)
{
    ConnectQp();
    auto wrap = static_cast<DeviceVerbsContext *>(context_)->wrapCtx_;
    auto queue = wrap->queues_.begin()->second;
    ASSERT_EQ(genetty_memory_register(context_, &segment_, 0), GEN_OK);
    HccpApi::RaQpDestroy_ptr = [](void *handle) {
        delete static_cast<int *>(handle);
        --queueState.live;
        ++queueState.destroyCalls;
        return MOCK_FAILURE;
    };
    genetty_destroy(qp_, 0);
    qp_ = nullptr;
    EXPECT_EQ(queueState.destroyCalls, 1);
    EXPECT_EQ(queue->Destroy(), GEN_DEVICE_ERROR);
    EXPECT_EQ(queueState.destroyCalls, 1);
    EXPECT_NE(genetty_memory_unregister(context_, &segment_, 0), GEN_OK);
    EXPECT_EQ(state.deregisterMrCalls, 0);
    /* The test owns the fake hardware; production keeps this tombstone until process exit. */
    queue->destroyFailed_ = false;
}

std::vector<ibv_qp_attr> hostAttributes;
ibv_send_wr hostRequest{};
ibv_sge hostSge{};
class HostQueueInteropTest : public testing::Test {
protected:
    void SetUp() override
    {
        hostAttributes.clear();
        ibv_port_attr port{};
        port.active_mtu = IBV_MTU_2048;
        ibv_device_attr attr{};
        auto device = mf::MakeRef<VerbsDevice>("mock_cx7", "", HOST_PORT_INDEX, 0, port, attr, nullptr);
        context_ = mf::MakeRef<VerbsContext>(&rawContext_, &rawPd_, device);
        rawQp_.context = &rawContext_;
        rawQp_.qp_num = TEST_QPN;
        queue_ = mf::MakeRef<VerbsQP>(&rawQp_, context_, nullptr, nullptr, TEST_DEPTH, TEST_DEPTH);
        qp_ = mf::MakeRef<VerbsHostQp>(HOST_RDMA_GENETTY);
        qp_->qp_ = queue_;
        qp_->inited_ = true;
        qp_->options_.sqDepth = TEST_DEPTH;
        qp_->options_.trafficClass = 132;
        VerbsApi::ibv_modify_qp_ptr = [](ibv_qp *, ibv_qp_attr *attributes, int) {
            hostAttributes.push_back(*attributes);
            return 0;
        };
        rawContext_.ops.post_send = [](ibv_qp *, ibv_send_wr *wr, ibv_send_wr **) {
            hostRequest = *wr;
            hostSge = *wr->sg_list;
            return 0;
        };
    }
    void TearDown() override
    {
        queue_->rawQp_ = nullptr;
        context_->rawContext_ = nullptr;
        context_->rawPd_ = nullptr;
        VerbsApi::ibv_modify_qp_ptr = nullptr;
    }
    int Connect(uint32_t mtu)
    {
        VerbsLinkInfo peer;
        peer.Type(DEVICE_RDMA_GENETTY);
        peer.Vendor(VT_ASCEND_NPU);
        peer.Qpn(TEST_QPN + 1);
        peer.Psn(TEST_PSN);
        peer.Mtu(mtu);
        uint8_t gid[ROCE_GID_LEN]{};
        gid[ROCE_GID_LEN - 1] = 1;
        peer.Gid(gid);
        genetty_link_info_t info{};
        EXPECT_EQ(peer.Export(&info, 0), GEN_OK);
        auto result = qp_->Import(0, &info);
        genetty_link_info_free(&info);
        return result;
    }
    ibv_context rawContext_{};
    ibv_pd rawPd_{};
    ibv_qp rawQp_{};
    VerbsContextPtr context_;
    VerbsQPPtr queue_;
    Ref<VerbsHostQp> qp_;
};

TEST_F(HostQueueInteropTest, UsesDevicePsnMtuAndRoceClassInRtr)
{
    ASSERT_EQ(Connect(TEST_MTU), GEN_OK);
    ASSERT_EQ(hostAttributes.size(), 3U);
    EXPECT_EQ(hostAttributes[1].path_mtu, IBV_MTU_2048);
    EXPECT_EQ(hostAttributes[1].rq_psn, TEST_PSN);
    EXPECT_EQ(hostAttributes[1].ah_attr.sl, 4U);
    EXPECT_EQ(hostAttributes[1].ah_attr.grh.traffic_class, 132U);
    EXPECT_EQ(hostAttributes[2].sq_psn, queue_->Psn());
}

TEST_F(HostQueueInteropTest, RejectsDeviceMtuAboveHostActiveMtu)
{
    EXPECT_EQ(Connect(IBV_MTU_4096), GEN_INVALID_PARAM);
    EXPECT_TRUE(hostAttributes.empty());
}

TEST_F(HostQueueInteropTest, PostsRegisteredAddressesAndKeysToProvider)
{
    ASSERT_EQ(Connect(TEST_MTU), GEN_OK);
    genetty_batch_op_data_t data{reinterpret_cast<void *>(MR_ADDRESS),           MR_SIZE, MR_LKEY,
                                 reinterpret_cast<void *>(MR_ADDRESS + MR_SIZE), MR_SIZE, MR_RKEY};
    genetty_batch_op_options_t options{TEST_DB, 0, OP_READ, 1, 1, 0};
    EXPECT_EQ(qp_->BatchPost(&data, 1, &options), GEN_OK);
    EXPECT_EQ(hostSge.addr, MR_ADDRESS);
    EXPECT_EQ(hostSge.lkey, MR_LKEY);
    EXPECT_EQ(hostRequest.wr.rdma.remote_addr, MR_ADDRESS + MR_SIZE);
    EXPECT_EQ(hostRequest.wr.rdma.rkey, MR_RKEY);
    EXPECT_EQ(hostRequest.wr_id, TEST_DB);
    EXPECT_EQ(hostRequest.opcode, IBV_WR_RDMA_READ);
    EXPECT_EQ(hostRequest.send_flags, IBV_SEND_SIGNALED);
}

void ConnectControlPair(SubCommandPp &client, SubCommandPp &server)
{
    constexpr uint32_t CONTROL_TIMEOUT_MS = 1000;
    int sockets[2]{};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    client.client_ = new SocketClient;
    client.client_->fd_ = sockets[0];
    client.client_->timeoutMs_ = CONTROL_TIMEOUT_MS;
    server.isServer_ = true;
    server.server_ = new SocketServer;
    server.server_->acceptedFd_ = sockets[1];
    server.server_->timeoutMs_ = CONTROL_TIMEOUT_MS;
}

TEST_F(DeviceQueueTest, TwoPhaseHandshakeUsesMtuReportedAfterDeviceConnect)
{
    HccpApi::RaGetQpAttr_ptr = [](void *, HccpQpAttr *attr) {
        attr->qpn = TEST_QPN;
        attr->psn = TEST_PSN;
        attr->pathMtu = queueState.local.qpn == 0 ? 0 : TEST_MTU;
        return 0;
    };
    CreateQp();
    ASSERT_NE(qp_, nullptr);
    ibv_port_attr port{};
    port.active_mtu = IBV_MTU_2048;
    ibv_device_attr attributes{};
    auto device = mf::MakeRef<VerbsDevice>("mock_cx7", "", HOST_PORT_INDEX, 0, port, attributes, nullptr);
    auto context = mf::MakeRef<VerbsContext>(nullptr, nullptr, device);
    ibv_qp raw{};
    raw.qp_num = TEST_QPN + 1;
    auto queue = mf::MakeRef<VerbsQP>(&raw, context, nullptr, nullptr, TEST_DEPTH, TEST_DEPTH);
    auto host = mf::MakeRef<VerbsHostQp>(HOST_RDMA_GENETTY);
    host->qp_ = queue;
    host->inited_ = true;
    hostAttributes.clear();
    VerbsApi::ibv_modify_qp_ptr = [](ibv_qp *, ibv_qp_attr *attr, int) {
        hostAttributes.push_back(*attr);
        return 0;
    };
    SubCommandPp client("pp", {});
    SubCommandPp server("pp", {});
    ConnectControlPair(client, server);
    client.endpoint_ = PP_ENDPOINT_DEVICE;
    client.genetty_ = qp_;
    server.genetty_ = host.Get();
    server.peerEndpoint_ = 1;
    int serverResult = -1;
    std::thread peer([&] {
        serverResult = server.ExchangeGenettyInfo();
        server.DestroySocket();
    });
    auto result = client.ExchangeGenettyInfo();
    client.DestroySocket();
    peer.join();
    EXPECT_EQ(result, 0);
    EXPECT_EQ(serverResult, 0);
    ASSERT_EQ(hostAttributes.size(), 3U);
    EXPECT_EQ(hostAttributes[1].path_mtu, IBV_MTU_2048);
    EXPECT_EQ(hostAttributes[1].rq_psn, TEST_PSN);
    queue->rawQp_ = nullptr;
    VerbsApi::ibv_modify_qp_ptr = nullptr;
}

TEST(OdccPingPongOptionsTest, DifferentRoundCountsFailBeforeCreatingDevices)
{
    SubCommandPp client("pp", {});
    SubCommandPp server("pp", {});
    ConnectControlPair(client, server);
    ++server.iterations_;
    int serverResult = 0;
    std::thread peer([&] {
        serverResult = server.ExchangeSettings();
        server.DestroySocket();
    });
    auto result = client.ExchangeSettings();
    client.DestroySocket();
    peer.join();
    EXPECT_NE(result, 0);
    EXPECT_NE(serverResult, 0);
}

TEST(VerbsProviderDispatchTest, InlineApisUseContextOperations)
{
    ibv_context context{};
    ibv_qp qp{};
    ibv_cq cq{};
    qp.context = &context;
    cq.context = &context;
    context.ops.post_send = [](ibv_qp *, ibv_send_wr *, ibv_send_wr **) { return MOCK_FAILURE; };
    context.ops.post_recv = [](ibv_qp *, ibv_recv_wr *, ibv_recv_wr **) { return MOCK_FAILURE; };
    context.ops.poll_cq = [](ibv_cq *, int num, ibv_wc *) {
        EXPECT_EQ(num, 1);
        return MOCK_FAILURE;
    };
    context.ops.req_notify_cq = [](ibv_cq *, int solicited) {
        EXPECT_EQ(solicited, 1);
        return MOCK_FAILURE;
    };
    EXPECT_EQ(VerbsApi::ibv_post_send(&qp, nullptr, nullptr), MOCK_FAILURE);
    EXPECT_EQ(VerbsApi::ibv_post_recv(&qp, nullptr, nullptr), MOCK_FAILURE);
    EXPECT_EQ(VerbsApi::ibv_poll_cq(&cq, 1, nullptr), MOCK_FAILURE);
    EXPECT_EQ(VerbsApi::ibv_req_notify_cq(&cq, 1), MOCK_FAILURE);
}
} // namespace
} // namespace verbs
} // namespace openDCC
} // namespace ock
