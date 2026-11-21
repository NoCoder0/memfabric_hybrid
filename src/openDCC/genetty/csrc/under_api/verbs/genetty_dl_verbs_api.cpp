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
#include "genetty_dl_verbs_api.h"

#include "genetty_def.h"

namespace ock {
namespace openDCC {
std::mutex VerbsApi::LOAD_MUTEX;
bool VerbsApi::LOADED = false;

DL_API_DEFINE(VerbsApi, ibv_get_device_list);
DL_API_DEFINE(VerbsApi, ibv_fork_init);
DL_API_DEFINE(VerbsApi, ibv_open_device);
DL_API_DEFINE(VerbsApi, ibv_alloc_pd);
DL_API_DEFINE(VerbsApi, ibv_query_port);
DL_API_DEFINE(VerbsApi, ibv_free_device_list);
DL_API_DEFINE(VerbsApi, ibv_reg_mr);
DL_API_DEFINE(VerbsApi, ibv_create_comp_channel);
DL_API_DEFINE(VerbsApi, ibv_get_cq_event);
DL_API_DEFINE(VerbsApi, ibv_get_async_event);
DL_API_DEFINE(VerbsApi, ibv_ack_async_event);
DL_API_DEFINE(VerbsApi, ibv_create_qp);
DL_API_DEFINE(VerbsApi, ibv_close_device);
DL_API_DEFINE(VerbsApi, ibv_dealloc_pd);
DL_API_DEFINE(VerbsApi, ibv_create_cq);
DL_API_DEFINE(VerbsApi, ibv_destroy_comp_channel);
DL_API_DEFINE(VerbsApi, ibv_destroy_cq);
DL_API_DEFINE(VerbsApi, ibv_ack_cq_events);
DL_API_DEFINE(VerbsApi, ibv_destroy_qp);
DL_API_DEFINE(VerbsApi, ibv_modify_qp);
DL_API_DEFINE(VerbsApi, ibv_dereg_mr);
DL_API_DEFINE(VerbsApi, ibv_query_gid);
DL_API_DEFINE(VerbsApi, ibv_query_device);
DL_API_DEFINE(VerbsApi, ibv_reg_mr_iova2);
DL_API_DEFINE(VerbsApi, ibv_port_state_str);

Result VerbsApi::Load() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    if (LOADED) {
        return GEN_OK;
    }

    /* step1: open library file */
    void *handle = dlopen("libibverbs.so", RTLD_NOW | RTLD_NODELETE | RTLD_GLOBAL);
    if (handle == nullptr) {
        GEN_LOG_ERROR("Open libibverbs failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }

    /* step2: load functions */
    DL_API_LOAD(ibv_get_device_list);
    DL_API_LOAD(ibv_fork_init);
    DL_API_LOAD(ibv_open_device);
    DL_API_LOAD(ibv_alloc_pd);
    DL_API_LOAD(ibv_query_port);
    DL_API_LOAD(ibv_free_device_list);
    DL_API_LOAD(ibv_reg_mr);
    DL_API_LOAD(ibv_create_comp_channel);
    DL_API_LOAD(ibv_get_cq_event);
    DL_API_LOAD(ibv_get_async_event);
    DL_API_LOAD(ibv_ack_async_event);
    DL_API_LOAD(ibv_create_qp);
    DL_API_LOAD(ibv_close_device);
    DL_API_LOAD(ibv_dealloc_pd);
    DL_API_LOAD(ibv_create_cq);
    DL_API_LOAD(ibv_destroy_comp_channel);
    DL_API_LOAD(ibv_destroy_cq);
    DL_API_LOAD(ibv_ack_cq_events);
    DL_API_LOAD(ibv_destroy_qp);
    DL_API_LOAD(ibv_modify_qp);
    DL_API_LOAD(ibv_dereg_mr);
    DL_API_LOAD(ibv_query_gid);
    DL_API_LOAD(ibv_query_device);
    DL_API_LOAD(ibv_reg_mr_iova2);
    DL_API_LOAD(ibv_port_state_str);

    /* step3: close handle */
    dlclose(handle);

    /* step4: set loaded */
    LOADED = true;
    return GEN_OK;
}

void VerbsApi::UnLoad() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    if (!LOADED) {
        return;
    }

    UnLoadInner();
    LOADED = false;
}

void VerbsApi::UnLoadInner() noexcept
{
    DL_API_SET_NULL(ibv_get_device_list);
    DL_API_SET_NULL(ibv_fork_init);
    DL_API_SET_NULL(ibv_open_device);
    DL_API_SET_NULL(ibv_alloc_pd);
    DL_API_SET_NULL(ibv_query_port);
    DL_API_SET_NULL(ibv_free_device_list);
    DL_API_SET_NULL(ibv_reg_mr);
    DL_API_SET_NULL(ibv_create_comp_channel);
    DL_API_SET_NULL(ibv_get_cq_event);
    DL_API_SET_NULL(ibv_get_async_event);
    DL_API_SET_NULL(ibv_ack_async_event);
    DL_API_SET_NULL(ibv_create_qp);
    DL_API_SET_NULL(ibv_close_device);
    DL_API_SET_NULL(ibv_dealloc_pd);
    DL_API_SET_NULL(ibv_create_cq);
    DL_API_SET_NULL(ibv_destroy_comp_channel);
    DL_API_SET_NULL(ibv_destroy_cq);
    DL_API_SET_NULL(ibv_ack_cq_events);
    DL_API_SET_NULL(ibv_destroy_qp);
    DL_API_SET_NULL(ibv_modify_qp);
    DL_API_SET_NULL(ibv_dereg_mr);
    DL_API_SET_NULL(ibv_query_gid);
    DL_API_SET_NULL(ibv_query_device);
    DL_API_SET_NULL(ibv_reg_mr_iova2);
    DL_API_SET_NULL(ibv_port_state_str);
}

} // namespace openDCC
} // namespace ock
