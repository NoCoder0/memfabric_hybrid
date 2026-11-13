/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "hybm_urma_device_copy.h"
#include "hybm_batch_transfer.h"
#include "hybm_def.h"
#include "hybm_kernel_log.h"
#include <algorithm>

namespace {
int32_t BuildUrmaDeviceCopyTile(const HybmUrmaDeviceCopyArgs &args, const HybmUrmaDeviceCopyContext &ctx,
                                uint32_t count, uint32_t &used)
{
    used = 0;
    if (args.startIndex >= count) {
        return BM_OK;
    }
    const uint32_t tileCount = std::min(ctx.capacity, count - args.startIndex);
    HybmDeviceCopyCacheRange(args.srcPtrs + args.startIndex, tileCount * sizeof(*args.srcPtrs), true);
    HybmDeviceCopyCacheRange(args.dstPtrs + args.startIndex, tileCount * sizeof(*args.dstPtrs), true);
    HybmDeviceCopyCacheRange(args.lengths + args.startIndex, tileCount * sizeof(*args.lengths), true);

    const auto poolBase = ctx.poolBase;
    const auto poolSize = ctx.poolSize;
    const auto importedBase = ctx.importedBase;
    for (uint32_t i = args.startIndex; i < args.startIndex + tileCount; ++i) {
        const auto length = args.lengths[i];
        if (length == 0) {
            continue;
        }
        const auto src = args.srcPtrs[i];
        const auto dst = args.dstPtrs[i];
        const auto srcOffset = src - poolBase;
        const auto dstOffset = dst - poolBase;
        const bool srcHost = srcOffset < poolSize;
        const bool dstHost = dstOffset < poolSize;
        const auto hostOffset = srcHost ? srcOffset : dstOffset;
        const auto device = srcHost ? dst : src;
        if (srcHost == dstHost || device == 0 || length > poolSize - hostOffset) {
            HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid URMA device copy record, index=%u src=%lu dst=%lu len=%u", i,
                      src, dst, length);
            return BM_INVALID_PARAM;
        }
        auto &desc = ctx.descriptors[used++];
        desc = {};
        auto *remotePtr = reinterpret_cast<void *>(importedBase + hostOffset);
        auto *localPtr = reinterpret_cast<void *>(device);
        desc.transType = srcHost ? ock::mf::HCOMM_TRANSFER_TYPE_READ : ock::mf::HCOMM_TRANSFER_TYPE_WRITE;
        if (srcHost) {
            desc.transferInfo.read = {length, localPtr, remotePtr};
        } else {
            desc.transferInfo.write = {length, remotePtr, localPtr};
        }
    }
    if (used != 0) {
        HybmDeviceCopyCacheRange(ctx.descriptors, used * sizeof(*ctx.descriptors), false);
    }
    return BM_OK;
}

int32_t CheckUrmaDeviceCopyContext(const HybmUrmaDeviceCopyContext &ctx)
{
    if (ctx.capacity == 0 || ctx.capacity > ock::mf::HCOMM_BATCH_TRANSFER_MAX_DESC_NUM || ctx.maxRecords == 0 ||
        ctx.maxRecords > HYBM_DEVICE_COPY_MAX_RECORDS || ctx.recordCount == nullptr || ctx.thread == 0 ||
        ctx.channel == 0 || ctx.descriptors == nullptr || ctx.poolSize == 0 ||
        ctx.marker.transType != ock::mf::HCOMM_TRANSFER_TYPE_READ || ctx.marker.transferInfo.read.len == 0 ||
        ctx.marker.transferInfo.read.src == nullptr || ctx.marker.transferInfo.read.dst == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid sparse copy context, rankId=%u capacity=%u", ctx.rankId,
                  ctx.capacity);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}
} // namespace

extern "C" int32_t HybmUrmaDeviceCopy(HybmUrmaDeviceCopyArgs *args)
{
    if (args == nullptr || args->context == nullptr || args->srcPtrs == nullptr || args->dstPtrs == nullptr ||
        args->lengths == nullptr || args->count == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid sparse copy context or device metadata pointers");
        return BM_INVALID_PARAM;
    }
    HybmDeviceCopyCacheRange(args->context, sizeof(*args->context), true);
    auto ctx = *args->context;
    auto ret = CheckUrmaDeviceCopyContext(ctx);
    if (ret != BM_OK) {
        return ret;
    }
    if (args->startIndex >= ctx.maxRecords) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid sparse copy startIndex=%u maxRecords=%u", args->startIndex,
                  ctx.maxRecords);
        return BM_INVALID_PARAM;
    }
    uint32_t count = 0;
    if (args->startIndex == 0) {
        HybmDeviceCopyCacheRange(args->count, sizeof(*args->count), true);
        count = *args->count;
        *ctx.recordCount = count <= ctx.maxRecords ? count : 0;
        HybmDeviceCopyCacheRange(ctx.recordCount, sizeof(*ctx.recordCount), false);
        if (count > ctx.maxRecords) {
            HYBM_LOGE(BM_INVALID_PARAM, "[hybm] device copy count=%u exceeds maxRecords=%u", count, ctx.maxRecords);
            return BM_INVALID_PARAM;
        }
    } else {
        HybmDeviceCopyCacheRange(ctx.recordCount, sizeof(*ctx.recordCount), true);
        count = *ctx.recordCount;
    }
    uint32_t used = 0;
    ret = BuildUrmaDeviceCopyTile(*args, ctx, count, used);
    if (ret != BM_OK) {
        *ctx.recordCount = 0;
        HybmDeviceCopyCacheRange(ctx.recordCount, sizeof(*ctx.recordCount), false);
        return ret;
    }
    uint32_t start = 0;
    HybmBatchTransferParam batch{1,    &ctx.rankId,     &start,     &used, &ctx.thread, &ctx.channel,
                                 used, ctx.descriptors, &ctx.marker};
    // Dynamic inputs must not reuse a batch cached under a fixed tag.
    return HybmSubmitBatchTransfer(&batch, "");
}
