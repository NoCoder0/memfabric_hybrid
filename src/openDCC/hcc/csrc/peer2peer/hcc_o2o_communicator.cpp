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
 * @file hcc_o2o_communicator.cpp
 * @brief Implementation of the internal one-to-one communicator.
 */

#include "hcc_o2o_communicator.h"

namespace ock {
namespace openDCC {
Result HccO2OCommunicator::CreateTunnel(hcc_tunnel_options_t *options, HccTunnelPtr &tunnel)
{
    (void)options;
    (void)tunnel;
    return HCC_OK;
}

Result HccO2OCommunicator::LookupTunnel(hcc_tunnel_t tunnel, HccTunnelPtr &out) noexcept
{
    (void)tunnel;
    (void)out;
    return HCC_OK;
}

void HccO2OCommunicator::DestroyTunnel(hcc_tunnel_t tunnel, uint32_t flags) noexcept
{
    (void)tunnel;
    (void)flags;
}

Result HccO2OCommunicator::RegisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept
{
    (void)segment;
    (void)flags;
    return HCC_OK;
}

void HccO2OCommunicator::UnregisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept
{
    (void)segment;
    (void)flags;
}
} // namespace openDCC
} // namespace ock
