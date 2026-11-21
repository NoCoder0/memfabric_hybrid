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
#include "hcc_o2o_communicator.h"
#include "hcc_o2o_tunnel.h"

namespace ock {
namespace openDCC {
Result HccO2OCommunicator::Initialize(const hcc_options_t &options)
{
    if (options.provider == DEVICE_RDMA_GENETTY) {
        context_ = genetty_context_get_by_id(options.provider, options.logic_device_id, 0);
    } else {
        context_ = genetty_context_get_by_ip(options.provider, const_cast<char *>(options.ip), 0);
    }
    HCC_ASSERT_MSG_RETURN(context_ != nullptr, "Get HCC context failed", HCC_DEVICE_ERROR);
    return HCC_OK;
}

Result HccO2OCommunicator::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    HCC_ASSERT_MSG_RETURN(tunnels_.empty(), "Destroy communicator with live tunnels", HCC_BUSY);
    while (!memories_.empty()) {
        auto iter = memories_.begin();
        HCC_ASSERT_MSG_RETURN(iter->second.use_count() == 1, "MR is still in use", HCC_BUSY);
        if (genetty_memory_unregister(context_, &iter->second->segment, 0) != GEN_OK) {
            HCC_LOG_ERROR("Unregister MR failed, address(" << iter->first << ")");
            return HCC_DEVICE_ERROR;
        }
        memories_.erase(iter);
    }
    genetty_context_return(context_, 0);
    context_ = nullptr;
    return HCC_OK;
}

Result HccO2OCommunicator::CreateTunnel(hcc_tunnel_options_t *options, HccTunnelPtr &out)
{
    auto tunnel = mf::MakeRef<HccO2OTunnel>(this);
    HCC_ASSERT_MSG_RETURN(tunnel != nullptr, "Allocate HCC tunnel failed", HCC_MALLOC_FAILED);
    std::lock_guard<std::mutex> lock(mutex_);
    auto ret = tunnel->Initialize(context_, *options);
    if (ret != HCC_OK) {
        return ret;
    }
    out = tunnel.Get();
    tunnels_[tunnel.Get()] = out;
    return HCC_OK;
}

Result HccO2OCommunicator::LookupTunnel(hcc_tunnel_t handle, HccTunnelPtr &out) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = tunnels_.find(handle);
    HCC_ASSERT_MSG_RETURN(iter != tunnels_.end(), "HCC tunnel not found", HCC_NOT_FOUND);
    out = iter->second;
    return HCC_OK;
}

void HccO2OCommunicator::DestroyTunnel(hcc_tunnel_t handle, uint32_t flags) noexcept
{
    (void)flags;
    std::lock_guard<std::mutex> lock(mutex_);
    tunnels_.erase(handle);
}

Result HccO2OCommunicator::RegisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = memories_.find(segment->address);
    if (iter != memories_.end()) {
        HCC_ASSERT_MSG_RETURN(iter->second->segment.size == segment->size &&
                                  iter->second->segment.type == segment->type,
                              "Conflicting HCC MR registration", HCC_INVALID_PARAM);
        *segment = iter->second->segment;
        ++iter->second->references;
        return HCC_OK;
    }
    auto memory = std::make_shared<HccMemory>();
    auto ret = genetty_memory_register(context_, segment, flags);
    if (ret != GEN_OK) {
        HCC_LOG_ERROR("Register HCC MR failed, result(" << ret << ")");
        return HCC_DEVICE_ERROR;
    }
    memory->segment = *segment;
    memories_.emplace(segment->address, memory);
    return HCC_OK;
}

Result HccO2OCommunicator::Unregister(hcc_mem_segment_t *segment)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = memories_.find(segment->address);
    HCC_ASSERT_MSG_RETURN(iter != memories_.end(), "HCC MR not found", HCC_NOT_FOUND);
    if (iter->second->references > 1) {
        --iter->second->references;
        return HCC_OK;
    }
    HCC_ASSERT_MSG_RETURN(iter->second.use_count() == 1, "Unregister busy HCC MR", HCC_BUSY);
    auto ret = genetty_memory_unregister(context_, &iter->second->segment, 0);
    if (ret != GEN_OK) {
        HCC_LOG_ERROR("Unregister HCC MR failed, result(" << ret << ")");
        return HCC_DEVICE_ERROR;
    }
    memories_.erase(iter);
    return HCC_OK;
}

void HccO2OCommunicator::UnregisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept
{
    (void)flags;
    (void)Unregister(segment);
}

std::shared_ptr<HccMemory> HccO2OCommunicator::FindMemory(uint64_t address, uint64_t size, uint32_t lkey)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &entry : memories_) {
        const auto &segment = entry.second->segment;
        if (segment.lkey == lkey && address >= segment.address && address - segment.address <= segment.size &&
            size <= segment.size - (address - segment.address)) {
            return entry.second;
        }
    }
    HCC_LOG_ERROR("Unregistered HCC local range, address(" << address << "), size(" << size << ")");
    return nullptr;
}
} // namespace openDCC
} // namespace ock
