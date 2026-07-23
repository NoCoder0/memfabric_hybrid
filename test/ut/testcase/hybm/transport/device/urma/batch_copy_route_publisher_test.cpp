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
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#define private public
#include "device/urma/batch_copy_route_publisher.h"
#include "dl_acl_api.h"
#undef private

using namespace ock::mf;
using namespace ock::mf::transport::device;
using namespace ock::mf::transport::urma;

namespace {
constexpr uint32_t MOCK_USER_DEVICE_ID = 3U;
constexpr uint64_t COMPLETION_ADDR = HYBM_BATCH_COPY_META_ADDR + BATCH_COPY_COMPLETION_OFFSET;
const EndpointHandle MOCK_ENDPOINT = reinterpret_cast<EndpointHandle>(0x9501UL);
const HcommMemHandle MOCK_COMPLETION_HANDLE = reinterpret_cast<HcommMemHandle>(0x9502UL);

struct CopyEvent {
    uint64_t destination{0};
    std::vector<uint8_t> bytes{};
};

std::vector<CopyEvent> g_copyEvents;
size_t g_failCopyIndex = std::numeric_limits<size_t>::max();
bool g_failMemReg = false;
uint32_t g_memRegCount = 0;
uint32_t g_memUnregCount = 0;
size_t g_copyCountAtMemReg = 0;

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
    EXPECT_EQ(endpoint, MOCK_ENDPOINT);
    const auto expectedTag = std::to_string(COMPLETION_ADDR);
    EXPECT_STREQ(memTag, expectedTag.c_str());
    EXPECT_NE(mem, nullptr);
    EXPECT_EQ(mem->type, COMM_MEM_TYPE_DEVICE);
    EXPECT_EQ(mem->addr, reinterpret_cast<void *>(COMPLETION_ADDR));
    EXPECT_EQ(mem->size, sizeof(BatchCopyCompletionArea));
    g_copyCountAtMemReg = g_copyEvents.size();
    ++g_memRegCount;
    if (g_failMemReg) {
        return BM_ERROR;
    }
    *memHandle = MOCK_COMPLETION_HANDLE;
    return BM_OK;
}

int32_t MockHcommMemUnreg(EndpointHandle endpoint, HcommMemHandle memHandle)
{
    EXPECT_EQ(endpoint, MOCK_ENDPOINT);
    EXPECT_EQ(memHandle, MOCK_COMPLETION_HANDLE);
    ++g_memUnregCount;
    return BM_OK;
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
        g_memRegCount = 0;
        g_memUnregCount = 0;
        g_copyCountAtMemReg = 0;
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
    endpoint->hcommEndpoint = MOCK_ENDPOINT;
    return endpoint;
}

std::vector<BatchCopyRouteSource> MakeSources()
{
    BatchCopyRouteSource first{};
    first.peerRank = 7U;
    first.thread = 0x101U;
    first.channel = 0x201U;
    first.remoteFlagAddr = 0x301U;
    first.ranges = {{0x5000U, 0x6000U}, {0x1000U, 0x2000U}};

    BatchCopyRouteSource second{};
    second.peerRank = 9U;
    second.thread = 0x102U;
    second.channel = 0x202U;
    second.remoteFlagAddr = 0x302U;
    second.ranges = {{0x3000U, 0x4000U}};
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
    BatchCopyRoutePublisher publisher(MOCK_USER_DEVICE_ID, MakeEndpoint(), manager);

    ASSERT_EQ(publisher.Publish(MakeSources()), BM_OK);
    ASSERT_TRUE(publisher.IsPublished());
    ASSERT_EQ(g_copyEvents.size(), 4U);
    EXPECT_EQ(g_copyEvents[0].destination, HYBM_BATCH_COPY_META_ADDR);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents[0]), 0U);
    EXPECT_EQ(g_copyEvents[1].destination, COMPLETION_ADDR);
    EXPECT_EQ(g_copyEvents[1].bytes.size(), sizeof(BatchCopyCompletionArea));
    EXPECT_TRUE(std::all_of(g_copyEvents[1].bytes.begin(), g_copyEvents[1].bytes.end(),
                            [](uint8_t value) { return value == 0; }));

    BatchCopyRouteTable table{};
    ASSERT_EQ(g_copyEvents[2].bytes.size(), sizeof(table));
    std::memcpy(&table, g_copyEvents[2].bytes.data(), sizeof(table));
    EXPECT_EQ(table.header.magic, 0U);
    EXPECT_EQ(table.header.peerCount, 2U);
    EXPECT_EQ(table.header.rangeCount, 3U);
    EXPECT_EQ(table.peers[0].thread, 0x101U);
    EXPECT_EQ(table.peers[1].channel, 0x202U);
    EXPECT_EQ(table.ranges[0].srcGvaBegin, 0x1000U);
    EXPECT_EQ(table.ranges[0].peerIndex, 0U);
    EXPECT_EQ(table.ranges[1].srcGvaBegin, 0x3000U);
    EXPECT_EQ(table.ranges[1].peerIndex, 1U);
    EXPECT_EQ(table.ranges[2].srcGvaBegin, 0x5000U);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents[3]), BATCH_COPY_ROUTE_MAGIC);
    EXPECT_EQ(g_memRegCount, 1U);
    EXPECT_EQ(g_copyCountAtMemReg, 2U);

    EXPECT_EQ(publisher.Clear(), BM_OK);
    EXPECT_FALSE(publisher.IsPublished());
    EXPECT_EQ(g_memUnregCount, 1U);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);
}

TEST(BatchCopyRoutePublisherTest, RejectsSecondOwnerOnSameDevice)
{
    ApiGuard guard;
    HcommTransportManager manager;
    const auto endpoint = MakeEndpoint();
    BatchCopyRoutePublisher first(MOCK_USER_DEVICE_ID, endpoint, manager);
    BatchCopyRoutePublisher second(MOCK_USER_DEVICE_ID, endpoint, manager);

    ASSERT_EQ(first.Publish(MakeSources()), BM_OK);
    EXPECT_EQ(second.Publish(MakeSources()), BM_BUSY);
    ASSERT_EQ(first.Clear(), BM_OK);
    EXPECT_EQ(second.Publish(MakeSources()), BM_OK);
    EXPECT_EQ(second.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, RegistrationFailureClearsMagicAndReleasesOwner)
{
    ApiGuard guard;
    HcommTransportManager manager;
    const auto endpoint = MakeEndpoint();
    BatchCopyRoutePublisher failed(MOCK_USER_DEVICE_ID, endpoint, manager);
    g_failMemReg = true;

    EXPECT_EQ(failed.Publish(MakeSources()), BM_DL_FUNCTION_FAILED);
    EXPECT_FALSE(failed.IsPublished());
    ASSERT_FALSE(g_copyEvents.empty());
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);

    g_failMemReg = false;
    BatchCopyRoutePublisher retry(MOCK_USER_DEVICE_ID, endpoint, manager);
    EXPECT_EQ(retry.Publish(MakeSources()), BM_OK);
    EXPECT_EQ(retry.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, RouteWriteFailureRollsBackRegistrationAndOwner)
{
    ApiGuard guard;
    HcommTransportManager manager;
    const auto endpoint = MakeEndpoint();
    BatchCopyRoutePublisher failed(MOCK_USER_DEVICE_ID, endpoint, manager);
    g_failCopyIndex = 2U;

    EXPECT_EQ(failed.Publish(MakeSources()), BM_ERROR);
    EXPECT_FALSE(failed.IsPublished());
    EXPECT_EQ(g_memUnregCount, 1U);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);

    g_failCopyIndex = std::numeric_limits<size_t>::max();
    BatchCopyRoutePublisher retry(MOCK_USER_DEVICE_ID, endpoint, manager);
    EXPECT_EQ(retry.Publish(MakeSources()), BM_OK);
    EXPECT_EQ(retry.Clear(), BM_OK);
}

TEST(BatchCopyRoutePublisherTest, MagicPublishFailureLeavesRouteInvalid)
{
    ApiGuard guard;
    HcommTransportManager manager;
    BatchCopyRoutePublisher publisher(MOCK_USER_DEVICE_ID, MakeEndpoint(), manager);
    g_failCopyIndex = 3U;

    EXPECT_EQ(publisher.Publish(MakeSources()), BM_ERROR);
    EXPECT_FALSE(publisher.IsPublished());
    EXPECT_EQ(g_memUnregCount, 1U);
    ASSERT_GE(g_copyEvents.size(), 5U);
    EXPECT_EQ(ReadEventValue<uint32_t>(g_copyEvents.back()), 0U);
}

TEST(BatchCopyRoutePublisherTest, RejectsOverlappingRangesBeforeAcquiringOwner)
{
    ApiGuard guard;
    HcommTransportManager manager;
    BatchCopyRoutePublisher publisher(MOCK_USER_DEVICE_ID, MakeEndpoint(), manager);
    auto sources = MakeSources();
    sources[1].ranges[0] = {0x1800U, 0x2800U};

    EXPECT_EQ(publisher.Publish(sources), BM_INVALID_PARAM);
    EXPECT_TRUE(g_copyEvents.empty());
    EXPECT_EQ(g_memRegCount, 0U);
}
