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
#include "hcc_o2o_tunnel.h"

#include <algorithm>
#include <thread>

namespace ock {
namespace openDCC {
Result HccO2OTunnel::Initialize(genetty_context_t context, const hcc_tunnel_options_t &options)
{
    genetty_cq_options_t cqOptions{};
    genetty_cq_options_init(&cqOptions);
    cqOptions.context = context;
    cqOptions.queue_depth = options.queue_depth;
    cq_ = genetty_cq_create(&cqOptions);
    HCC_ASSERT_MSG_RETURN(cq_ != nullptr, "Create HCC CQ failed", HCC_DEVICE_ERROR);
    genetty_options_t qpOptions{};
    genetty_options_init(&qpOptions);
    qpOptions.cq_handle = cq_;
    qpOptions.sq_depth = options.queue_depth;
    qpOptions.rq_depth = options.queue_depth;
    qpOptions.traffic_class = options.traffic_class;
    qp_ = genetty_create(&qpOptions);
    if (qp_ == nullptr) {
        genetty_cq_destroy(cq_, 0);
        cq_ = nullptr;
        HCC_LOG_ERROR("Create HCC QP failed");
        return HCC_DEVICE_ERROR;
    }
    return HCC_OK;
}

Result HccO2OTunnel::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    HCC_ASSERT_MSG_RETURN(request_ == nullptr, "Destroy tunnel with live request", HCC_BUSY);
    if (qp_ != nullptr) {
        auto ret = genetty_quiesce(qp_);
        HCC_ASSERT_MSG_RETURN(ret == GEN_OK, "Quiesce HCC QP failed, result(" << ret << ")", HCC_DEVICE_ERROR);
        genetty_destroy(qp_, 0);
        qp_ = nullptr;
    }
    if (cq_ != nullptr) {
        genetty_cq_destroy(cq_, 0);
        cq_ = nullptr;
    }
    ready_ = false;
    stopped_ = true;
    return HCC_OK;
}

Result HccO2OTunnel::Quiesce()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto ret = qp_ == nullptr ? GEN_OK : genetty_quiesce(qp_);
    HCC_ASSERT_MSG_RETURN(ret == GEN_OK, "Quiesce HCC tunnel failed, result(" << ret << ")", HCC_DEVICE_ERROR);
    stopped_ = true;
    ready_ = false;
    if (request_ != nullptr && request_->result == HCC_BUSY) {
        request_->posted = false;
        request_->result = HCC_DEVICE_ERROR;
    }
    return HCC_OK;
}

Result HccO2OTunnel::ExportLinkInfo(uint32_t flags, hcc_link_info_t *info) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto ret = genetty_export(qp_, flags, info);
    HCC_ASSERT_MSG_RETURN(ret == GEN_OK, "Export HCC link failed, result(" << ret << ")", HCC_DEVICE_ERROR);
    return HCC_OK;
}

Result HccO2OTunnel::ImportLinkInfo(uint32_t flags, hcc_link_info_t *info) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    HCC_ASSERT_MSG_RETURN(!stopped_ && request_ == nullptr, "Import busy or stopped tunnel", HCC_BUSY);
    auto ret = genetty_import(qp_, info, flags);
    HCC_ASSERT_MSG_RETURN(ret == GEN_OK, "Import HCC link failed, result(" << ret << ")", HCC_CONNECT_FAILED);
    ready_ = true;
    return HCC_OK;
}

Result HccO2OTunnel::Submit(genetty_op_type_t operation, const hcc_copy_desc_t *descs, uint32_t count,
                            hcc_request_t *out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HCC_ASSERT_MSG_RETURN(ready_ && !stopped_, "HCC tunnel is not ready", HCC_CONNECT_FAILED);
    HCC_ASSERT_MSG_RETURN(request_ == nullptr, "HCC tunnel has an active batch", HCC_BUSY);
    auto request = std::make_unique<HccRequest>();
    request->operation = operation;
    request->descriptors.assign(descs, descs + count);
    for (const auto &desc : request->descriptors) {
        HCC_ASSERT_MSG_RETURN(desc.size != 0 && desc.size <= UINT64_MAX - desc.remote_address,
                              "Invalid HCC remote range", HCC_INVALID_PARAM);
        auto memory = owner_->FindMemory(desc.local_address, desc.size, desc.lkey);
        if (memory == nullptr) {
            return HCC_INVALID_PARAM;
        }
        request->memories.push_back(memory);
    }
    request->tunnel = this;
    request_ = request.release();
    *out = request_;
    return HCC_OK;
}

Result HccO2OTunnel::Fail(HccRequest &request, Result error)
{
    HCC_LOG_ERROR("HCC batch failed, request(" << request.completion << "), result(" << error << ")");
    ready_ = false;
    stopped_ = true;
    auto ret = genetty_quiesce(qp_);
    if (ret != GEN_OK) {
        HCC_LOG_ERROR("Cannot stop HCC DMA, result(" << ret << ")");
        return HCC_DEVICE_ERROR;
    }
    request.posted = false;
    request.result = error;
    return error;
}

Result HccO2OTunnel::Progress(HccRequest &request)
{
    if (request.posted) {
        genetty_cq_wc_t wc{};
        auto count = genetty_cq_poll(cq_, 1, &wc, 0);
        if (count < 0 || (count > 0 && (wc.status != OPS_SUCCESS || wc.usr_ctx != request.completion ||
                                        wc.type != request.operation))) {
            return Fail(request, HCC_DEVICE_ERROR);
        }
        if (count == 0) {
            return HCC_BUSY;
        }
        request.posted = false;
        request.offset += request.postedSize;
        if (request.offset == request.descriptors[request.index].size) {
            request.offset = 0;
            ++request.index;
        }
    }
    if (request.index == request.descriptors.size()) {
        request.result = HCC_OK;
        return HCC_OK;
    }
    const auto &desc = request.descriptors[request.index];
    genetty_batch_op_data_t data{};
    data.laddress = reinterpret_cast<void *>(desc.local_address + request.offset);
    data.raddress = reinterpret_cast<void *>(desc.remote_address + request.offset);
    data.lsize = data.rsize = static_cast<uint32_t>(std::min<uint64_t>(desc.size - request.offset, UINT32_MAX));
    data.lkey = desc.lkey;
    data.rkey = desc.rkey;
    genetty_batch_op_options_t options{};
    options.op_type = request.operation;
    options.signal = options.kick_db = 1;
    options.usr_ctx = request.completion = ++nextId_;
    request.postedSize = data.lsize;
    request.posted = true;
    auto ret = genetty_batch_post(qp_, &data, 1, &options);
    return ret == GEN_OK ? HCC_BUSY : Fail(request, HCC_POST_FAILED);
}

Result HccO2OTunnel::Wait(HccRequest &request, uint32_t timeoutMs)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (request.result != HCC_BUSY) {
                return request.result;
            }
            HCC_ASSERT_MSG_RETURN(!stopped_, "HCC tunnel requires DMA quiescence", HCC_DEVICE_ERROR);
            auto ret = Progress(request);
            if (ret != HCC_BUSY) {
                return ret;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return Fail(request, HCC_TIMEOUT);
            }
        }
        std::this_thread::yield();
    }
}

Result HccO2OTunnel::Release(HccRequest *request)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HCC_ASSERT_MSG_RETURN(request == request_ && request->result != HCC_BUSY && !request->posted,
                          "Release unfinished HCC request", HCC_BUSY);
    request_ = nullptr;
    return HCC_OK;
}
} // namespace openDCC
} // namespace ock
