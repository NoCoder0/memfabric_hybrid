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
#ifndef MEMFABRIC_SMEM_CONFIG_STORE_API_H
#define MEMFABRIC_SMEM_CONFIG_STORE_API_H

#include "smem_config_store_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the TLS configuration used when creating configuration stores.
 *
 * Call this API before smem_config_store_create_server. The configuration strings are copied;
 * this call does not load libraries or validate certificate files.
 *
 * @param tls              [in] TLS options, NULL disables TLS
 * @param tls_library      [in] TLS library path, NULL selects an empty path
 * @param decrypter_library [in] private key password decrypter library path, NULL selects an empty path
 * @return 0 after storing the configuration
 */
int32_t smem_config_store_set_tls(const smem_tls_options_t *tls, const char *tls_library,
                                  const char *decrypter_library);

/**
 * @brief Create a configuration store in server mode for the given URL.
 *
 * TLS settings must be configured before this call if TLS is required.
 *
 * @param url              [in] store URL, e.g. tcp://ip:port, must not be NULL
 * @return Owned store handle on success, or NULL on failure; release with smem_config_store_destroy_server
 */
smem_config_store_t smem_config_store_create_server(const char *url);

/**
 * @brief Release a configuration store handle and remove the store registered for its URL.
 *
 * The handle must not be used after this call.
 *
 * @param store            [in] owned store handle, NULL is ignored
 * @param url              [in] URL used to create the store; NULL skips removal from the store factory
 */
void smem_config_store_destroy_server(smem_config_store_t store, const char *url);

/**
 * @brief Register a configuration store connection failure callback.
 *
 * The callback receives the broken link identifier and its associated rank identifier.
 *
 * @param store            [in] store handle
 * @param callback         [in] connection failure callback, NULL clears the handler
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 */
void smem_config_store_set_server_broken_handler(smem_config_store_t store, smem_config_store_broken_cb callback,
                                                 void *context);

/**
 * @brief Register a leader change callback for an HA configuration store.
 *
 * Leader information and its address string are borrowed and valid only during the callback.
 * Support depends on the backend and build; release/1.2 does not support leader notifications.
 *
 * @param store            [in] store handle
 * @param callback         [in] leader change callback, NULL clears the handler when supported
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 * @return 0 if registered or cleared, 1 if unsupported, or -1 if the handle is NULL or empty
 */
int32_t smem_config_store_set_leader_handler(smem_config_store_t store, smem_config_store_leader_change_cb callback,
                                             void *context);

#ifdef __cplusplus
}
#endif

#endif // MEMFABRIC_SMEM_CONFIG_STORE_API_H
