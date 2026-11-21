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
#ifndef MF_GENETTY_URMA_BACKEND_H
#define MF_GENETTY_URMA_BACKEND_H

#include <map>
#include <mutex>

#include "genetty_urma_host_ctx_pool.h"
#include "genetty_types.h"

namespace ock {
namespace openDCC {
namespace urma {
class UrmaBackend : public GenettyBackend {
public:
    static UrmaBackend &Instance()
    {
        static UrmaBackend INSTANCE;
        return INSTANCE;
    }

public:
    Result Initialize(const GenettyBackendOptions &options) noexcept override;
    void Destroy() noexcept override;
    Result GetDevicesInfo(std::vector<GenettyDeviceInfo> &info) noexcept override;
    Result GetContext(const GenettyCtxGetOptions &options, genetty_context_t *context) noexcept override;
    void ReturnContext(genetty_context_t *context, uint32_t flags) noexcept override;

    genetty_cq_t GenettyCqCreate(genetty_cq_options_t *options) noexcept override;
    Result GenettyCqDestroy(genetty_cq_t cq, uint32_t flags) noexcept override;

    genetty_t GenettyCreate(genetty_options_t *options) noexcept override;
    Result GenettyDestroy(genetty_t handle, uint32_t flags) noexcept override;

private:
    Result InitInner(const GenettyBackendOptions &options) noexcept;
    GenettyCQPtr CreateCQByType(genetty_cq_options_t *options) noexcept;
    GenettyPtr CreateGenettyByType(genetty_options_t *options) noexcept;

private:
    bool inited_ = false;                          /* inited or not */
    std::map<uint64_t, GenettyCQPtr> allCq_;       /* address to cq map */
    std::map<uint64_t, GenettyPtr> allGenetty_;    /* address to genetty map */
    UrmaHostContextPoolPtr ctxPool_;               /* context pool */
    std::mutex mutex_;                             /* map mutex */
    GenettyBackendOptions options_;                /* backend options */
};
} // namespace urma
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_URMA_BACKEND_H
