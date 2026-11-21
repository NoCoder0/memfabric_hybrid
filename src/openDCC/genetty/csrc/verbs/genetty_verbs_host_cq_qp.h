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
#ifndef MF_OPENDCC_GENETTY_VERBS_HOST_H
#define MF_OPENDCC_GENETTY_VERBS_HOST_H

#include "genetty_verbs_host_wrapper.h"

namespace ock {
namespace openDCC {
namespace verbs {
class VerbsHostCQ final : public GenettyCQ {
public:
    explicit VerbsHostCQ(genetty_type_t t, const VerbsContextPtr &ctx) noexcept : GenettyCQ(t), ctx_(ctx) {}
    ~VerbsHostCQ() noexcept override
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

    const VerbsCQPtr &Cq() const noexcept
    {
        return cq_;
    }

private:
    Result VerifyOptions(const GenettyCQOptions &options) noexcept;
    void DestroyInner() noexcept;

private:
    const VerbsContextPtr ctx_;
    VerbsCQPtr cq_;
    GenettyCQOptions options_;
    std::mutex mutex_;
    bool inited_ = false;

    friend class VerbsHostQp;
};

ALWAYS_INLINE int32_t VerbsHostCQ::Rearm(int32_t solicited_only, uint32_t flags) noexcept
{
    auto result = cq_->Rearm(solicited_only != 0);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Rearm cq failed, cq(" << id_ << "), result(" << result << "), solicited only(" << solicited_only
                                             << "), flags(" << flags << ")");
        return result;
    }

    return GEN_OK;
}

ALWAYS_INLINE int32_t VerbsHostCQ::Ack(int32_t eventCount, uint32_t flags) noexcept
{
    /* caller must ensure the param is valid */
    cq_->Ack(static_cast<uint32_t>(eventCount));
    return GEN_OK;
}

class VerbsHostQp final : public Genetty {
public:
    Result Quiesce() noexcept override;
    explicit VerbsHostQp(genetty_type_t t) noexcept : Genetty(t) {}
    ~VerbsHostQp() noexcept override
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
    void DestroyInner() noexcept;

private:
    VerbsQPPtr qp_;          /* real qp */
    GenettyOptions options_; /* options of the genetty */
    std::mutex mutex_;       /* mutex */
    bool inited_ = false;    /* init flag */
};
} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_GENETTY_VERBS_HOST_H
