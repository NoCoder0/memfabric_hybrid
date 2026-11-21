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
 * @file hcc_p2p.cpp
 * @brief Implementation of the HCC point-to-point APIs.
 */

#include "hcc_p2p.h"
#include "common/hcc_common_includes.h"
#include "hcc_types.h"
#include "peer2peer/hcc_o2o_communicator.h"
#include "peer2peer/hcc_o2o_tunnel.h"

namespace ock {
namespace openDCC {
#ifdef __cplusplus
extern "C" {
#endif

int32_t HCC_API hcc_init(PROTOCOL protocol, const char *vendor, uint32_t flags)
{
    /* step1: check param */
    HCC_ASSERT_MSG_RETURN(protocol == GEN_PROT_HOST_RDMA || protocol == GEN_PROT_DEVICE_RDMA,
                          "Unsupported HCC P2P protocol", HCC_NOT_SUPPORTED);
    HCC_ASSERT_MSG_RETURN(vendor != nullptr, "hcc init failed as vendor is null", HCC_INVALID_PARAM);

    /* step2: create HccInitOptions */
    HccInitOptions options{protocol, vendor, flags, nullptr};

    /* step3: call Hcc::Instance to initialize */
    Result result = Hcc::Instance().Init(options);
    if (result != HCC_OK) {
        HCC_LOG_ERROR("hcc init failed, protocol(" << protocol << "), vendor(" << vendor << "), result(" << result
                                                   << ")");
        return result;
    }

    return HCC_OK;
}

void HCC_API hcc_uninit(uint32_t flags)
{
    /* call Hcc::Instance() to uninit */
    Hcc::Instance().Uninit(flags);
}

void HCC_API hcc_options_init(hcc_options_t *options)
{
    HCC_ASSERT_MSG_NO_RETURN(options != nullptr, "HCC options is null");
    *options = {};
    options->provider = HOST_RDMA_GENETTY;
}

hcc_t HCC_API hcc_create(hcc_options_t *options)
{
    HCC_ASSERT_MSG_RETURN(options != nullptr && options->mode == TM_ONE_2_ONE && options->flags == 0 &&
                              (options->provider == HOST_RDMA_GENETTY || options->provider == DEVICE_RDMA_GENETTY) &&
                              memchr(options->ip, 0, sizeof(options->ip)) != nullptr,
                          "Invalid HCC communicator options", nullptr);
    hcc_t out = nullptr;
    auto ret = Hcc::Instance().CreateP2PCommunicator(options, &out);
    return ret == HCC_OK ? out : nullptr;
}

int32_t HCC_API hcc_destroy(hcc_t *handle, uint32_t flags)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr && *handle != nullptr, "Invalid HCC handle", HCC_INVALID_PARAM);
    auto comm = static_cast<HccO2OCommunicator *>(*handle);
    auto ret = comm->Close();
    if (ret == HCC_OK) {
        Hcc::Instance().DestroyP2PCommunicator(*handle, flags);
        *handle = nullptr;
    }
    return ret;
}

int32_t HCC_API hcc_memory_register(hcc_t *handle, hcc_mem_segment_t *segment, uint32_t flags)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr && *handle != nullptr && segment != nullptr && segment->size != 0 &&
                              segment->size <= UINT64_MAX - segment->address,
                          "Invalid HCC memory registration", HCC_INVALID_PARAM);
    return static_cast<HccO2OCommunicator *>(*handle)->RegisterSegment(segment, flags);
}

int32_t HCC_API hcc_memory_unregister(hcc_t *handle, hcc_mem_segment_t *segment, uint32_t flags)
{
    (void)flags;
    HCC_ASSERT_MSG_RETURN(handle != nullptr && *handle != nullptr && segment != nullptr,
                          "Invalid HCC memory unregistration", HCC_INVALID_PARAM);
    return static_cast<HccO2OCommunicator *>(*handle)->Unregister(segment);
}

hcc_tunnel_t HCC_API hcc_tunnel_create(hcc_t handle, hcc_tunnel_options_t *options)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr && options != nullptr && options->queue_depth != 0 &&
                              options->queue_depth <= INT32_MAX && options->traffic_class <= UINT8_MAX,
                          "Invalid HCC tunnel options", nullptr);
    HccTunnelPtr tunnel;
    auto ret = static_cast<HccO2OCommunicator *>(handle)->CreateTunnel(options, tunnel);
    return ret == HCC_OK ? tunnel.Get() : nullptr;
}

int32_t HCC_API hcc_tunnel_destroy(hcc_tunnel_t *handle, uint32_t flags)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr && *handle != nullptr, "Invalid HCC tunnel", HCC_INVALID_PARAM);
    auto tunnel = static_cast<HccO2OTunnel *>(*handle);
    auto ret = tunnel->Close();
    if (ret == HCC_OK) {
        tunnel->Owner()->DestroyTunnel(*handle, flags);
        *handle = nullptr;
    }
    return ret;
}

int32_t HCC_API hcc_tunnel_export(hcc_tunnel_t *handle, uint32_t flags, hcc_link_info_t *info)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr && *handle != nullptr && info != nullptr, "Invalid HCC link export",
                          HCC_INVALID_PARAM);
    return static_cast<HccO2OTunnel *>(*handle)->ExportLinkInfo(flags, info);
}

int32_t HCC_API hcc_tunnel_import(hcc_tunnel_t *handle, uint32_t flags, hcc_link_info_t *info)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr && *handle != nullptr && info != nullptr && info->data != nullptr &&
                              info->length != 0,
                          "Invalid HCC link import", HCC_INVALID_PARAM);
    return static_cast<HccO2OTunnel *>(*handle)->ImportLinkInfo(flags, info);
}

void HCC_API hcc_tunnel_free_link_info(hcc_link_info_t *info)
{
    genetty_link_info_free(info);
    if (info != nullptr) {
        *info = {};
    }
}

void HCC_API hcc_memory_free_info(hcc_memory_info_t *info)
{
    genetty_memory_free_info(info);
    if (info != nullptr) {
        *info = {};
    }
}

int32_t HCC_API hcc_tunnel_submit(hcc_tunnel_t tunnel, genetty_op_type_t operation, const hcc_copy_desc_t *descs,
                                  uint32_t count, hcc_request_t *request)
{
    HCC_ASSERT_MSG_RETURN(tunnel != nullptr && descs != nullptr && count != 0 && request != nullptr &&
                              (operation == OP_READ || operation == OP_WRITE),
                          "Invalid HCC batch submission", HCC_INVALID_PARAM);
    *request = nullptr;
    return static_cast<HccO2OTunnel *>(tunnel)->Submit(operation, descs, count, request);
}

int32_t HCC_API hcc_request_wait(hcc_request_t handle, uint32_t timeout_ms)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr, "Invalid HCC request", HCC_INVALID_PARAM);
    auto request = static_cast<HccRequest *>(handle);
    return request->tunnel->Wait(*request, timeout_ms);
}

int32_t HCC_API hcc_request_release(hcc_request_t *handle)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr && *handle != nullptr, "Invalid HCC request", HCC_INVALID_PARAM);
    auto request = static_cast<HccRequest *>(*handle);
    auto ret = request->tunnel->Release(request);
    if (ret == HCC_OK) {
        delete request;
        *handle = nullptr;
    }
    return ret;
}

int32_t HCC_API hcc_tunnel_quiesce(hcc_tunnel_t handle)
{
    HCC_ASSERT_MSG_RETURN(handle != nullptr, "Invalid HCC tunnel", HCC_INVALID_PARAM);
    return static_cast<HccO2OTunnel *>(handle)->Quiesce();
}

#ifdef __cplusplus
}
#endif
} // namespace openDCC
} // namespace ock
