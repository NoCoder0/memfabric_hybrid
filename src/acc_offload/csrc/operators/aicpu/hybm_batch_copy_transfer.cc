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

#include "hybm_batch_copy_transfer.h"

#include <algorithm>
#include <cstdint>

#include "hybm_def.h"
#include "hybm_kernel_log.h"

extern "C" {
__attribute__((weak)) int32_t HcommBatchModeStart(const char *batchTag);
__attribute__((weak)) int32_t HcommBatchModeEnd(const char *batchTag);
__attribute__((weak)) int32_t HcommReadOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel, void *dst,
                                                const void *src, uint64_t len);
__attribute__((weak)) int32_t HcommChannelFenceOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel);
__attribute__((weak)) int32_t HcommBatchTransferOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel,
                                                         const ock::mf::HcommBatchTransferDesc *transferDescs,
                                                         uint32_t transferDescNum);
}

namespace {
constexpr uint32_t kMaxBatchSize = 1000U;
constexpr const char *kBatchTag = "HybmKernel";

bool IsNotSupported(int32_t ret)
{
    return ret == BM_NOT_SUPPORTED || ret == BM_NOT_SUPPORT_FUNC || ret == BM_UNDER_API_UNLOAD;
}

int32_t BatchModeStart()
{
    return HcommBatchModeStart == nullptr ? BM_NOT_SUPPORTED : HcommBatchModeStart(kBatchTag);
}

int32_t BatchModeEnd()
{
    return HcommBatchModeEnd == nullptr ? BM_NOT_SUPPORTED : HcommBatchModeEnd(kBatchTag);
}

int32_t ReadOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel, void *destination,
                     const void *source, uint64_t length)
{
    if (HcommReadOnThread == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return HcommReadOnThread(thread, channel, destination, source, length);
}

int32_t BatchTransferOnThread(const HybmBatchCopyTransferParam &param, uint32_t offset, uint32_t batchSize)
{
    if (HcommBatchTransferOnThread == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return HcommBatchTransferOnThread(param.thread, param.channel, param.descriptors + offset, batchSize);
}

int32_t TransferWithBatch(const HybmBatchCopyTransferParam &param)
{
    uint32_t offset = 0U;
    while (offset < param.descriptorCount) {
        const uint32_t batchSize = std::min(kMaxBatchSize, param.descriptorCount - offset);
        const int32_t ret = BatchTransferOnThread(param, offset, batchSize);
        if (ret != BM_OK) {
            if (IsNotSupported(ret)) {
                return BM_NOT_SUPPORTED;
            }
            HYBM_LOGE(BM_ERROR,
                      "BatchCopy HcommBatchTransferOnThread failed, thread=%lu channel=%lu offset=%u batch=%u ret=%d",
                      param.thread, param.channel, offset, batchSize, ret);
            return ret;
        }
        offset += batchSize;
    }
    return BM_OK;
}

uint32_t TransferWithSingle(const HybmBatchCopyTransferParam &param)
{
    for (uint32_t index = 0U; index < param.descriptorCount; ++index) {
        const auto &item = param.descriptors[index].transferInfo.read;
        const int32_t ret = ReadOnThread(param.thread, param.channel, item.dst, item.src, item.len);
        if (ret != BM_OK) {
            HYBM_LOGE(BM_ERROR, "BatchCopy HcommReadOnThread failed, thread=%lu channel=%lu index=%u ret=%d",
                      param.thread, param.channel, index, ret);
            return BM_ERROR;
        }
    }
    return BM_OK;
}

uint32_t TransferDescriptors(const HybmBatchCopyTransferParam &param)
{
    const int32_t ret = TransferWithBatch(param);
    if (ret == BM_NOT_SUPPORTED) {
        return TransferWithSingle(param);
    }
    return ret == BM_OK ? BM_OK : BM_ERROR;
}

uint32_t FenceAndReadCompletion(const HybmBatchCopyTransferParam &param)
{
    if (HcommChannelFenceOnThread == nullptr) {
        HYBM_LOGE(BM_ERROR, "BatchCopy fence is unavailable, thread=%lu channel=%lu", param.thread, param.channel);
        return BM_ERROR;
    }
    int32_t ret = HcommChannelFenceOnThread(param.thread, param.channel);
    if (ret != BM_OK) {
        HYBM_LOGE(BM_ERROR, "BatchCopy fence failed, thread=%lu channel=%lu ret=%d", param.thread, param.channel, ret);
        return BM_ERROR;
    }
    ret = ReadOnThread(param.thread, param.channel,
                       reinterpret_cast<void *>(static_cast<uintptr_t>(param.localFlagAddr)),
                       reinterpret_cast<void *>(static_cast<uintptr_t>(param.remoteFlagAddr)), param.flagSize);
    if (ret != BM_OK) {
        HYBM_LOGE(BM_ERROR, "BatchCopy completion read failed, thread=%lu channel=%lu flagSize=%u ret=%d",
                  param.thread, param.channel, param.flagSize, ret);
        return BM_ERROR;
    }
    return BM_OK;
}

} // namespace

uint32_t HybmBatchCopyStartBatchMode()
{
    int32_t ret = BatchModeStart();
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(BM_ERROR, "BatchCopy HcommBatchModeStart failed, batchTag=%s ret=%d", kBatchTag, ret);
        return BM_ERROR;
    }
    return BM_OK;
}

uint32_t HybmBatchCopySubmitDescriptors(const HybmBatchCopyTransferParam &param)
{
    return TransferDescriptors(param);
}

uint32_t HybmBatchCopyFenceAndReadCompletion(const HybmBatchCopyTransferParam &param)
{
    return FenceAndReadCompletion(param);
}

uint32_t HybmBatchCopyEndBatchMode()
{
    const int32_t ret = BatchModeEnd();
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(BM_ERROR, "BatchCopy HcommBatchModeEnd failed, batchTag=%s ret=%d", kBatchTag, ret);
        return BM_ERROR;
    }
    return BM_OK;
}
