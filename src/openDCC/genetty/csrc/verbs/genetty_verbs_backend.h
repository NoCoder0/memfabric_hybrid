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
#ifndef MF_GENETTY_VERBS_MANAGER_H
#define MF_GENETTY_VERBS_MANAGER_H

#include "genetty_verbs_types.h"
#include "genetty_verbs_device_ctx_pool.h"
#include "genetty_verbs_host_ctx_pool.h"

namespace ock {
namespace openDCC {
namespace verbs {
class VerbsBackend : public GenettyBackend {
public:
    static VerbsBackend &Instance()
    {
        static VerbsBackend INSTANCE;
        return INSTANCE;
    }

public:
    Result Initialize(const GenettyBackendOptions &options) noexcept override;
    void Destroy() noexcept override;
    Result GetDevicesInfo(std::vector<GenettyDeviceInfo> &info) noexcept override;
    Result GetContext(const GenettyCtxGetOptions &ctxOptions, genetty_context_t *context) noexcept override;
    void ReturnContext(genetty_context_t *context, uint32_t flags) noexcept override;

    genetty_cq_t GenettyCqCreate(genetty_cq_options_t *options) noexcept override;
    Result GenettyCqDestroy(genetty_cq_t cq, uint32_t flags) noexcept override;

    genetty_t GenettyCreate(genetty_options_t *options) noexcept override;
    Result GenettyDestroy(genetty_t handle, uint32_t flags) noexcept override;

private:
    Result InitInner(const GenettyBackendOptions &options) noexcept;
    Result GetHostContext(const GenettyCtxGetOptions &ctxOptions, genetty_context_t *context) noexcept;
    Result GetDeviceContext(const GenettyCtxGetOptions &ctxOptions, genetty_context_t *context) noexcept;
    void ReturnDeviceContext(genetty_context_t *context, uint32_t flags) noexcept;
    GenettyCQPtr CreateDeviceCQ(const GenettyCQOptions &options) noexcept;
    GenettyCQPtr DoCreateCQ(genetty_cq_options_t *options) noexcept;
    GenettyPtr DoCreateGenetty(genetty_options_t *options) noexcept;

private:
    bool inited_ = false;                       /* inited or not */
    std::map<uint64_t, GenettyCQPtr> allCq_;    /* address to cq map */
    std::map<uint64_t, GenettyPtr> allGenetty_; /* address to genetty map */
    std::mutex mutex_;                          /* map mutex */
    HostVerbsContextPoolPtr hostCtxPool_;       /* context pool */
    DeviceVerbsContextPoolPtr deviceCtxPool_;   /* device context pool */
    GenettyBackendOptions options_;             /* backend options */
};
} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_VERBS_MANAGER_H
