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
#ifndef MF_OPENDCC_HCC_CS_H
#define MF_OPENDCC_HCC_CS_H

#include "hcc_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the HCC (Host Centric Communicator) in server mode
 *
 * @param protocol         [in] protocol to be initialized, see the GEN_PROT_* macros in genetty_def.h
 * @param vendor           [in] vendor of the device to be used, e.g. "ascend"
 * @param options          [in] server init options, containing the listen ip, port and flags
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_server_init(PROTOCOL protocol, const char *vendor, hcc_server_init_options_t *options);

/**
 * @brief Un-initialize the HCC (Host Centric Communicator) in server mode, all resources are released
 *
 * @param flags            [in] optional flags, should be 0 currently
 */
void hcc_server_uninit(uint32_t flags);

/**
 * @brief Initialize the options with default values
 *
 * @param options          [out] options to be initialized, must not be null
 */
void hcc_server_options_init(hcc_options_t *options);

/**
 * @brief Create an HCC (Host Centric Communicator) in server mode
 *
 * @param options          [in] options initialized by hcc_server_options_init
 * @return handle of the server, null if failed
 */
hcc_server_t hcc_server_create(hcc_options_t *options);

/**
 * @brief Destroy the HCC (Host Centric Communicator) in server mode and release all resources
 *
 * @param svr_handle       [in] server handle created by hcc_server_create
 * @param flags            [in] optional flags, should be 0 currently
 */
void hcc_server_destroy(hcc_server_t svr_handle, uint32_t flags);

/**
 * @brief Register a memory segment to the HCC (Host Centric Communicator) in server mode
 *
 * @param svr_handle       [in] server handle created by hcc_server_create
 * @param segment          [in,out] memory segment to be registered, the key fields are filled by this call
 * @param flags            [in] optional flags, should be 0 currently
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_server_memory_register(hcc_server_t svr_handle, hcc_mem_segment_t *segment, uint32_t flags);

/**
 * @brief Unregister a memory segment from the HCC (Host Centric Communicator) in server mode
 *
 * @param svr_handle       [in] server handle created by hcc_server_create
 * @param segment          [in] memory segment to be unregistered
 * @param flags            [in] optional flags, should be 0 currently
 */
void hcc_server_memory_unregister(hcc_server_t svr_handle, hcc_mem_segment_t *segment, uint32_t flags);

/**
 * @brief Create a server-side tunnel for data transfer
 *
 * @param svr_handle       [in] server handle created by hcc_server_create
 * @param options          [in] options of the tunnel
 * @return handle of the tunnel, null if failed
 */
hcc_tunnel_t hcc_server_tunnel_create(hcc_server_t svr_handle, hcc_tunnel_options_t *options);

/**
 * @brief Get the URL of the server-side tunnel, which is used by the client to connect
 *
 * @param tunnel_handle    [in] tunnel handle created by hcc_server_tunnel_create
 * @param flags            [in] optional flags, should be 0 currently
 * @return tunnel URL, null if failed
 */
char *hcc_server_tunnel_get_url(hcc_tunnel_t tunnel_handle, uint32_t flags);

/**
 * @brief Destroy the server-side tunnel and release all resources
 *
 * @param tunnel_handle    [in] tunnel handle created by hcc_server_tunnel_create
 * @param flags            [in] optional flags, should be 0 currently
 */
void hcc_server_tunnel_destroy(hcc_tunnel_t tunnel_handle, uint32_t flags);

/**
 * @brief Connect to the server through the tunnel by the given URL
 *
 * @param tunnel_handle    [in] tunnel handle
 * @param url              [in] URL of the server-side tunnel, returned by hcc_server_tunnel_get_url
 * @param timeoutMs        [in] connect timeout in milliseconds
 * @param flags            [in] optional flags, should be 0 currently
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_tunnel_connect(hcc_tunnel_t tunnel_handle, char *url, uint32_t timeoutMs, uint32_t flags);

/**
 * @brief Connect to the server through the tunnel and report the result through the callback
 *
 * @param tunnel_handle    [in] tunnel handle
 * @param url              [in] URL of the server-side tunnel, returned by hcc_server_tunnel_get_url
 * @param timeoutMs        [in] connect timeout in milliseconds
 * @param flags            [in] optional flags, should be 0 currently
 * @param ctx              [in] user context passed back to the callback
 * @param callback         [in] callback invoked with the tunnel handle and user context
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_tunnel_connect_async(hcc_tunnel_t tunnel_handle, char *url, uint32_t timeoutMs, uint32_t flags, uint64_t ctx,
                                void (*callback)(hcc_tunnel_t *tunnel_handle, uint64_t ctx, int32_t result));

/**
 * @brief Disconnect the tunnel
 *
 * @param tunnel_handle    [in] tunnel handle
 * @param flags            [in] optional flags, should be 0 currently
 */
void hcc_tunnel_disconnect(hcc_tunnel_t tunnel_handle, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif // MF_OPENDCC_HCC_CS_H
