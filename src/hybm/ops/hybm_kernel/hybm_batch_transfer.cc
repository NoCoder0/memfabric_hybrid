/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include "hybm_batch_transfer.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>

#include "hybm_def.h"
#include "hybm_kernel_log.h"

namespace {
constexpr uint32_t MAX_BATCH_SIZE = 1000;
constexpr const char *BATCH_TAG = "HybmKernel";

// Match HCCL's device loader: HCOMM device primitives are provided by libccl_kernel.so.
constexpr const char *HCOMM_LIB_NAME = "libccl_kernel.so";

// 接口签名须与 CANN 设备接口保持一致；签名写错不会编译报错，运行时直接踩内存
using HcommBatchModeStartFunc = int32_t (*)(const char *batchTag);
using HcommBatchModeEndFunc = int32_t (*)(const char *batchTag);
using HcommReadOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel, void *dst,
                                          const void *src, uint64_t len);
using HcommChannelFenceOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel);
using HcommChannelDrainOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel);
using HcommBatchTransferOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel,
                                                   const ock::mf::HcommBatchTransferDesc *transferDescs,
                                                   uint32_t transferDescNum);
using HcommAclrtNotifyRecordOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, uint64_t dstNotifyId);

void *g_hcommHandle = nullptr;
HcommBatchModeStartFunc g_hcommBatchModeStart = nullptr;
HcommBatchModeEndFunc g_hcommBatchModeEnd = nullptr;
HcommReadOnThreadFunc g_hcommReadOnThread = nullptr;
HcommChannelFenceOnThreadFunc g_hcommChannelFenceOnThread = nullptr;
HcommChannelDrainOnThreadFunc g_hcommChannelDrainOnThread = nullptr;
HcommBatchTransferOnThreadFunc g_hcommBatchTransferOnThread = nullptr;
HcommAclrtNotifyRecordOnThreadFunc g_hcommAclrtNotifyRecordOnThread = nullptr;

int32_t ResetHcommSymbols()
{
    if (g_hcommHandle != nullptr && dlclose(g_hcommHandle) != 0) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] dlclose failed, lib=%s error=%s", HCOMM_LIB_NAME, dlerror());
        return BM_DL_FUNCTION_FAILED;
    }
    g_hcommHandle = nullptr;
    g_hcommBatchModeStart = nullptr;
    g_hcommBatchModeEnd = nullptr;
    g_hcommReadOnThread = nullptr;
    g_hcommChannelFenceOnThread = nullptr;
    g_hcommChannelDrainOnThread = nullptr;
    g_hcommBatchTransferOnThread = nullptr;
    g_hcommAclrtNotifyRecordOnThread = nullptr;
    return BM_OK;
}

void *LoadRequiredSymbol(const char *symbol)
{
    void *addr = dlsym(g_hcommHandle, symbol);
    if (addr == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] dlsym failed, lib=%s symbol=%s error=%s", HCOMM_LIB_NAME, symbol,
                  dlerror());
    }
    return addr;
}

void *LoadOptionalSymbol(const char *symbol)
{
    void *addr = dlsym(g_hcommHandle, symbol);
    if (addr == nullptr) {
        HYBM_LOGW("[hybm] optional symbol unavailable, lib=%s symbol=%s reason=%s", HCOMM_LIB_NAME, symbol, dlerror());
    }
    return addr;
}

int32_t LoadRequiredSymbols()
{
    g_hcommReadOnThread = reinterpret_cast<HcommReadOnThreadFunc>(LoadRequiredSymbol("HcommReadOnThread"));
    g_hcommChannelFenceOnThread =
        reinterpret_cast<HcommChannelFenceOnThreadFunc>(LoadRequiredSymbol("HcommChannelFenceOnThread"));
    if (g_hcommReadOnThread == nullptr || g_hcommChannelFenceOnThread == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] required hcomm symbols missing, lib=%s", HCOMM_LIB_NAME);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

void LoadOptionalSymbols()
{
    g_hcommChannelDrainOnThread =
        reinterpret_cast<HcommChannelDrainOnThreadFunc>(LoadOptionalSymbol("HcommChannelDrainOnThread"));
    g_hcommBatchModeStart = reinterpret_cast<HcommBatchModeStartFunc>(LoadOptionalSymbol("HcommBatchModeStart"));
    g_hcommBatchModeEnd = reinterpret_cast<HcommBatchModeEndFunc>(LoadOptionalSymbol("HcommBatchModeEnd"));
    g_hcommBatchTransferOnThread =
        reinterpret_cast<HcommBatchTransferOnThreadFunc>(LoadOptionalSymbol("HcommBatchTransferOnThread"));
    g_hcommAclrtNotifyRecordOnThread =
        reinterpret_cast<HcommAclrtNotifyRecordOnThreadFunc>(LoadOptionalSymbol("HcommAclrtNotifyRecordOnThread"));
}

int32_t LoadHcommLibrary()
{
    g_hcommHandle = dlopen(HCOMM_LIB_NAME, RTLD_NOW | RTLD_NODELETE);
    if (g_hcommHandle == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED,
                  "[hybm] device dlopen failed, lib=%s error=%s; check device HCOMM library deployment", HCOMM_LIB_NAME,
                  dlerror());
        return BM_DL_FUNCTION_FAILED;
    }
    (void)dlerror(); // 清空历史错误，确保后续 dlerror() 反映的是本次 dlsym 的结果

    if (LoadRequiredSymbols() != BM_OK) {
        ResetHcommSymbols();
        return BM_DL_FUNCTION_FAILED;
    }
    LoadOptionalSymbols();

    HYBM_LOGI("[hybm] hcomm library loaded, lib=%s batchTransfer=%d batchMode=%d", HCOMM_LIB_NAME,
              static_cast<int32_t>(g_hcommBatchTransferOnThread != nullptr),
              static_cast<int32_t>(g_hcommBatchModeStart != nullptr));
    return BM_OK;
}

// The device module owns the loader for its lifetime. Its normal teardown must
// happen after all kernel calls have finished, just like unloading the kernel itself.
class HcommLibrary {
public:
    HcommLibrary() : result_(LoadHcommLibrary()) {}

    ~HcommLibrary()
    {
        (void)ResetHcommSymbols();
    }

    HcommLibrary(const HcommLibrary &) = delete;
    HcommLibrary &operator=(const HcommLibrary &) = delete;

    int32_t Result() const
    {
        return result_;
    }

private:
    const int32_t result_;
};

// C++ guarantees thread-safe first initialization; subsequent calls reuse it.
int32_t EnsureHcommLoaded()
{
    static const HcommLibrary library;
    return library.Result();
}

bool IsNotSupported(int32_t ret)
{
    return ret == BM_NOT_SUPPORTED || ret == BM_NOT_SUPPORT_FUNC || ret == BM_UNDER_API_UNLOAD;
}

int32_t BatchModeStart(const char *batchTag)
{
    if (g_hcommBatchModeStart == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return g_hcommBatchModeStart(batchTag);
}

int32_t BatchModeEnd(const char *batchTag)
{
    if (g_hcommBatchModeEnd == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return g_hcommBatchModeEnd(batchTag);
}

int32_t ReadOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel, void *dst, const void *src,
                     uint64_t len)
{
    if (g_hcommReadOnThread == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return g_hcommReadOnThread(thread, channel, dst, src, len);
}

int32_t ChannelFenceOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel)
{
    if (g_hcommChannelFenceOnThread == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return g_hcommChannelFenceOnThread(thread, channel);
}

int32_t ChannelDrainOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel)
{
    if (g_hcommChannelDrainOnThread == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return g_hcommChannelDrainOnThread(thread, channel);
}

int32_t BatchTransferOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel,
                              const ock::mf::HcommBatchTransferDesc *transferDescs, uint32_t transferDescNum)
{
    if (g_hcommBatchTransferOnThread == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return g_hcommBatchTransferOnThread(thread, channel, transferDescs, transferDescNum);
}

int32_t CheckBatchParam(const HybmBatchTransferParam *param)
{
    if (param == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid HybmBatchTransferParam: param is null");
        return BM_INVALID_PARAM;
    }
    if (param->rank_num == 0 || param->rank_id_list == nullptr || param->thread_list == nullptr ||
        param->channel_list == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid rank param, rankNum=%u rankIds=%p threads=%p channels=%p",
                  param->rank_num, static_cast<void *>(param->rank_id_list), static_cast<void *>(param->thread_list),
                  static_cast<void *>(param->channel_list));
        return BM_INVALID_PARAM;
    }
    if (param->notify_id != 0U && param->rank_num != 1U) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] notify requires one rank, rankNum=%u notifyId=%u", param->rank_num,
                  param->notify_id);
        return BM_INVALID_PARAM;
    }
    for (uint32_t rankIdx = 0; rankIdx < param->rank_num; ++rankIdx) {
        if (param->thread_list[rankIdx] == 0 || param->channel_list[rankIdx] == 0) {
            HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid rank handle, rankIdx=%u rankId=%u thread=%lu channel=%lu",
                      rankIdx, param->rank_id_list[rankIdx], param->thread_list[rankIdx], param->channel_list[rankIdx]);
            return BM_INVALID_PARAM;
        }
    }
    if (param->transfer_descs == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] transfer descriptors are null, totalListNum=%u", param->total_list_num);
        return BM_INVALID_PARAM;
    }
    if (param->total_list_num == 0 || param->rank_start_idx_list == nullptr || param->rank_list_num_list == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid IO range param, totalListNum=%u starts=%p counts=%p",
                  param->total_list_num, static_cast<void *>(param->rank_start_idx_list),
                  static_cast<void *>(param->rank_list_num_list));
        return BM_INVALID_PARAM;
    }
    for (uint32_t rankIdx = 0; rankIdx < param->rank_num; ++rankIdx) {
        const uint32_t start = param->rank_start_idx_list[rankIdx];
        const uint32_t count = param->rank_list_num_list[rankIdx];
        if (start > param->total_list_num || count > param->total_list_num - start) {
            HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid IO range, rankIdx=%u rankId=%u start=%u count=%u total=%u",
                      rankIdx, param->rank_id_list[rankIdx], start, count, param->total_list_num);
            return BM_INVALID_PARAM;
        }
        if (param->marker_descs != nullptr) {
            const auto &marker = param->marker_descs[rankIdx];
            if (marker.transType != ock::mf::HCOMM_TRANSFER_TYPE_READ || marker.transferInfo.read.src == nullptr ||
                marker.transferInfo.read.dst == nullptr || marker.transferInfo.read.len == 0) {
                HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid marker descriptor, rankIdx=%u rankId=%u", rankIdx,
                          param->rank_id_list[rankIdx]);
                return BM_INVALID_PARAM;
            }
        }
    }
    return BM_OK;
}

int32_t MultiRankTransferWithBatch(HybmBatchTransferParam *param, uint32_t rankIdx)
{
    const uint32_t start = param->rank_start_idx_list[rankIdx];
    const uint32_t count = param->rank_list_num_list[rankIdx];
    const auto thread = param->thread_list[rankIdx];
    const auto channel = param->channel_list[rankIdx];
    for (uint32_t offset = 0, batchSize = 0; offset < count; offset += batchSize) {
        batchSize = std::min(MAX_BATCH_SIZE, count - offset);
        int32_t ret = BatchTransferOnThread(thread, channel, param->transfer_descs + start + offset, batchSize);
        if (ret != BM_OK) {
            if (IsNotSupported(ret)) {
                HYBM_LOGE(BM_NOT_SUPPORTED,
                          "[hybm] HcommBatchTransferOnThread unavailable; upgrade to CANN 9.1 or later, "
                          "ret=%d rankId=%u count=%u",
                          ret, param->rank_id_list[rankIdx], count);
                return BM_NOT_SUPPORTED;
            }
            HYBM_LOGE(BM_ERROR,
                      "[hybm] HcommBatchTransferOnThread failed, rankId=%u thread=%lu channel=%lu "
                      "count=%u offset=%u batch=%u ret=%d",
                      param->rank_id_list[rankIdx], thread, channel, count, offset, batchSize, ret);
            return ret;
        }
    }
    return BM_OK;
}

int32_t TransferRank(HybmBatchTransferParam *param, uint32_t rankIdx)
{
    const auto thread = param->thread_list[rankIdx];
    const auto channel = param->channel_list[rankIdx];
    if (param->total_list_num > 0) {
        const int32_t ret = MultiRankTransferWithBatch(param, rankIdx);
        if (ret != BM_OK) {
            return ret;
        }
    }
    const int32_t ret = ChannelFenceOnThread(thread, channel);
    if (ret != BM_OK) {
        HYBM_LOGE(BM_ERROR, "[hybm] HcommChannelFenceOnThread failed, rankId=%u thread=%lu channel=%lu ret=%d",
                  param->rank_id_list[rankIdx], thread, channel, ret);
        return BM_ERROR;
    }
    // notify部分
    if (param->marker_descs != nullptr) {
        const auto &marker = param->marker_descs[rankIdx];
        const int32_t markerRet = ReadOnThread(thread, channel, marker.transferInfo.read.dst,
                                               marker.transferInfo.read.src, marker.transferInfo.read.len);
        if (markerRet != BM_OK) {
            HYBM_LOGE(BM_ERROR, "[hybm] marker submit failed, rankId=%u thread=%lu channel=%lu ret=%d",
                      param->rank_id_list[rankIdx], thread, channel, markerRet);
            return markerRet;
        }
    }
    if (param->require_drain != 0U) {
        const auto drainRet = ChannelDrainOnThread(thread, channel);
        if (drainRet != BM_OK) {
            HYBM_LOGE(BM_ERROR, "[hybm] HcommChannelDrainOnThread failed, rankId=%u thread=%lu channel=%lu ret=%d",
                      param->rank_id_list[rankIdx], thread, channel, drainRet);
            return BM_ERROR;
        }
    }
    if (param->notify_id != 0U) {
        if (g_hcommAclrtNotifyRecordOnThread == nullptr) {
            HYBM_LOGE(BM_NOT_SUPPORTED, "[hybm] notify record API unavailable, rankId=%u thread=%lu notifyId=%u",
                      param->rank_id_list[rankIdx], thread, param->notify_id);
            return BM_NOT_SUPPORTED;
        }
        const auto notifyRet = g_hcommAclrtNotifyRecordOnThread(thread, static_cast<uint64_t>(param->notify_id));
        if (notifyRet != BM_OK) {
            HYBM_LOGE(BM_ERROR, "[hybm] notify record failed, rankId=%u thread=%lu notifyId=%u ret=%d",
                      param->rank_id_list[rankIdx], thread, param->notify_id, notifyRet);
            return BM_ERROR;
        }
    }
    return BM_OK;
}

} // namespace

int32_t HybmSubmitBatchTransfer(HybmBatchTransferParam *param, const char *batchTag)
{
    auto ret = EnsureHcommLoaded();
    if (ret != BM_OK) {
        return ret;
    }
    ret = BatchModeStart(batchTag);
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(ret, "[hybm] BatchModeStart failed, tag=%s ret=%d", batchTag, ret);
        return ret;
    }
    for (uint32_t rankIdx = 0; rankIdx < param->rank_num; ++rankIdx) {
        ret = TransferRank(param, rankIdx);
        if (ret != BM_OK) {
            break;
        }
    }
    const auto endRet = BatchModeEnd(batchTag);
    if (endRet != BM_OK && !IsNotSupported(endRet)) {
        HYBM_LOGE(endRet, "[hybm] BatchModeEnd failed, tag=%s ret=%d", batchTag, endRet);
        return ret == BM_OK ? endRet : ret;
    }
    return ret;
}

extern "C" {
int32_t HybmBatchTransfer(HybmBatchTransferParam *param)
{
    HYBM_LOGD("[hybm] HybmBatchTransfer start");
    int32_t ret = CheckBatchParam(param);
    if (ret != BM_OK) {
        return ret;
    }
    ret = EnsureHcommLoaded();
    if (ret != BM_OK) {
        HYBM_LOGE(ret, "[hybm] hcomm library unavailable, rankNum=%u", param->rank_num);
        return ret;
    }
    ret = BatchModeStart(BATCH_TAG);
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(BM_ERROR, "[hybm] HcommBatchModeStart failed, batchTag=%s ret=%d", BATCH_TAG, ret);
        return BM_ERROR;
    }

    for (uint32_t rankIdx = 0; rankIdx < param->rank_num; ++rankIdx) {
        ret = TransferRank(param, rankIdx);
        if (ret != BM_OK) {
            const bool notSupported = ret == BM_NOT_SUPPORTED;
            if (notSupported) {
                HYBM_LOGW("[hybm] rank batch transfer unsupported, batchTag=%s rankIdx=%u rankId=%u ret=%d", BATCH_TAG,
                          rankIdx, param->rank_id_list[rankIdx], ret);
            } else {
                HYBM_LOGE(BM_ERROR, "[hybm] rank transfer failed, batchTag=%s rankIdx=%u rankId=%u ret=%d", BATCH_TAG,
                          rankIdx, param->rank_id_list[rankIdx], ret);
            }
            const int32_t endRet = BatchModeEnd(BATCH_TAG);
            if (endRet != BM_OK && !IsNotSupported(endRet)) {
                HYBM_LOGE(BM_ERROR,
                          "[hybm] HcommBatchModeEnd after transfer failure failed, batchTag=%s rankId=%u ret=%d",
                          BATCH_TAG, param->rank_id_list[rankIdx], endRet);
            }
            return notSupported ? BM_NOT_SUPPORTED : BM_ERROR;
        }
    }

    ret = BatchModeEnd(BATCH_TAG);
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(BM_ERROR, "[hybm] HcommBatchModeEnd failed, batchTag=%s ret=%d", BATCH_TAG, ret);
        return BM_ERROR;
    }
    HYBM_LOGI("[hybm] HybmBatchTransfer success, rankNum=%u totalListNum=%u", param->rank_num, param->total_list_num);
    return BM_OK;
}
}
