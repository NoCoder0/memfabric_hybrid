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
#ifndef MF_GENETTY_DEVICE_QUEUE_H
#define MF_GENETTY_DEVICE_QUEUE_H

#include "genetty_verbs_device_ts.h"
#include "genetty_verbs_info.h"
#include "under_api/hccp/genetty_dl_hccp_api.h"

namespace ock {
namespace openDCC {
namespace verbs {
class DeviceVerbsQueue : public Referable {
public:
    Result Initialize(void *rdev, uint32_t userId, uint32_t logicId, const GenettyOptions &options, uint32_t cqDepth,
                      uint64_t id) noexcept;
    Result Destroy() noexcept;
    Result Export(genetty_link_info_t *info, uint32_t flags) noexcept;
    Result Import(genetty_link_info_t *info, uint32_t flags) noexcept;
    Result Post(genetty_batch_op_data_t *data, uint32_t count, genetty_batch_op_options_t *options) noexcept;
    int32_t Poll(int32_t count, genetty_cq_wc_t *wc) noexcept;
    bool CleanupPending() noexcept;

private:
    std::mutex mutex_;                      /* mutex of the queue */
    void *qp_ = nullptr;                    /* HCCP QP handle */
    DeviceTsWriter ts_;                     /* TS queue used to ring the RDMA doorbell */
    HccpQpAttr attr_{};                     /* local QP attributes */
    GenettyOptions options_;                /* QP options without a CQ reference */
    uint64_t id_ = 0;                       /* genetty id reported in completions */
    uint32_t userId_ = 0;                   /* ACL user device id */
    uint32_t logicId_ = 0;                  /* HAL logical device id */
    uint64_t requestId_ = 0;                /* outstanding request id */
    genetty_op_type_t operation_ = OP_READ; /* outstanding operation */
    bool connected_ = false;                /* QP has reached RTS */
    bool tsReady_ = false;                  /* TS resources are initialized */
    bool pending_ = false;                  /* waiting for an RDMA completion */
    bool failed_ = false;                   /* reject new requests after a transport error */
    bool closing_ = false;                  /* queue destruction has started */
    bool destroyFailed_ = false;            /* HCCP consumed the handle but reported a cleanup error */
};
using DeviceVerbsQueuePtr = Ref<DeviceVerbsQueue>;
} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_DEVICE_QUEUE_H
