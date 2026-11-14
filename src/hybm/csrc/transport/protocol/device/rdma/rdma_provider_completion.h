/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef MF_PROVIDER_RDMA_COMPLETION_H
#define MF_PROVIDER_RDMA_COMPLETION_H

#include <cstdint>
#include <memory>
#include <netinet/in.h>

#include "dl_hccp_def.h"

namespace ock {
namespace mf {
class HybmStream;
namespace transport {
namespace device {
class RdmaProviderSignal {
public:
    RdmaProviderSignal(uint32_t userId, void *rdev) : userId_(userId), rdev_(rdev) {}
    ~RdmaProviderSignal();
    RdmaProviderSignal(const RdmaProviderSignal &) = delete;
    RdmaProviderSignal &operator=(const RdmaProviderSignal &) = delete;

    bool Init();
    bool Close();
    uint64_t GetAddress() const
    {
        return reinterpret_cast<uint64_t>(info_.addr);
    }
    uint32_t GetRkey() const
    {
        return info_.rkey;
    }

private:
    const uint32_t userId_;
    void *const rdev_;
    void *buffer_{nullptr};
    void *mr_{nullptr};
    HccpMrInfo info_{};
    bool ready_{false};
};

class RdmaProviderCompletion {
public:
    RdmaProviderCompletion(uint32_t userId, uint32_t logicId, void *rdev, bool useNativeStream = false)
        : userId_(userId), logicId_(logicId), rdev_(rdev), useNativeStream_(useNativeStream)
    {}
    ~RdmaProviderCompletion();
    RdmaProviderCompletion(const RdmaProviderCompletion &) = delete;
    RdmaProviderCompletion &operator=(const RdmaProviderCompletion &) = delete;

    bool Init();
    bool SubmitFenceRead(void *qp, uint64_t remoteSignalAddr, uint32_t remoteSignalRkey);
    bool WaitFenceNotify();
    bool Close();
    bool UsesNativeStream() const
    {
        return useNativeStream_;
    }
    bool SubmitNativeDoorbell(const send_wr_rsp &response);

private:
    bool WaitNativeNotify();
    bool SubmitDoorbell(const send_wr_rsp &response);
    bool WaitRuntimeNotify();

    const uint32_t userId_;
    const uint32_t logicId_;
    void *const rdev_;
    const bool useNativeStream_;
    std::shared_ptr<HybmStream> nativeStream_;
    uint32_t notifyId_{UINT32_MAX};
    uint64_t nativeDoorbellAddress_{0};
    void *stream_{nullptr};
    void *notify_{nullptr};
    uint64_t notifyAddress_{0};
    uint32_t notifyLkey_{0};
    uint64_t nextWrId_{0};
    bool ready_{false};
    bool failed_{false};
};
} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
#endif // MF_PROVIDER_RDMA_COMPLETION_H
