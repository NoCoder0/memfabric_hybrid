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
#include "genetty_verbs_device_cq_qp.h"
namespace ock {
namespace openDCC {
namespace verbs {
Result VerbsDeviceCQ::Initialize(const GenettyCQOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(ctx_ != nullptr && ctx_->Handle() != nullptr && options.queueDepth != 0 &&
                              options.queueDepth <= INT32_MAX && options.eventFd == UINT32_MAX,
                          "Invalid Device CQ context, depth or event fd", GEN_INVALID_PARAM);
    depth_ = options.queueDepth;
    id_ = GENETTY_CQ_ID_GENERATOR.fetch_add(1, std::memory_order_relaxed);
    inited_ = true;
    return GEN_OK;
}

Result VerbsDeviceCQ::Bind(const GenettyOptions &options, uint64_t id, DeviceVerbsQueuePtr &queue) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(inited_ && queue_ == nullptr, "Device CQ supports one QP", GEN_INVALID_PARAM);
    auto result = ctx_->CreateQueue(options, depth_, id, queue);
    if (result == GEN_OK) {
        queue_ = queue;
    }
    return result;
}

void VerbsDeviceCQ::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_ != nullptr) {
        (void)ctx_->DestroyQueue(queue_);
        queue_ = nullptr;
    }
    inited_ = false;
}

int32_t VerbsDeviceCQ::Poll(int32_t num, genetty_cq_wc_t *wc, uint32_t flags) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(inited_ && queue_ != nullptr, "Device CQ has no active QP", GEN_NOT_INITIALIZED);
    return queue_->Poll(num, wc);
}

int32_t VerbsDeviceCQ::GetEventFd(int32_t *fd, uint32_t flags) noexcept
{
    GEN_LOG_ERROR("Device CQ event notification is not supported");
    return GEN_ERROR;
}
int32_t VerbsDeviceCQ::Rearm(int32_t solicited, uint32_t flags) noexcept
{
    GEN_LOG_ERROR("Device CQ rearm is not supported");
    return GEN_ERROR;
}
int32_t VerbsDeviceCQ::Ack(int32_t count, uint32_t flags) noexcept
{
    GEN_LOG_ERROR("Device CQ event acknowledgement is not supported");
    return GEN_ERROR;
}
Result VerbsDeviceCQ::GetDeviceAccessInfo(uint32_t flags, genetty_cq_device_access_info_t *info) noexcept
{
    GEN_LOG_ERROR("Device CQ raw address export is not supported");
    return GEN_ERROR;
}

Result VerbsDeviceQp::Initialize(const GenettyOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto cq = dynamic_cast<VerbsDeviceCQ *>(options.cq.Get());
    GEN_ASSERT_MSG_RETURN(cq != nullptr && queue_ == nullptr, "Invalid Device QP CQ", GEN_INVALID_PARAM);
    id_ = GENETTY_ID_GENERATOR.fetch_add(1, std::memory_order_relaxed);
    auto result = cq->Bind(options, id_, queue_);
    if (result != GEN_OK) {
        return result;
    }
    ctx_ = cq->ctx_;
    cq_ = options.cq;
    return GEN_OK;
}

void VerbsDeviceQp::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_ != nullptr && ctx_ != nullptr) {
        (void)ctx_->DestroyQueue(queue_);
    }
    queue_ = nullptr;
    cq_ = nullptr;
    ctx_ = nullptr;
}

Result VerbsDeviceQp::Export(uint32_t flags, genetty_link_info_t *info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(queue_ != nullptr, "Device QP is not initialized", GEN_NOT_INITIALIZED);
    return queue_->Export(info, flags);
}
Result VerbsDeviceQp::Import(uint32_t flags, genetty_link_info_t *info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(queue_ != nullptr, "Device QP is not initialized", GEN_NOT_INITIALIZED);
    return queue_->Import(info, flags);
}
Result VerbsDeviceQp::BatchPost(genetty_batch_op_data_t *data, uint32_t count,
                                genetty_batch_op_options_t *options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(queue_ != nullptr, "Device QP is not initialized", GEN_NOT_INITIALIZED);
    return queue_->Post(data, count, options);
}
Result VerbsDeviceQp::GetDeviceAccessInfo(uint32_t flags, genetty_device_access_info_t *info) noexcept
{
    GEN_LOG_ERROR("Device QP raw address export is not supported");
    return GEN_ERROR;
}
} // namespace verbs
} // namespace openDCC
} // namespace ock
