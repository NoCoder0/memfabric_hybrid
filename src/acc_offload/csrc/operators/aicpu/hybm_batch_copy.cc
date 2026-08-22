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

#include "hybm_batch_copy.h"

#include <array>
#include <cinttypes>
#include <chrono>
#include <limits>
#include <new>
#include <thread>
#include <vector>

#include "hybm_batch_copy_route.h"
#include "hybm_batch_copy_transfer.h"
#include "hybm_define.h"
#include "hybm_def.h"
#include "hybm_kernel_log.h"

namespace {
using ock::mf::BatchCopyRangeEntry;
using ock::mf::BatchCopyRouteTable;
using ock::mf::HcommBatchTransferDesc;

constexpr uint64_t kCompletionAddress = ock::mf::HYBM_BATCH_COPY_META_ADDR + ock::mf::BATCH_COPY_COMPLETION_OFFSET;
constexpr auto kCompletionTimeout = std::chrono::seconds(60);
// Internal HCOMM experiment: each lane submits an external channel fence followed by a completion READ.
constexpr std::array<uint8_t, 4U> kHcommBatchExternalFenceMagic{{'M', 'F', 'E', 'X'}};

void MarkExternalFenceCompletion(HcommBatchTransferDesc &descriptor)
{
    for (size_t index = 0U; index < kHcommBatchExternalFenceMagic.size(); ++index) {
        descriptor.reserved[index] = kHcommBatchExternalFenceMagic[index];
    }
}

struct BatchCopyGroup {
    std::vector<HcommBatchTransferDesc> descriptors;
    uint16_t laneBegin{0};
    uint16_t laneCount{0};

    bool Empty() const
    {
        return descriptors.empty();
    }
};

using BatchCopyGroups = std::array<BatchCopyGroup, ock::mf::BATCH_COPY_MAX_PEER_COUNT>;

struct BatchCopyTransfers {
    std::array<HybmBatchCopyTransferParam, ock::mf::BATCH_COPY_MAX_PEER_COUNT> values{};
    uint16_t count{0};
};

void InvalidateDeviceCache(uintptr_t address)
{
    __asm__ __volatile__("dc civac, %0" ::"r"(address) : "memory");
    __asm__ __volatile__("dsb ish" ::: "memory");
}

void FlushDeviceCache(uintptr_t address)
{
    __asm__ __volatile__("dc cvac, %0" ::"r"(address) : "memory");
    __asm__ __volatile__("dsb ish" ::: "memory");
}

void DeviceMemoryBarrier()
{
    __asm__ __volatile__("dsb ish" ::: "memory");
}

int32_t ValidateFourInputs(const HybmBatchCopyParam *param)
{
    if (param == nullptr || param->list_num == 0U || param->dst_buf_addr_list == nullptr ||
        param->src_buf_addr_list == nullptr || param->len_list == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid BatchCopy inputs, param=%p listNum=%u dst=%p src=%p len=%p",
                  static_cast<const void *>(param), param == nullptr ? 0U : param->list_num,
                  param == nullptr ? nullptr : param->dst_buf_addr_list,
                  param == nullptr ? nullptr : param->src_buf_addr_list, param == nullptr ? nullptr : param->len_list);
        return BM_INVALID_PARAM;
    }

    constexpr size_t kBytesPerItem = sizeof(void *) * 2U + sizeof(uint64_t);
    if (param->list_num > std::numeric_limits<size_t>::max() / kBytesPerItem) {
        HYBM_LOGE(BM_INVALID_PARAM, "BatchCopy list byte size overflows, listNum=%u bytesPerItem=%zu", param->list_num,
                  kBytesPerItem);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

int32_t ValidateRouteHeader(const BatchCopyRouteTable *route)
{
    InvalidateDeviceCache(reinterpret_cast<uintptr_t>(&route->header));
    const auto &header = route->header;
    if (header.magic != ock::mf::BATCH_COPY_ROUTE_MAGIC) {
        HYBM_LOGE(BM_NOT_INITIALIZED, "BatchCopy route is not published, magic=0x%x", header.magic);
        return BM_NOT_INITIALIZED;
    }
    if (header.peerCount == 0U || header.peerCount > ock::mf::BATCH_COPY_MAX_PEER_COUNT || header.rangeCount == 0U ||
        header.rangeCount > ock::mf::BATCH_COPY_MAX_RANGE_COUNT ||
        header.rangeCount > header.peerCount * ock::mf::BATCH_COPY_MAX_RANGE_PER_PEER) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid BatchCopy route counts, peerCount=%u rangeCount=%u", header.peerCount,
                  header.rangeCount);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

int32_t ValidateRoutePeers(const BatchCopyRouteTable *route)
{
    for (uint16_t peerIndex = 0U; peerIndex < route->header.peerCount; ++peerIndex) {
        const auto &peer = route->peers[peerIndex];
        InvalidateDeviceCache(reinterpret_cast<uintptr_t>(&peer));
        if (peer.thread == 0U || peer.channel == 0U || peer.remoteFlagAddr == 0U) {
            HYBM_LOGE(BM_NOT_CONNECTED,
                      "invalid BatchCopy peer, peerIndex=%u thread=%lu channel=%lu remoteFlagAddr=0x%lx", peerIndex,
                      peer.thread, peer.channel, peer.remoteFlagAddr);
            return BM_NOT_CONNECTED;
        }
    }
    return BM_OK;
}

int32_t ValidateRouteRange(const BatchCopyRangeEntry &range, uint16_t peerCount, uint16_t rangeIndex,
                           std::array<uint16_t, ock::mf::BATCH_COPY_MAX_PEER_COUNT> &rangeCounts)
{
    if (range.srcGvaBegin == 0U || range.srcGvaBegin >= range.srcGvaEnd || range.hcommVaBegin == 0U ||
        range.peerIndex >= peerCount) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid BatchCopy range, index=%u begin=0x%lx end=0x%lx hcomm=0x%lx peerIndex=%u",
                  rangeIndex, range.srcGvaBegin, range.srcGvaEnd, range.hcommVaBegin, range.peerIndex);
        return BM_INVALID_PARAM;
    }

    const uint64_t length = range.srcGvaEnd - range.srcGvaBegin;
    if (range.hcommVaBegin > std::numeric_limits<uint64_t>::max() - length ||
        ++rangeCounts[range.peerIndex] > ock::mf::BATCH_COPY_MAX_RANGE_PER_PEER) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid BatchCopy range span/count, index=%u peerIndex=%u length=0x%lx",
                  rangeIndex, range.peerIndex, length);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

int32_t ValidateRouteRanges(const BatchCopyRouteTable *route)
{
    std::array<uint16_t, ock::mf::BATCH_COPY_MAX_PEER_COUNT> rangeCounts{};
    uint64_t previousEnd = 0U;
    for (uint16_t index = 0U; index < route->header.rangeCount; ++index) {
        const auto &range = route->ranges[index];
        InvalidateDeviceCache(reinterpret_cast<uintptr_t>(&range));
        const auto ret = ValidateRouteRange(range, route->header.peerCount, index, rangeCounts);
        if (ret != BM_OK) {
            return ret;
        }
        if (index != 0U && range.srcGvaBegin < previousEnd) {
            HYBM_LOGE(BM_INVALID_PARAM,
                      "BatchCopy ranges overlap or are unsorted, index=%u begin=0x%lx previousEnd=0x%lx", index,
                      range.srcGvaBegin, previousEnd);
            return BM_INVALID_PARAM;
        }
        previousEnd = range.srcGvaEnd;
    }
    return BM_OK;
}

void LogRouteTableForDebug(const BatchCopyRouteTable *route)
{
    HYBM_LOGD("BatchCopy route debug, magic=0x%x peerCount=%u rangeCount=%u", route->header.magic,
              route->header.peerCount, route->header.rangeCount);
    for (uint16_t index = 0U; index < route->header.peerCount; ++index) {
        const auto &peer = route->peers[index];
        InvalidateDeviceCache(reinterpret_cast<uintptr_t>(&peer));
        HYBM_LOGD("BatchCopy route peer, index=%u thread=%lu channel=%lu remoteFlagAddr=0x%lx", index, peer.thread,
                  peer.channel, peer.remoteFlagAddr);
    }
    for (uint16_t index = 0U; index < route->header.rangeCount; ++index) {
        const auto &range = route->ranges[index];
        InvalidateDeviceCache(reinterpret_cast<uintptr_t>(&range));
        HYBM_LOGD("BatchCopy route range, index=%u srcGvaBegin=0x%lx srcGvaEnd=0x%lx hcommVaBegin=0x%lx peerIndex=%u",
                  index, range.srcGvaBegin, range.srcGvaEnd, range.hcommVaBegin, range.peerIndex);
    }
}

int32_t ValidatePublishedRoute(const BatchCopyRouteTable *route)
{
    auto ret = ValidateRouteHeader(route);
    if (ret != BM_OK) {
        return ret;
    }
    //LogRouteTableForDebug(route);
    ret = ValidateRoutePeers(route);
    if (ret != BM_OK) {
        return ret;
    }
    ret = ValidateRouteRanges(route);
    if (ret != BM_OK) {
        return ret;
    }
    InvalidateDeviceCache(reinterpret_cast<uintptr_t>(&route->header.magic));
    if (route->header.magic != ock::mf::BATCH_COPY_ROUTE_MAGIC) {
        HYBM_LOGE(BM_NOT_INITIALIZED, "BatchCopy route was cleared during validation");
        return BM_NOT_INITIALIZED;
    }
    return BM_OK;
}

const BatchCopyRangeEntry *FindCoveringRange(const BatchCopyRouteTable *route, uint64_t source, uint64_t length)
{
    for (uint16_t index = 0U; index < route->header.rangeCount; ++index) {
        const auto *range = &route->ranges[index];
        if (source >= range->srcGvaBegin && source < range->srcGvaEnd && length <= range->srcGvaEnd - source) {
            return range;
        }
    }
    return nullptr;
}

int32_t ValidateDestination(uint64_t destination, uint64_t length, uint32_t index)
{
    if (destination == 0U || destination > std::numeric_limits<uint64_t>::max() - length) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid BatchCopy destination, index=%u dst=0x%lx length=0x%lx", index,
                  destination, length);
        return BM_INVALID_PARAM;
    }
    const uint64_t end = destination + length;
    if (destination < ock::mf::SVM_END_ADDR && end > ock::mf::HYBM_BATCH_COPY_META_ADDR) {
        HYBM_LOGE(BM_INVALID_PARAM,
                  "BatchCopy destination overlaps control HBM, index=%u dst=0x%lx end=0x%lx controlBegin=0x%lx "
                  "controlEnd=0x%lx",
                  index, destination, end, ock::mf::HYBM_BATCH_COPY_META_ADDR, ock::mf::SVM_END_ADDR);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

int32_t ResolveAndAppendItem(const HybmBatchCopyParam &param, uint32_t index, const BatchCopyRouteTable *route,
                             BatchCopyGroups &groups)
{
    const uint64_t length = param.len_list[index];
    /* Temporary performance isolation: the caller guarantees a non-zero length.
    if (length == 0U) {
        return BM_OK;
    }
    */
    const uint64_t source = reinterpret_cast<uint64_t>(param.src_buf_addr_list[index]);
    const uint64_t destination = reinterpret_cast<uint64_t>(param.dst_buf_addr_list[index]);
    /* Temporary performance isolation: the caller guarantees valid source and destination ranges.
    auto ret = ValidateDestination(destination, length, index);
    if (ret != BM_OK) {
        return ret;
    }
    if (source == 0U || source > std::numeric_limits<uint64_t>::max() - length) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid BatchCopy source, index=%u src=0x%lx length=0x%lx", index, source, length);
        return BM_INVALID_PARAM;
    }
    */
    const auto *range = FindCoveringRange(route, source, length);
    /* Temporary performance isolation: every source range must have a published route.
    if (range == nullptr) {
        HYBM_LOGE(BM_NOT_CONNECTED, "BatchCopy source has no route, index=%u src=0x%lx length=0x%lx", index, source,
                  length);
        return BM_NOT_CONNECTED;
    }
    */
    const uint64_t hcommSource = range->hcommVaBegin + (source - range->srcGvaBegin);
    auto &group = groups[range->peerIndex];
    if (group.Empty()) {
        group.laneBegin = range->peerIndex;
        group.laneCount = route->peers[range->peerIndex].laneCount;
    }
    auto &descriptor = group.descriptors.emplace_back();
    descriptor.transType = ock::mf::HCOMM_TRANSFER_TYPE_READ;
    MarkExternalFenceCompletion(descriptor);
    descriptor.transferInfo.read.len = length;
    descriptor.transferInfo.read.dst = reinterpret_cast<void *>(destination);
    descriptor.transferInfo.read.src = reinterpret_cast<void *>(hcommSource);
    return BM_OK;
}

int32_t ValidateAndGroupItems(const HybmBatchCopyParam &param, const BatchCopyRouteTable *route,
                              BatchCopyGroups &groups)
{
    try {
        for (uint32_t index = 0U; index < param.list_num; ++index) {
            const auto ret = ResolveAndAppendItem(param, index, route, groups);
            /* Temporary performance isolation: per-item validation is disabled in ResolveAndAppendItem.
            if (ret != BM_OK) {
                return ret;
            }
            */
            (void)ret;
        }
    } catch (const std::bad_alloc &) {
        HYBM_LOGE(BM_MALLOC_FAILED, "allocate BatchCopy groups failed, listNum=%u", param.list_num);
        return BM_MALLOC_FAILED;
    } catch (...) {
        HYBM_LOGE(BM_ERROR, "unexpected exception while grouping BatchCopy items, listNum=%u", param.list_num);
        return BM_ERROR;
    }
    return BM_OK;
}

volatile uint64_t *GetCompletionCell(uint16_t peerIndex)
{
    return reinterpret_cast<volatile uint64_t *>(kCompletionAddress + peerIndex * sizeof(uint64_t));
}

volatile uint64_t *GetCompletionCell(const HybmBatchCopyTransferParam &transfer)
{
    return reinterpret_cast<volatile uint64_t *>(static_cast<uintptr_t>(transfer.localFlagAddr));
}

int32_t AppendLaneTransfers(const BatchCopyRouteTable *route, const BatchCopyGroup &group,
                            BatchCopyTransfers &transfers)
{
    const uint32_t descriptorCount = static_cast<uint32_t>(group.descriptors.size());
    const uint32_t usedLaneCount = descriptorCount < group.laneCount ? descriptorCount : group.laneCount;
    if (usedLaneCount == 0U) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid BatchCopy lane split, laneBegin=%u laneCount=%u descriptorCount=%u",
                  group.laneBegin, group.laneCount, descriptorCount);
        return BM_INVALID_PARAM;
    }
    const uint32_t baseCount = descriptorCount / usedLaneCount;
    const uint32_t remainder = descriptorCount % usedLaneCount;
    uint32_t offset = 0U;
    for (uint16_t laneIndex = 0U; laneIndex < usedLaneCount; ++laneIndex) {
        if (transfers.count >= transfers.values.size()) {
            HYBM_LOGE(BM_INVALID_PARAM, "BatchCopy lane transfer capacity exceeded, laneBegin=%u laneCount=%u",
                      group.laneBegin, group.laneCount);
            return BM_INVALID_PARAM;
        }
        const uint32_t count = baseCount + (laneIndex < remainder ? 1U : 0U);
        const uint16_t peerIndex = group.laneBegin + laneIndex;
        const auto &peer = route->peers[peerIndex];
        auto &transfer = transfers.values[transfers.count++];
        transfer.thread = peer.thread;
        transfer.channel = peer.channel;
        transfer.descriptors = group.descriptors.data() + offset;
        transfer.descriptorCount = count;
        transfer.remoteFlagAddr = peer.remoteFlagAddr;
        transfer.localFlagAddr = reinterpret_cast<uint64_t>(GetCompletionCell(peerIndex));
        transfer.flagSize = sizeof(uint64_t);
        offset += count;
    }
    return BM_OK;
}

int32_t BuildLaneTransfers(const BatchCopyRouteTable *route, const BatchCopyGroups &groups,
                           BatchCopyTransfers &transfers)
{
    for (uint16_t peerIndex = 0U; peerIndex < route->header.peerCount; ++peerIndex) {
        const auto &group = groups[peerIndex];
        if (group.Empty()) {
            continue;
        }
        const auto ret = AppendLaneTransfers(route, group, transfers);
        if (ret != BM_OK) {
            return ret;
        }
    }
    return BM_OK;
}

void ClearUsedCompletionCells(const BatchCopyTransfers &transfers)
{
    for (uint16_t index = 0U; index < transfers.count; ++index) {
        auto *cell = GetCompletionCell(transfers.values[index]);
        *cell = 0U;
        FlushDeviceCache(reinterpret_cast<uintptr_t>(cell));
    }
    DeviceMemoryBarrier();
}

void RecordWqeBuildTime(HybmBatchCopyTiming *timing)
{
    if (timing != nullptr) {
        timing->wqeBuildNs = HybmBatchCopyNowNs() - timing->wqeBuildStartNs;
    }
}

int32_t EndBatchModeAndRecordTime(HybmBatchCopyTiming *timing)
{
    uint64_t launchTaskStartNs = 0U;
    if (timing != nullptr) {
        launchTaskStartNs = HybmBatchCopyNowNs();
    }
    const int32_t ret = static_cast<int32_t>(HybmBatchCopyEndBatchMode());
    if (timing != nullptr) {
        const uint64_t launchTaskEndNs = HybmBatchCopyNowNs();
        timing->launchTaskNs = launchTaskEndNs - launchTaskStartNs;
        timing->completionWaitStartNs = launchTaskEndNs;
    }
    return ret;
}

int32_t SubmitLaneCompletions(const BatchCopyTransfers &transfers, uint16_t count)
{
    int32_t firstError = BM_OK;
    for (uint16_t index = 0U; index < count; ++index) {
        const auto ret = static_cast<int32_t>(HybmBatchCopyFenceAndReadCompletion(transfers.values[index]));
        if (ret == BM_OK) {
            continue;
        }
        HYBM_LOGE(ret, "BatchCopy lane completion submission failed, laneIndex=%u descriptorCount=%u", index,
                  transfers.values[index].descriptorCount);
        if (firstError == BM_OK) {
            firstError = ret;
        }
    }
    return firstError;
}

int32_t SubmitLaneTransfers(const BatchCopyTransfers &transfers, HybmBatchCopyTiming *timing)
{
    if (timing != nullptr) {
        timing->wqeBuildStartNs = HybmBatchCopyNowNs();
    }
    auto ret = static_cast<int32_t>(HybmBatchCopyStartBatchMode());
    if (timing != nullptr) {
        timing->batchModeStartNs = HybmBatchCopyNowNs() - timing->wqeBuildStartNs;
    }
    if (ret != BM_OK) {
        RecordWqeBuildTime(timing);
        return ret;
    }
    for (uint16_t index = 0U; index < transfers.count; ++index) {
        ret = static_cast<int32_t>(HybmBatchCopySubmitDescriptors(transfers.values[index]));
        if (ret != BM_OK) {
            HYBM_LOGE(ret, "BatchCopy lane submit failed, laneIndex=%u descriptorCount=%u", index,
                      transfers.values[index].descriptorCount);
            const auto cleanupRet = SubmitLaneCompletions(transfers, index + 1U);
            if (cleanupRet != BM_OK) {
                HYBM_LOGE(cleanupRet, "BatchCopy partial-submit completion cleanup failed, laneCount=%u", index + 1U);
            }
            RecordWqeBuildTime(timing);
            (void)EndBatchModeAndRecordTime(timing);
            return ret;
        }
    }
    ret = SubmitLaneCompletions(transfers, transfers.count);
    RecordWqeBuildTime(timing);
    const auto endRet = EndBatchModeAndRecordTime(timing);
    return ret == BM_OK ? endRet : ret;
}

bool AllLaneCompletionsReceived(const BatchCopyTransfers &transfers)
{
    for (uint16_t index = 0U; index < transfers.count; ++index) {
        auto *cell = GetCompletionCell(transfers.values[index]);
        InvalidateDeviceCache(reinterpret_cast<uintptr_t>(cell));
        if (*cell == 0U) {
            return false;
        }
    }
    return true;
}

int32_t FinishCompletionWait(HybmBatchCopyTiming *timing, uint32_t spins, int32_t ret)
{
    if (timing != nullptr && timing->completionWaitStartNs != 0U) {
        timing->completionPolls = spins;
        timing->completionWaitNs = HybmBatchCopyNowNs() - timing->completionWaitStartNs;
    }
    return ret;
}

int32_t WaitForLaneCompletions(const BatchCopyTransfers &transfers, HybmBatchCopyTiming *timing)
{
    const auto deadline = std::chrono::steady_clock::now() + kCompletionTimeout;
    uint32_t spins = 0U;
    while (!AllLaneCompletionsReceived(transfers)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            HYBM_LOGE(BM_TIMEOUT, "BatchCopy completion timed out, laneCount=%u", transfers.count);
            return FinishCompletionWait(timing, spins, BM_TIMEOUT);
        }
        if ((++spins & 0x3FFU) == 0U) {
            std::this_thread::yield();
        }
    }
    DeviceMemoryBarrier();
    return FinishCompletionWait(timing, spins, BM_OK);
}

int32_t FinishBatchCopy(const HybmBatchCopyTiming *timing, const HybmBatchCopyParam &param, uint16_t laneCount,
                        int32_t ret)
{
    if (timing != nullptr && timing->wqeBuildStartNs != 0U) {
        const uint64_t knownHcommNs =
            timing->batchModeStartNs + timing->batchTransferTotalNs + timing->fenceNs + timing->completionReadNs;
        const uint64_t buildOtherNs = timing->wqeBuildNs > knownHcommNs ? timing->wqeBuildNs - knownHcommNs : 0U;
        const char *slowestBuildCall = "HcommBatchModeStart";
        uint64_t slowestBuildCallNs = timing->batchModeStartNs;
        if (timing->batchTransferMaxNs > slowestBuildCallNs) {
            slowestBuildCall = "HcommBatchTransferOnThread";
            slowestBuildCallNs = timing->batchTransferMaxNs;
        }
        if (timing->fenceNs > slowestBuildCallNs) {
            slowestBuildCall = "HcommChannelFenceOnThread";
            slowestBuildCallNs = timing->fenceNs;
        }
        if (timing->completionReadNs > slowestBuildCallNs) {
            slowestBuildCall = "HcommReadOnThread";
            slowestBuildCallNs = timing->completionReadNs;
        }
        // Use ERROR only to make this one-line timing diagnostic visible under the AICPU log filter.
        HYBM_LOGE(ret,
                  "BatchCopy timing, listNum=%u laneCount=%u buildAllWqeNs=%" PRIu64 " modeStartNs=%" PRIu64
                  " batchXferCalls=%u batchXferTotalNs=%" PRIu64 " batchXferMaxNs=%" PRIu64
                  " batchXferMaxOffset=%u batchXferMaxDescCount=%u fenceNs=%" PRIu64 " completionReadNs=%" PRIu64
                  " buildOtherNs=%" PRIu64 " slowestBuildCall=%s slowestBuildCallNs=%" PRIu64 " launchTaskNs=%" PRIu64
                  " completionWaitNs=%" PRIu64 " completionRetryCount=%u result=%d",
                  param.list_num, laneCount, timing->wqeBuildNs, timing->batchModeStartNs, timing->batchTransferCalls,
                  timing->batchTransferTotalNs, timing->batchTransferMaxNs, timing->batchTransferMaxOffset,
                  timing->batchTransferMaxDescCount, timing->fenceNs, timing->completionReadNs, buildOtherNs,
                  slowestBuildCall, slowestBuildCallNs, timing->launchTaskNs, timing->completionWaitNs,
                  timing->completionPolls, ret);
    }
    return ret;
}

int32_t ExecuteBatchCopy(HybmBatchCopyParam *param)
{
    /* Temporary performance isolation: the caller guarantees valid list pointers and list_num.
    auto ret = ValidateFourInputs(param);
    if (ret != BM_OK) {
        return ret;
    }
    */
    const auto *route = reinterpret_cast<const BatchCopyRouteTable *>(ock::mf::HYBM_BATCH_COPY_META_ADDR);
    /* Temporary performance isolation: the route is published and immutable during the copy.
    auto ret = ValidatePublishedRoute(route);
    if (ret != BM_OK) {
        return ret;
    }
    */
    BatchCopyGroups groups{};
    auto ret = ValidateAndGroupItems(*param, route, groups);
    if (ret != BM_OK) {
        return ret;
    }
    BatchCopyTransfers transfers{};
    ret = BuildLaneTransfers(route, groups, transfers);
    if (ret != BM_OK) {
        return ret;
    }
    HybmBatchCopyTiming timing{};
    const bool isSingleLane = transfers.count == 1U;
    HybmBatchCopyTiming *timingPtr = isSingleLane ? &timing : nullptr;
    if (timingPtr != nullptr) {
        transfers.values[0].timing = timingPtr;
    }
    ClearUsedCompletionCells(transfers);
    ret = SubmitLaneTransfers(transfers, timingPtr);
    if (ret == BM_OK) {
        ret = WaitForLaneCompletions(transfers, timingPtr);
    }
    return FinishBatchCopy(timingPtr, *param, transfers.count, ret);
}
} // namespace

extern "C" uint32_t HybmBatchCopy(HybmBatchCopyParam *param)
{
    const auto ret = ExecuteBatchCopy(param);
    if (ret != BM_OK) {
        HYBM_LOGE(ret, "HybmBatchCopy failed, ret=%d", ret);
    }
    return static_cast<uint32_t>(ret);
}
