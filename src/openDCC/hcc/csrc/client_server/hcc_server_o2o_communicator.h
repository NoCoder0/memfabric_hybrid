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
#ifndef MF_OPENDCC_HCC_SERVER_O2O_COMMUNICATOR_H
#define MF_OPENDCC_HCC_SERVER_O2O_COMMUNICATOR_H

#include "hcc_types.h"
#include "hcc_server_client.h"
namespace ock {
namespace openDCC {
class HccServerO2OCommunicator : public HccCommunicator {
public:
    Result CreateTunnel(hcc_tunnel_options_t *options, HccTunnelPtr &tunnel) override;
    Result LookupTunnel(hcc_tunnel_t tunnel, HccTunnelPtr &out) noexcept override;
    void DestroyTunnel(hcc_tunnel_t tunnel, uint32_t flags) noexcept override;

    Result RegisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept override;
    void UnregisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept override;

    Result HandlePeerMessage(const TcpMessage &msg, bool isClient) noexcept;

private:


};
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_SERVER_O2O_COMMUNICATOR_H
