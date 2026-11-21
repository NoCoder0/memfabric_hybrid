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
#include "genetty_verbs_device_ctx_pool.h"

#include "common/genetty_functions.h"
#include "under_api/acl/genetty_dl_acl_api.h"
#include "under_api/hccp/genetty_dl_hccp_api.h"

namespace ock {
namespace openDCC {
namespace verbs {
Result DeviceVerbsContext::RegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
{
    /* step1: check params */
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Register memory failed, as segment is null", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    /* step2: create MemoryInfo */
    MemoryInfo memoryInfo;
    memoryInfo.MemType(segment->type);
    memoryInfo.Address(segment->address);
    memoryInfo.Size(segment->size);

    /* step3: get lock and find in mrs_ map */
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = mrs_.find(memoryInfo);

    /* step4: if found set lkey/rkey to segment then return */
    if (iter != mrs_.end() && iter->second.handle != nullptr) {
        ++iter->second.references;
        segment->lkey = iter->second.lkey;
        segment->rkey = iter->second.rkey;
        GEN_LOG_DEBUG("Register memory skipped, as already registered, mem type("
                      << segment->type << "), address(" << segment->address << "), size(" << segment->size << ")");
        return GEN_OK;
    }

    /* step5: if not found, do register and check result */
    if (UNLIKELY(wrapCtx_ == nullptr || wrapCtx_->Handle() == nullptr)) {
        GEN_LOG_ERROR("Register memory failed, as RDMA device handle is null, mem type("
                      << segment->type << "), address(" << segment->address << "), size(" << segment->size << ")");
        return GEN_DEVICE_ERROR;
    }

    HccpMrInfo mrInfo{};
    mrInfo.addr = reinterpret_cast<void *>(segment->address);
    mrInfo.size = segment->size;
    mrInfo.access = HCCP_MR_ACCESS_LOCAL_WRITE | HCCP_MR_ACCESS_REMOTE_WRITE | HCCP_MR_ACCESS_REMOTE_READ;
    void *mrHandle = nullptr;
    auto result = HccpApi::RaRegisterMr(wrapCtx_->Handle(), mrInfo, mrHandle);
    if (UNLIKELY(result != 0 || mrHandle == nullptr)) {
        GEN_LOG_ERROR("[HCCP_API] RaRegisterMr failed, address(0x" << std::hex << segment->address << std::dec
                                                                   << "), size(" << segment->size << "), result("
                                                                   << result << "), mrHandle(" << mrHandle << ")");
        return GEN_ERROR;
    }

    /* step6: add to mrs */
    mrs_[memoryInfo] = {mrHandle, segment->address, segment->size, mrInfo.lkey, mrInfo.rkey};

    /* step7: set lkey and rkey to segment */
    segment->lkey = mrInfo.lkey;
    segment->rkey = mrInfo.rkey;

    GEN_LOG_DEBUG("Register memory finished, mem type(" << segment->type << "), address(" << segment->address
                                                        << "), size(" << segment->size << "), lkey(" << segment->lkey
                                                        << "), rkey(" << segment->rkey << ")");
    return GEN_OK;
}

Result DeviceVerbsContext::UnRegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
{
    /* step1: check params */
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Unregister memory failed, as segment is null", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    /* step2: create MemoryInfo */
    MemoryInfo memoryInfo;
    memoryInfo.MemType(segment->type);
    memoryInfo.Address(segment->address);
    memoryInfo.Size(segment->size);

    /* step3: get lock and find in mrs_ map */
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = mrs_.find(memoryInfo);
    if (UNLIKELY(iter == mrs_.end())) {
        GEN_LOG_ERROR("Unregister memory failed, as the memory is not registered, mem type("
                      << segment->type << "), address(0x" << std::hex << segment->address << std::dec << "), size("
                      << segment->size << ")");
        return GEN_NOT_FOUND;
    }

    if (iter->second.references > 1) {
        --iter->second.references;
        return GEN_OK;
    }

    /* step4: store it to tmpMr and check the handles */
    auto tmpMr = iter->second;

    if (UNLIKELY(tmpMr.handle == nullptr)) {
        GEN_LOG_ERROR("Unregister memory failed, as mr is null, mem type("
                      << segment->type << "), address(0x" << std::hex << segment->address << std::dec << "), size("
                      << segment->size << ")");
        return GEN_ERROR;
    }

    if (UNLIKELY(wrapCtx_ == nullptr || wrapCtx_->Handle() == nullptr)) {
        GEN_LOG_ERROR("Unregister memory failed, as RDMA device handle is null, mem type("
                      << segment->type << "), address(0x" << std::hex << segment->address << std::dec << "), size("
                      << segment->size << ")");
        return GEN_DEVICE_ERROR;
    }

    GEN_ASSERT_MSG_RETURN(!wrapCtx_->HasPendingQueueCleanup(), "Unregister memory blocked by unfinished QP cleanup",
                          GEN_DEVICE_ERROR);

    /* step5: do unregister with tmpMr */
    auto result = HccpApi::RaDeregisterMr(wrapCtx_->Handle(), tmpMr.handle);
    if (UNLIKELY(result != 0)) {
        GEN_LOG_ERROR("[HCCP_API] RaDeregisterMr failed, address(0x"
                      << std::hex << tmpMr.address << std::dec << "), size(" << tmpMr.size << "), result(" << result
                      << ")");
        return GEN_ERROR;
    }

    /* step6: remove from mrs_ after success, keep the record on failure for retry */
    mrs_.erase(iter);

    GEN_LOG_DEBUG("Unregister memory finished, mem type(" << segment->type << "), address(" << segment->address
                                                          << "), size(" << segment->size << ")");
    return GEN_OK;
}

Result DeviceVerbsContext::ImportMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
{
    /* no need to import for verbs */
    (void)segment, (void)flags;
    return GEN_OK;
}

void DeviceVerbsContext::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (wrapCtx_ == nullptr) {
        GEN_ASSERT_MSG_NO_RETURN(mrs_.empty(),
                                 "Destroy context failed, as RDMA context is null, mr count(" << mrs_.size() << ")");
        return;
    }

    /* transfer the MRs to the device context for ordered cleanup and retry */
    wrapCtx_->Destroy(mrs_);
}

Result DeviceVerbsContextPool::AddDevice(uint16_t logicDeviceId) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &ctx : contexts_) {
        if (ctx->wrapCtx_->LogicDeviceId() == logicDeviceId) {
            return GEN_OK;
        }
    }
    auto result = AclApi::Init();
    if (result != GEN_OK) {
        return result;
    }
    uint32_t count = 0;
    result = AclApi::AclrtGetDeviceCount(&count);
    if (result != 0) {
        GEN_LOG_ERROR("Query ACL device count failed, result(" << result << ")");
        return GEN_DEVICE_ERROR;
    }
    for (uint32_t userId = 0; userId < count; ++userId) {
        int32_t logicId = -1;
        result = AclApi::AclrtGetLogicDevIdByUserDevId(userId, &logicId);
        if (result != 0 || logicId != logicDeviceId) {
            continue;
        }
        int32_t previous = -1;
        (void)AclApi::AclrtGetDevice(&previous);
        DeviceRdmaContextPtr ctx;
        result = DeviceRdmaContext::CreateContext(userId, "", ctx);
        if (previous >= 0 && AclApi::AclrtSetDevice(previous) != 0) {
            GEN_LOG_ERROR("Restore device failed, userDeviceId(" << previous << ")");
            return GEN_DEVICE_ERROR;
        }
        if (result != GEN_OK) {
            return result;
        }
        auto context = mf::MakeRef<DeviceVerbsContext>(DEVICE_RDMA_GENETTY, ctx);
        GEN_ASSERT_MSG_RETURN(context != nullptr, "Allocate Device context failed", GEN_MALLOC_FAILED);
        contexts_.push_back(context);
        inited_ = true;
        return GEN_OK;
    }
    GEN_LOG_ERROR("No user device matches logicDeviceId(" << logicDeviceId << ")");
    return GEN_NOT_FOUND;
}

Result DeviceVerbsContextPool::AddAllDevices() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Add all devices skipped, as already inited, context count(" << contexts_.size() << ")");
        return GEN_OK;
    }

    /* step1: initialize ACL and get the available device count */
    auto result = AclApi::Init();
    if (result != GEN_OK) {
        return result;
    }
    uint32_t deviceCount = 0;
    result = AclApi::AclrtGetDeviceCount(&deviceCount);
    GEN_ASSERT_MSG_RETURN(result == 0 && deviceCount != 0 && deviceCount <= static_cast<uint32_t>(INT32_MAX),
                          "Add all devices failed, result(" << result << "), device count(" << deviceCount << ")",
                          GEN_DEVICE_ERROR);

    /* step2: create contexts and restore the calling thread's device if one was selected */
    int32_t previousDeviceId = -1;
    if (AclApi::AclrtGetDevice(&previousDeviceId) != 0) {
        previousDeviceId = -1;
        GEN_LOG_DEBUG("No current device to restore after adding devices");
    }
    result = AddDevices(deviceCount);
    if (previousDeviceId >= 0) {
        auto restoreResult = AclApi::AclrtSetDevice(previousDeviceId);
        if (restoreResult != 0) {
            GEN_LOG_ERROR("Restore device failed, result(" << restoreResult << "), userDeviceId(" << previousDeviceId
                                                           << ")");
            CloseAllDevicesInner();
            return GEN_DEVICE_ERROR;
        }
    }
    if (result != GEN_OK) {
        return result;
    }

    inited_ = true;
    GEN_LOG_DEBUG("Add all devices finished, context count(" << contexts_.size() << ")");
    return GEN_OK;
}

Result DeviceVerbsContextPool::AddDevices(uint32_t deviceCount) noexcept
{
    GEN_LOG_DEBUG("Got all devices, device count(" << deviceCount << ")");
    for (uint32_t userDeviceId = 0; userDeviceId < deviceCount; ++userDeviceId) {
        DeviceRdmaContextPtr ctx;
        auto result = DeviceRdmaContext::CreateContext(userDeviceId, "", ctx);
        if (result != GEN_OK) {
            GEN_LOG_WARN("Create context for device(" << userDeviceId << ") failed, result(" << result << ")");
            continue;
        }

        auto context = mf::MakeRef<DeviceVerbsContext>(DEVICE_RDMA_GENETTY, ctx);
        if (UNLIKELY(context == nullptr)) {
            GEN_LOG_ERROR("Create DeviceVerbsContext failed, probably out of memory, userDeviceId(" << userDeviceId
                                                                                                    << ")");
            CloseAllDevicesInner();
            return GEN_MALLOC_FAILED;
        }
        contexts_.push_back(context);
        GEN_LOG_DEBUG("Add context for device(" << userDeviceId << "), ip(" << ctx->Ip() << ")");
    }

    if (contexts_.empty()) {
        GEN_LOG_ERROR("Add all devices failed, as no context created, device count(" << deviceCount << ")");
        return GEN_DEVICE_ERROR;
    }
    return GEN_OK;
}

void DeviceVerbsContextPool::CloseAllDevices() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    /* Clean up partial initialization even if the pool is not inited. */
    CloseAllDevicesInner();
    inited_ = false;

    auto result = DeviceRdmaContext::RetryPendingCleanup();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Close all devices failed, as pending cleanup failed, result(" << result << ")");
    }
}

void DeviceVerbsContextPool::CloseAllDevicesInner() noexcept
{
    for (auto &ctx : contexts_) {
        if (ctx != nullptr) {
            ctx->Destroy();
        }
    }

    if (!contexts_.empty()) {
        GEN_LOG_WARN("Close all devices, context count(" << contexts_.size() << ")");
    }

    contexts_.clear();
    badPorts_.clear();
}

Result DeviceVerbsContextPool::SelectContext(uint16_t deviceId, DeviceVerbsContextPtr &ctx) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_ERROR("Select context failed, as pool is not inited, device id(" << deviceId << ")");
        return GEN_ERROR;
    }

    for (const auto &item : contexts_) {
        if (item == nullptr || item->wrapCtx_ == nullptr || item->wrapCtx_->Handle() == nullptr) {
            continue;
        }

        const auto &verbsCtx = item->wrapCtx_;
        if (deviceId != UINT16_MAX && verbsCtx->LogicDeviceId() != deviceId) {
            continue;
        }

        ctx = item;
        GEN_LOG_DEBUG("Select context(" << verbsCtx->LogicDeviceId() << ") of device(" << verbsCtx->Ip()
                                        << ") for device id(" << deviceId << ")");
        return GEN_OK;
    }

    GEN_LOG_ERROR("Select context failed, as no device matched, device id(" << deviceId << "), context count("
                                                                            << contexts_.size() << ")");
    return GEN_INVALID_PARAM;
}

Result DeviceVerbsContextPool::SelectContext(const std::string &ipWithMask, DeviceVerbsContextPtr &ctx) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_ERROR("Select context failed, as pool is not inited, ip(" << ipWithMask << ")");
        return GEN_ERROR;
    }

    for (const auto &item : contexts_) {
        if (item == nullptr || item->wrapCtx_ == nullptr || item->wrapCtx_->Handle() == nullptr) {
            continue;
        }

        const auto &verbsCtx = item->wrapCtx_;
        if (!ipWithMask.empty() && verbsCtx->Ip() != ipWithMask.substr(0, ipWithMask.find('/'))) {
            continue;
        }

        ctx = item;
        GEN_LOG_DEBUG("Select context(" << verbsCtx->LogicDeviceId() << ") of device(" << verbsCtx->Ip() << ") for ip("
                                        << ipWithMask << ")");
        return GEN_OK;
    }

    GEN_LOG_ERROR("Select context failed, as no device matched, ip(" << ipWithMask << "), context count("
                                                                     << contexts_.size() << ")");
    return GEN_INVALID_PARAM;
}

Result DeviceVerbsContextPool::Lookup(genetty_context_t ctx, DeviceVerbsContextPtr &out) noexcept
{
    GEN_ASSERT_MSG_RETURN(ctx != nullptr, "Lookup context failed, as ctx is null", GEN_INVALID_PARAM);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_ERROR("Lookup context failed, as pool is not inited, ctx(" << ctx << ")");
        return GEN_ERROR;
    }

    for (const auto &item : contexts_) {
        if (item == nullptr || item->wrapCtx_ == nullptr) {
            continue;
        }

        auto wrapHandle = reinterpret_cast<genetty_context_t>(item.Get());
        auto deviceHandle = reinterpret_cast<genetty_context_t>(item->wrapCtx_.Get());
        if (wrapHandle == ctx || deviceHandle == ctx) {
            out = item;
            GEN_LOG_DEBUG("Lookup device context success, ctx(" << ctx << ")");
            return GEN_OK;
        }
    }

    GEN_LOG_ERROR("Lookup context failed, as ctx(" << ctx << ") is not found, context count(" << contexts_.size()
                                                   << ")");
    return GEN_NOT_FOUND;
}

void DeviceVerbsContextPool::RecordBadPort(uint8_t badPortId) noexcept
{
    size_t badCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        badPorts_[badPortId] = MonotonicTime::TimeNs();
        badCount = badPorts_.size();
    }

    GEN_LOG_WARN("Record bad port(" << static_cast<uint32_t>(badPortId) << "), bad port count(" << badCount << ")");
}
} // namespace verbs
} // namespace openDCC
} // namespace ock
