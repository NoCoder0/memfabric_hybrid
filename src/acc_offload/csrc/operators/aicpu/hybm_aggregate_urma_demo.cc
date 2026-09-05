/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include "hybm_aggregate_urma_demo.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "hybm_batch_copy_route.h"
#include "hybm_batch_transfer.h"
#include "hybm_def.h"
#include "hybm_define.h"
#include "hybm_kernel_log.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr uintptr_t kCacheLineBytes = 64U;
constexpr uint32_t kBenchmarkIterations = 20U;
constexpr uint32_t kWarmupIterations = 5U;
constexpr uint32_t kMeasuredIterations = kBenchmarkIterations - kWarmupIterations;

static_assert(kMeasuredIterations > 0U, "benchmark needs measured samples");

uint64_t ElapsedNs(Clock::time_point begin, Clock::time_point end)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

void InvalidateDeviceCache(uintptr_t address)
{
    __asm__ __volatile__("dc civac, %0" : : "r"(address) : "memory");
    __asm__ __volatile__("dsb ish" : : : "memory");
}

void FlushDeviceCache(uintptr_t address)
{
    __asm__ __volatile__("dc cvac, %0" : : "r"(address) : "memory");
    __asm__ __volatile__("dsb ish" : : : "memory");
}

void FlushDeviceCacheRange(uintptr_t address, uint64_t bytes)
{
    const uintptr_t end = address + bytes;
    for (uintptr_t line = address & ~(kCacheLineBytes - 1U); line < end; line += kCacheLineBytes) {
        __asm__ __volatile__("dc cvac, %0" : : "r"(line) : "memory");
    }
}

void InvalidateDeviceCacheRange(uintptr_t address, uint64_t bytes)
{
    const uintptr_t end = address + bytes;
    for (uintptr_t line = address & ~(kCacheLineBytes - 1U); line < end; line += kCacheLineBytes) {
        __asm__ __volatile__("dc civac, %0" : : "r"(line) : "memory");
    }
    __asm__ __volatile__("dsb ish" : : : "memory");
}

const ock::mf::BatchCopyRangeEntry *FindMailboxRange(const ock::mf::BatchCopyRouteTable *route, uint64_t mailbox)
{
    for (uint16_t index = 0; index < route->header.rangeCount; ++index) {
        const auto *range = &route->ranges[index];
        InvalidateDeviceCache(reinterpret_cast<uintptr_t>(range));
        if (mailbox >= range->srcGvaBegin && mailbox + sizeof(HybmAggregateUrmaDemoMessage) <= range->srcGvaEnd) {
            return range;
        }
    }
    return nullptr;
}

uint32_t WriteRequestAndDoorbell(const ock::mf::BatchCopyPeerEntry &peer, uint64_t remote,
                                 HybmAggregateUrmaDemoMessage *message)
{
    return HybmWriteOrderedPair(peer.thread, peer.channel, reinterpret_cast<void *>(remote), &message->request,
                                sizeof(message->request),
                                reinterpret_cast<void *>(remote + offsetof(HybmAggregateUrmaDemoMessage, doorbell)),
                                &message->doorbell, sizeof(message->doorbell));
}

void WaitForHost(const HybmAggregateUrmaDemoParam &param, uint64_t doorbell)
{
    do {
        InvalidateDeviceCache(reinterpret_cast<uintptr_t>(param.ready));
    } while (*param.ready != doorbell);
}

void StorePercentiles(uint64_t *samples, uint64_t &p50, uint64_t &p99)
{
    std::sort(samples, samples + kMeasuredIterations);
    p50 = samples[kMeasuredIterations / 2U];
    p99 = samples[kMeasuredIterations - 1U];
}

void Scatter(const HybmAggregateUrmaDemoParam &param)
{
    const auto &request = param.message->request;
    InvalidateDeviceCacheRange(reinterpret_cast<uintptr_t>(param.dstNew), request.totalBytes);
    for (uint32_t index = 0; index < request.segmentCount; ++index) {
        auto *destination = param.dstBase + index * request.dstStride;
        std::memcpy(destination, param.dstNew + index * request.segmentBytes, request.segmentBytes);
        //FlushDeviceCacheRange(reinterpret_cast<uintptr_t>(destination), request.segmentBytes);
    }
    __asm__ __volatile__("dsb ish" : : : "memory");
}
} // namespace

extern "C" uint32_t HybmAggregateUrmaDemo(HybmAggregateUrmaDemoParam *param)
{
    const auto *route = reinterpret_cast<const ock::mf::BatchCopyRouteTable *>(ock::mf::HYBM_BATCH_COPY_META_ADDR);
    InvalidateDeviceCache(reinterpret_cast<uintptr_t>(&route->header));
    const auto *range = FindMailboxRange(route, param->message->request.hostMailboxGva);
    if (range == nullptr) {
        HYBM_LOGE(BM_NOT_CONNECTED, "aggregate demo mailbox has no route, gva=0x%lx",
                  param->message->request.hostMailboxGva);
        return BM_NOT_CONNECTED;
    }
    auto &message = *param->message;
    const auto &peer = route->peers[range->peerIndex];
    InvalidateDeviceCache(reinterpret_cast<uintptr_t>(&peer));
    const uint64_t remote = range->hcommVaBegin + param->message->request.hostMailboxGva - range->srcGvaBegin;
    const uint64_t firstDoorbell = message.doorbell;
    if (firstDoorbell == 0U) {
        HYBM_LOGE(BM_INVALID_PARAM, "aggregate demo doorbell must be nonzero");
        return BM_INVALID_PARAM;
    }

    uint64_t requestSamples[kMeasuredIterations];
    uint64_t waitHostSamples[kMeasuredIterations];
    uint64_t scatterSamples[kMeasuredIterations];
    uint64_t totalSamples[kMeasuredIterations];
    for (uint32_t iteration = 0; iteration < kBenchmarkIterations; ++iteration) {
        const uint64_t doorbell = firstDoorbell + iteration;
        message.doorbell = doorbell;
        FlushDeviceCache(reinterpret_cast<uintptr_t>(&message.doorbell));

        const auto begin = Clock::now();
        const auto ret = WriteRequestAndDoorbell(peer, remote, &message);
        if (ret != BM_OK) {
            HYBM_LOGE(ret, "aggregate demo request write failed, iteration=%u ret=%u", iteration, ret);
            return ret;
        }
        const auto requested = Clock::now();
        WaitForHost(*param, doorbell);
        const auto ready = Clock::now();
        Scatter(*param);
        const auto done = Clock::now();

        if (iteration >= kWarmupIterations) {
            const uint32_t sample = iteration - kWarmupIterations;
            requestSamples[sample] = ElapsedNs(begin, requested);
            waitHostSamples[sample] = ElapsedNs(requested, ready);
            scatterSamples[sample] = ElapsedNs(ready, done);
            totalSamples[sample] = ElapsedNs(begin, done);
        }
    }
    StorePercentiles(requestSamples, param->timing->requestP50Ns, param->timing->requestP99Ns);
    StorePercentiles(waitHostSamples, param->timing->waitHostP50Ns, param->timing->waitHostP99Ns);
    StorePercentiles(scatterSamples, param->timing->scatterP50Ns, param->timing->scatterP99Ns);
    StorePercentiles(totalSamples, param->timing->totalP50Ns, param->timing->totalP99Ns);
    FlushDeviceCache(reinterpret_cast<uintptr_t>(param->timing));
    return BM_OK;
}
