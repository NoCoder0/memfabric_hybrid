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
 * @file hcc_o2o_tunnel.cpp
 * @brief Implementation of the internal one-to-one tunnel.
 */

#include "hcc_o2o_tunnel.h"

namespace ock {
namespace openDCC {
Result HccO2OTunnel::ExportLinkInfo(uint32_t flags, hcc_link_info_t *link_info) noexcept
{
    (void)flags;
    (void)link_info;
    return HCC_OK;
}

Result HccO2OTunnel::ImportLinkInfo(uint32_t flags, hcc_link_info_t *link_info) noexcept
{
    (void)flags;
    (void)link_info;
    return HCC_OK;
}
} // namespace openDCC
} // namespace ock
