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
/**
 * @file hcc_types.cpp
 * @brief Implementation of the internal HCC data types.
 */

#include "hcc_types.h"
#include "peer2peer/hcc_o2o_communicator.h"

#include <sys/time.h>

namespace ock {
namespace openDCC {
std::atomic<uint64_t> HccCommunicator::NEXT_ID{0};

Result Hcc::Init(const HccInitOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        HCC_ASSERT_MSG_RETURN(options.protocol == options_.protocol && options.vendor == options_.vendor,
                              "Conflicting HCC initialization", HCC_INVALID_PARAM);
        ++references_;
        return HCC_OK;
    }
    auto ret = genetty_init(options.protocol, options.vendor.c_str(), GEN_INIT_LAZY_DEVICE);
    HCC_ASSERT_MSG_RETURN(ret == GEN_OK, "Initialize HCC provider failed, result(" << ret << ")", HCC_DEVICE_ERROR);
    options_ = options;
    references_ = 1;
    inited_ = true;
    return HCC_OK;
}

void Hcc::Uninit(uint32_t flags) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        return;
    }
    if (references_ == 1 && !communicators_.empty()) {
        HCC_LOG_ERROR("Cannot uninitialize HCC with live communicators");
        return;
    }
    if (--references_ == 0) {
        genetty_uninit(flags);
        inited_ = false;
    }
}

Result Hcc::CreateP2PCommunicator(hcc_options_t *options, hcc_t *out) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    HCC_ASSERT_MSG_RETURN(inited_, "HCC is not initialized", HCC_NOT_INITIALIZED);
    auto expected = options_.protocol == GEN_PROT_DEVICE_RDMA ? DEVICE_RDMA_GENETTY : HOST_RDMA_GENETTY;
    HCC_ASSERT_MSG_RETURN(options->provider == expected, "HCC provider differs from initialization", HCC_INVALID_PARAM);
    auto comm = mf::MakeRef<HccO2OCommunicator>();
    HCC_ASSERT_MSG_RETURN(comm != nullptr, "Allocate HCC communicator failed", HCC_MALLOC_FAILED);
    auto ret = comm->Initialize(*options);
    if (ret != HCC_OK) {
        return ret;
    }
    *out = comm.Get();
    communicators_[*out] = comm.Get();
    return HCC_OK;
}

Result Hcc::LookupP2PCommunicator(hcc_t handle, HccCommunicatorPtr &out) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = communicators_.find(handle);
    HCC_ASSERT_MSG_RETURN(iter != communicators_.end(), "HCC communicator not found", HCC_NOT_FOUND);
    out = iter->second;
    return HCC_OK;
}

void Hcc::DestroyP2PCommunicator(hcc_t handle, uint32_t flags) noexcept
{
    (void)flags;
    std::lock_guard<std::mutex> lock(mutex_);
    communicators_.erase(handle);
}

Result Hcc::CreateServerCommunicator(hcc_server_options_t *options, hcc_server_t *out) noexcept
{
    (void)options;
    (void)out;
    return HCC_OK;
}

Result Hcc::LookupServerCommunicator(hcc_server_t h, HccCommunicatorPtr &out) noexcept
{
    (void)h;
    (void)out;
    return HCC_OK;
}

void Hcc::DestroyServerCommunicator(hcc_server_t h, uint32_t flags) noexcept
{
    (void)h;
    (void)flags;
}

/**********************************************************************/
void HccCommunicator::GenId() noexcept
{
    /* step1: get current time in microseconds */
    struct timeval tv {};
    (void)::gettimeofday(&tv, nullptr);
    uint64_t nowUs = static_cast<uint64_t>(tv.tv_sec) * 1000000ULL + static_cast<uint64_t>(tv.tv_usec);

    /* step2: compose id_ with time and increased NEXT_ID */
    id_ = nowUs + NEXT_ID.fetch_add(1, std::memory_order_relaxed);
}
} // namespace openDCC
} // namespace ock
