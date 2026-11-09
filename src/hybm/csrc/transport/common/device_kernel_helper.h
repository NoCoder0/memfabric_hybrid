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

#ifndef MF_HYBM_DEVICE_KERNEL_HELPER_H
#define MF_HYBM_DEVICE_KERNEL_HELPER_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "hybm_logger.h"
#include "dl_acl_api.h"
#include "hybm_kernel_hepler.h"
#include "hybm_transport_common.h"
#include "hybm_batch_transfer.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

// ─────────────────────────────────────────────────────────────────
// DeviceTransferBuffers — AICPU kernel batch buffer tracking
// Allocated via AclrtMalloc, freed after Synchronize.
// ─────────────────────────────────────────────────────────────────
struct DeviceTransferBuffers {
    void *dstList{nullptr};
    void *srcList{nullptr};
    void *lenList{nullptr};
};

inline void ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers)
{
    if (buffers.dstList != nullptr) {
        (void)DlAclApi::AclrtFree(buffers.dstList);
    }
    buffers.dstList = nullptr;
    buffers.srcList = nullptr;
    buffers.lenList = nullptr;
}

// ─────────────────────────────────────────────────────────────────
// KernelLaunchConfig — parameters for LaunchDeviceKernel
// ─────────────────────────────────────────────────────────────────
struct KernelLaunchConfig {
    aclrtFuncHandle funcHandle{nullptr};
    ThreadHandle thread{0};
    ChannelHandle channel{0};
    void *stream{nullptr};
    bool isRead{false};
    uint32_t batchSize{0};
    const uint64_t *localAddrs{nullptr};
    const uint64_t *remoteAddrs{nullptr};
    const uint64_t *sizes{nullptr};
    uint32_t notifyId{0}; // 非 0 时内核在数据 WR 之后记录一次 NotifyRecord，host 侧等待它
    uint32_t rankId{0};
};

struct DeviceBatchHeader {
    ThreadHandle thread;
    ChannelHandle channel;
    uint32_t rankId;
    uint32_t start;
    uint32_t count;
};

/**
 * @brief Allocate device buffer and copy host data for kernel launch.
 */
inline Result PrepareLaunchBuffer(const KernelLaunchConfig &config, void *&dstListDev)
{
    const auto totalBytes = sizeof(DeviceBatchHeader) + config.batchSize * sizeof(HcommBatchTransferDesc);
    std::vector<uint8_t> hostBuf(totalBytes);
    auto ret = DlAclApi::AclrtMalloc(&dstListDev, totalBytes, 0);
    if (ret != BM_OK || dstListDev == nullptr) {
        BM_LOG_ERROR("LaunchDeviceKernel AclrtMalloc failed, size=" << totalBytes << " ret=" << ret);
        return ret;
    }
    const DeviceBatchHeader header{config.thread, config.channel, config.rankId, 0U, config.batchSize};
    std::memcpy(hostBuf.data(), &header, sizeof(header));
    for (uint32_t i = 0; i < config.batchSize; ++i) {
        HcommBatchTransferDesc desc{};
        desc.transType = config.isRead ? HCOMM_TRANSFER_TYPE_READ : HCOMM_TRANSFER_TYPE_WRITE;
        if (config.isRead) {
            desc.transferInfo.read = {config.sizes[i], reinterpret_cast<void *>(config.localAddrs[i]),
                                      reinterpret_cast<void *>(config.remoteAddrs[i])};
        } else {
            desc.transferInfo.write = {config.sizes[i], reinterpret_cast<void *>(config.remoteAddrs[i]),
                                       reinterpret_cast<void *>(config.localAddrs[i])};
        }
        std::memcpy(hostBuf.data() + sizeof(header) + i * sizeof(desc), &desc, sizeof(desc));
    }
    ret = DlAclApi::AclrtMemcpy(dstListDev, totalBytes, hostBuf.data(), totalBytes, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("LaunchDeviceKernel AclrtMemcpy H2D failed, ret=" << ret);
        (void)DlAclApi::AclrtFree(dstListDev);
    }
    return ret;
}

/**
 * @brief Set up kernel args and launch AICPU kernel.
 */
inline Result LaunchDeviceKernel(const KernelLaunchConfig &config, DeviceTransferBuffers &outBuffers)
{
    if (config.batchSize == 0) {
        return BM_OK;
    }
    if (config.localAddrs == nullptr || config.remoteAddrs == nullptr || config.sizes == nullptr) {
        BM_LOG_ERROR("LaunchDeviceKernel: null address arrays");
        return BM_INVALID_PARAM;
    }
    if (config.funcHandle == nullptr) {
        BM_LOG_ERROR("LaunchDeviceKernel: funcHandle not loaded");
        return BM_DL_FUNCTION_FAILED;
    }
    if (config.stream == nullptr) {
        BM_LOG_ERROR("LaunchDeviceKernel: null stream");
        return BM_INVALID_PARAM;
    }

    void *dstListDev = nullptr;
    auto ret = PrepareLaunchBuffer(config, dstListDev);
    if (ret != BM_OK) {
        return ret;
    }
    auto *base = static_cast<uint8_t *>(dstListDev);
    HybmBatchTransferParam args{};
    args.rank_num = 1U;
    args.rank_id_list = reinterpret_cast<uint32_t *>(base + offsetof(DeviceBatchHeader, rankId));
    args.rank_start_idx_list = reinterpret_cast<uint32_t *>(base + offsetof(DeviceBatchHeader, start));
    args.rank_list_num_list = reinterpret_cast<uint32_t *>(base + offsetof(DeviceBatchHeader, count));
    args.thread_list = reinterpret_cast<ThreadHandle *>(base + offsetof(DeviceBatchHeader, thread));
    args.channel_list = reinterpret_cast<ChannelHandle *>(base + offsetof(DeviceBatchHeader, channel));
    args.total_list_num = config.batchSize;
    args.transfer_descs = reinterpret_cast<HcommBatchTransferDesc *>(base + sizeof(DeviceBatchHeader));
    args.notify_id = config.notifyId;
    args.require_drain = 1U;

    ret = LaunchAicpuKernel(config.funcHandle, config.stream, &args, sizeof(args));
    if (ret != BM_OK) {
        BM_LOG_ERROR("LaunchDeviceKernel launch failed, kernel=HybmBatchTransfer, rank=" << config.rankId
                                                                                         << " ret=" << ret);
        (void)DlAclApi::AclrtFree(dstListDev);
        return ret;
    }

    outBuffers.dstList = dstListDev;
    BM_LOG_INFO("LaunchDeviceKernel success, isRead=" << config.isRead << " batchSize=" << config.batchSize);
    return BM_OK;
}

/**
 * @brief Query device info via RtGetDeviceInfo into output, with error log.
 */
inline Result QueryDeviceInfo(int32_t userId, int32_t infoType, uint32_t &output, const char *typeName)
{
    int64_t infoValue = 0;
    auto ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(userId), 0, infoType, &infoValue);
    if (ret != 0) {
        BM_LOG_ERROR("RtGetDeviceInfo(" << typeName << ") return=" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    output = static_cast<uint32_t>(infoValue);
    return BM_OK;
}

/**
 * @brief 获取本地设备信息，包括 phyDeviceId, sdid, serverId, superPodId。
 */
inline Result InitLocalDeviceInfo(uint32_t &phyDeviceId, uint32_t &sdid, uint32_t &serverId, uint32_t &superPodId)
{
    int32_t userId = -1;
    auto ret = DlAclApi::AclrtGetDevice(&userId);
    if (ret != 0 || userId < 0) {
        BM_LOG_ERROR("AclrtGetDevice() return=" << ret << ", output deviceId=" << userId);
        return BM_DL_FUNCTION_FAILED;
    }
    int32_t phyId = 0;
    ret = DlAclApi::AclrtGetPhyDevIdByLogicDevId(userId, &phyId);
    if (ret != 0) {
        BM_LOG_ERROR("AclrtGetPhyDevIdByLogicDevId() return=" << ret << ", userDeviceId=" << userId);
        return BM_DL_FUNCTION_FAILED;
    }
    phyDeviceId = static_cast<uint32_t>(phyId);
    ret = QueryDeviceInfo(userId, INFO_TYPE_SDID, sdid, "INFO_TYPE_SDID");
    if (ret != BM_OK) {
        return ret;
    }
    ret = QueryDeviceInfo(userId, INFO_TYPE_SERVER_ID, serverId, "INFO_TYPE_SERVER_ID");
    if (ret != BM_OK) {
        return ret;
    }
    ret = QueryDeviceInfo(userId, INFO_TYPE_SUPER_POD_ID, superPodId, "INFO_TYPE_SUPER_POD_ID");
    if (ret != BM_OK) {
        return ret;
    }
    BM_LOG_INFO("local device info: phyId=" << phyDeviceId << " sdid=" << sdid << " serverId=" << serverId
                                            << " superPodId=" << superPodId);
    return BM_OK;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_DEVICE_KERNEL_HELPER_H
