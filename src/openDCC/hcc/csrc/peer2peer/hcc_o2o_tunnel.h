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
#ifndef MF_OPENDCC_HCC_O2O_TUNNEL_H
#define MF_OPENDCC_HCC_O2O_TUNNEL_H

#include <chrono>
#include <memory>
#include <vector>
#include "hcc_o2o_communicator.h"

namespace ock {
namespace openDCC {
class HccO2OTunnel;
struct HccRequest {
    Ref<HccO2OTunnel> tunnel;
    std::vector<hcc_copy_desc_t> descriptors;
    std::vector<std::shared_ptr<HccMemory>> memories;
    genetty_op_type_t operation = OP_READ;
    size_t index = 0;
    uint64_t offset = 0;
    uint64_t completion = 0;
    uint32_t postedSize = 0;
    Result result = HCC_BUSY;
    bool posted = false;
};

class HccO2OTunnel final : public HccTunnel {
public:
    explicit HccO2OTunnel(HccO2OCommunicator *owner) : owner_(owner) {}
    Result Initialize(genetty_context_t context, const hcc_tunnel_options_t &options);
    Result Close();
    Result Quiesce();
    char *GetUrl() noexcept override
    {
        return nullptr;
    }
    Result ExportLinkInfo(uint32_t flags, hcc_link_info_t *info) noexcept override;
    Result ImportLinkInfo(uint32_t flags, hcc_link_info_t *info) noexcept override;
    Result Submit(genetty_op_type_t operation, const hcc_copy_desc_t *descs, uint32_t count, hcc_request_t *out);
    Result Wait(HccRequest &request, uint32_t timeoutMs);
    Result Release(HccRequest *request);
    HccO2OCommunicator *Owner() const
    {
        return owner_;
    }

private:
    Result Progress(HccRequest &request);
    Result Fail(HccRequest &request, Result error);
    std::mutex mutex_;
    HccO2OCommunicator *owner_;
    genetty_cq_t cq_ = nullptr;
    genetty_t qp_ = nullptr;
    HccRequest *request_ = nullptr;
    uint64_t nextId_ = 0;
    bool ready_ = false;
    bool stopped_ = false;
};
} // namespace openDCC
} // namespace ock
#endif
