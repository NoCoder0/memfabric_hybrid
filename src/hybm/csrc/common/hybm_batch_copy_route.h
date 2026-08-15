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

#ifndef MEM_FABRIC_HYBRID_HYBM_BATCH_COPY_ROUTE_H
#define MEM_FABRIC_HYBRID_HYBM_BATCH_COPY_ROUTE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

namespace ock {
namespace mf {

constexpr uint32_t BATCH_COPY_ROUTE_MAGIC = 0x42435059U;
constexpr uint16_t BATCH_COPY_MAX_PEER_COUNT = 64U;
constexpr uint16_t BATCH_COPY_MAX_RANGE_PER_PEER = 16U;
constexpr uint16_t BATCH_COPY_MAX_RANGE_COUNT = BATCH_COPY_MAX_PEER_COUNT * BATCH_COPY_MAX_RANGE_PER_PEER;
constexpr char BATCH_COPY_LANES_ENV[] = "ASCEND_MF_BATCH_COPY_LANES";
constexpr uint16_t BATCH_COPY_DEFAULT_LANE_COUNT = 1U;
constexpr uint16_t BATCH_COPY_TWO_LANE_COUNT = 2U;
constexpr uint16_t BATCH_COPY_MAX_LANE_COUNT = 4U;

inline bool IsBatchCopyLaneCountSupported(uint16_t laneCount)
{
    return laneCount == BATCH_COPY_DEFAULT_LANE_COUNT || laneCount == BATCH_COPY_TWO_LANE_COUNT ||
           laneCount == BATCH_COPY_MAX_LANE_COUNT;
}

struct alignas(64) BatchCopyRouteHeader {
    uint32_t magic{0};
    uint16_t peerCount{0};
    uint16_t rangeCount{0};
};

struct alignas(32) BatchCopyPeerEntry {
    uint64_t thread{0};
    uint64_t channel{0};
    uint64_t remoteFlagAddr{0};
    uint16_t laneCount{0};
};

struct alignas(32) BatchCopyRangeEntry {
    uint64_t srcGvaBegin{0};
    uint64_t srcGvaEnd{0};
    uint64_t hcommVaBegin{0};
    uint16_t peerIndex{0};
};

struct alignas(64) BatchCopyRouteTable {
    BatchCopyRouteHeader header{};
    BatchCopyPeerEntry peers[BATCH_COPY_MAX_PEER_COUNT]{};
    BatchCopyRangeEntry ranges[BATCH_COPY_MAX_RANGE_COUNT]{};
};

struct alignas(64) BatchCopyCompletionArea {
    uint64_t cells[BATCH_COPY_MAX_PEER_COUNT]{};
};

inline bool IsBatchCopyRangeValid(const BatchCopyRangeEntry &range)
{
    if (range.srcGvaBegin >= range.srcGvaEnd) {
        return false;
    }
    const uint64_t length = range.srcGvaEnd - range.srcGvaBegin;
    return range.hcommVaBegin <= std::numeric_limits<uint64_t>::max() - length;
}

inline bool GetBatchCopyLaneCountFromEnv(uint16_t &laneCount)
{
    const char *value = std::getenv(BATCH_COPY_LANES_ENV);
    laneCount = BATCH_COPY_DEFAULT_LANE_COUNT;
    if (value == nullptr || value[0] == '\0') {
        return true;
    }
    if (std::strcmp(value, "1") == 0) {
        return true;
    }
    if (std::strcmp(value, "2") == 0) {
        laneCount = BATCH_COPY_TWO_LANE_COUNT;
        return true;
    }
    if (std::strcmp(value, "4") == 0) {
        laneCount = 4U;
        return true;
    }
    return false;
}

inline bool IsBatchCopyRouteLayoutValid(const BatchCopyRouteTable &table)
{
    if (table.header.peerCount == 0U || table.header.peerCount > BATCH_COPY_MAX_PEER_COUNT ||
        table.header.rangeCount == 0U || table.header.rangeCount > BATCH_COPY_MAX_RANGE_COUNT ||
        table.header.rangeCount > table.header.peerCount * BATCH_COPY_MAX_RANGE_PER_PEER) {
        return false;
    }
    uint16_t peerIndex = 0U;
    while (peerIndex < table.header.peerCount) {
        const uint16_t laneCount = table.peers[peerIndex].laneCount;
        if (!IsBatchCopyLaneCountSupported(laneCount) ||
            laneCount > table.header.peerCount - peerIndex) {
            return false;
        }
        for (uint16_t laneIndex = 0U; laneIndex < laneCount; ++laneIndex) {
            const auto &lane = table.peers[peerIndex + laneIndex];
            if (lane.thread == 0U || lane.channel == 0U || lane.remoteFlagAddr == 0U) {
                return false;
            }
        }
        peerIndex += laneCount;
    }

    uint16_t peerRangeCounts[BATCH_COPY_MAX_PEER_COUNT]{};
    for (uint16_t index = 0U; index < table.header.rangeCount; ++index) {
        const auto &range = table.ranges[index];
        if (!IsBatchCopyRangeValid(range) || range.peerIndex >= table.header.peerCount ||
            table.peers[range.peerIndex].laneCount == 0U ||
            ++peerRangeCounts[range.peerIndex] > BATCH_COPY_MAX_RANGE_PER_PEER) {
            return false;
        }
        if (index > 0U && range.srcGvaBegin < table.ranges[index - 1U].srcGvaEnd) {
            return false;
        }
    }
    return true;
}

static_assert(BATCH_COPY_MAX_RANGE_COUNT == 1024U);
static_assert(std::is_standard_layout<BatchCopyRouteHeader>::value);
static_assert(std::is_trivially_copyable<BatchCopyRouteHeader>::value);
static_assert(std::is_standard_layout<BatchCopyPeerEntry>::value);
static_assert(std::is_trivially_copyable<BatchCopyPeerEntry>::value);
static_assert(std::is_standard_layout<BatchCopyRangeEntry>::value);
static_assert(std::is_trivially_copyable<BatchCopyRangeEntry>::value);
static_assert(std::is_standard_layout<BatchCopyRouteTable>::value);
static_assert(std::is_trivially_copyable<BatchCopyRouteTable>::value);
static_assert(std::is_standard_layout<BatchCopyCompletionArea>::value);
static_assert(std::is_trivially_copyable<BatchCopyCompletionArea>::value);
static_assert(alignof(BatchCopyRouteHeader) == 0x40U);
static_assert(alignof(BatchCopyPeerEntry) == 0x20U);
static_assert(alignof(BatchCopyRangeEntry) == 0x20U);
static_assert(alignof(BatchCopyRouteTable) == 0x40U);
static_assert(alignof(BatchCopyCompletionArea) == 0x40U);
static_assert(sizeof(BatchCopyRouteHeader) == 0x40U);
static_assert(sizeof(BatchCopyPeerEntry) == 0x20U);
static_assert(sizeof(BatchCopyRangeEntry) == 0x20U);
static_assert(offsetof(BatchCopyRouteHeader, magic) == 0x00U);
static_assert(offsetof(BatchCopyRouteHeader, peerCount) == 0x04U);
static_assert(offsetof(BatchCopyRouteHeader, rangeCount) == 0x06U);
static_assert(offsetof(BatchCopyPeerEntry, thread) == 0x00U);
static_assert(offsetof(BatchCopyPeerEntry, channel) == 0x08U);
static_assert(offsetof(BatchCopyPeerEntry, remoteFlagAddr) == 0x10U);
static_assert(offsetof(BatchCopyPeerEntry, laneCount) == 0x18U);
static_assert(offsetof(BatchCopyRangeEntry, srcGvaBegin) == 0x00U);
static_assert(offsetof(BatchCopyRangeEntry, srcGvaEnd) == 0x08U);
static_assert(offsetof(BatchCopyRangeEntry, hcommVaBegin) == 0x10U);
static_assert(offsetof(BatchCopyRangeEntry, peerIndex) == 0x18U);
static_assert(offsetof(BatchCopyRouteTable, header) == 0x00U);
static_assert(offsetof(BatchCopyRouteTable, peers) == 0x40U);
static_assert(offsetof(BatchCopyRouteTable, ranges) == 0x840U);
static_assert(offsetof(BatchCopyCompletionArea, cells) == 0x00U);
static_assert(sizeof(BatchCopyRouteTable) == 0x8840U);
static_assert(sizeof(BatchCopyCompletionArea) == 0x0200U);

constexpr uint64_t BATCH_COPY_COMPLETION_OFFSET = sizeof(BatchCopyRouteTable);
constexpr uint64_t BATCH_COPY_CONTROL_USED_SIZE = sizeof(BatchCopyRouteTable) + sizeof(BatchCopyCompletionArea);
static_assert(BATCH_COPY_COMPLETION_OFFSET == 0x8840U);
static_assert(BATCH_COPY_CONTROL_USED_SIZE == 0x8A40U);

} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_BATCH_COPY_ROUTE_H
