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

#pragma once

#include <memory>
#include <mutex>

#include "dl_hccp_def.h"
#include "provider_device_context.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

class RdmaProviderControlServer;
class RdmaProxyTransportManager;

struct RdmaProviderDevice {
    ~RdmaProviderDevice();
    uint32_t userId{0};
    uint32_t logicId{0};
    uint32_t phyId{0};
    void *rdmaHandle{nullptr};
    void *mrHandle{nullptr};
    void *dvaBase{nullptr};
    void *hostAddr{nullptr};
    uint64_t mrSize{0};
    HccpMrInfo mrInfo{};
    std::mutex qpMutex;
    std::shared_ptr<RdmaProviderDeviceContext> context;

    bool Open(uint32_t userDeviceId, uint32_t rankCount);
    void Close();
    bool RegisterMemory(void *hostAddr, uint64_t size);
    bool RegisterMr(void *addr, uint64_t size, bool isHbm, HccpMrInfo &info, void *&mrHandle);
    void UnregisterMemory();
    bool CreateQp(void *&qpHandle, TypicalQp &localQp);
    bool ModifyQp(void *qpHandle, TypicalQp &localQp, TypicalQp &remoteQp);
    bool UsesNotifyCompletion() const;
    bool GetNotifySource(uint64_t &addr, uint32_t &rkey);
    // 只提交一笔数据 WR（含敲 doorbell），不做完成等待：批量提交 N 笔后由 WaitTransfer 统一等一次。
    bool SubmitTransfer(void *qpHandle, uint64_t localDva, uint32_t lkey, uint64_t remoteAddr, uint32_t rkey,
                        uint64_t size, bool write);
    // 一次完成等待，覆盖该 QP 上此前提交的全部 WR（fence 读 + notify），实现"一批一次往返"。
    bool WaitTransfer(void *qpHandle, uint64_t notifySourceAddr, uint32_t notifySourceRkey);
    void DestroyQp(void *qpHandle);

private:
    friend class RdmaProviderControlServer;
    friend class RdmaProxyTransportManager;
    bool TryDestroyQp(void *&qpHandle);
    bool cleanupBlocked_{false};
};

bool RdmaProviderLinkReady(const RdmaProviderDevice &device, void *qp, int fd);
bool ReleaseRegisteredMr(RdmaProviderDevice &device, void *&handle, void *hostAddress, bool isHbm);

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
