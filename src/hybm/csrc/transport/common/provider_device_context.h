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
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "vendor/vendor_api.h"
#include "vendor/ascend/dl_hccp_def.h"
#include "vendor/ascend/dl_hcomm_api.h"
#include "hybm_common_include.h"
namespace ock::mf::transport::device {
// Defined in rdma_provider_completion.h and only held through unique_ptr here, so this header stays protocol-light.
class RdmaProviderSignal;
class RdmaProviderCompletion;

// ACL context guard for the URMA provider: HCCL caches the current device per thread independently of the ACL
// context, so switching context alone is not enough.
class UrmaProviderContext {
public:
    explicit UrmaProviderContext(void *context = nullptr)
    {
        VendorApi::Api()->GetCurrentContext(&previous_);
        valid_ = context == nullptr || SetCurrentContext(context) == BM_OK;
    }
    ~UrmaProviderContext()
    {
        if (previous_ != nullptr && SetCurrentContext(previous_) != BM_OK)
            BM_LOG_ERROR("URMA provider cannot restore caller ACL context");
    }
    static Result SetCurrentContext(void *context)
    {
        auto ret = VendorApi::Api()->SetCurrentContext(context);
        if (ret != BM_OK) {
            BM_LOG_ERROR("URMA provider cannot activate ACL context, ret=" << ret);
            return ret;
        }
        // HCCL caches the device per thread independently of the ACL context.
        ret = DlHcommApi::RefreshCurrentDevice();
        if (ret != BM_OK) {
            BM_LOG_ERROR("URMA provider cannot refresh HCCL current device, ret=" << ret);
        }
        return ret;
    }
    bool Valid() const
    {
        return valid_;
    }

private:
    void *previous_{nullptr};
    bool valid_{false};
};

// Device state shared by every RdmaProviderDevice instance of the same user device id: the completion registry has to
// outlive the QPs that use it, so the instances share this object instead of owning their own copy.
struct RdmaProviderDeviceContext {
    uint32_t userId{0};
    uint32_t logicId{0};
    uint32_t phyId{0};
    uint32_t deviceIp{0};
    void *rdmaHandle{nullptr};
    void *stream{nullptr};
    bool netServiceOwned{false};
    bool raInitialized{false};
    bool ownsRdev{false};
    HccpRaInitConfig init{};
    std::mutex transferMutex;
    std::unique_ptr<RdmaProviderSignal> notifySource;
    std::unordered_map<void *, std::unique_ptr<RdmaProviderCompletion>> completions;
    std::atomic_bool completionCleanupBlocked{false};

    ~RdmaProviderDeviceContext();
    bool Open(uint32_t userDeviceId, uint32_t rankCount);
    bool OpenStream();
    bool CloseCompletionResources();
};
} // namespace ock::mf::transport::device
