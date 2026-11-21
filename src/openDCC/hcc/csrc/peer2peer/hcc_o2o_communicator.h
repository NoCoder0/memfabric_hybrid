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
#ifndef MF_OPENDCC_HCC_O2O_COMMUNICATOR_H
#define MF_OPENDCC_HCC_O2O_COMMUNICATOR_H

#include <map>
#include <memory>
#include "hcc_types.h"
#include "genetty.h"

namespace ock {
namespace openDCC {
struct HccMemory {
    hcc_mem_segment_t segment;
    uint32_t references = 1;
};

class HccO2OCommunicator final : public HccCommunicator {
public:
    Result Initialize(const hcc_options_t &options);
    Result Close();
    Result CreateTunnel(hcc_tunnel_options_t *options, HccTunnelPtr &tunnel) override;
    Result LookupTunnel(hcc_tunnel_t tunnel, HccTunnelPtr &out) noexcept override;
    void DestroyTunnel(hcc_tunnel_t tunnel, uint32_t flags) noexcept override;
    Result RegisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept override;
    void UnregisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept override;
    Result Unregister(hcc_mem_segment_t *segment);
    std::shared_ptr<HccMemory> FindMemory(uint64_t address, uint64_t size, uint32_t lkey);

private:
    std::mutex mutex_;
    genetty_context_t context_ = nullptr;
    std::map<uint64_t, std::shared_ptr<HccMemory>> memories_;
    std::map<hcc_tunnel_t, HccTunnelPtr> tunnels_;
};
} // namespace openDCC
} // namespace ock
#endif
