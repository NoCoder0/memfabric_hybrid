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
 * @file hcc_o2o_tunnel.h
 * @brief Internal one-to-one tunnel of the HCC point-to-point path.
 */
#ifndef MF_OPENDCC_HCC_O2O_TUNNEL_H
#define MF_OPENDCC_HCC_O2O_TUNNEL_H

#include "hcc_types.h"

namespace ock {
namespace openDCC {
class HccO2OTunnel final : public HccTunnel {
public:
    Result ExportLinkInfo(uint32_t flags, hcc_link_info_t *link_info) noexcept override;
    Result ImportLinkInfo(uint32_t flags, hcc_link_info_t *link_info) noexcept override;
};
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_O2O_TUNNEL_H
