/* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#include "hybm_sdma_device_copy.h"
#include "hybm_def.h"
#include "hybm_kernel_log.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <dlfcn.h>

namespace {
struct SqCqInfo {
    uint32_t type, tsId, sqId, cqId, prop;
    uint32_t value[8];
};
using SqCqFunction = int (*)(uint32_t, SqCqInfo *);
struct SdmaHal {
    void *handle;
    SqCqFunction query;
    SqCqFunction config;
    SdmaHal() : handle(dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL)), query(nullptr), config(nullptr)
    {
        if (handle != nullptr) {
            query = reinterpret_cast<SqCqFunction>(dlsym(handle, "halSqCqQuery"));
            config = reinterpret_cast<SqCqFunction>(dlsym(handle, "halSqCqConfig"));
        }
    }
};
SdmaHal &Hal()
{
    static SdmaHal hal;
    return hal;
}
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

void CacheRange(const void *pointer, size_t bytes, bool invalidate)
{
#if defined(__aarch64__)
    uint64_t ctr = 0;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t lineBytes = 4ULL << ((ctr >> 16U) & 0xfU);
    const auto begin = reinterpret_cast<uintptr_t>(pointer);
    for (uintptr_t line = begin & ~(lineBytes - 1U); line < begin + bytes; line += lineBytes) {
        if (invalidate) {
            __asm__ __volatile__("dc civac, %0" ::"r"(line) : "memory");
        } else {
            __asm__ __volatile__("dc cvac, %0" ::"r"(line) : "memory");
        }
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    (void)pointer;
    (void)bytes;
    (void)invalidate;
#endif
}

int32_t WaitCompletion(HybmSdmaDeviceCopyContext &ctx, Deadline deadline)
{
    for (;;) {
        CacheRange(&ctx.completed, sizeof(ctx.completed), true);
        if (*reinterpret_cast<volatile uint64_t *>(&ctx.completed) == ctx.sequence) {
            return BM_OK;
        }
        if (Clock::now() >= deadline) {
            HYBM_LOGE(BM_TIMEOUT, "SDMA completion timeout, sq=%u sequence=%lu", ctx.channel.sqId, ctx.sequence);
            return BM_TIMEOUT;
        }
    }
}

int32_t Submit(HybmSdmaDeviceCopyContext &ctx, uint32_t count, bool last, Deadline deadline)
{
    auto &ch = ctx.channel;
    ctx.sentinel = ++ctx.sequence;
    CacheRange(&ctx.sentinel, sizeof(ctx.sentinel), false);
    ctx.staging[count++] = HybmSdmaMemcpySqe(ch.streamId, reinterpret_cast<uint64_t>(&ctx.sentinel),
                                             reinterpret_cast<uint64_t>(&ctx.completed), sizeof(ctx.completed));
    if (last) {
        ctx.staging[count++] = HybmSdmaNotifySqe(ch.streamId, ctx.notifyId);
    }
    SqCqInfo info{};
    info.sqId = ch.sqId;
    info.prop = 1;
    for (;;) {
        auto ret = Hal().query(ch.devId, &info);
        if (ret != 0 || info.value[0] >= ch.sqDepth) {
            HYBM_LOGE(BM_ERROR, "SDMA SQ head query failed, sq=%u ret=%d head=%u", ch.sqId, ret, info.value[0]);
            return BM_ERROR;
        }
        const auto freeSlots = (info.value[0] + ch.sqDepth - ch.sqTail - 1) % ch.sqDepth;
        if (freeSlots >= count) {
            break;
        }
        if (Clock::now() >= deadline) {
            HYBM_LOGE(BM_TIMEOUT, "SDMA SQ full, sq=%u head=%u tail=%u", ch.sqId, info.value[0], ch.sqTail);
            return BM_TIMEOUT;
        }
    }
    auto *sq = reinterpret_cast<HybmSdmaSqe *>(ch.sqBase);
    for (uint32_t i = 0; i < count; ++i) {
        auto slot = (ch.sqTail + i) % ch.sqDepth;
        ctx.staging[i].words[1] = ch.streamId | (slot << 16U);
        sq[slot] = ctx.staging[i];
    }
    const auto first = std::min(count, ch.sqDepth - ch.sqTail);
    CacheRange(sq + ch.sqTail, first * sizeof(*sq), false);
    if (first < count) {
        CacheRange(sq, (count - first) * sizeof(*sq), false);
    }
    info = {};
    info.sqId = ch.sqId;
    info.prop = 2;
    info.value[0] = (ch.sqTail + count) % ch.sqDepth;
    auto ret = Hal().config(ch.devId, &info);
    if (ret != 0) {
        HYBM_LOGE(BM_ERROR, "SDMA doorbell failed, sq=%u tail=%u ret=%d", ch.sqId, info.value[0], ret);
        return BM_ERROR;
    }
    ch.sqTail = info.value[0];
    return last ? BM_OK : WaitCompletion(ctx, deadline);
}

int32_t Transfer(const HybmSdmaDeviceCopyArgs &args, Deadline deadline)
{
    auto &ctx = *args.context;
    CacheRange(args.count, sizeof(*args.count), true);
    const auto count = *args.count;
    if (count > HYBM_SDMA_MAX_RECORDS) {
        HYBM_LOGE(BM_INVALID_PARAM, "SDMA count=%u exceeds limit=%u", count, HYBM_SDMA_MAX_RECORDS);
        return BM_INVALID_PARAM;
    }
    if (count == 0) {
        // An empty valid call still releases the user-stream wait.
        return Submit(ctx, 0, true, deadline);
    }
    CacheRange(args.srcPtrs, count * sizeof(*args.srcPtrs), true);
    CacheRange(args.dstPtrs, count * sizeof(*args.dstPtrs), true);
    CacheRange(args.lengths, count * sizeof(*args.lengths), true);

    uint32_t pending = 0;
    const auto capacity = std::min(HYBM_SDMA_BATCH_SIZE, ctx.channel.sqDepth - 3);
    const auto poolBase = ctx.poolBase;
    const auto poolSize = ctx.poolSize;
    const auto devicePoolBase = ctx.devicePoolBase;
    for (uint32_t i = 0; i < count; ++i) {
        auto remaining = args.lengths[i];
        if (remaining == 0) {
            continue;
        }
        auto src = args.srcPtrs[i];
        auto dst = args.dstPtrs[i];
        const auto srcOffset = src - poolBase;
        const auto dstOffset = dst - poolBase;
        const bool srcHost = srcOffset < poolSize;
        const bool dstHost = dstOffset < poolSize;
        const auto hostOffset = srcHost ? srcOffset : dstOffset;
        const auto device = srcHost ? dst : src;
        if (srcHost == dstHost || device == 0 || remaining > poolSize - hostOffset) {
            HYBM_LOGE(BM_INVALID_PARAM, "invalid SDMA record, index=%u src=%lu dst=%lu len=%u", i, src, dst, remaining);
            return BM_INVALID_PARAM;
        }
        auto &host = srcHost ? src : dst;
        host = devicePoolBase + hostOffset;
        while (remaining != 0) {
            if (pending == capacity) {
                const auto ret = Submit(ctx, pending, false, deadline);
                if (ret != BM_OK) {
                    return ret;
                }
                pending = 0;
            }
            const auto bytes = std::min(remaining, HYBM_SDMA_MAX_COPY_BYTES);
            ctx.staging[pending++] = HybmSdmaMemcpySqe(ctx.channel.streamId, src, dst, bytes);
            src += bytes;
            dst += bytes;
            remaining -= bytes;
        }
    }
    return Submit(ctx, pending, true, deadline);
}
} // namespace

extern "C" int32_t HybmSdmaDeviceCopy(HybmSdmaDeviceCopyArgs *args)
{
    if (args == nullptr || args->context == nullptr || args->srcPtrs == nullptr || args->dstPtrs == nullptr ||
        args->lengths == nullptr || args->count == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "missing SDMA kernel context or metadata pointers");
        return BM_INVALID_PARAM;
    }
    auto &ctx = *args->context;
    CacheRange(&ctx, offsetof(HybmSdmaDeviceCopyContext, completed), true);
    if (ctx.faulted != 0) {
        HYBM_LOGE(BM_ERROR, "SDMA context faulted, sq=%u", ctx.channel.sqId);
        return BM_ERROR;
    }
    if (Hal().query == nullptr || Hal().config == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "SDMA device HAL unavailable");
        return BM_DL_FUNCTION_FAILED;
    }
    const auto deadline = Clock::now() + std::chrono::seconds(55);
    const auto ret = Transfer(*args, deadline);
    if (ret != BM_OK && ret != BM_INVALID_PARAM) {
        ctx.faulted = 1;
    }
    CacheRange(&ctx, offsetof(HybmSdmaDeviceCopyContext, completed), false);
    return ret;
}
