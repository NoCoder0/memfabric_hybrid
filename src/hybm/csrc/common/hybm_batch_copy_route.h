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
#include <type_traits>

namespace ock {
namespace mf {

constexpr uint32_t BATCH_COPY_ROUTE_MAGIC = 0x42435059U;
constexpr uint16_t BATCH_COPY_MAX_PEER_COUNT = 64U;
constexpr uint16_t BATCH_COPY_MAX_RANGE_PER_PEER = 16U;
constexpr uint16_t BATCH_COPY_MAX_RANGE_COUNT = BATCH_COPY_MAX_PEER_COUNT * BATCH_COPY_MAX_RANGE_PER_PEER;

struct alignas(64) BatchCopyRouteHeader {
    uint32_t magic{0};
    uint16_t peerCount{0};
    uint16_t rangeCount{0};
};

struct alignas(32) BatchCopyPeerEntry {
    uint64_t thread{0};
    uint64_t channel{0};
    uint64_t remoteFlagAddr{0};
};

struct alignas(32) BatchCopyRangeEntry {
    uint64_t srcGvaBegin{0};
    uint64_t srcGvaEnd{0};
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

static_assert(std::is_standard_layout<BatchCopyRouteTable>::value);
static_assert(std::is_trivially_copyable<BatchCopyRouteTable>::value);
static_assert(sizeof(BatchCopyRouteHeader) == 0x40U);
static_assert(sizeof(BatchCopyPeerEntry) == 0x20U);
static_assert(sizeof(BatchCopyRangeEntry) == 0x20U);
static_assert(offsetof(BatchCopyRouteTable, peers) == 0x40U);
static_assert(offsetof(BatchCopyRouteTable, ranges) == 0x840U);
static_assert(sizeof(BatchCopyRouteTable) == 0x8840U);
static_assert(sizeof(BatchCopyCompletionArea) == 0x200U);

constexpr uint64_t BATCH_COPY_COMPLETION_OFFSET = sizeof(BatchCopyRouteTable);
constexpr uint64_t BATCH_COPY_CONTROL_USED_SIZE = sizeof(BatchCopyRouteTable) + sizeof(BatchCopyCompletionArea);
static_assert(BATCH_COPY_COMPLETION_OFFSET == 0x8840U);
static_assert(BATCH_COPY_CONTROL_USED_SIZE == 0x8A40U);

} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_BATCH_COPY_ROUTE_H
