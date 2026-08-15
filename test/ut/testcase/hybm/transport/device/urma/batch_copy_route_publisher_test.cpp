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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#define private public
#include "device/urma/batch_copy_route_publisher.h"
#include "dl_acl_api.h"
#include "dl_hcomm_api.h"
#include "hybm_define.h"
#undef private

using namespace ock::mf;
using namespace ock::mf::transport::device;
using namespace ock::mf::transport::urma;

namespace {
constexpr uint32_t kUserDeviceId = 3U;
constexpr uint32_t kFirstPeerRank = 7U;
constexpr uint32_t kSecondPeerRank = 9U;
constexpr uint64_t kCompletionAddr = HYBM_BATCH_COPY_META_ADDR + BATCH_COPY_COMPLETION_OFFSET;
constexpr uintptr_t kEndpointValue = 0x9501U;
constexpr uintptr_t kCompletionHandleValue = 0x9502U;
constexpr uint64_t kFirstThread = 0x101U;
constexpr uint64_t kSecondThread = 0x102U;
constexpr uint64_t kFirstExtraThread = 0x103U;
constexpr uint64_t kFirstChannel = 0x201U;
constexpr uint64_t kSecondChannel = 0x202U;
constexpr uint64_t kFirstExtraChannel = 0x203U;
constexpr uint64_t kFirstRemoteFlag = 0x301U;
constexpr uint64_t kSecondRemoteFlag = 0x302U;
constexpr uint64_t kLowGvaBegin = 0x1000U;
constexpr uint64_t kLowGvaEnd = 0x2000U;
constexpr uint64_t kMiddleGvaBegin = 0x3000U;
constexpr uint64_t kMiddleGvaEnd = 0x4000U;
constexpr uint64_t kHighGvaBegin = 0x5000U;
constexpr uint64_t kHighGvaEnd = 0x6000U;
constexpr uint64_t kLowHcommBegin = 0xA1000U;
constexpr uint64_t kMiddleHcommBegin = 0xA3000U;
constexpr uint64_t kHighHcommBegin = 0xA5000U;
constexpr size_t kClearMagicCopyIndex = 0U;
constexpr size_t kClearCompletionCopyIndex = 1U;
constexpr size_t kRouteImageCopyIndex = 2U;
constexpr size_t kPublishMagicCopyIndex = 3U;
constexpr size_t kPublishCopyCount = 4U;
const EndpointHandle kMockEndpoint = reinterpret_cast<EndpointHandle>(kEndpointValue);
const HcommMemHandle kMockCompletionHandle = reinterpret_cast<HcommMemHandle>(kCompletionHandleValue);

struct CopyEvent {
    uint64_t destination{0};
    std::vector<uint8_t> bytes{};
};

std::vector<CopyEvent> g_copyEvents;
size_t g_failCopyIndex = std::numeric_limits<size_t>::max();
bool g_failMemReg = false;
bool g_failMemUnreg = false;
uint32_t g_memRegCount = 0U;
uint32_t g_memUnregCount = 0U;
size_t g_copyCountAtMemReg = 0U;

int32_t MockAclrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
{
    EXPECT_EQ(destMax, count);
    EXPECT_EQ(kind, ACL_MEMCPY_HOST_TO_DEVICE);
    CopyEvent event{};
    event.destination = reinterpret_cast<uint64_t>(dst);
    event.bytes.resize(count);
    std::memcpy(event.bytes.data(), src, count);
    g_copyEvents.push_back(std::move(event));
    return g_copyEvents.size() - 1U == g_failCopyIndex ? BM_ERROR : BM_OK;
}

int32_t MockHcommMemReg(EndpointHandle endpoint, const char *memTag, const HcommCommMem *mem, HcommMemHandle *memHandle)
{
    EXPECT_EQ(endpoint, kMockEndpoint);
    const auto expectedTag = std::to_string(kCompletionAddr);
    EXPECT_STREQ(memTag, expectedTag.c_str());
    EXPECT_NE(mem, nullptr);
    if (mem == nullptr) {
        return BM_INVALID_PARAM;
    }
    EXPECT_EQ(mem->type, COMM_MEM_TYPE_DEVICE);
    EXPECT_EQ(mem->addr, reinterpret_cast<void *>(kCompletionAddr));
    EXPECT_EQ(mem->size, sizeof(BatchCopyCompletionArea));
    g_copyCountAtMemReg = g_copyEvents.size();
    ++g_memRegCount;
    if (g_failMemReg) {
        return BM_ERROR;
    }
    *memHandle = kMockCompletionHandle;
    return BM_OK;
}

int32_t MockHcommMemUnreg(EndpointHandle endpoint, HcommMemHandle memHandle)
{
    EXPECT_EQ(endpoint, kMockEndpoint);
    EXPECT_EQ(memHandle, kMockCompletionHandle);
    ++g_memUnregCount;
    return g_failMemUnreg ? BM_ERROR : BM_OK;
}

struct ApiGuard {
    aclrtMemcpyFunc oldMemcpy{DlAclApi::pAclrtMemcpy};
    hcommMemRegFunc oldMemReg{DlHcommApi::gHcommMemReg};
    hcommMemUnregFunc oldMemUnreg{DlHcommApi::gHcommMemUnreg};

    ApiGuard()
    {
        DlAclApi::pAclrtMemcpy = MockAclrtMemcpy;
        DlHcommApi::gHcommMemReg = MockHcommMemReg;
        DlHcommApi::gHcommMemUnreg = MockHcommMemUnreg;
        g_copyEvents.clear();
        g_failCopyIndex = std::numeric_limits<size_t>::max();
        g_failMemReg = false;
        g_failMemUnreg = false;
        g_memRegCount = 0U;
        g_memUnregCount = 0U;
        g_copyCountAtMemReg = 0U;
    }

    ~ApiGuard()
    {
        DlAclApi::pAclrtMemcpy = oldMemcpy;
        DlHcommApi::gHcommMemReg = oldMemReg;
        DlHcommApi::gHcommMemUnreg = oldMemUnreg;
    }
};

UrmaEndpointHandle MakeEndpoint()
{
    auto endpoint = std::make_shared<UrmaEndpointEntity>();
    endpoint->hcommEndpoint = kMockEndpoint;
    return endpoint;
}

std::vector<BatchCopyRouteSource> MakeSources()
{
    BatchCopyRouteSource first{};
    first.peerRank = kFirstPeerRank;
    first.thread = kFirstThread;
    first.channel = kFirstChannel;
    first.remoteFlagAddr = kFirstRemoteFlag;
    first.ranges = {{kHighGvaBegin, kHighGvaEnd, kHighHcommBegin}, {kLowGvaBegin, kLowGvaEnd, kLowHcommBegin}};

    BatchCopyRouteSource second{};
    second.peerRank = kSecondPeerRank;
    second.thread = kSecondThread;
    second.channel = kSecondChannel;
    second.remoteFlagAddr = kSecondRemoteFlag;
    second.ranges = {{kMiddleGvaBegin, kMiddleGvaEnd, kMiddleHcommBegin}};
    return {first, second};
}

template<typename T>
T ReadEventValue(const CopyEvent &event)
{
    EXPECT_EQ(event.bytes.size(), sizeof(T));
    T value{};
    std::memcpy(&value, event.bytes.data(), sizeof(value));
    return value;
}
} // namespace

TEST(BatchCopyRoutePublisherTest, PublishesSortedImageAndMagicLast)
{
    ApiGuard guard;
    HcommTransportManager manager;
    BatchCopyRoutePublisher publisher(kUserDeviceId, MakeEndpoint(), manager);

    ASSERT_EQ(publisher.Publish(MakeSources()), BM_OK);
    ASSERT_TRUE(publisher.IsPublished());
    ASSERT_EQ(g_copyEvents.size(), kPublishCopyCount);
    EXPECT_EQ(g_copyEvents[kClearMagicCopyIndex].destination, HYBM_BATCH_COPY_META_ADDR);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents[kClearMagicCopyIndex]), 0U);
    EXPECT_EQ(g_copyEvents[kClearCompletionCopyIndex].destination, kCompletionAddr);
    EXPECT_TRUE(std::all_of(g_copyEvents[kClearCompletionCopyIndex].bytes.begin(),
                            g_copyEvents[kClearCompletionCopyIndex].bytes.end(),
                            [](uint8_t value) { return value == 0U; }));

    BatchCopyRouteTable table{};
    ASSERT_EQ(g_copyEvents[kRouteImageCopyIndex].bytes.size(), sizeof(table));
    std::memcpy(&table, g_copyEvents[kRouteImageCopyIndex].bytes.data(), sizeof(table));
    EXPECT_EQ(table.header.magic, 0U);
    EXPECT_EQ(table.header.peerCount, 2U);
    EXPECT_EQ(table.header.rangeCount, 3U);
    EXPECT_EQ(table.peers[0].thread, kFirstThread);
    EXPECT_EQ(table.peers[0].laneCount, 1U);
    EXPECT_EQ(table.peers[1].channel, kSecondChannel);
    EXPECT_EQ(table.peers[1].laneCount, 1U);
    EXPECT_EQ(table.ranges[0].srcGvaBegin, kLowGvaBegin);
    EXPECT_EQ(table.ranges[0].hcommVaBegin, kLowHcommBegin);
    EXPECT_EQ(table.ranges[0].peerIndex, 0U);
    EXPECT_EQ(table.ranges[1].srcGvaBegin, kMiddleGvaBegin);
    EXPECT_EQ(table.ranges[1].peerIndex, 1U);
    EXPECT_EQ(table.ranges[2].srcGvaBegin, kHighGvaBegin);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents[kPublishMagicCopyIndex]), BATCH_COPY_ROUTE_MAGIC);
    EXPECT_EQ(g_memRegCount, 1U);
    EXPECT_EQ(g_copyCountAtMemReg, kRouteImageCopyIndex);

    EXPECT_EQ(publisher.Clear(), BM_OK);
    EXPECT_FALSE(publisher.IsPublished());
    EXPECT_EQ(g_memUnregCount, 1U);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);
}

TEST(BatchCopyRoutePublisherTest, ExpandsLogicalPeerIntoContiguousLanes)
{
    ApiGuard guard;
    HcommTransportManager manager;
    BatchCopyRoutePublisher publisher(kUserDeviceId, MakeEndpoint(), manager);
    auto sources = MakeSources();
    sources[0].extraLanes.push_back({kFirstExtraThread, kFirstExtraChannel});

    ASSERT_EQ(publisher.Publish(sources), BM_OK);
    BatchCopyRouteTable table{};
    ASSERT_EQ(g_copyEvents[kRouteImageCopyIndex].bytes.size(), sizeof(table));
    std::memcpy(&table, g_copyEvents[kRouteImageCopyIndex].bytes.data(), sizeof(table));
    EXPECT_EQ(table.header.peerCount, 3U);
    EXPECT_EQ(table.peers[0].laneCount, 2U);
    EXPECT_EQ(table.peers[1].thread, kFirstExtraThread);
    EXPECT_EQ(table.peers[1].channel, kFirstExtraChannel);
    EXPECT_EQ(table.peers[1].laneCount, 0U);
    EXPECT_EQ(table.peers[2].laneCount, 1U);
    EXPECT_EQ(table.ranges[1].peerIndex, 2U);

    EXPECT_EQ(publisher.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, RejectsUnsupportedThreeLaneTopology)
{
    ApiGuard guard;
    HcommTransportManager manager;
    BatchCopyRoutePublisher publisher(kUserDeviceId, MakeEndpoint(), manager);
    auto sources = MakeSources();
    sources[0].extraLanes.push_back({kFirstExtraThread, kFirstExtraChannel});
    sources[0].extraLanes.push_back({kSecondThread, kSecondChannel});

    EXPECT_EQ(publisher.Publish(sources), BM_INVALID_PARAM);
    EXPECT_FALSE(publisher.IsPublished());
    EXPECT_TRUE(g_copyEvents.empty());
}

TEST(BatchCopyRoutePublisherTest, RepeatedPublishDoesNotRefreshRoute)
{
    ApiGuard guard;
    HcommTransportManager manager;
    BatchCopyRoutePublisher publisher(kUserDeviceId, MakeEndpoint(), manager);
    ASSERT_EQ(publisher.Publish(MakeSources()), BM_OK);
    const size_t copyCount = g_copyEvents.size();

    EXPECT_EQ(publisher.Publish({}), BM_OK);
    EXPECT_EQ(g_copyEvents.size(), copyCount);
    EXPECT_EQ(g_memRegCount, 1U);
    EXPECT_EQ(publisher.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, RegistrationFailureClearsMagicAndResources)
{
    ApiGuard guard;
    HcommTransportManager manager;
    const auto endpoint = MakeEndpoint();
    BatchCopyRoutePublisher failed(kUserDeviceId, endpoint, manager);
    g_failMemReg = true;

    EXPECT_EQ(failed.Publish(MakeSources()), BM_DL_FUNCTION_FAILED);
    EXPECT_FALSE(failed.IsPublished());
    ASSERT_FALSE(g_copyEvents.empty());
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);

    g_failMemReg = false;
    BatchCopyRoutePublisher retry(kUserDeviceId, endpoint, manager);
    EXPECT_EQ(retry.Publish(MakeSources()), BM_OK);
    EXPECT_EQ(retry.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, CompletionClearFailureAllowsRetry)
{
    ApiGuard guard;
    HcommTransportManager manager;
    const auto endpoint = MakeEndpoint();
    BatchCopyRoutePublisher failed(kUserDeviceId, endpoint, manager);
    g_failCopyIndex = kClearCompletionCopyIndex;

    EXPECT_EQ(failed.Publish(MakeSources()), BM_ERROR);
    EXPECT_FALSE(failed.IsPublished());
    EXPECT_EQ(g_memRegCount, 0U);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);

    g_failCopyIndex = std::numeric_limits<size_t>::max();
    BatchCopyRoutePublisher retry(kUserDeviceId, endpoint, manager);
    EXPECT_EQ(retry.Publish(MakeSources()), BM_OK);
    EXPECT_EQ(retry.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, RouteImageWriteFailureRollsBackCompletion)
{
    ApiGuard guard;
    HcommTransportManager manager;
    const auto endpoint = MakeEndpoint();
    BatchCopyRoutePublisher failed(kUserDeviceId, endpoint, manager);
    g_failCopyIndex = kRouteImageCopyIndex;

    EXPECT_EQ(failed.Publish(MakeSources()), BM_ERROR);
    EXPECT_FALSE(failed.IsPublished());
    EXPECT_EQ(g_memUnregCount, 1U);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);

    g_failCopyIndex = std::numeric_limits<size_t>::max();
    BatchCopyRoutePublisher retry(kUserDeviceId, endpoint, manager);
    EXPECT_EQ(retry.Publish(MakeSources()), BM_OK);
    EXPECT_EQ(retry.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, MagicPublishFailureLeavesRouteInvalid)
{
    ApiGuard guard;
    HcommTransportManager manager;
    BatchCopyRoutePublisher publisher(kUserDeviceId, MakeEndpoint(), manager);
    g_failCopyIndex = kPublishMagicCopyIndex;

    EXPECT_EQ(publisher.Publish(MakeSources()), BM_ERROR);
    EXPECT_FALSE(publisher.IsPublished());
    EXPECT_EQ(g_memUnregCount, 1U);
    ASSERT_GT(g_copyEvents.size(), kPublishCopyCount);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);
}

TEST(BatchCopyRoutePublisherTest, ClearMagicFailureRetainsCompletionForRetry)
{
    ApiGuard guard;
    HcommTransportManager manager;
    const auto endpoint = MakeEndpoint();
    BatchCopyRoutePublisher publisher(kUserDeviceId, endpoint, manager);

    ASSERT_EQ(publisher.Publish(MakeSources()), BM_OK);
    g_failCopyIndex = g_copyEvents.size();
    EXPECT_EQ(publisher.Clear(), BM_ERROR);
    EXPECT_TRUE(publisher.IsPublished());
    EXPECT_EQ(g_memUnregCount, 0U);

    g_failCopyIndex = std::numeric_limits<size_t>::max();
    EXPECT_EQ(publisher.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, CompletionUnregisterFailureRequiresCleanupRetry)
{
    ApiGuard guard;
    HcommTransportManager manager;
    const auto endpoint = MakeEndpoint();
    BatchCopyRoutePublisher publisher(kUserDeviceId, endpoint, manager);

    ASSERT_EQ(publisher.Publish(MakeSources()), BM_OK);
    g_failMemUnreg = true;
    EXPECT_EQ(publisher.Clear(), BM_DL_FUNCTION_FAILED);
    EXPECT_FALSE(publisher.IsPublished());
    const size_t copyCount = g_copyEvents.size();
    EXPECT_EQ(publisher.Publish(MakeSources()), BM_ERROR);
    EXPECT_EQ(g_copyEvents.size(), copyCount);

    g_failMemUnreg = false;
    EXPECT_EQ(publisher.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, RejectsOverlapAndRangeCapacityBeforeWritingMetadata)
{
    ApiGuard guard;
    HcommTransportManager manager;
    BatchCopyRoutePublisher publisher(kUserDeviceId, MakeEndpoint(), manager);
    auto sources = MakeSources();
    sources[1].ranges[0] = {kLowGvaEnd - 1U, kMiddleGvaBegin, kMiddleHcommBegin};

    EXPECT_EQ(publisher.Publish(sources), BM_INVALID_PARAM);
    EXPECT_TRUE(g_copyEvents.empty());
    EXPECT_EQ(g_memRegCount, 0U);

    sources = MakeSources();
    sources[0].ranges.resize(BATCH_COPY_MAX_RANGE_PER_PEER + 1U, sources[0].ranges.front());
    EXPECT_EQ(publisher.Publish(sources), BM_INVALID_PARAM);
    EXPECT_TRUE(g_copyEvents.empty());
}
