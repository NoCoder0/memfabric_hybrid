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

#include "hybm_batch_copy.h"
#include "hybm_batch_copy_route.h"
#include "hybm_define.h"
#include "hybm_kvcache_scatter_copy.h"

using namespace ock::mf;

namespace {
constexpr uint64_t kFirstGvaBegin = 0x1000ULL;
constexpr uint64_t kFirstGvaEnd = 0x2000ULL;
constexpr uint64_t kSecondGvaBegin = 0x3000ULL;
constexpr uint64_t kSecondGvaEnd = 0x4000ULL;
constexpr uint64_t kFirstHcommBegin = 0xA1000ULL;
constexpr uint64_t kSecondHcommBegin = 0xA3000ULL;
constexpr uint64_t kRangeStride = 0x1000ULL;
constexpr uint64_t kRangeLength = 0x0800ULL;

BatchCopyRouteTable MakeValidRoute()
{
    BatchCopyRouteTable table{};
    table.header.peerCount = 2U;
    table.header.rangeCount = 2U;
    table.ranges[0] = {kFirstGvaBegin, kFirstGvaEnd, kFirstHcommBegin, 0U};
    table.ranges[1] = {kSecondGvaBegin, kSecondGvaEnd, kSecondHcommBegin, 1U};
    return table;
}
} // namespace

TEST(HybmBatchCopyRouteTest, AbiOffsetsAndSizesAreStable)
{
    EXPECT_EQ(BATCH_COPY_MAX_PEER_COUNT, 64U);
    EXPECT_EQ(BATCH_COPY_MAX_RANGE_PER_PEER, 16U);
    EXPECT_EQ(BATCH_COPY_MAX_RANGE_COUNT, 1024U);
    EXPECT_EQ(sizeof(BatchCopyRouteHeader), 0x40U);
    EXPECT_EQ(sizeof(BatchCopyPeerEntry), 0x20U);
    EXPECT_EQ(sizeof(BatchCopyRangeEntry), 0x20U);
    EXPECT_EQ(offsetof(BatchCopyRangeEntry, hcommVaBegin), 0x10U);
    EXPECT_EQ(offsetof(BatchCopyRangeEntry, peerIndex), 0x18U);
    EXPECT_EQ(offsetof(BatchCopyRouteTable, peers), 0x40U);
    EXPECT_EQ(offsetof(BatchCopyRouteTable, ranges), 0x840U);
    EXPECT_EQ(sizeof(BatchCopyRouteTable), 0x8840U);
    EXPECT_EQ(sizeof(BatchCopyCompletionArea), 0x0200U);
    EXPECT_EQ(BATCH_COPY_COMPLETION_OFFSET, 0x8840U);
    EXPECT_EQ(BATCH_COPY_CONTROL_USED_SIZE, 0x8A40U);
}

TEST(HybmBatchCopyRouteTest, OperatorAbiContainsOnlyFourBusinessFields)
{
    EXPECT_EQ(offsetof(HybmBatchCopyParam, list_num), 0x00U);
    EXPECT_EQ(offsetof(HybmBatchCopyParam, dst_buf_addr_list), 0x08U);
    EXPECT_EQ(offsetof(HybmBatchCopyParam, src_buf_addr_list), 0x10U);
    EXPECT_EQ(offsetof(HybmBatchCopyParam, len_list), 0x18U);
    EXPECT_EQ(sizeof(HybmBatchCopyParam), 0x20U);
}

TEST(HybmBatchCopyRouteTest, KvcacheScatterCopyOperatorAbiIsStable)
{
    EXPECT_EQ(offsetof(HybmKvcacheScatterCopyParam, offloadSlots), 0x20U);
    EXPECT_EQ(offsetof(HybmKvcacheScatterCopyParam, readyFlag), 0x40U);
    EXPECT_EQ(offsetof(HybmKvcacheScatterCopyParam, dramBlockTableRows), 0x60U);
    EXPECT_EQ(offsetof(HybmKvcacheScatterCopyParam, layerId), 0x70U);
    EXPECT_EQ(sizeof(HybmKvcacheScatterCopyParam), 0x78U);
}

TEST(HybmBatchCopyRouteTest, ControlRegionPrecedesExistingDeviceMetadata)
{
    EXPECT_EQ(HYBM_BATCH_COPY_META_SIZE, HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(HYBM_BATCH_COPY_META_ADDR + HYBM_BATCH_COPY_META_SIZE, HYBM_DEVICE_META_ADDR);
    EXPECT_EQ(HYBM_DEVICE_CONTROL_ADDR, HYBM_BATCH_COPY_META_ADDR);
    EXPECT_EQ(HYBM_DEVICE_CONTROL_SIZE, 34U * MB);
    EXPECT_EQ(HYBM_DEVICE_CONTROL_ADDR + HYBM_DEVICE_CONTROL_SIZE, SVM_END_ADDR);
    EXPECT_LE(BATCH_COPY_CONTROL_USED_SIZE, HYBM_BATCH_COPY_META_SIZE);
}

TEST(HybmBatchCopyRouteTest, AcceptsValidNonEmptyRanges)
{
    EXPECT_TRUE(IsBatchCopyRouteLayoutValid(MakeValidRoute()));
}

TEST(HybmBatchCopyRouteTest, RejectsEmptyRange)
{
    auto table = MakeValidRoute();
    table.ranges[0].srcGvaEnd = table.ranges[0].srcGvaBegin;

    EXPECT_FALSE(IsBatchCopyRouteLayoutValid(table));
}

TEST(HybmBatchCopyRouteTest, RejectsHcommAddressOverflow)
{
    auto table = MakeValidRoute();
    const uint64_t rangeLength = table.ranges[0].srcGvaEnd - table.ranges[0].srcGvaBegin;
    table.ranges[0].hcommVaBegin = std::numeric_limits<uint64_t>::max() - rangeLength + 1U;

    EXPECT_FALSE(IsBatchCopyRouteLayoutValid(table));
}

TEST(HybmBatchCopyRouteTest, RejectsWrappedGvaRange)
{
    auto table = MakeValidRoute();
    table.ranges[0].srcGvaBegin = std::numeric_limits<uint64_t>::max();
    table.ranges[0].srcGvaEnd = 1ULL;

    EXPECT_FALSE(IsBatchCopyRouteLayoutValid(table));
}

TEST(HybmBatchCopyRouteTest, RejectsOverlappingRangesAcrossPeers)
{
    auto table = MakeValidRoute();
    table.ranges[1].srcGvaBegin = kFirstGvaEnd - 1U;

    EXPECT_FALSE(IsBatchCopyRouteLayoutValid(table));
}

TEST(HybmBatchCopyRouteTest, RejectsPeerAndTotalCapacityOverflow)
{
    auto table = MakeValidRoute();
    table.header.peerCount = BATCH_COPY_MAX_PEER_COUNT + 1U;
    EXPECT_FALSE(IsBatchCopyRouteLayoutValid(table));

    table = MakeValidRoute();
    table.header.rangeCount = BATCH_COPY_MAX_RANGE_COUNT + 1U;
    EXPECT_FALSE(IsBatchCopyRouteLayoutValid(table));
}

TEST(HybmBatchCopyRouteTest, RejectsPerPeerRangeCapacityOverflow)
{
    BatchCopyRouteTable table{};
    table.header.peerCount = 2U;
    table.header.rangeCount = BATCH_COPY_MAX_RANGE_PER_PEER + 1U;
    for (uint16_t index = 0U; index < table.header.rangeCount; ++index) {
        const uint64_t begin = kFirstGvaBegin + static_cast<uint64_t>(index) * kRangeStride;
        table.ranges[index] = {begin, begin + kRangeLength, kFirstHcommBegin + begin, 0U};
    }

    EXPECT_FALSE(IsBatchCopyRouteLayoutValid(table));
}
