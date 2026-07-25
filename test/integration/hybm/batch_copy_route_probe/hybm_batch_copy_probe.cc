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

#include "hybm_batch_copy_probe.h"

#include <ctime>
#include <unistd.h>

#include "hybm_batch_copy_route.h"
#include "hybm_batch_transfer.h"
#include "hybm_define.h"
#include "hybm_kernel_log.h"

namespace {
constexpr uint64_t PROBE_TIMEOUT_SECONDS = 60U;
constexpr useconds_t PROBE_POLL_INTERVAL_US = 1000U;

const ock::mf::BatchCopyRangeEntry *FindPeerRange(const ock::mf::BatchCopyRouteTable &table, uint32_t peerIndex,
                                                   uint32_t rangeIndex)
{
    uint32_t peerRangeIndex = 0;
    for (uint32_t index = 0; index < table.header.rangeCount; ++index) {
        const auto &range = table.ranges[index];
        if (range.peerIndex >= table.header.peerCount) {
            HYBM_LOGE(BM_INVALID_PARAM, "invalid route range peer, index=%u peerIndex=%u peerCount=%u", index,
                      range.peerIndex, table.header.peerCount);
            return nullptr;
        }
        if (range.peerIndex != peerIndex) {
            continue;
        }
        if (peerRangeIndex++ == rangeIndex) {
            return &range;
        }
    }
    HYBM_LOGE(BM_INVALID_PARAM, "route range not found, peerIndex=%u rangeIndex=%u rangeCount=%u", peerIndex,
              rangeIndex, table.header.rangeCount);
    return nullptr;
}

uint32_t ValidateRoute(const ock::mf::BatchCopyRouteTable &table, const HybmBatchCopyProbeParam &param,
                       const ock::mf::BatchCopyRangeEntry *&range)
{
    const auto &header = table.header;
    if (header.magic != ock::mf::BATCH_COPY_ROUTE_MAGIC || header.peerCount == 0 ||
        header.peerCount > ock::mf::BATCH_COPY_MAX_PEER_COUNT || header.rangeCount == 0 ||
        header.rangeCount > ock::mf::BATCH_COPY_MAX_RANGE_COUNT || param.peerIndex >= header.peerCount) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid route header, magic=0x%x peerCount=%u rangeCount=%u peerIndex=%u",
                  header.magic, header.peerCount, header.rangeCount, param.peerIndex);
        return BM_INVALID_PARAM;
    }
    const auto &peer = table.peers[param.peerIndex];
    if (peer.thread == 0 || peer.channel == 0 || peer.remoteFlagAddr == 0) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid route peer, peerIndex=%u thread=%lu channel=%lu remoteFlag=0x%lx",
                  param.peerIndex, peer.thread, peer.channel, peer.remoteFlagAddr);
        return BM_INVALID_PARAM;
    }
    range = FindPeerRange(table, param.peerIndex, param.rangeIndex);
    if (range == nullptr) {
        return BM_INVALID_PARAM;
    }
    if (range->srcGvaBegin == 0 || range->srcGvaBegin >= range->srcGvaEnd) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid route range, begin=0x%lx end=0x%lx", range->srcGvaBegin,
                  range->srcGvaEnd);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

uint32_t ValidateCopy(const HybmBatchCopyProbeParam *param, const ock::mf::BatchCopyRangeEntry &range)
{
    if (param == nullptr || param->dstHbm == nullptr || param->length == 0) {
        HYBM_LOGE(BM_INVALID_PARAM, "invalid probe param, param=%p dst=%p length=%lu", (void *)param,
                  param == nullptr ? nullptr : param->dstHbm, param == nullptr ? 0 : param->length);
        return BM_INVALID_PARAM;
    }
    const uint64_t rangeSize = range.srcGvaEnd - range.srcGvaBegin;
    if (param->srcOffset > rangeSize || param->length > rangeSize - param->srcOffset) {
        HYBM_LOGE(BM_INVALID_PARAM, "probe source out of range, offset=%lu length=%lu rangeSize=%lu",
                  param->srcOffset, param->length, rangeSize);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

bool ProbeTimedOut(const timespec &start)
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return true;
    }
    const uint64_t elapsed = static_cast<uint64_t>(now.tv_sec - start.tv_sec);
    return elapsed >= PROBE_TIMEOUT_SECONDS;
}

uint32_t WaitForCompletion(volatile uint64_t *completion, uint32_t peerIndex)
{
    timespec start{};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        HYBM_LOGE(BM_ERROR, "clock_gettime failed before completion wait, peerIndex=%u", peerIndex);
        return BM_ERROR;
    }
    while (*completion == 0) {
        if (ProbeTimedOut(start)) {
            HYBM_LOGE(BM_TIMEOUT, "route probe completion timeout, peerIndex=%u timeoutSeconds=%lu", peerIndex,
                      PROBE_TIMEOUT_SECONDS);
            return BM_TIMEOUT;
        }
        (void)usleep(PROBE_POLL_INTERVAL_US);
    }
    return BM_OK;
}
} // namespace

extern "C" uint32_t HybmBatchCopyProbe(void *args)
{
    auto *param = static_cast<HybmBatchCopyProbeParam *>(args);
    if (param == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "probe args is null");
        return BM_INVALID_PARAM;
    }
    const auto *table =
        reinterpret_cast<const ock::mf::BatchCopyRouteTable *>(
            static_cast<uintptr_t>(ock::mf::HYBM_BATCH_COPY_META_ADDR));
    const ock::mf::BatchCopyRangeEntry *range = nullptr;
    auto ret = ValidateRoute(*table, *param, range);
    if (ret != BM_OK) {
        return ret;
    }
    ret = ValidateCopy(param, *range);
    if (ret != BM_OK) {
        return ret;
    }
    auto *completionArea = reinterpret_cast<ock::mf::BatchCopyCompletionArea *>(
        static_cast<uintptr_t>(ock::mf::HYBM_BATCH_COPY_META_ADDR + ock::mf::BATCH_COPY_COMPLETION_OFFSET));
    volatile uint64_t *completion = &completionArea->cells[param->peerIndex];
    *completion = 0;
    __sync_synchronize();

    const auto &peer = table->peers[param->peerIndex];
    void *dstList[] = {param->dstHbm};
    void *srcList[] = {reinterpret_cast<void *>(range->srcGvaBegin + param->srcOffset)};
    uint64_t lengthList[] = {param->length};
    HybmOneSideOpParam readParam{peer.thread, peer.channel, 1U, dstList, srcList, lengthList, peer.remoteFlagAddr,
                                 reinterpret_cast<uint64_t>(completion), sizeof(uint64_t)};
    ret = HybmBatchRead(&readParam);
    if (ret != BM_OK) {
        HYBM_LOGE(BM_ERROR, "HybmBatchRead failed in route probe, peerIndex=%u rangeIndex=%u ret=%u",
                  param->peerIndex, param->rangeIndex, ret);
        return BM_ERROR;
    }
    return WaitForCompletion(completion, param->peerIndex);
}
