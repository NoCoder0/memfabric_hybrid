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
#ifndef MF_GENETTY_DEVICE_CQ_QP_H
#define MF_GENETTY_DEVICE_CQ_QP_H

#include "genetty_verbs_device_wrapper.h"

namespace ock {
namespace openDCC {
namespace verbs {
class VerbsDeviceCQ final : public GenettyCQ {
public:
    explicit VerbsDeviceCQ(genetty_type_t type, const DeviceRdmaContextPtr &ctx) : GenettyCQ(type), ctx_(ctx) {}
    ~VerbsDeviceCQ() override
    {
        Destroy();
    }
    Result Initialize(const GenettyCQOptions &options) noexcept override;
    void Destroy() noexcept override;
    int32_t Poll(int32_t num, genetty_cq_wc_t *wc, uint32_t flags) noexcept override;
    int32_t GetEventFd(int32_t *fd, uint32_t flags) noexcept override;
    int32_t Rearm(int32_t solicited, uint32_t flags) noexcept override;
    int32_t Ack(int32_t count, uint32_t flags) noexcept override;
    Result GetDeviceAccessInfo(uint32_t flags, genetty_cq_device_access_info_t *info) noexcept override;
    Result Bind(const GenettyOptions &options, uint64_t id, DeviceVerbsQueuePtr &queue) noexcept;

private:
    const DeviceRdmaContextPtr ctx_; /* hold the RDMA context */
    DeviceVerbsQueuePtr queue_;      /* HCCP QP and its completion queues */
    uint32_t depth_ = 0;             /* completion queue depth */
    bool inited_ = false;            /* initialized or not */
    std::mutex mutex_;               /* object mutex */
    friend class VerbsDeviceQp;
};
class VerbsDeviceQp final : public Genetty {
public:
    explicit VerbsDeviceQp(genetty_type_t type) : Genetty(type) {}
    ~VerbsDeviceQp() override
    {
        Destroy();
    }
    Result Initialize(const GenettyOptions &options) noexcept override;
    void Destroy() noexcept override;
    Result Export(uint32_t flags, genetty_link_info_t *info) override;
    Result Import(uint32_t flags, genetty_link_info_t *info) override;
    Result BatchPost(genetty_batch_op_data_t *data, uint32_t count,
                     genetty_batch_op_options_t *options) noexcept override;
    Result GetDeviceAccessInfo(uint32_t flags, genetty_device_access_info_t *info) noexcept override;

private:
    DeviceRdmaContextPtr ctx_;  /* hold the RDMA context */
    DeviceVerbsQueuePtr queue_; /* HCCP QP and its completion queues */
    GenettyCQPtr cq_;           /* hold the logical CQ */
    std::mutex mutex_;          /* object mutex */
};
} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_DEVICE_CQ_QP_H
