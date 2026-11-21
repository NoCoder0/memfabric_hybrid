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
#include "genetty_verbs_host_ctx_pool.h"

#include "common/genetty_functions.h"

namespace ock {
namespace openDCC {
namespace verbs {
Result HostVerbsContext::RegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
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
        segment->lkey = iter->second.handle->lkey;
        segment->rkey = iter->second.handle->rkey;
        GEN_LOG_DEBUG("Register memory skipped, as already registered, mem type("
                      << segment->type << "), address(" << segment->address << "), size(" << segment->size << ")");
        return GEN_OK;
    }

    /* step5: if not found, do register and check result */
    if (UNLIKELY(wrapCtx_ == nullptr || wrapCtx_->PdHandle() == nullptr)) {
        GEN_LOG_ERROR("Register memory failed, as pd is null, mem type("
                      << segment->type << "), address(" << segment->address << "), size(" << segment->size << ")");
        return GEN_DEVICE_ERROR;
    }

    constexpr int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    auto mr = VerbsApi::ibv_reg_mr(wrapCtx_->PdHandle(), reinterpret_cast<void *>(segment->address),
                                   static_cast<size_t>(segment->size), access);
    if (UNLIKELY(mr == nullptr)) {
        GEN_LOG_ERROR("[IBV_API] ibv_reg_mr failed, address(0x" << std::hex << segment->address << std::dec
                                                                << "), size(" << segment->size << "), errno(" << errno
                                                                << ")");
        return GEN_ERROR;
    }

    /* step6: add to mrs */
    mrs_[memoryInfo] = {mr, 1};

    /* step7: set lkey and rkey to segment */
    segment->lkey = mr->lkey;
    segment->rkey = mr->rkey;

    GEN_LOG_DEBUG("Register memory finished, mem type(" << segment->type << "), address(" << segment->address
                                                        << "), size(" << segment->size << "), lkey(" << segment->lkey
                                                        << "), rkey(" << segment->rkey << ")");
    return GEN_OK;
}

Result HostVerbsContext::UnRegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
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
    auto tmpMr = iter->second.handle;

    /* step5: do unregister with tmpMr */
    if (UNLIKELY(tmpMr == nullptr)) {
        GEN_LOG_ERROR("Unregister memory failed, as mr is null, mem type("
                      << segment->type << "), address(0x" << std::hex << segment->address << std::dec << "), size("
                      << segment->size << ")");
        return GEN_ERROR;
    }

    auto result = VerbsApi::ibv_dereg_mr(tmpMr);
    if (UNLIKELY(result != 0)) {
        GEN_LOG_ERROR("[IBV_API] ibv_dereg_mr failed, address(0x" << std::hex << segment->address << std::dec
                                                                  << "), size(" << segment->size << "), errno(" << errno
                                                                  << ")");
        return GEN_ERROR;
    }

    mrs_.erase(iter);
    GEN_LOG_DEBUG("Unregister memory finished, mem type(" << segment->type << "), address(" << segment->address
                                                          << "), size(" << segment->size << ")");
    return GEN_OK;
}

Result HostVerbsContext::ImportMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
{
    /* no need to import for verbs */
    (void)segment, (void)flags;
    return GEN_OK;
}

Result HostVerbsContextPool::AddAllDevices() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Add all devices skipped, as already inited, context count(" << contexts_.size() << ")");
        return GEN_OK;
    }

    /* step1: init devices */
    auto result = VerbsDevice::Init();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Add all devices failed, as init devices failed, result(" << result << ")");
        return result;
    }

    /* step2: get and loop all devices */
    const auto &devices = VerbsDevice::AllDevices();
    GEN_LOG_DEBUG("Got all devices, device count(" << devices.size() << ")");
    for (const auto &item : devices) {
        VerbsContextPtr ctx;
        result = VerbsContext::CreateContext(item.first, ctx);
        if (result != GEN_OK) {
            GEN_LOG_WARN("Create context for device(" << item.first << ") failed, result(" << result << ")");
            continue;
        }

        ctx->SetUserData(static_cast<uint64_t>(HOST_RDMA_GENETTY));

        contexts_.push_back(mf::MakeRef<HostVerbsContext>(HOST_RDMA_GENETTY, ctx).Get());
        GEN_LOG_DEBUG("Add context(" << ctx->Id() << ") for device(" << item.first << ")");
    }

    if (contexts_.empty()) {
        GEN_LOG_ERROR("Add all devices failed, as no context created, device count(" << devices.size() << ")");
        return GEN_DEVICE_ERROR;
    }

    inited_ = true;
    GEN_LOG_DEBUG("Add all devices finished, context count(" << contexts_.size() << ")");
    return GEN_OK;
}

void HostVerbsContextPool::CloseAllDevices() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Close all devices skipped, as not inited");
        return;
    }

    CloseAllDevicesInner();
    inited_ = false;
}

void HostVerbsContextPool::CloseAllDevicesInner() noexcept
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

bool HostVerbsContextPool::IsBadPort(uint16_t port) const noexcept
{
    auto iter = badPorts_.find(port);
    if (iter == badPorts_.end()) {
        return false;
    }

    /* the port is treated as bad until the recover window is passed */
    return (MonotonicTime::TimeNs() - iter->second) < (UB_BAD_PORT_RECOVER_SECONDS * 1000000000UL);
}

Result HostVerbsContextPool::SelectContext(uint16_t deviceId, HostVerbsContextPtr &ctx) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_ERROR("Select context failed, as pool is not inited, device id(" << deviceId << ")");
        return GEN_ERROR;
    }

    for (const auto &item : contexts_) {
        if (item == nullptr || item->wrapCtx_ == nullptr || item->wrapCtx_->Device() == nullptr) {
            continue;
        }

        const auto &verbsCtx = item->wrapCtx_;
        if (deviceId != UINT16_MAX && verbsCtx->Device()->DeviceId() != deviceId) {
            continue;
        }

        auto port = verbsCtx->Device()->PortIndex();
        if (IsBadPort(port)) {
            GEN_LOG_WARN("Skip bad port(" << port << ") of device(" << verbsCtx->Device()->DeviceName() << ")");
            continue;
        }

        ctx = item;
        GEN_LOG_DEBUG("Select context(" << verbsCtx->Id() << ") of device(" << verbsCtx->Device()->DeviceName()
                                        << ") for device id(" << deviceId << ")");
        return GEN_OK;
    }

    GEN_LOG_ERROR("Select context failed, as no device matched, device id(" << deviceId << "), context count("
                                                                            << contexts_.size() << ")");
    return GEN_INVALID_PARAM;
}

Result HostVerbsContextPool::SelectContext(const std::string &ipWithMask, HostVerbsContextPtr &ctx) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_ERROR("Select context failed, as pool is not inited, ip(" << ipWithMask << ")");
        return GEN_ERROR;
    }

    VerbsContextPtr matched;
    VerbsGidInfo selected;
    for (const auto &item : contexts_) {
        if (item == nullptr || item->wrapCtx_ == nullptr || item->wrapCtx_->Device() == nullptr) {
            continue;
        }

        const auto &verbsCtx = item->wrapCtx_;
        if (!ipWithMask.empty() && !verbsCtx->Device()->MatchIp(ipWithMask)) {
            continue;
        }

        auto port = verbsCtx->Device()->PortIndex();
        if (IsBadPort(port)) {
            GEN_LOG_WARN("Skip bad port(" << port << ") of device(" << verbsCtx->Device()->DeviceName() << ")");
            continue;
        }

        if (!ipWithMask.empty()) {
            VerbsGidInfo candidate;
            if (verbsCtx->Device()->GidForIp(ipWithMask, candidate) != GEN_OK || candidate.index > UINT8_MAX) {
                continue;
            }
            if (item->gid_.index != candidate.index) {
                matched = verbsCtx;
                selected = candidate;
                continue;
            }
        }
        ctx = item;
        GEN_LOG_DEBUG("Select context(" << verbsCtx->Id() << ") of device(" << verbsCtx->Device()->DeviceName()
                                        << ") for ip(" << ipWithMask << ")");
        return GEN_OK;
    }

    if (matched != nullptr) {
        ctx = mf::MakeRef<HostVerbsContext>(HOST_RDMA_GENETTY, matched);
        ctx->gid_ = selected;
        contexts_.push_back(ctx);
        return GEN_OK;
    }

    GEN_LOG_ERROR("Select context failed, as no device matched, ip(" << ipWithMask << "), context count("
                                                                     << contexts_.size() << ")");
    return GEN_INVALID_PARAM;
}

Result HostVerbsContextPool::Lookup(genetty_context_t ctx, HostVerbsContextPtr &out) noexcept
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
        auto verbsHandle = reinterpret_cast<genetty_context_t>(item->wrapCtx_.Get());
        if (wrapHandle == ctx || verbsHandle == ctx) {
            out = item;
            GEN_LOG_DEBUG("Lookup context(" << item->wrapCtx_->Id() << ") success, ctx(" << ctx << ")");
            return GEN_OK;
        }
    }

    GEN_LOG_ERROR("Lookup context failed, as ctx(" << ctx << ") is not found, context count(" << contexts_.size()
                                                   << ")");
    return GEN_NOT_FOUND;
}

void HostVerbsContextPool::RecordBadPort(uint8_t badPortId) noexcept
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
