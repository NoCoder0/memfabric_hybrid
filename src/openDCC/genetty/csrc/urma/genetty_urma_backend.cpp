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
#include "genetty_urma_backend.h"

#include <cstdio>

#include "genetty_host_urma_wrapper.h"
#include "genetty_urma_host_cq_qp.h"

namespace ock {
namespace openDCC {
namespace urma {
Result UrmaBackend::Initialize(const GenettyBackendOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Already initialized");
        return GEN_OK;
    }

    /* do initialize */
    auto result = InitInner(options);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize failed, result(" << result << ")");
        return result;
    }

    /* mark inited */
    options_ = options;
    inited_ = true;

    GEN_LOG_DEBUG("Initialized successfully, protocol(" << options.protocol << ")");
    return GEN_OK;
}

Result UrmaBackend::InitInner(const GenettyBackendOptions &options) noexcept
{
    /* only the host urma protocol is served by this backend */
    if ((options.protocol & GEN_PROT_HOST_URMA) == 0) {
        GEN_LOG_DEBUG("Skip urma init, as protocol(" << options.protocol << ") has no host urma");
        return GEN_OK;
    }

    /* the urma device init does not load the api by itself, load it first */
    auto result = UrmaApi::Load();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Init urma backend failed, as load urma api failed, result(" << result << "), protocol("
                                                                                    << options.protocol << ")");
        return result;
    }

    result = UrmaUtil::UrmaInit();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Init urma backend failed, as init urma failed, result(" << result << "), protocol("
                                                                               << options.protocol << ")");
        return result;
    }

    /* create the context pool and add all devices */
    ctxPool_ = mf::MakeRef<UrmaHostContextPool>();
    if (UNLIKELY(ctxPool_ == nullptr)) {
        GEN_LOG_ERROR("Init urma backend failed, as ctxPool is null, protocol(" << options.protocol << ")");
        return GEN_MALLOC_FAILED;
    }

    result = ctxPool_->AddAllDevices();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Init urma backend failed, as add all devices failed, result(" << result << "), protocol("
                                                                                     << options.protocol << ")");
        ctxPool_ = nullptr;
        return result;
    }

    GEN_LOG_DEBUG("Init urma backend finished, context count(" << ctxPool_->Count() << ")");
    return GEN_OK;
}

void UrmaBackend::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Not initialized");
        return;
    }

    auto cqCount = allCq_.size();
    auto genettyCount = allGenetty_.size();
    auto contextCount = ctxPool_ == nullptr ? 0 : ctxPool_->Count();
    allCq_.clear();
    allGenetty_.clear();
    if (ctxPool_ != nullptr) {
        ctxPool_->CloseAllDevices();
        ctxPool_ = nullptr;
    }

    inited_ = false;

    GEN_LOG_DEBUG("Destroyed, cq count(" << cqCount << "), genetty count(" << genettyCount << "), context count("
                                         << contextCount << ")");
}

Result UrmaBackend::GetDevicesInfo(std::vector<GenettyDeviceInfo> &info) noexcept
{
    if ((options_.protocol & GEN_PROT_HOST_URMA) == GEN_PROT_HOST_URMA) {
        const auto &devices = UrmaDevice::AllDevices();
        if (devices.empty()) {
            GEN_LOG_WARN("Get devices info, no host urma device found");
            return GEN_OK;
        }

        uint16_t deviceId = 0;
        for (const auto &item : devices) {
            const auto &device = item.second;
            if (device == nullptr) {
                continue;
            }

            GenettyDeviceInfo deviceInfo;
            deviceInfo.deviceId = deviceId++;
            deviceInfo.active = false;

            const auto &attr = device->DeviceAttributes();
            if (attr.port_cnt > 0 && attr.port_attr[0].state == URMA_PORT_ACTIVE) {
                deviceInfo.active = true;
            }

            const auto &eidList = device->EidList();
            if (!eidList.empty()) {
                deviceInfo.portId = static_cast<uint8_t>(eidList[0].eid_index);
                char eidText[64] = {};
                snprintf(eidText, sizeof(eidText), EID_FMT, EID_ARGS(eidList[0].eid));
                deviceInfo.ip = eidText;
            }

            deviceInfo.deviceName = device->DeviceName();
            deviceInfo.type = HOST_URMA_GENETTY;
            info.emplace_back(deviceInfo);
        }
    }

    GEN_LOG_DEBUG("Get devices info finished, device count(" << info.size() << ")");
    return GEN_OK;
}

Result UrmaBackend::GetContext(const GenettyCtxGetOptions &options, genetty_context_t *context) noexcept
{
    GEN_ASSERT_MSG_RETURN(context != nullptr, "Invalid param, context is null", GEN_INVALID_PARAM);
    *context = nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(inited_, "Not initialized", GEN_NOT_INITIALIZED);
    GEN_ASSERT_MSG_RETURN(ctxPool_ != nullptr, "Not initialized, as context pool is null", GEN_NOT_INITIALIZED);

    /* step1: select the context from the context pool by the policy */
    UrmaHostContextPtr ctx;
    Result result = GEN_INVALID_PARAM;
    if (options.type == GCCP_BY_DEVICE_ID) {
        result = ctxPool_->SelectContext(options.deviceId, ctx);
    } else if (options.type == GCCP_BY_IP) {
        result = ctxPool_->SelectContext(options.ipMask, ctx);
    } else if (options.type == GCCP_AUTO) {
        result = ctxPool_->SelectContext(static_cast<uint16_t>(UINT16_MAX), ctx);
    } else {
        GEN_LOG_ERROR("Get context failed, as policy(" << options.type << ") is not supported");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Get context failed, as select context failed, result("
                      << result << "), policy(" << options.type << "), device id(" << options.deviceId << "), eid("
                      << options.ipMask << ")");
        return result;
    }

    /* step2: increase reference and output the context handle */
    ctx->IncreaseRef();
    *context = reinterpret_cast<genetty_context_t>(ctx.Get());
    GEN_LOG_DEBUG("Get context(" << ctx->Context()->Id() << ") finished, policy(" << options.type << ")");
    return GEN_OK;
}

void UrmaBackend::ReturnContext(genetty_context_t *context, uint32_t flags) noexcept
{
    GEN_ASSERT_MSG_NO_RETURN(context != nullptr && *context != nullptr, "Return context failed, as context is null");

    auto genettyCtx = reinterpret_cast<GenettyContext *>(*context);
    if (UNLIKELY(genettyCtx->Type() != HOST_URMA_GENETTY)) {
        GEN_LOG_ERROR("Return context failed, as type(" << genettyCtx->Type() << ") is not supported by urma backend");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_NO_RETURN(ctxPool_ != nullptr, "Return context failed, as context pool is null");

    UrmaHostContextPtr ctx;
    auto result = ctxPool_->Lookup(*context, ctx);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Return context failed, as look up context failed, result(" << result << "), context("
                                                                                  << *context << ")");
        return;
    }

    ctx->DecreaseRef();
    GEN_LOG_DEBUG("Return context(" << ctx->Context()->Id() << ") finished, flags(" << flags << ")");
}

GenettyCQPtr UrmaBackend::CreateCQByType(genetty_cq_options_t *options) noexcept
{
    /* step1: convert the c options to the cxx options */
    GenettyCQOptions cqOptions;
    cqOptions.eventFd = options->event_fd;
    cqOptions.queueDepth = options->queue_depth;
    cqOptions.flags = options->flags;
    cqOptions.preferred_ip_mask = options->preferred_ip_mask;
    cqOptions.lockFree = options->lock_free != 0;
    cqOptions.context = options->context;

    /* step2: look up the context passed in by the cq options */
    if (UNLIKELY(options->context == nullptr)) {
        GEN_LOG_ERROR("Create urma cq failed, as context is null, queue depth(" << cqOptions.queueDepth << ")");
        return nullptr;
    }

    if (UNLIKELY(ctxPool_ == nullptr)) {
        GEN_LOG_ERROR("Create urma cq failed, as context pool is null, queue depth(" << cqOptions.queueDepth << ")");
        return nullptr;
    }

    UrmaHostContextPtr hostCtx;
    auto result = ctxPool_->Lookup(options->context, hostCtx);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Create urma cq failed, as look up context failed, result(" << result << "), context("
                                                                                  << options->context << ")");
        return nullptr;
    }

    /* step3: create and initialize the urma host cq */
    GenettyCQPtr cq = mf::MakeRef<UrmaHostCQ>(HOST_URMA_GENETTY, hostCtx).Get();
    if (UNLIKELY(cq == nullptr)) {
        GEN_LOG_ERROR("Create urma cq failed, probably out of memory, queue depth(" << cqOptions.queueDepth << ")");
        return nullptr;
    }

    result = cq->Initialize(cqOptions);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Create urma cq failed, as initialize failed, result(" << result << "), queue depth("
                                                                            << cqOptions.queueDepth << ")");
        return nullptr;
    }

    return cq;
}

genetty_cq_t UrmaBackend::GenettyCqCreate(genetty_cq_options_t *options) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Create cq failed, as options is null", nullptr);

    /* step2: create and initialize the cq by the genetty type */
    auto cq = CreateCQByType(options);
    if (UNLIKELY(cq == nullptr)) {
        GEN_LOG_ERROR("Create cq failed, as create cq by type failed");
        return nullptr;
    }

    /* step3: add to map with lock */
    std::lock_guard<std::mutex> lock(mutex_);
    allCq_[reinterpret_cast<uint64_t>(cq.Get())] = cq;

    GEN_LOG_DEBUG("Create cq(" << cq->Id() << ") finished, queue depth(" << options->queue_depth << ")");
    return reinterpret_cast<genetty_cq_t>(cq.Get());
}

Result UrmaBackend::GenettyCqDestroy(genetty_cq_t cq, uint32_t flags) noexcept
{
    GEN_ASSERT_MSG_RETURN(cq != nullptr, "Destroy cq failed, as cq is null", GEN_INVALID_PARAM);

    /* step1: remove the cq from the registry under the lock */
    auto handle = reinterpret_cast<uint64_t>(cq);
    GenettyCQPtr tmpCQ;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iter = allCq_.find(handle);
        if (UNLIKELY(iter == allCq_.end())) {
            GEN_LOG_DEBUG("Destroy cq skipped, as cq(" << handle << ") is not found in this backend, flags(" << flags
                                                       << ")");
            return GEN_NOT_FOUND;
        }

        tmpCQ = iter->second;
        allCq_.erase(iter);
    }

    /* step2: destroy it out of the lock, as the destroy may take time */
    if (LIKELY(tmpCQ != nullptr)) {
        tmpCQ->Destroy();
        GEN_LOG_DEBUG("Destroy cq(" << tmpCQ->Id() << ") finished");
    }

    return GEN_OK;
}

GenettyPtr UrmaBackend::CreateGenettyByType(genetty_options_t *options) noexcept
{
    /* step1: find the cq created by genetty cq create */
    auto cqHandle = reinterpret_cast<uint64_t>(options->cq_handle);
    GenettyCQPtr cq;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iter = allCq_.find(cqHandle);
        if (UNLIKELY(iter == allCq_.end())) {
            GEN_LOG_ERROR("Create genetty failed, as cq(" << cqHandle << ") is not found");
            return nullptr;
        }

        cq = iter->second;
    }

    /* step2: convert the c options to the cxx options */
    GenettyOptions genettyOptions;
    genettyOptions.cq = cq;
    genettyOptions.sqDepth = options->sq_depth;
    genettyOptions.rqDepth = options->rq_depth;
    genettyOptions.flags = options->flags;
    genettyOptions.lockFree = options->lock_free != 0;
    genettyOptions.serviceLevel = options->service_level;
    genettyOptions.trafficClass = options->traffic_class;

    /* step3: create and initialize the urma host genetty */
    GenettyPtr genetty = mf::MakeRef<UrmaHostQp>(HOST_URMA_GENETTY).Get();
    if (UNLIKELY(genetty == nullptr)) {
        GEN_LOG_ERROR("Create urma genetty failed, probably out of memory, sq depth(" << genettyOptions.sqDepth << ")");
        return nullptr;
    }

    auto result = genetty->Initialize(genettyOptions);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Create urma genetty failed, as initialize failed, result(" << result << "), sq depth("
                                                                                  << genettyOptions.sqDepth << ")");
        return nullptr;
    }

    return genetty;
}

genetty_t UrmaBackend::GenettyCreate(genetty_options_t *options) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Create genetty failed, as options is null", nullptr);

    /* step2: create and initialize the genetty by the genetty type */
    auto genetty = CreateGenettyByType(options);
    if (UNLIKELY(genetty == nullptr)) {
        GEN_LOG_ERROR("Create genetty failed, as create genetty by type failed");
        return nullptr;
    }

    /* step3: added into map */
    std::lock_guard<std::mutex> lock(mutex_);
    allGenetty_[reinterpret_cast<uint64_t>(genetty.Get())] = genetty;

    GEN_LOG_DEBUG("Create genetty(" << genetty->Id() << ") finished, sq depth(" << options->sq_depth << ")");
    return reinterpret_cast<genetty_t>(genetty.Get());
}

Result UrmaBackend::GenettyDestroy(genetty_t handle, uint32_t flags) noexcept
{
    GEN_ASSERT_MSG_RETURN(handle != nullptr, "Destroy genetty failed, as handle is null", GEN_INVALID_PARAM);

    /* step1: remove the genetty from the registry under the lock */
    auto address = reinterpret_cast<uint64_t>(handle);
    GenettyPtr tmpGenetty;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iter = allGenetty_.find(address);
        if (UNLIKELY(iter == allGenetty_.end())) {
            GEN_LOG_DEBUG("Destroy genetty skipped, as genetty(" << address << ") is not found in this backend, "
                                                                 << "flags(" << flags << ")");
            return GEN_ERROR;
        }

        tmpGenetty = iter->second;
        allGenetty_.erase(iter);
    }

    /* step2: destroy it out of the lock, as the destroy may take time */
    if (LIKELY(tmpGenetty != nullptr)) {
        tmpGenetty->Destroy();
        GEN_LOG_DEBUG("Destroy genetty(" << tmpGenetty->Id() << ") finished");
    }

    return GEN_OK;
}
} // namespace urma
} // namespace openDCC
} // namespace ock
