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
 * @file hcc_p2p.h
 * @brief Point-to-point (P2P) communication APIs of the HCC module.
 */
#ifndef MF_OPENDCC_HCC_P2P_H
#define MF_OPENDCC_HCC_P2P_H
#include "hcc_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the HCC (Host Centric Communicator) module
 *
 * @param protocol         [in] protocol to be initialized, see the GEN_PROT_* macros in genetty_def.h
 * @param vendor           [in] vendor of the device to be used, e.g. "ascend"
 * @param flags            [in] optional flags, should be 0 currently
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_init(PROTOCOL protocol, const char *vendor, uint32_t flags);

/**
 * @brief Un-initialize the HCC (Host Centric Communicator) module, all resources are released
 */
void hcc_uninit(uint32_t flags);

/**
 * @brief Initialize the options with default values
 *
 * @param options          [out] options to be initialized, must not be null
 */
void hcc_options_init(hcc_options_t *options);

/**
 * @brief Create an HCC(Host Centric Communicator) in P2P mode
 *
 * @param options          [in] options initialized by hcc_options_init
 * @return handle of the communicator, null if failed
 */
hcc_t hcc_create(hcc_options_t *options);

/**
 * @brief Destroy the HCC(Host Centric Communicator) in P2P mode and release all resources
 *
 * @param handle           [in] communicator handle created by hcc_create
 * @param flags            [in] optional flags, should be 0 currently
 */
void hcc_destroy(hcc_t *handle, uint32_t flags);

/**
 * @brief Register a memory segment to the HCC(Host Centric Communicator) in P2P mode
 *
 * @param handle           [in] communicator handle created by hcc_create
 * @param segment          [in,out] memory segment to be registered, the key fields are filled by this call
 * @param flags            [in] optional flags, should be 0 currently
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_memory_register(hcc_t *handle, hcc_mem_segment_t *segment, uint32_t flags);

/**
 * @brief Unregister a memory segment from the HCC(Host Centric Communicator) in P2P mode
 *
 * @param handle           [in] communicator handle created by hcc_create
 * @param segment          [in] memory segment to be unregistered
 * @param flags            [in] optional flags, should be 0 currently
 */
void hcc_memory_unregister(hcc_t *handle, hcc_mem_segment_t *segment, uint32_t flags);

/**
 * @brief Create a tunnel for point-to-point data transfer
 *
 * @param tunnel_handle    [in] communicator handle created by hcc_create
 * @param options          [in] options of the tunnel
 * @return handle of the tunnel, null if failed
 */
hcc_tunnel_t hcc_tunnel_create(hcc_t tunnel_handle, hcc_tunnel_options_t *options);

/**
 * @brief Destroy the tunnel and release all resources
 *
 * @param tunnel_handle    [in] tunnel handle created by hcc_tunnel_create
 * @param flags            [in] optional flags, should be 0 currently
 */
void hcc_tunnel_destroy(hcc_tunnel_t *tunnel_handle, uint32_t flags);

/**
 * @brief Export the link info of the tunnel, which is exchanged with the peer
 *
 * @param tunnel_handle    [in] tunnel handle created by hcc_tunnel_create
 * @param flags            [in] optional flags, should be 0 currently
 * @param link_info        [out] link info exported, should be freed by hcc_tunnel_free_link_info
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_tunnel_export(hcc_tunnel_t *tunnel_handle, uint32_t flags, hcc_link_info_t *link_info);

/**
 * @brief Import the link info exported by the peer, then the tunnel is ready to transfer data
 *
 * @param tunnel_handle    [in] tunnel handle created by hcc_tunnel_create
 * @param flags            [in] optional flags, should be 0 currently
 * @param link_info        [in] link info exported by the peer
 */
void hcc_tunnel_import(hcc_tunnel_t *tunnel_handle, uint32_t flags, hcc_link_info_t *link_info);

/**
 * @brief Free the link info allocated by hcc_tunnel_export
 *
 * @param link_info        [in] link info to be freed, can be null
 */
void hcc_tunnel_free_link_info(hcc_link_info_t *link_info);

#ifdef __cplusplus
}
#endif

#endif // MF_OPENDCC_HCC_P2P_H
