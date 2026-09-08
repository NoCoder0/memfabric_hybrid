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
#ifndef MEMFABRIC_SMEM_CONFIG_STORE_DEF_H
#define MEMFABRIC_SMEM_CONFIG_STORE_DEF_H

#include <stdint.h>

#include "smem_tls_options.h"

typedef void *smem_config_store_t;

typedef struct {
    uint8_t is_leader;
    const char *meta_service_address;
} smem_config_store_leader_info_t;

/**
 * @brief Handle a configuration store connection failure.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param link_id          [in] identifier of the broken connection
 * @param rank_id          [in] rank identifier resolved by the store for the broken connection
 * @return 0 if successful, or an error code on failure
 */
typedef int32_t (*smem_config_store_broken_cb)(void *context, uint32_t link_id, uint32_t rank_id);

/**
 * @brief Handle an HA configuration store leader change.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param leader           [in] borrowed leader information, including its address string;
 *                             valid only during the callback
 */
typedef void (*smem_config_store_leader_change_cb)(void *context, const smem_config_store_leader_info_t *leader);

#endif // MEMFABRIC_SMEM_CONFIG_STORE_DEF_H
