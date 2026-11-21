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
#ifndef MF_GENETTY_URMA_HOST_CQ_QP_H
#define MF_GENETTY_URMA_HOST_CQ_QP_H

#include "genetty_urma_host_ctx_pool.h"

namespace ock {
namespace openDCC {
namespace urma {
class UrmaHostCQ final : public GenettyCQ {
public:
    explicit UrmaHostCQ(genetty_type_t t, const UrmaHostContextPtr &ctx) noexcept : GenettyCQ(t), hostCtx_(ctx) {}
    ~UrmaHostCQ() noexcept override
    {
        /* since this is a final class, we can call virtual function in deconstructor */
        Destroy();
    }

    Result Initialize(const GenettyCQOptions &options) noexcept override;
    void Destroy() noexcept override;

    int32_t GetEventFd(int32_t *fd, uint32_t flags) noexcept override;
    int32_t Poll(int32_t num, genetty_cq_wc_t *wc, uint32_t flags) noexcept override;
    int32_t Rearm(int32_t solicited_only, uint32_t flags) noexcept override;
    int32_t Ack(int32_t eventCount, uint32_t flags) noexcept override;
    Result GetDeviceAccessInfo(uint32_t flags, genetty_cq_device_access_info_t *info) noexcept override;

    const UrmaHostContextPtr &HostContext() const noexcept
    {
        return hostCtx_;
    }

    const UrmaJfcPtr &Jfc() const noexcept
    {
        return jfc_;
    }

private:
    Result VerifyOptions(const GenettyCQOptions &options) noexcept;

private:
    const UrmaHostContextPtr hostCtx_; /* context of this cq */
    UrmaJfcPtr jfc_;                   /* real jfc */
    GenettyCQOptions options_;         /* options of the cq */
    std::mutex mutex_;                 /* mutex */
    bool inited_ = false;              /* init flag */
};

class UrmaHostQp final : public Genetty {
public:
    explicit UrmaHostQp(genetty_type_t t) noexcept : Genetty(t) {}
    ~UrmaHostQp() noexcept override
    {
        /* since this is a final class, we can call virtual function in deconstructor */
        Destroy();
    }

    Result Initialize(const GenettyOptions &options) noexcept override;
    void Destroy() noexcept override;

    Result Export(uint32_t flags, genetty_link_info_t *info) override;
    Result Import(uint32_t flags, genetty_link_info_t *info) override;

    Result BatchPost(genetty_batch_op_data_t *op_data, uint32_t batchSize,
                     genetty_batch_op_options_t *options) noexcept override;
    Result GetDeviceAccessInfo(uint32_t flags, genetty_device_access_info_t *info) noexcept override;

private:
    Result VerifyOptions(const GenettyOptions &options) noexcept;
    Result PostOne(genetty_batch_op_data_t &opData, genetty_batch_op_options_t &options) noexcept;

private:
    UrmaHostContextPtr ctx_;  /* context of this qp */
    UrmaJfsPtr jfs_;          /* real jfs */
    UrmaJfrPtr jfr_;          /* real jfr */
    UrmaJettyPtr jetty_;      /* real jetty */
    GenettyOptions options_;  /* options of the genetty */
    std::mutex mutex_;        /* mutex */
    bool inited_ = false;     /* init flag */
};
} // namespace urma
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_URMA_HOST_CQ_QP_H
