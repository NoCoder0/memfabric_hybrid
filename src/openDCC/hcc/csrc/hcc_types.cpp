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

#include <sys/time.h>

namespace ock {
namespace openDCC {
std::atomic<uint64_t> HccCommunicator::NEXT_ID{0};

Result Hcc::Init(const HccInitOptions &options) noexcept
{
    (void)options;
    return HCC_OK;
}

void Hcc::Uninit(uint32_t flags) noexcept
{
}

Result Hcc::CreateP2PCommunicator(hcc_options_t *options, hcc_t *out) noexcept
{
    (void)options;
    (void)out;
    return HCC_OK;
}

Result Hcc::LookupP2PCommunicator(hcc_t h, HccCommunicatorPtr &out) noexcept
{
    (void)h;
    (void)out;
    return HCC_OK;
}

void Hcc::DestroyP2PCommunicator(hcc_t h, uint32_t flags) noexcept
{
    (void)h;
    (void)flags;
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
