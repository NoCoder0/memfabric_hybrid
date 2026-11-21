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
 * @file hcc_cs.cpp
 * @brief Implementation of the HCC client-server APIs.
 */

#include "hcc_cs.h"
#include "common/hcc_common_includes.h"
#include "hcc_types.h"

namespace ock {
namespace openDCC {
#ifdef __cplusplus
extern "C" {
#endif

int32_t HCC_API hcc_server_init(PROTOCOL protocol, const char *vendor, hcc_server_init_options_t *options)
{
    /* step1: validate param */
    HCC_ASSERT_MSG_RETURN(protocol != 0, "hcc server init failed as protocol is 0", HCC_INVALID_PARAM);
    HCC_ASSERT_MSG_RETURN(vendor != nullptr, "hcc server init failed as vendor is null", HCC_INVALID_PARAM);
    HCC_ASSERT_MSG_RETURN(options != nullptr, "hcc server init failed as options is null", HCC_INVALID_PARAM);

    /* step2: create Hcc init options */
    HccInitOptions initOptions{protocol, vendor, options->flags, options};

    /* step3: call Hcc::Init, HccServerClient need to be started in it */
    Result result = Hcc::Instance().Init(initOptions);
    if (result != HCC_OK) {
        HCC_LOG_ERROR("hcc server init failed, protocol(" << protocol << "), vendor(" << vendor << "), result("
                                                          << result << ")");
        return result;
    }

    return HCC_OK;
}

void HCC_API hcc_server_uninit(uint32_t flags)
{
    /* step1: call Hcc to do uninit */
    Hcc::Instance().Uninit(flags);
}

void HCC_API hcc_server_options_init(hcc_options_t *options)
{
    /* step1: check param */
    HCC_ASSERT_MSG_NO_RETURN(options != nullptr, "Init server options failed as options is null");

    /* step2: set default value */
    options->mode = TM_ONE_2_ONE;
    options->flags = 0;
}

hcc_server_t HCC_API hcc_server_create(hcc_options_t *options)
{
    /* step1: check param */
    HCC_ASSERT_MSG_RETURN(options != nullptr, "Create server failed as options is null", nullptr);

    /* step2: create options */
    hcc_server_options_t serverOptions{};
    serverOptions.hcc_options = *options;

    /* step3: call Hcc to create server communicator */
    hcc_server_t handle = nullptr;
    Result result = Hcc::Instance().CreateServerCommunicator(&serverOptions, &handle);
    if (result != HCC_OK) {
        HCC_LOG_ERROR("Create server failed, mode(" << options->mode << "), result(" << result << ")");
        return nullptr;
    }

    return handle;
}

void HCC_API hcc_server_destroy(hcc_server_t svr_handle, uint32_t flags)
{
    /* step1: check param */
    HCC_ASSERT_MSG_NO_RETURN(svr_handle != nullptr, "Destroy server failed as server handle is null");

    /* step2: call Hcc to destory server communicator */
    Hcc::Instance().DestroyServerCommunicator(svr_handle, flags);
}

int32_t HCC_API hcc_server_memory_register(hcc_server_t svr_handle, hcc_mem_segment_t *segment, uint32_t flags)
{
    HCC_ASSERT_MSG_RETURN(svr_handle != nullptr && svr_handle != nullptr,
                          "Register memory failed as server handle is null", HCC_INVALID_PARAM);
    HCC_ASSERT_MSG_RETURN(segment != nullptr,
                          "Register memory failed as segment is null, handle(" << svr_handle << ")",
                          HCC_INVALID_PARAM);

    HccCommunicatorPtr communicator;
    Result result = Hcc::Instance().LookupServerCommunicator(svr_handle, communicator);
    if (UNLIKELY(result != HCC_OK || communicator == nullptr)) {
        HCC_LOG_ERROR("Register memory failed as communicator not found, handle(" << svr_handle << "), result("
                                                                                  << result << ")");
        return result != HCC_OK ? result : HCC_NOT_FOUND;
    }

    return communicator->RegisterSegment(segment, flags);
}

void HCC_API hcc_server_memory_unregister(hcc_server_t svr_handle, hcc_mem_segment_t *segment, uint32_t flags)
{
    HCC_ASSERT_MSG_NO_RETURN(svr_handle != nullptr && svr_handle != nullptr,
                             "Unregister memory failed as server handle is null");
    HCC_ASSERT_MSG_NO_RETURN(segment != nullptr,
                             "Unregister memory failed as segment is null, handle(" << svr_handle << ")");

    HccCommunicatorPtr communicator;
    Result result = Hcc::Instance().LookupServerCommunicator(svr_handle, communicator);
    if (UNLIKELY(result != HCC_OK || communicator == nullptr)) {
        HCC_LOG_ERROR("Unregister memory failed as communicator not found, handle(" << svr_handle << "), result("
                                                                                    << result << ")");
        return;
    }

    communicator->UnregisterSegment(segment, flags);
}

hcc_tunnel_t HCC_API hcc_server_tunnel_create(hcc_server_t svr_handle, hcc_tunnel_options_t *options)
{
    /* step1: check param */
    HCC_ASSERT_MSG_RETURN(svr_handle != nullptr, "Create tunnel failed as server handle is null", nullptr);

    /* step2: lookup communicator */
    HccCommunicatorPtr communicator;
    Result result = Hcc::Instance().LookupServerCommunicator(svr_handle, communicator);
    if (result != HCC_OK || communicator == nullptr) {
        HCC_LOG_ERROR("Create tunnel failed as communicator not found, handle(" << svr_handle << "), result(" << result
                                                                                << ")");
        return nullptr;
    }

    /* step3: call communicator to create tunnel */
    HccTunnelPtr tunnel;
    result = communicator->CreateTunnel(options, tunnel);
    if (result != HCC_OK || tunnel == nullptr) {
        HCC_LOG_ERROR("Create tunnel failed, handle(" << svr_handle << "), result(" << result << ")");
        return nullptr;
    }

    /* step4: increase ref count and return */
    tunnel->IncreaseRef();
    return tunnel.Get();
}

HCC_API char *hcc_server_tunnel_get_url(hcc_tunnel_t tunnel_handle, uint32_t flags)
{
    (void)flags;
    /* step1: check param */
    HCC_ASSERT_MSG_RETURN(tunnel_handle != nullptr, "Get tunnel url failed as tunnel handle is null", nullptr);

    /* step2: convert HccTunnel object and call its getUrl function */
    auto tunnel = static_cast<HccTunnel *>(tunnel_handle);
    return tunnel->GetUrl();
}

void HCC_API hcc_server_tunnel_destroy(hcc_tunnel_t tunnel_handle, uint32_t flags)
{
    (void)tunnel_handle;
    (void)flags;
}

int32_t HCC_API hcc_tunnel_connect(hcc_tunnel_t tunnel_handle, char *url, uint32_t timeoutMs, uint32_t flags)
{
    (void)tunnel_handle;
    (void)url;
    (void)timeoutMs;
    (void)flags;
    return HCC_OK;
}

int32_t HCC_API hcc_tunnel_connect_async(hcc_tunnel_t tunnel_handle, char *url, uint32_t timeoutMs, uint32_t flags,
                                        uint64_t ctx,
                                        void (*callback)(hcc_tunnel_t *tunnel_handle, uint64_t ctx, int32_t result))
{
    (void)tunnel_handle;
    (void)url;
    (void)timeoutMs;
    (void)flags;
    (void)ctx;
    (void)callback;
    return HCC_OK;
}

void HCC_API hcc_tunnel_disconnect(hcc_tunnel_t tunnel_handle, uint32_t flags)
{
    (void)tunnel_handle;
    (void)flags;
}

#ifdef __cplusplus
}
#endif
} // namespace openDCC
} // namespace ock
