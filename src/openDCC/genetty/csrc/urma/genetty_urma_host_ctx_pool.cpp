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
#include "genetty_urma_host_ctx_pool.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <iterator>

#include "common/genetty_functions.h"

namespace ock {
namespace openDCC {
namespace urma {
namespace {
std::atomic<uint32_t> SEG_TOKEN_GENERATOR(1);
constexpr uint32_t SEG_ACCESS_READ_WRITE = (0x1 << 1) | (0x1 << 2); /* remote read and write */
} // namespace

UrmaHostContext::UrmaHostContext(genetty_type_t type, const UrmaContextPtr &context) noexcept
    : GenettyContext(type), ctx_(context)
{
    inited_ = (ctx_ != nullptr);
}

UrmaHostContext::~UrmaHostContext()
{
    Destroy();
}

Result UrmaHostContext::RegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Register memory failed, as segment is null", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    std::lock_guard<std::mutex> lock(mutex_);
    if (UNLIKELY(!inited_)) {
        GEN_LOG_ERROR("Register memory failed, as context is not inited, address(" << segment->address << ")");
        return GEN_NOT_INITIALIZED;
    }

    auto memoryInfo = MakeMemoryInfo(*segment);
    auto iter = localSegs_.find(memoryInfo);
    if (iter != localSegs_.end()) {
        for (const auto &item : localSegsByToken_) {
            if (item.second == iter->second) {
                segment->lkey = item.first;
                segment->rkey = item.first;
                break;
            }
        }

        GEN_LOG_DEBUG("Register memory skipped, as already registered, address(" << segment->address << ")");
        return GEN_OK;
    }

    /* step2: register the segment with a token, the token is used as both the local and remote key */
    uint32_t token = segment->lkey != 0 ? segment->lkey : SEG_TOKEN_GENERATOR.fetch_add(1);
    urma_seg_cfg_t cfg{};
    cfg.va = segment->address;
    cfg.len = segment->size;
    cfg.token_value.token = token;
    cfg.flag.bs.token_policy = URMA_TOKEN_PLAIN_TEXT;
    cfg.flag.bs.access = SEG_ACCESS_READ_WRITE;

    auto targetSeg = UrmaApi::urma_register_seg(ctx_->Handle(), &cfg);
    if (UNLIKELY(targetSeg == nullptr)) {
        GEN_LOG_ERROR("[URMA_API] urma_register_seg failed, address(" << segment->address << "), size("
                                                                     << segment->size << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    /* step3: save the segment and fill the keys into the given segment */
    localSegs_[memoryInfo] = targetSeg;
    localSegsByToken_[token] = targetSeg;
    segment->lkey = token;
    segment->rkey = token;

    GEN_LOG_DEBUG("Register memory finished, address(" << segment->address << "), size(" << segment->size
                                                       << "), token(" << token << ")");
    return GEN_OK;
}

Result UrmaHostContext::UnRegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Unregister memory failed, as segment is null", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    std::lock_guard<std::mutex> lock(mutex_);
    auto memoryInfo = MakeMemoryInfo(*segment);
    auto iter = localSegs_.find(memoryInfo);
    if (UNLIKELY(iter == localSegs_.end())) {
        GEN_LOG_ERROR("Unregister memory failed, as the memory is not registered, address(" << segment->address
                                                                                          << "), size("
                                                                                          << segment->size << ")");
        return GEN_NOT_FOUND;
    }

    /* step2: remove the segment and unregister it */
    auto targetSeg = iter->second;
    localSegs_.erase(iter);
    for (auto item = localSegsByToken_.begin(); item != localSegsByToken_.end();) {
        item = item->second == targetSeg ? localSegsByToken_.erase(item) : std::next(item);
    }

    if (UNLIKELY(UrmaApi::urma_unregister_seg(targetSeg) != URMA_SUCCESS)) {
        GEN_LOG_ERROR("[URMA_API] urma_unregister_seg failed, address(" << segment->address << "), size("
                                                                       << segment->size << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    GEN_LOG_DEBUG("Unregister memory finished, address(" << segment->address << "), size(" << segment->size << ")");
    return GEN_OK;
}

Result UrmaHostContext::ImportMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Import memory failed, as segment is null", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    std::lock_guard<std::mutex> lock(mutex_);
    auto memoryInfo = MakeMemoryInfo(*segment);
    if (remoteSegs_.find(memoryInfo) != remoteSegs_.end()) {
        GEN_LOG_DEBUG("Import memory skipped, as already imported, address(" << segment->address << ")");
        return GEN_OK;
    }

    /* step2: the peer eid is carried by the jetty which is imported by genetty import */
    bool eidReady = false;
    for (const auto &byte : peerEid_.raw) {
        eidReady = eidReady || byte != 0;
    }

    if (UNLIKELY(!eidReady)) {
        GEN_LOG_ERROR("Import memory failed, as the peer eid is not ready, address(" << segment->address << "), size("
                                                                                    << segment->size << ")");
        return GEN_ERROR;
    }

    /* step3: import the peer segment with its address and token */
    urma_seg_t seg{};
    seg.ubva.eid = peerEid_;
    seg.ubva.uasid = 0;
    seg.ubva.va = segment->address;
    seg.len = segment->size;
    seg.attr.bs.token_policy = URMA_TOKEN_PLAIN_TEXT;
    seg.attr.bs.access = SEG_ACCESS_READ_WRITE;

    urma_token_t token{segment->rkey};
    urma_import_seg_flag_t importFlag{};
    auto targetSeg = UrmaApi::urma_import_seg(ctx_->Handle(), &seg, &token, 0, importFlag);
    if (UNLIKELY(targetSeg == nullptr)) {
        GEN_LOG_ERROR("[URMA_API] urma_import_seg failed, address(" << segment->address << "), size("
                                                                   << segment->size << "), token(" << segment->rkey
                                                                   << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    remoteSegs_[memoryInfo] = targetSeg;
    remoteSegsByToken_[segment->rkey] = targetSeg;

    GEN_LOG_DEBUG("Import memory finished, address(" << segment->address << "), size(" << segment->size << "), token("
                                                     << segment->rkey << ")");
    return GEN_OK;
}

void UrmaHostContext::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Destroy context skipped, as it is not inited");
        return;
    }

    for (const auto &item : remoteSegs_) {
        if (item.second != nullptr) {
            UrmaApi::urma_unimport_seg(item.second);
        }
    }

    for (const auto &item : localSegs_) {
        if (item.second != nullptr) {
            UrmaApi::urma_unregister_seg(item.second);
        }
    }

    remoteSegs_.clear();
    remoteSegsByToken_.clear();
    localSegs_.clear();
    localSegsByToken_.clear();
    inited_ = false;

    GEN_LOG_DEBUG("Destroy context finished, context id(" << ctx_->Id() << ")");
}

urma_target_seg_t *UrmaHostContext::FindLocalSeg(uint32_t lkey) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = localSegsByToken_.find(lkey);
    return iter == localSegsByToken_.end() ? nullptr : iter->second;
}

urma_target_seg_t *UrmaHostContext::FindRemoteSeg(uint32_t rkey) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = remoteSegsByToken_.find(rkey);
    return iter == remoteSegsByToken_.end() ? nullptr : iter->second;
}

MemoryInfo UrmaHostContext::MakeMemoryInfo(const genetty_memory_segment_t &segment) noexcept
{
    MemoryInfo memoryInfo;
    memoryInfo.MemType(segment.type);
    memoryInfo.Address(segment.address);
    memoryInfo.Size(segment.size);
    return memoryInfo;
}

namespace {
bool MatchEid(const UrmaHostContextPtr &ctx, const std::string &eidWithMask)
{
    if (eidWithMask.empty()) {
        return true;
    }

    if (ctx == nullptr || ctx->Context() == nullptr) {
        return false;
    }

    char text[64] = {};
    snprintf(text, sizeof(text), EID_FMT, EID_ARGS(ctx->Context()->EidInfo().eid));
    return eidWithMask == text;
}
} // namespace

Result UrmaHostContextPool::AddAllDevices() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Add all devices skipped, as already inited, context count(" << contexts_.size() << ")");
        return GEN_OK;
    }

    /* step1: init devices */
    UrmaDevice::Init();

    /* step2: create one context for each device with its first eid */
    uint16_t deviceId = 0;
    for (const auto &item : UrmaDevice::AllDevices()) {
        const auto &device = item.second;
        if (device == nullptr || device->EidList().empty()) {
            deviceId++;
            continue;
        }

        UrmaContextPtr urmaCtx;
        auto result = UrmaContext::CreateContext(device->DeviceName(), device->EidList()[0].eid_index, urmaCtx);
        if (result != GEN_OK) {
            GEN_LOG_WARN("Create context for device(" << device->DeviceName() << ") failed, result(" << result << ")");
            deviceId++;
            continue;
        }

        auto hostCtx = UrmaHostContextPtr(mf::MakeRef<UrmaHostContext>(HOST_URMA_GENETTY, urmaCtx).Get());
        if (UNLIKELY(hostCtx == nullptr)) {
            GEN_LOG_ERROR("Create UrmaHostContext object failed, probably out of memory, device("
                          << device->DeviceName() << ")");
            deviceId++;
            continue;
        }

        contexts_.push_back(hostCtx);
        deviceIds_.push_back(deviceId);
        GEN_LOG_DEBUG("Add context(" << urmaCtx->Id() << ") for device(" << device->DeviceName() << ")");
        deviceId++;
    }

    if (contexts_.empty()) {
        GEN_LOG_ERROR("Add all devices failed, as no context created");
        return GEN_DEVICE_ERROR;
    }

    inited_ = true;
    GEN_LOG_DEBUG("Add all devices finished, context count(" << contexts_.size() << ")");
    return GEN_OK;
}

void UrmaHostContextPool::CloseAllDevices() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Close all devices skipped, as not inited");
        return;
    }

    CloseAllDevicesInner();
    inited_ = false;
}

void UrmaHostContextPool::CloseAllDevicesInner() noexcept
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
    deviceIds_.clear();
    badPorts_.clear();
}

Result UrmaHostContextPool::SelectContext(uint16_t deviceId, UrmaHostContextPtr &ctx) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_ERROR("Select context failed, as pool is not inited, device id(" << deviceId << ")");
        return GEN_ERROR;
    }

    for (size_t i = 0; i < contexts_.size(); i++) {
        if (deviceId != UINT16_MAX && deviceIds_[i] != deviceId) {
            continue;
        }

        auto port = contexts_[i]->Context()->EidInfo().eid_index;
        if (IsBadPort(port)) {
            GEN_LOG_WARN("Skip bad port(" << port << ") of device(" << contexts_[i]->Context()->Device()->DeviceName()
                                          << ")");
            continue;
        }

        ctx = contexts_[i];
        GEN_LOG_DEBUG("Select context(" << ctx->Context()->Id() << ") of device("
                                        << ctx->Context()->Device()->DeviceName() << ") for device id(" << deviceId
                                        << ")");
        return GEN_OK;
    }

    GEN_LOG_ERROR("Select context failed, as no device matched, device id(" << deviceId << "), context count("
                                                                           << contexts_.size() << ")");
    return GEN_INVALID_PARAM;
}

Result UrmaHostContextPool::SelectContext(const std::string &eidWithMask, UrmaHostContextPtr &ctx) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_ERROR("Select context failed, as pool is not inited, eid(" << eidWithMask << ")");
        return GEN_ERROR;
    }

    for (auto &item : contexts_) {
        if (!MatchEid(item, eidWithMask)) {
            continue;
        }

        auto port = item->Context()->EidInfo().eid_index;
        if (IsBadPort(port)) {
            GEN_LOG_WARN("Skip bad port(" << port << ") of device(" << item->Context()->Device()->DeviceName() << ")");
            continue;
        }

        ctx = item;
        GEN_LOG_DEBUG("Select context(" << ctx->Context()->Id() << ") of device("
                                        << ctx->Context()->Device()->DeviceName() << ") for eid(" << eidWithMask
                                        << ")");
        return GEN_OK;
    }

    GEN_LOG_ERROR("Select context failed, as no device matched, eid(" << eidWithMask << "), context count("
                                                                     << contexts_.size() << ")");
    return GEN_INVALID_PARAM;
}

Result UrmaHostContextPool::Lookup(genetty_context_t ctx, UrmaHostContextPtr &out) noexcept
{
    GEN_ASSERT_MSG_RETURN(ctx != nullptr, "Lookup context failed, as ctx is null", GEN_INVALID_PARAM);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_ERROR("Lookup context failed, as pool is not inited, ctx(" << ctx << ")");
        return GEN_ERROR;
    }

    for (const auto &item : contexts_) {
        if (item != nullptr && reinterpret_cast<genetty_context_t>(item.Get()) == ctx) {
            out = item;
            GEN_LOG_DEBUG("Lookup context(" << item->Context()->Id() << ") success, ctx(" << ctx << ")");
            return GEN_OK;
        }
    }

    GEN_LOG_ERROR("Lookup context failed, as ctx(" << ctx << ") is not found, context count(" << contexts_.size()
                                                   << ")");
    return GEN_NOT_FOUND;
}

uint32_t UrmaHostContextPool::Count() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(contexts_.size());
}

void UrmaHostContextPool::RecordBadPort(uint8_t badPortId) noexcept
{
    size_t badCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        badPorts_[badPortId] = MonotonicTime::TimeNs();
        badCount = badPorts_.size();
    }

    GEN_LOG_WARN("Record bad port(" << static_cast<uint32_t>(badPortId) << "), bad port count(" << badCount << ")");
}

bool UrmaHostContextPool::IsBadPort(uint16_t port) const noexcept
{
    auto iter = badPorts_.find(port);
    if (iter == badPorts_.end()) {
        return false;
    }

    /* the port is treated as bad until the recover window is passed */
    return (MonotonicTime::TimeNs() - iter->second) < (UB_BAD_PORT_RECOVER_SECONDS * 1000000000UL);
}
} // namespace urma
} // namespace openDCC
} // namespace ock
