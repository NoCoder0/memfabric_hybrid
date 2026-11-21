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
#include "genetty_verbs_backend.h"
#include "genetty_verbs_host_cq_qp.h"
#include "genetty_verbs_device_cq_qp.h"

namespace ock {
namespace openDCC {
namespace verbs {
Result VerbsBackend::Initialize(const GenettyBackendOptions &options) noexcept
{
    Result result = GEN_OK;
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Already initialized");
        return result;
    }

    /* do initialize */
    result = InitInner(options);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize failed, result(" << result << ")");
        return result;
    }

    /* mark inited */
    options_ = options;
    inited_ = true;

    GEN_LOG_DEBUG("Initialized successfully");
    return result;
}

Result VerbsBackend::InitInner(const GenettyBackendOptions &options) noexcept
{
    Result result = GEN_OK;
    HostVerbsContextPoolPtr hostCtxPool;
    DeviceVerbsContextPoolPtr deviceCtxPool;
    /* Step1: create and initialize context pool
     * case1: create context for host RDMA */
    if ((options.protocol & GEN_PROT_HOST_RDMA) == GEN_PROT_HOST_RDMA) {
        /* create context pool */
        hostCtxPool = mf::MakeRef<HostVerbsContextPool>();
        if (UNLIKELY(hostCtxPool == nullptr)) {
            GEN_LOG_ERROR("Create ctxPool failed, as ctxPool is null");
            return GEN_MALLOC_FAILED;
        }

        /* loop all devices */
        result = hostCtxPool->AddAllDevices();
        if (result != GEN_OK) {
            GEN_LOG_ERROR("Create ctxPool failed, result(" << result << ")");
            return result;
        }

        GEN_LOG_DEBUG("Create ctxPool success for host RDMA");
    }

    /* case2: create context for device RDMA */
    if ((options.protocol & GEN_PROT_DEVICE_RDMA) == GEN_PROT_DEVICE_RDMA) {
        /* create context pool */
        deviceCtxPool = mf::MakeRef<DeviceVerbsContextPool>();
        if (UNLIKELY(deviceCtxPool == nullptr)) {
            GEN_LOG_ERROR("Create device ctxPool failed, as ctxPool is null");
            return GEN_MALLOC_FAILED;
        }

        /* loop all devices */
        result = (options.flags & GEN_INIT_LAZY_DEVICE) != 0 ? GEN_OK : deviceCtxPool->AddAllDevices();
        if (result != GEN_OK) {
            GEN_LOG_ERROR("Create device ctxPool failed, result(" << result << ")");
            return result;
        }

        GEN_LOG_DEBUG("Create ctxPool success for device RDMA");
    }

    hostCtxPool_ = hostCtxPool;
    deviceCtxPool_ = deviceCtxPool;
    return result;
}

void VerbsBackend::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Not initialized");
        return;
    }

    for (auto &entry : allGenetty_) {
        entry.second->Destroy();
    }
    allGenetty_.clear();
    for (auto &entry : allCq_) {
        entry.second->Destroy();
    }
    allCq_.clear();

    if (hostCtxPool_ != nullptr) {
        hostCtxPool_->CloseAllDevices();
        hostCtxPool_ = nullptr;
    }

    if (deviceCtxPool_ != nullptr) {
        deviceCtxPool_->CloseAllDevices();
        deviceCtxPool_ = nullptr;
    }

    inited_ = false;
}

Result VerbsBackend::GetDevicesInfo(std::vector<GenettyDeviceInfo> &info) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(inited_, "Not initialized", GEN_NOT_INITIALIZED);

    if ((options_.protocol & GEN_PROT_HOST_RDMA) == GEN_PROT_HOST_RDMA) {
        const auto &devices = VerbsDevice::AllDevices();
        if (devices.empty()) {
            GEN_LOG_INFO("Get devices info, no host rdma device found");
            return GEN_OK;
        }

        for (const auto &item : devices) {
            const auto &device = item.second;
            if (device == nullptr) {
                continue;
            }

            GenettyDeviceInfo deviceInfo;
            deviceInfo.deviceId = device->DeviceId();
            deviceInfo.portId = static_cast<uint8_t>(device->PortIndex());
            deviceInfo.active = device->PortAttributes().state == IBV_PORT_ACTIVE;
            deviceInfo.deviceName = device->DeviceName();
            deviceInfo.ip = VerbsDevice::GidToIp(device->IbvGid());
            deviceInfo.type = HOST_RDMA_GENETTY;
            info.emplace_back(deviceInfo);
        }
    }

    GEN_LOG_DEBUG("Get devices info finished, device count(" << info.size() << ")");
    return GEN_OK;
}

Result VerbsBackend::GetContext(const GenettyCtxGetOptions &ctxOptions, genetty_context_t *context) noexcept
{
    GEN_ASSERT_MSG_RETURN(context != nullptr, "Invalid param, context is null", GEN_INVALID_PARAM);
    *context = nullptr;

    GEN_ASSERT_MSG_RETURN(ctxOptions.type != GCCP_INVALID,
                          "Invalid param, policy(" << ctxOptions.type << ") is invalid", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(ctxOptions.type != GCCP_COUNT, "Invalid param, policy(" << ctxOptions.type << ") is invalid",
                          GEN_INVALID_PARAM);

    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(inited_, "Not initialized", GEN_NOT_INITIALIZED);
    if (ctxOptions.genettyType == HOST_RDMA_GENETTY) {
        return GetHostContext(ctxOptions, context);
    } else if (ctxOptions.genettyType == DEVICE_RDMA_GENETTY) {
        return GetDeviceContext(ctxOptions, context);
    }

    GEN_LOG_ERROR("Get context failed, as type(" << ctxOptions.genettyType << ") is not supported by verbs backend");
    return GEN_INVALID_PARAM;
}

Result VerbsBackend::GetHostContext(const GenettyCtxGetOptions &ctxOptions, genetty_context_t *context) noexcept
{
    GEN_ASSERT_MSG_RETURN(hostCtxPool_ != nullptr, "Not initialized, as context pool is null", GEN_NOT_INITIALIZED);

    /* step1: select the context from the context pool by the policy */
    HostVerbsContextPtr ctx;
    Result result = GEN_INVALID_PARAM;
    if (ctxOptions.type == GCCP_BY_DEVICE_ID) {
        result = hostCtxPool_->SelectContext(ctxOptions.deviceId, ctx);
    } else if (ctxOptions.type == GCCP_BY_IP) {
        result = hostCtxPool_->SelectContext(ctxOptions.ipMask, ctx);
    } else if (ctxOptions.type == GCCP_AUTO) {
        GEN_LOG_DEBUG("Get context by auto policy(" << CtxPolicyStr(ctxOptions.whichAutoPolicy) << ")");
        result = hostCtxPool_->SelectContext(static_cast<uint16_t>(UINT16_MAX), ctx);
    } else {
        GEN_LOG_ERROR("Get context failed, as policy(" << ctxOptions.type << ") is not supported");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Get context failed, as select context failed, result("
                      << result << "), policy(" << ctxOptions.type << "), device id(" << ctxOptions.deviceId
                      << "), ip mask(" << ctxOptions.ipMask << ")");
        return result;
    }

    /* step2: increase reference and output the context handle */
    ctx->IncreaseRef();
    *context = reinterpret_cast<genetty_context_t>(ctx.Get());
    GEN_LOG_DEBUG("Get context(" << ctx->wrapCtx_->Id() << ") finished, policy(" << ctxOptions.type << "), device("
                                 << ctx->wrapCtx_->Device()->DeviceName() << ")");
    return GEN_OK;
}

Result VerbsBackend::GetDeviceContext(const GenettyCtxGetOptions &ctxOptions, genetty_context_t *context) noexcept
{
    GEN_ASSERT_MSG_RETURN(deviceCtxPool_ != nullptr, "Not initialized, as context pool is null", GEN_NOT_INITIALIZED);

    /* step1: select the context from the context pool by the policy */
    DeviceVerbsContextPtr ctx;
    Result result = GEN_INVALID_PARAM;
    if (ctxOptions.type == GCCP_BY_DEVICE_ID) {
        if ((options_.flags & GEN_INIT_LAZY_DEVICE) != 0) {
            result = deviceCtxPool_->AddDevice(ctxOptions.deviceId);
            if (result != GEN_OK) {
                return result;
            }
        }
        result = deviceCtxPool_->SelectContext(ctxOptions.deviceId, ctx);
    } else if (ctxOptions.type == GCCP_BY_IP) {
        result = deviceCtxPool_->SelectContext(ctxOptions.ipMask, ctx);
    } else if (ctxOptions.type == GCCP_AUTO) {
        GEN_LOG_DEBUG("Get context by auto policy(" << CtxPolicyStr(ctxOptions.whichAutoPolicy) << ")");
        result = deviceCtxPool_->SelectContext(static_cast<uint16_t>(UINT16_MAX), ctx);
    } else {
        GEN_LOG_ERROR("Get context failed, as policy(" << ctxOptions.type << ") is not supported");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Get context failed, as select context failed, result("
                      << result << "), policy(" << ctxOptions.type << "), device id(" << ctxOptions.deviceId
                      << "), ip mask(" << ctxOptions.ipMask << ")");
        return result;
    }

    /* step2: increase reference and output the context handle */
    ctx->IncreaseRef();
    *context = reinterpret_cast<genetty_context_t>(ctx.Get());
    GEN_LOG_DEBUG("Get context for device(" << ctx->wrapCtx_->LogicDeviceId() << ") finished, policy("
                                            << ctxOptions.type << "), ip(" << ctx->wrapCtx_->Ip() << ")");
    return GEN_OK;
}

void VerbsBackend::ReturnContext(genetty_context_t *context, uint32_t flags) noexcept
{
    /* step1: check param */
    GEN_ASSERT_MSG_NO_RETURN(context != nullptr && *context != nullptr, "Return context failed, as context is null");

    /* step2: convert to GenettyContext */
    auto genettyCtx = reinterpret_cast<GenettyContext *>(*context);
    auto type = genettyCtx->Type();
    if (type != HOST_RDMA_GENETTY && type != DEVICE_RDMA_GENETTY) {
        GEN_LOG_ERROR("Return context failed, as type(" << type << ") is not supported by verbs backend");
        return;
    }

    /* step3: get type and call related context pool to find it */
    std::lock_guard<std::mutex> lock(mutex_);
    if (type == DEVICE_RDMA_GENETTY) {
        ReturnDeviceContext(context, flags);
        return;
    }

    GEN_ASSERT_MSG_NO_RETURN(hostCtxPool_ != nullptr, "Return context failed, as context pool is null");

    HostVerbsContextPtr ctx;
    auto result = hostCtxPool_->Lookup(*context, ctx);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Return context failed, as look up context failed, result(" << result << "), context(" << *context
                                                                                  << ")");
        return;
    }

    /* step4: decrease reference if found, the reference is increased by GetContext */
    ctx->DecreaseRef();

    GEN_LOG_DEBUG("Return context(" << ctx->wrapCtx_->Id() << ") finished, flags(" << flags << ")");
}

void VerbsBackend::ReturnDeviceContext(genetty_context_t *context, uint32_t flags) noexcept
{
    GEN_ASSERT_MSG_NO_RETURN(deviceCtxPool_ != nullptr, "Return context failed, as context pool is null");

    DeviceVerbsContextPtr ctx;
    auto result = deviceCtxPool_->Lookup(*context, ctx);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Return context failed, as look up context failed, result(" << result << "), context(" << *context
                                                                                  << ")");
        return;
    }

    /* step4: decrease reference if found, the reference is increased by GetContext */
    ctx->DecreaseRef();

    GEN_LOG_DEBUG("Return context for device(" << ctx->wrapCtx_->LogicDeviceId() << ") finished, flags(" << flags
                                               << ")");
}

GenettyCQPtr VerbsBackend::CreateDeviceCQ(const GenettyCQOptions &options) noexcept
{
    GEN_ASSERT_MSG_RETURN(deviceCtxPool_ != nullptr, "Device context pool is not initialized", nullptr);
    DeviceVerbsContextPtr context;
    if (deviceCtxPool_->Lookup(options.context, context) != GEN_OK) {
        return nullptr;
    }
    GenettyCQPtr cq = mf::MakeRef<VerbsDeviceCQ>(DEVICE_RDMA_GENETTY, context->wrapCtx_).Get();
    GEN_ASSERT_MSG_RETURN(cq != nullptr, "Allocate Device CQ failed", nullptr);
    if (cq->Initialize(options) != GEN_OK) {
        return nullptr;
    }
    return cq;
}

GenettyCQPtr VerbsBackend::DoCreateCQ(genetty_cq_options_t *options) noexcept
{
    /* step1: convert the c options to the cxx options */
    GenettyCQOptions cqOptions;
    cqOptions.eventFd = options->event_fd;
    cqOptions.queueDepth = options->queue_depth;
    cqOptions.flags = options->flags;
    cqOptions.preferred_ip_mask = options->preferred_ip_mask;
    cqOptions.lockFree = options->lock_free != 0;
    cqOptions.context = options->context;

    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(inited_, "Backend is not initialized", nullptr);
    if (reinterpret_cast<GenettyContext *>(options->context)->Type() == DEVICE_RDMA_GENETTY) {
        return CreateDeviceCQ(cqOptions);
    }

    /* step2: create the cq with the context passed in by the cq options */
    if (UNLIKELY(hostCtxPool_ == nullptr)) {
        GEN_LOG_ERROR("Create cq failed, as context pool is null, queue depth(" << cqOptions.queueDepth << ")");
        return nullptr;
    }

    HostVerbsContextPtr ctx;
    auto result = hostCtxPool_->Lookup(cqOptions.context, ctx);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Create cq failed, as look up context failed, result(" << result << "), context("
                                                                             << cqOptions.context << ")");
        return nullptr;
    }

    GenettyCQPtr cq = mf::MakeRef<VerbsHostCQ>(HOST_RDMA_GENETTY, ctx->wrapCtx_, &ctx->gid_).Get();
    if (UNLIKELY(cq == nullptr)) {
        GEN_LOG_ERROR("Create cq failed, probably out of memory, queue depth(" << cqOptions.queueDepth << ")");
        return nullptr;
    }

    /* step3: initialize the cq with the converted options */
    result = cq->Initialize(cqOptions);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Create cq failed, as initialize failed, result(" << result << "), queue depth("
                                                                        << cqOptions.queueDepth << ")");
        return nullptr;
    }

    return cq;
}

genetty_cq_t VerbsBackend::GenettyCqCreate(genetty_cq_options_t *options) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Create cq failed, as options is null", nullptr);

    /* step2: create and initialize the cq by the genetty type */
    auto cq = DoCreateCQ(options);
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

Result VerbsBackend::GenettyCqDestroy(genetty_cq_t cq, uint32_t flags) noexcept
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

GenettyPtr VerbsBackend::DoCreateGenetty(genetty_options_t *options) noexcept
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
    genettyOptions.vendor = options_.vendor;
    genettyOptions.lockFree = options->lock_free != 0;
    genettyOptions.serviceLevel = options->service_level;
    genettyOptions.trafficClass = options->traffic_class;

    /* step3: create the genetty of the host rdma type */
    GenettyPtr genetty;
    if (cq->Type() == DEVICE_RDMA_GENETTY) {
        genetty = mf::MakeRef<VerbsDeviceQp>(DEVICE_RDMA_GENETTY).Get();
    } else {
        genetty = mf::MakeRef<VerbsHostQp>(HOST_RDMA_GENETTY).Get();
    }
    if (UNLIKELY(genetty == nullptr)) {
        GEN_LOG_ERROR("Create genetty failed, probably out of memory, sq depth(" << genettyOptions.sqDepth << ")");
        return nullptr;
    }

    /* step4: initialize the genetty with the converted options */
    auto result = genetty->Initialize(genettyOptions);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Create genetty failed, as initialize failed, result(" << result << "), sq depth("
                                                                             << genettyOptions.sqDepth << ")");
        return nullptr;
    }

    return genetty;
}

genetty_t VerbsBackend::GenettyCreate(genetty_options_t *options) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Create genetty failed, as options is null", nullptr);

    /* step2: create and initialize the genetty by the genetty type */
    auto genetty = DoCreateGenetty(options);
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

Result VerbsBackend::GenettyDestroy(genetty_t handle, uint32_t flags) noexcept
{
    if (UNLIKELY(handle == nullptr)) {
        GEN_LOG_DEBUG("Destroy genetty skipped, as handle is null, flags(" << flags << ")");
        return GEN_INVALID_PARAM;
    }

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
} // namespace verbs
} // namespace openDCC
} // namespace ock
