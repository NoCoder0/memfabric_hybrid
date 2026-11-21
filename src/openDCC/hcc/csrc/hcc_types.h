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
 * @file hcc_types.h
 * @brief Internal data types shared by the HCC implementation.
 */
#ifndef MF_OPENDCC_HCC_TYPES_H
#define MF_OPENDCC_HCC_TYPES_H

#include "common/hcc_common_includes.h"
#include "hcc_def.h"

namespace ock {
namespace openDCC {

class Hcc;
class HccCqeHarvester;
class HccCommunicator;
class HccTunnel;
using HccCqeHarvesterPtr = Ref<HccCqeHarvester>;
using HccCommunicatorPtr = Ref<HccCommunicator>;
using HccTunnelPtr = Ref<HccTunnel>;

struct HccInitOptions {
    PROTOCOL protocol;
    std::string vendor;
    uint32_t flags;
    hcc_server_init_options_t *serverInitOptions;
};

class Hcc {
public:
    static Hcc &Instance() noexcept
    {
        static Hcc instance;
        return instance;
    }

    /* init and uninit */
    Result Init(const HccInitOptions &options) noexcept;
    void Uninit(uint32_t flags) noexcept;

    /* life cycle management of p2p communicator */
    Result CreateP2PCommunicator(hcc_options_t *options, hcc_t *out) noexcept;
    Result LookupP2PCommunicator(hcc_t h, HccCommunicatorPtr &out) noexcept;
    void DestroyP2PCommunicator(hcc_t h, uint32_t flags) noexcept;

    /* life cycle management of server communicator */
    Result CreateServerCommunicator(hcc_server_options_t *options, hcc_server_t *out) noexcept;
    Result LookupServerCommunicator(hcc_server_t h, HccCommunicatorPtr &out) noexcept;
    void DestroyServerCommunicator(hcc_server_t h, uint32_t flags) noexcept;

private:
    std::mutex mutex_;
    uint32_t references_ = 0;
    HccInitOptions options_{};
    std::map<hcc_t, HccCommunicatorPtr> communicators_;
    bool inited_ = false;
};

class HccCommunicator : public Referable {
public:
    virtual Result CreateTunnel(hcc_tunnel_options_t *options, HccTunnelPtr &tunnel) = 0;
    virtual Result LookupTunnel(hcc_tunnel_t tunnel, HccTunnelPtr &out) noexcept = 0;
    virtual void DestroyTunnel(hcc_tunnel_t tunnel, uint32_t flags) noexcept = 0;

    virtual Result RegisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept = 0;
    virtual void UnregisterSegment(hcc_mem_segment_t *segment, uint32_t flags) noexcept = 0;

    void GenId() noexcept;

private:
    uint64_t id_ = 0;

private:
    static std::atomic<uint64_t> NEXT_ID;
};

class HccTunnel : public Referable {
public:
    virtual char *GetUrl() noexcept = 0;
    virtual Result ExportLinkInfo(uint32_t flags, hcc_link_info_t *link_info) noexcept = 0;
    virtual Result ImportLinkInfo(uint32_t flags, hcc_link_info_t *link_info) noexcept = 0;
};

} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_TYPES_H
