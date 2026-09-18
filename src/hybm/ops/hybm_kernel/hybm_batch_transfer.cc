/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include "hybm_batch_transfer.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "hybm_def.h"
#include "hybm_kernel_log.h"

namespace {
constexpr uint32_t kMaxBatchSize = 1000;
constexpr const char *kBatchTag = "HybmKernel";

// Match HCCL's device loader: HCOMM device primitives are provided by libccl_kernel.so.
constexpr const char *kHcommLibName = "libccl_kernel.so";

// 接口签名须与 CANN 设备接口保持一致；签名写错不会编译报错，运行时直接踩内存
using HcommBatchModeStartFunc = int32_t (*)(const char *batchTag);
using HcommBatchModeEndFunc = int32_t (*)(const char *batchTag);
using HcommReadOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel, void *dst,
                                          const void *src, uint64_t len);
using HcommWriteOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel, void *dst,
                                           const void *src, uint64_t len);
using HcommChannelFenceOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel);
using HcommBatchTransferOnThreadFunc = int32_t (*)(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel,
                                                   const ock::mf::HcommBatchTransferDesc *transferDescs,
                                                   uint32_t transferDescNum);

void *g_hcommHandle = nullptr;
HcommBatchModeStartFunc g_hcommBatchModeStart = nullptr;
HcommBatchModeEndFunc g_hcommBatchModeEnd = nullptr;
HcommReadOnThreadFunc g_hcommReadOnThread = nullptr;
HcommWriteOnThreadFunc g_hcommWriteOnThread = nullptr;
HcommChannelFenceOnThreadFunc g_hcommChannelFenceOnThread = nullptr;
HcommBatchTransferOnThreadFunc g_hcommBatchTransferOnThread = nullptr;

int32_t ResetHcommSymbols()
{
    if (g_hcommHandle != nullptr && dlclose(g_hcommHandle) != 0) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] dlclose failed, lib=%s error=%s", kHcommLibName, dlerror());
        return BM_DL_FUNCTION_FAILED;
    }
    g_hcommHandle = nullptr;
    g_hcommBatchModeStart = nullptr;
    g_hcommBatchModeEnd = nullptr;
    g_hcommReadOnThread = nullptr;
    g_hcommWriteOnThread = nullptr;
    g_hcommChannelFenceOnThread = nullptr;
    g_hcommBatchTransferOnThread = nullptr;
    return BM_OK;
}

void *LoadRequiredSymbol(const char *symbol)
{
    void *addr = dlsym(g_hcommHandle, symbol);
    if (addr == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] dlsym failed, lib=%s symbol=%s error=%s", kHcommLibName, symbol,
                  dlerror());
    }
    return addr;
}

void *LoadOptionalSymbol(const char *symbol)
{
    void *addr = dlsym(g_hcommHandle, symbol);
    if (addr == nullptr) {
        HYBM_LOGW("[hybm] optional symbol unavailable, lib=%s symbol=%s reason=%s", kHcommLibName, symbol, dlerror());
    }
    return addr;
}

int32_t LoadRequiredSymbols()
{
    g_hcommReadOnThread = reinterpret_cast<HcommReadOnThreadFunc>(LoadRequiredSymbol("HcommReadOnThread"));
    g_hcommWriteOnThread = reinterpret_cast<HcommWriteOnThreadFunc>(LoadRequiredSymbol("HcommWriteOnThread"));
    g_hcommChannelFenceOnThread =
        reinterpret_cast<HcommChannelFenceOnThreadFunc>(LoadRequiredSymbol("HcommChannelFenceOnThread"));
    if (g_hcommReadOnThread == nullptr || g_hcommWriteOnThread == nullptr || g_hcommChannelFenceOnThread == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] required hcomm symbols missing, lib=%s", kHcommLibName);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

void LoadOptionalSymbols()
{
    g_hcommBatchModeStart = reinterpret_cast<HcommBatchModeStartFunc>(LoadOptionalSymbol("HcommBatchModeStart"));
    g_hcommBatchModeEnd = reinterpret_cast<HcommBatchModeEndFunc>(LoadOptionalSymbol("HcommBatchModeEnd"));
    g_hcommBatchTransferOnThread =
        reinterpret_cast<HcommBatchTransferOnThreadFunc>(LoadOptionalSymbol("HcommBatchTransferOnThread"));
}

int32_t LoadHcommLibrary()
{
    g_hcommHandle = dlopen(kHcommLibName, RTLD_NOW | RTLD_NODELETE);
    if (g_hcommHandle == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED,
                  "[hybm] device dlopen failed, lib=%s error=%s; check device HCOMM library deployment", kHcommLibName,
                  dlerror());
        return BM_DL_FUNCTION_FAILED;
    }
    (void)dlerror(); // 清空历史错误，确保后续 dlerror() 反映的是本次 dlsym 的结果

    if (LoadRequiredSymbols() != BM_OK) {
        ResetHcommSymbols();
        return BM_DL_FUNCTION_FAILED;
    }
    LoadOptionalSymbols();

    HYBM_LOGI("[hybm] hcomm library loaded, lib=%s batchTransfer=%d batchMode=%d", kHcommLibName,
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

bool IsMarkerOnly(const HybmOneSideOpParam *param)
{
    return param->list_num == 0 && param->dst_buf_addr_list == nullptr && param->src_buf_addr_list == nullptr &&
           param->len_list == nullptr && param->remote_flag_addr != 0 && param->local_flag_addr != 0 &&
           param->flag_size != 0;
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

int32_t WriteOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel, void *dst, const void *src,
                      uint64_t len)
{
    if (g_hcommWriteOnThread == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return g_hcommWriteOnThread(thread, channel, dst, src, len);
}

int32_t ChannelFenceOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel)
{
    if (g_hcommChannelFenceOnThread == nullptr) {
        return BM_NOT_SUPPORTED;
    }
    return g_hcommChannelFenceOnThread(thread, channel);
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
    uint32_t offset = 0;
    while (offset < count) {
        const uint32_t batchSize = std::min(kMaxBatchSize, count - offset);
        int32_t ret = BatchTransferOnThread(thread, channel, param->transfer_descs + start + offset, batchSize);
        if (ret != BM_OK) {
            if (IsNotSupported(ret)) {
                HYBM_LOGW("[hybm] HcommBatchTransferOnThread unavailable, ret=%d rankId=%u count=%u", ret,
                          param->rank_id_list[rankIdx], count);
                return BM_NOT_SUPPORTED;
            }
            HYBM_LOGE(BM_ERROR,
                      "[hybm] HcommBatchTransferOnThread failed, rankId=%u thread=%lu channel=%lu "
                      "count=%u offset=%u batch=%u ret=%d",
                      param->rank_id_list[rankIdx], thread, channel, count, offset, batchSize, ret);
            return ret;
        }
        offset += batchSize;
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
    return BM_OK;
}

int32_t CheckParam(const HybmOneSideOpParam *param)
{
    if (param == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid HybmOneSideOpParam: param is null");
        return BM_INVALID_PARAM;
    }

    if (param->thread == 0 || param->channel == 0) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid HybmOneSideOpParam handle, param=%p thread=%lu channel=%lu",
                  static_cast<const void *>(param), param->thread, param->channel);
        return BM_INVALID_PARAM;
    }

    if (param->list_num == 0) {
        if (IsMarkerOnly(param)) {
            return BM_OK;
        }
        HYBM_LOGE(BM_INVALID_PARAM,
                  "[hybm] invalid marker-only: list_num=0 dstList=%p srcList=%p lenList=%p remote=0x%lx local=0x%lx "
                  "fsize=%u",
                  static_cast<void *>(param->dst_buf_addr_list), static_cast<void *>(param->src_buf_addr_list),
                  static_cast<void *>(param->len_list), param->remote_flag_addr, param->local_flag_addr,
                  param->flag_size);
        return BM_INVALID_PARAM;
    }

    if (param->dst_buf_addr_list == nullptr || param->src_buf_addr_list == nullptr || param->len_list == nullptr) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] incomplete data param: list_num=%u dstList=%p srcList=%p lenList=%p",
                  param->list_num, static_cast<void *>(param->dst_buf_addr_list),
                  static_cast<void *>(param->src_buf_addr_list), static_cast<void *>(param->len_list));
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

int32_t TransferWithBatch(bool isRead, HybmOneSideOpParam *param)
{
    HYBM_LOGD("[hybm] batch transfer start, isRead=%d thread=%lu channel=%lu list_num=%u", isRead, param->thread,
              param->channel, param->list_num);
    std::vector<ock::mf::HcommBatchTransferDesc> descs(param->list_num);
    for (uint32_t idx = 0; idx < param->list_num; ++idx) {
        auto &desc = descs[idx];
        desc.transType = isRead ? ock::mf::HCOMM_TRANSFER_TYPE_READ : ock::mf::HCOMM_TRANSFER_TYPE_WRITE;
        if (isRead) {
            desc.transferInfo.read.len = param->len_list[idx];
            desc.transferInfo.read.dst = param->dst_buf_addr_list[idx];
            desc.transferInfo.read.src = param->src_buf_addr_list[idx];
        } else {
            desc.transferInfo.write.len = param->len_list[idx];
            desc.transferInfo.write.dst = param->dst_buf_addr_list[idx];
            desc.transferInfo.write.src = param->src_buf_addr_list[idx];
        }
    }

    uint32_t offset = 0;
    while (offset < param->list_num) {
        const uint32_t batchSize = std::min(kMaxBatchSize, param->list_num - offset);
        HYBM_LOGD("[hybm] batch transfer summary, offset=%u batchSize=%u", offset, batchSize);
        int32_t ret = BatchTransferOnThread(param->thread, param->channel, descs.data() + offset, batchSize);
        if (ret != BM_OK) {
            if (IsNotSupported(ret)) {
                HYBM_LOGW("[hybm] HcommBatchTransferOnThread unavailable, ret=%d isRead=%d thread=%lu "
                          "channel=%lu list_num=%u",
                          ret, isRead, param->thread, param->channel, param->list_num);
                return BM_NOT_SUPPORTED;
            }
            HYBM_LOGE(BM_ERROR,
                      "[hybm] HcommBatchTransferOnThread failed, isRead=%d thread=%lu channel=%lu list_num=%u "
                      "offset=%u batch=%u ret=%d",
                      isRead, param->thread, param->channel, param->list_num, offset, batchSize, ret);
            return ret;
        }
        offset += batchSize;
    }
    return BM_OK;
}

int32_t TransferWithSingle(bool isRead, HybmOneSideOpParam *param)
{
    for (uint32_t idx = 0; idx < param->list_num; ++idx) {
        int32_t ret = isRead ? ReadOnThread(param->thread, param->channel, param->dst_buf_addr_list[idx],
                                            param->src_buf_addr_list[idx], param->len_list[idx])
                             : WriteOnThread(param->thread, param->channel, param->dst_buf_addr_list[idx],
                                             param->src_buf_addr_list[idx], param->len_list[idx]);
        if (ret != BM_OK) {
            if (isRead) {
                HYBM_LOGE(BM_ERROR,
                          "[hybm] HcommReadOnThread failed, thread=%lu channel=%lu list_num=%u idx=%u, dst=%p, "
                          "src=%p, len=%" PRIu64 ", ret=%d",
                          param->thread, param->channel, param->list_num, idx, param->dst_buf_addr_list[idx],
                          param->src_buf_addr_list[idx], param->len_list[idx], ret);
            } else {
                HYBM_LOGE(BM_ERROR,
                          "[hybm] HcommWriteOnThread failed, thread=%lu channel=%lu list_num=%u idx=%u, dst=%p, "
                          "src=%p, len=%" PRIu64 ", ret=%d",
                          param->thread, param->channel, param->list_num, idx, param->dst_buf_addr_list[idx],
                          param->src_buf_addr_list[idx], param->len_list[idx], ret);
            }
            return BM_ERROR;
        }
    }
    return BM_OK;
}

int32_t HybmBatchTransferTask(bool isRead, HybmOneSideOpParam *param)
{
    const int32_t batchRet = TransferWithBatch(isRead, param);
    if (batchRet == BM_NOT_SUPPORTED) {
        HYBM_LOGW("[hybm] batch transfer API unsupported, fallback to single, isRead=%d list_num=%u", isRead,
                  param->list_num);
        return TransferWithSingle(isRead, param);
    }
    if (batchRet != BM_OK) {
        return BM_ERROR;
    }
    return BM_OK;
}

int32_t ReadRemoteFlag(const HybmOneSideOpParam *param)
{
    if (param->remote_flag_addr == 0 || param->flag_size == 0) {
        return BM_OK;
    }
    HYBM_LOGD("[hybm] remote flag read start, thread=%lu channel=%lu localFlag=0x%lx remoteFlag=0x%lx flagSize=%u",
              param->thread, param->channel, param->local_flag_addr, param->remote_flag_addr, param->flag_size);
    int32_t ret = ReadOnThread(
        param->thread, param->channel, reinterpret_cast<void *>(static_cast<uintptr_t>(param->local_flag_addr)),
        reinterpret_cast<void *>(static_cast<uintptr_t>(param->remote_flag_addr)), param->flag_size);
    if (ret != BM_OK) {
        HYBM_LOGE(BM_ERROR,
                  "[hybm] remote flag read failed, thread=%lu channel=%lu localFlag=0x%lx remoteFlag=0x%lx "
                  "flagSize=%u ret=%d",
                  param->thread, param->channel, param->local_flag_addr, param->remote_flag_addr, param->flag_size,
                  ret);
        return BM_ERROR;
    }
    HYBM_LOGD("[hybm] remote flag read success, thread=%lu channel=%lu flagSize=%u", param->thread, param->channel,
              param->flag_size);
    return BM_OK;
}

int32_t HybmBatchTransfer(bool isRead, HybmOneSideOpParam *param)
{
    HYBM_LOGD("[hybm] HybmBatchTransfer start, isRead=%d", isRead);
    int32_t ret = CheckParam(param);
    if (ret != BM_OK) {
        return ret;
    }

    ret = EnsureHcommLoaded();
    if (ret != BM_OK) {
        HYBM_LOGE(ret, "[hybm] hcomm library unavailable, isRead=%d thread=%lu channel=%lu", isRead, param->thread,
                  param->channel);
        return ret;
    }
    HYBM_LOGI("[hybm] HybmBatchTransfer entry, isRead=%d param=%p thread=%lu channel=%lu list_num=%u "
              "remoteFlag=0x%lx localFlag=0x%lx flagSize=%u",
              isRead, static_cast<void *>(param), param->thread, param->channel, param->list_num,
              param->remote_flag_addr, param->local_flag_addr, param->flag_size);

    ret = BatchModeStart(kBatchTag);
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(BM_ERROR, "[hybm] HcommBatchModeStart failed, batchTag=%s ret=%d", kBatchTag, ret);
        return BM_ERROR;
    }

    if (param->list_num > 0) {
        ret = HybmBatchTransferTask(isRead, param);
        if (ret != BM_OK) {
            (void)BatchModeEnd(kBatchTag);
            return BM_ERROR;
        }
    }

    HYBM_LOGD("[hybm] channel fence start, thread=%lu channel=%lu", param->thread, param->channel);
    ret = ChannelFenceOnThread(param->thread, param->channel);
    if (ret != BM_OK) {
        HYBM_LOGE(BM_ERROR, "[hybm] HcommChannelFenceOnThread failed, thread=%lu channel=%lu ret=%d", param->thread,
                  param->channel, ret);
        (void)BatchModeEnd(kBatchTag);
        return BM_ERROR;
    }
    HYBM_LOGD("[hybm] channel fence success, thread=%lu channel=%lu", param->thread, param->channel);

    ret = ReadRemoteFlag(param);
    if (ret != BM_OK) {
        (void)BatchModeEnd(kBatchTag);
        return BM_ERROR;
    }

    ret = BatchModeEnd(kBatchTag);
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(BM_ERROR, "[hybm] HcommBatchModeEnd failed, batchTag=%s ret=%d", kBatchTag, ret);
        return BM_ERROR;
    }
    HYBM_LOGI("[hybm] HybmBatchTransfer success, isRead=%d list_num=%u", isRead, param->list_num);
    return BM_OK;
}

} // namespace

extern "C" {
int32_t HybmBatchWrite(HybmOneSideOpParam *param)
{
    int32_t ret = HybmBatchTransfer(false, param);
    if (ret != BM_OK) {
        return BM_ERROR;
    }
    return ret;
}

int32_t HybmBatchRead(HybmOneSideOpParam *param)
{
    int32_t ret = HybmBatchTransfer(true, param);
    if (ret != BM_OK) {
        return BM_ERROR;
    }
    return ret;
}

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
    ret = BatchModeStart(kBatchTag);
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(BM_ERROR, "[hybm] HcommBatchModeStart failed, batchTag=%s ret=%d", kBatchTag, ret);
        return BM_ERROR;
    }

    for (uint32_t rankIdx = 0; rankIdx < param->rank_num; ++rankIdx) {
        ret = TransferRank(param, rankIdx);
        if (ret != BM_OK) {
            const bool notSupported = ret == BM_NOT_SUPPORTED;
            if (notSupported) {
                HYBM_LOGW("[hybm] rank batch transfer unsupported, batchTag=%s rankIdx=%u rankId=%u ret=%d", kBatchTag,
                          rankIdx, param->rank_id_list[rankIdx], ret);
            } else {
                HYBM_LOGE(BM_ERROR, "[hybm] rank transfer failed, batchTag=%s rankIdx=%u rankId=%u ret=%d", kBatchTag,
                          rankIdx, param->rank_id_list[rankIdx], ret);
            }
            const int32_t endRet = BatchModeEnd(kBatchTag);
            if (endRet != BM_OK && !IsNotSupported(endRet)) {
                HYBM_LOGE(BM_ERROR,
                          "[hybm] HcommBatchModeEnd after transfer failure failed, batchTag=%s rankId=%u ret=%d",
                          kBatchTag, param->rank_id_list[rankIdx], endRet);
            }
            return notSupported ? BM_NOT_SUPPORTED : BM_ERROR;
        }
    }

    ret = BatchModeEnd(kBatchTag);
    if (ret != BM_OK && !IsNotSupported(ret)) {
        HYBM_LOGE(BM_ERROR, "[hybm] HcommBatchModeEnd failed, batchTag=%s ret=%d", kBatchTag, ret);
        return BM_ERROR;
    }
    HYBM_LOGI("[hybm] HybmBatchTransfer success, rankNum=%u totalListNum=%u", param->rank_num, param->total_list_num);
    return BM_OK;
}
}
