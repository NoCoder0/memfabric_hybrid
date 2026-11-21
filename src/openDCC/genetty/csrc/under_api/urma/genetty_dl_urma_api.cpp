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
#include "common/genetty_common_includes.h"
#include "genetty_dl_urma_api.h"
#include "genetty_def.h"

namespace ock {
namespace openDCC {
std::mutex UrmaApi::LOAD_MUTEX;
bool UrmaApi::LOADED = false;

DL_API_DEFINE(UrmaApi, urma_init);
DL_API_DEFINE(UrmaApi, urma_uninit);
DL_API_DEFINE(UrmaApi, urma_get_device_list);
DL_API_DEFINE(UrmaApi, urma_free_device_list);
DL_API_DEFINE(UrmaApi, urma_get_eid_list);
DL_API_DEFINE(UrmaApi, urma_free_eid_list);
DL_API_DEFINE(UrmaApi, urma_get_device_by_name);
DL_API_DEFINE(UrmaApi, urma_get_device_by_eid);
DL_API_DEFINE(UrmaApi, urma_query_device);
DL_API_DEFINE(UrmaApi, urma_create_context);
DL_API_DEFINE(UrmaApi, urma_delete_context);
DL_API_DEFINE(UrmaApi, urma_set_context_opt);

DL_API_DEFINE(UrmaApi, urma_create_jfc);
DL_API_DEFINE(UrmaApi, urma_modify_jfc);
DL_API_DEFINE(UrmaApi, urma_delete_jfc);
DL_API_DEFINE(UrmaApi, urma_alloc_jfc);
DL_API_DEFINE(UrmaApi, urma_set_jfc_opt);
DL_API_DEFINE(UrmaApi, urma_active_jfc);
DL_API_DEFINE(UrmaApi, urma_get_jfc_opt);
DL_API_DEFINE(UrmaApi, urma_deactive_jfc);
DL_API_DEFINE(UrmaApi, urma_free_jfc);
DL_API_DEFINE(UrmaApi, urma_delete_jfc_batch);

DL_API_DEFINE(UrmaApi, urma_create_jfs);
DL_API_DEFINE(UrmaApi, urma_modify_jfs);
DL_API_DEFINE(UrmaApi, urma_query_jfs);
DL_API_DEFINE(UrmaApi, urma_delete_jfs);
DL_API_DEFINE(UrmaApi, urma_delete_jfs_batch);
DL_API_DEFINE(UrmaApi, urma_flush_jfs);
DL_API_DEFINE(UrmaApi, urma_alloc_jfs);
DL_API_DEFINE(UrmaApi, urma_set_jfs_opt);
DL_API_DEFINE(UrmaApi, urma_active_jfs);
DL_API_DEFINE(UrmaApi, urma_get_jfs_opt);
DL_API_DEFINE(UrmaApi, urma_deactive_jfs);
DL_API_DEFINE(UrmaApi, urma_free_jfs);

DL_API_DEFINE(UrmaApi, urma_create_jfr);
DL_API_DEFINE(UrmaApi, urma_modify_jfr);
DL_API_DEFINE(UrmaApi, urma_query_jfr);
DL_API_DEFINE(UrmaApi, urma_delete_jfr);
DL_API_DEFINE(UrmaApi, urma_delete_jfr_batch);
DL_API_DEFINE(UrmaApi, urma_import_jfr);
DL_API_DEFINE(UrmaApi, urma_import_jfr_ex);
DL_API_DEFINE(UrmaApi, urma_unimport_jfr);
DL_API_DEFINE(UrmaApi, urma_advise_jfr);
DL_API_DEFINE(UrmaApi, urma_advise_jfr_async);
DL_API_DEFINE(UrmaApi, urma_unadvise_jfr);
DL_API_DEFINE(UrmaApi, urma_alloc_jfr);
DL_API_DEFINE(UrmaApi, urma_set_jfr_opt);
DL_API_DEFINE(UrmaApi, urma_active_jfr);
DL_API_DEFINE(UrmaApi, urma_get_jfr_opt);
DL_API_DEFINE(UrmaApi, urma_deactive_jfr);
DL_API_DEFINE(UrmaApi, urma_free_jfr);

DL_API_DEFINE(UrmaApi, urma_create_jetty);
DL_API_DEFINE(UrmaApi, urma_modify_jetty);
DL_API_DEFINE(UrmaApi, urma_query_jetty);
DL_API_DEFINE(UrmaApi, urma_delete_jetty);
DL_API_DEFINE(UrmaApi, urma_delete_jetty_batch);
DL_API_DEFINE(UrmaApi, urma_import_jetty);
DL_API_DEFINE(UrmaApi, urma_import_jetty_ex);
DL_API_DEFINE(UrmaApi, urma_unimport_jetty);
DL_API_DEFINE(UrmaApi, urma_advise_jetty);
DL_API_DEFINE(UrmaApi, urma_unadvise_jetty);
DL_API_DEFINE(UrmaApi, urma_bind_jetty);
DL_API_DEFINE(UrmaApi, urma_bind_jetty_ex);
DL_API_DEFINE(UrmaApi, urma_unbind_jetty);
DL_API_DEFINE(UrmaApi, urma_flush_jetty);
DL_API_DEFINE(UrmaApi, urma_import_jetty_async);
DL_API_DEFINE(UrmaApi, urma_unimport_jetty_async);
DL_API_DEFINE(UrmaApi, urma_bind_jetty_async);
DL_API_DEFINE(UrmaApi, urma_unbind_jetty_async);
DL_API_DEFINE(UrmaApi, urma_create_notifier);
DL_API_DEFINE(UrmaApi, urma_delete_notifier);
DL_API_DEFINE(UrmaApi, urma_alloc_jetty);
DL_API_DEFINE(UrmaApi, urma_set_jetty_opt);
DL_API_DEFINE(UrmaApi, urma_active_jetty);
DL_API_DEFINE(UrmaApi, urma_get_jetty_opt);
DL_API_DEFINE(UrmaApi, urma_deactive_jetty);
DL_API_DEFINE(UrmaApi, urma_free_jetty);
DL_API_DEFINE(UrmaApi, urma_wait_notify);
DL_API_DEFINE(UrmaApi, urma_ack_notify);

DL_API_DEFINE(UrmaApi, urma_create_jetty_grp);
DL_API_DEFINE(UrmaApi, urma_delete_jetty_grp);

DL_API_DEFINE(UrmaApi, urma_create_jfce);
DL_API_DEFINE(UrmaApi, urma_delete_jfce);
DL_API_DEFINE(UrmaApi, urma_get_async_event);
DL_API_DEFINE(UrmaApi, urma_ack_async_event);
DL_API_DEFINE(UrmaApi, urma_alloc_token_id);
DL_API_DEFINE(UrmaApi, urma_alloc_token_id_ex);
DL_API_DEFINE(UrmaApi, urma_free_token_id);

DL_API_DEFINE(UrmaApi, urma_register_seg);
DL_API_DEFINE(UrmaApi, urma_unregister_seg);
DL_API_DEFINE(UrmaApi, urma_import_seg);
DL_API_DEFINE(UrmaApi, urma_unimport_seg);

DL_API_DEFINE(UrmaApi, urma_post_jfs_wr);
DL_API_DEFINE(UrmaApi, urma_post_jfr_wr);
DL_API_DEFINE(UrmaApi, urma_post_jetty_send_wr);
DL_API_DEFINE(UrmaApi, urma_post_jetty_recv_wr);

DL_API_DEFINE(UrmaApi, urma_write);
DL_API_DEFINE(UrmaApi, urma_read);
DL_API_DEFINE(UrmaApi, urma_send);
DL_API_DEFINE(UrmaApi, urma_recv);

DL_API_DEFINE(UrmaApi, urma_poll_jfc);
DL_API_DEFINE(UrmaApi, urma_rearm_jfc);
DL_API_DEFINE(UrmaApi, urma_wait_jfc);
DL_API_DEFINE(UrmaApi, urma_ack_jfc);

DL_API_DEFINE(UrmaApi, urma_get_uasid);
DL_API_DEFINE(UrmaApi, urma_user_ctl);

DL_API_DEFINE(UrmaApi, urma_register_log_func);
DL_API_DEFINE(UrmaApi, urma_unregister_log_func);
DL_API_DEFINE(UrmaApi, urma_log_get_level);
DL_API_DEFINE(UrmaApi, urma_log_set_level);
DL_API_DEFINE(UrmaApi, urma_log_get_thread_tag);
DL_API_DEFINE(UrmaApi, urma_log_set_thread_tag);

DL_API_DEFINE(UrmaApi, urma_get_tpn);
DL_API_DEFINE(UrmaApi, urma_get_net_addr_list);
DL_API_DEFINE(UrmaApi, urma_free_net_addr_list);
DL_API_DEFINE(UrmaApi, urma_modify_tp);
DL_API_DEFINE(UrmaApi, urma_get_tp_list);
DL_API_DEFINE(UrmaApi, urma_set_tp_attr);
DL_API_DEFINE(UrmaApi, urma_get_tp_attr);
DL_API_DEFINE(UrmaApi, urma_get_eid_by_ip);
DL_API_DEFINE(UrmaApi, urma_get_ip_by_eid);
DL_API_DEFINE(UrmaApi, urma_get_smac);
DL_API_DEFINE(UrmaApi, urma_get_dmac);
DL_API_DEFINE(UrmaApi, urma_get_perf_info);

Result UrmaApi::Load() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    if (LOADED) {
        return GEN_OK;
    }

    /* step1: open library file */
    void *handle = dlopen("liburma.so", RTLD_NOW | RTLD_NODELETE | RTLD_GLOBAL);
    if (handle == nullptr) {
        GEN_LOG_ERROR("Open liburma failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }

    /* step2: load functions */
    DL_API_LOAD(urma_init);
    DL_API_LOAD(urma_uninit);
    DL_API_LOAD(urma_get_device_list);
    DL_API_LOAD(urma_free_device_list);
    DL_API_LOAD(urma_get_eid_list);
    DL_API_LOAD(urma_free_eid_list);
    DL_API_LOAD(urma_get_device_by_name);
    DL_API_LOAD(urma_get_device_by_eid);
    DL_API_LOAD(urma_query_device);
    DL_API_LOAD(urma_create_context);
    DL_API_LOAD(urma_delete_context);
    DL_API_LOAD(urma_set_context_opt);

    DL_API_LOAD(urma_create_jfc);
    DL_API_LOAD(urma_modify_jfc);
    DL_API_LOAD(urma_delete_jfc);
    DL_API_LOAD(urma_alloc_jfc);
    DL_API_LOAD(urma_set_jfc_opt);
    DL_API_LOAD(urma_active_jfc);
    DL_API_LOAD(urma_get_jfc_opt);
    DL_API_LOAD(urma_deactive_jfc);
    DL_API_LOAD(urma_free_jfc);
    DL_API_LOAD(urma_delete_jfc_batch);

    DL_API_LOAD(urma_create_jfs);
    DL_API_LOAD(urma_modify_jfs);
    DL_API_LOAD(urma_query_jfs);
    DL_API_LOAD(urma_delete_jfs);
    DL_API_LOAD(urma_delete_jfs_batch);
    DL_API_LOAD(urma_flush_jfs);
    DL_API_LOAD(urma_alloc_jfs);
    DL_API_LOAD(urma_set_jfs_opt);
    DL_API_LOAD(urma_active_jfs);
    DL_API_LOAD(urma_get_jfs_opt);
    DL_API_LOAD(urma_deactive_jfs);
    DL_API_LOAD(urma_free_jfs);

    DL_API_LOAD(urma_create_jfr);
    DL_API_LOAD(urma_modify_jfr);
    DL_API_LOAD(urma_query_jfr);
    DL_API_LOAD(urma_delete_jfr);
    DL_API_LOAD(urma_delete_jfr_batch);
    DL_API_LOAD(urma_import_jfr);
    DL_API_LOAD(urma_import_jfr_ex);
    DL_API_LOAD(urma_unimport_jfr);
    DL_API_LOAD(urma_advise_jfr);
    DL_API_LOAD(urma_advise_jfr_async);
    DL_API_LOAD(urma_unadvise_jfr);
    DL_API_LOAD(urma_alloc_jfr);
    DL_API_LOAD(urma_set_jfr_opt);
    DL_API_LOAD(urma_active_jfr);
    DL_API_LOAD(urma_get_jfr_opt);
    DL_API_LOAD(urma_deactive_jfr);
    DL_API_LOAD(urma_free_jfr);

    DL_API_LOAD(urma_create_jetty);
    DL_API_LOAD(urma_modify_jetty);
    DL_API_LOAD(urma_query_jetty);
    DL_API_LOAD(urma_delete_jetty);
    DL_API_LOAD(urma_delete_jetty_batch);
    DL_API_LOAD(urma_import_jetty);
    DL_API_LOAD(urma_import_jetty_ex);
    DL_API_LOAD(urma_unimport_jetty);
    DL_API_LOAD(urma_advise_jetty);
    DL_API_LOAD(urma_unadvise_jetty);
    DL_API_LOAD(urma_bind_jetty);
    DL_API_LOAD(urma_bind_jetty_ex);
    DL_API_LOAD(urma_unbind_jetty);
    DL_API_LOAD(urma_flush_jetty);
    DL_API_LOAD(urma_import_jetty_async);
    DL_API_LOAD(urma_unimport_jetty_async);
    DL_API_LOAD(urma_bind_jetty_async);
    DL_API_LOAD(urma_unbind_jetty_async);
    DL_API_LOAD(urma_create_notifier);
    DL_API_LOAD(urma_delete_notifier);
    DL_API_LOAD(urma_alloc_jetty);
    DL_API_LOAD(urma_set_jetty_opt);
    DL_API_LOAD(urma_active_jetty);
    DL_API_LOAD(urma_get_jetty_opt);
    DL_API_LOAD(urma_deactive_jetty);
    DL_API_LOAD(urma_free_jetty);
    DL_API_LOAD(urma_wait_notify);
    DL_API_LOAD(urma_ack_notify);

    DL_API_LOAD(urma_create_jetty_grp);
    DL_API_LOAD(urma_delete_jetty_grp);

    DL_API_LOAD(urma_create_jfce);
    DL_API_LOAD(urma_delete_jfce);
    DL_API_LOAD(urma_get_async_event);
    DL_API_LOAD(urma_ack_async_event);
    DL_API_LOAD(urma_alloc_token_id);
    DL_API_LOAD(urma_alloc_token_id_ex);
    DL_API_LOAD(urma_free_token_id);

    DL_API_LOAD(urma_register_seg);
    DL_API_LOAD(urma_unregister_seg);
    DL_API_LOAD(urma_import_seg);
    DL_API_LOAD(urma_unimport_seg);

    DL_API_LOAD(urma_post_jfs_wr);
    DL_API_LOAD(urma_post_jfr_wr);
    DL_API_LOAD(urma_post_jetty_send_wr);
    DL_API_LOAD(urma_post_jetty_recv_wr);

    DL_API_LOAD(urma_write);
    DL_API_LOAD(urma_read);
    DL_API_LOAD(urma_send);
    DL_API_LOAD(urma_recv);

    DL_API_LOAD(urma_poll_jfc);
    DL_API_LOAD(urma_rearm_jfc);
    DL_API_LOAD(urma_wait_jfc);
    DL_API_LOAD(urma_ack_jfc);

    DL_API_LOAD(urma_get_uasid);
    DL_API_LOAD(urma_user_ctl);

    DL_API_LOAD(urma_register_log_func);
    DL_API_LOAD(urma_unregister_log_func);
    DL_API_LOAD(urma_log_get_level);
    DL_API_LOAD(urma_log_set_level);
    DL_API_LOAD(urma_log_get_thread_tag);
    DL_API_LOAD(urma_log_set_thread_tag);

    DL_API_LOAD(urma_get_tpn);
    DL_API_LOAD(urma_get_net_addr_list);
    DL_API_LOAD(urma_free_net_addr_list);
    DL_API_LOAD(urma_modify_tp);
    DL_API_LOAD(urma_get_tp_list);
    DL_API_LOAD(urma_set_tp_attr);
    DL_API_LOAD(urma_get_tp_attr);
    DL_API_LOAD(urma_get_eid_by_ip);
    DL_API_LOAD(urma_get_ip_by_eid);
    DL_API_LOAD(urma_get_smac);
    DL_API_LOAD(urma_get_dmac);
    DL_API_LOAD(urma_get_perf_info);

    /* step3: close handle */
    dlclose(handle);

    /* step4: set loaded */
    LOADED = true;
    return GEN_OK;
}

void UrmaApi::UnLoad() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    if (!LOADED) {
        return;
    }

    UnLoadInner();
    LOADED = false;
}

void UrmaApi::UnLoadInner() noexcept
{
    DL_API_SET_NULL(urma_init);
    DL_API_SET_NULL(urma_uninit);
    DL_API_SET_NULL(urma_get_device_list);
    DL_API_SET_NULL(urma_free_device_list);
    DL_API_SET_NULL(urma_get_eid_list);
    DL_API_SET_NULL(urma_free_eid_list);
    DL_API_SET_NULL(urma_get_device_by_name);
    DL_API_SET_NULL(urma_get_device_by_eid);
    DL_API_SET_NULL(urma_query_device);
    DL_API_SET_NULL(urma_create_context);
    DL_API_SET_NULL(urma_delete_context);
    DL_API_SET_NULL(urma_set_context_opt);

    DL_API_SET_NULL(urma_create_jfc);
    DL_API_SET_NULL(urma_modify_jfc);
    DL_API_SET_NULL(urma_delete_jfc);
    DL_API_SET_NULL(urma_alloc_jfc);
    DL_API_SET_NULL(urma_set_jfc_opt);
    DL_API_SET_NULL(urma_active_jfc);
    DL_API_SET_NULL(urma_get_jfc_opt);
    DL_API_SET_NULL(urma_deactive_jfc);
    DL_API_SET_NULL(urma_free_jfc);
    DL_API_SET_NULL(urma_delete_jfc_batch);

    DL_API_SET_NULL(urma_create_jfs);
    DL_API_SET_NULL(urma_modify_jfs);
    DL_API_SET_NULL(urma_query_jfs);
    DL_API_SET_NULL(urma_delete_jfs);
    DL_API_SET_NULL(urma_delete_jfs_batch);
    DL_API_SET_NULL(urma_flush_jfs);
    DL_API_SET_NULL(urma_alloc_jfs);
    DL_API_SET_NULL(urma_set_jfs_opt);
    DL_API_SET_NULL(urma_active_jfs);
    DL_API_SET_NULL(urma_get_jfs_opt);
    DL_API_SET_NULL(urma_deactive_jfs);
    DL_API_SET_NULL(urma_free_jfs);

    DL_API_SET_NULL(urma_create_jfr);
    DL_API_SET_NULL(urma_modify_jfr);
    DL_API_SET_NULL(urma_query_jfr);
    DL_API_SET_NULL(urma_delete_jfr);
    DL_API_SET_NULL(urma_delete_jfr_batch);
    DL_API_SET_NULL(urma_import_jfr);
    DL_API_SET_NULL(urma_import_jfr_ex);
    DL_API_SET_NULL(urma_unimport_jfr);
    DL_API_SET_NULL(urma_advise_jfr);
    DL_API_SET_NULL(urma_advise_jfr_async);
    DL_API_SET_NULL(urma_unadvise_jfr);
    DL_API_SET_NULL(urma_alloc_jfr);
    DL_API_SET_NULL(urma_set_jfr_opt);
    DL_API_SET_NULL(urma_active_jfr);
    DL_API_SET_NULL(urma_get_jfr_opt);
    DL_API_SET_NULL(urma_deactive_jfr);
    DL_API_SET_NULL(urma_free_jfr);

    DL_API_SET_NULL(urma_create_jetty);
    DL_API_SET_NULL(urma_modify_jetty);
    DL_API_SET_NULL(urma_query_jetty);
    DL_API_SET_NULL(urma_delete_jetty);
    DL_API_SET_NULL(urma_delete_jetty_batch);
    DL_API_SET_NULL(urma_import_jetty);
    DL_API_SET_NULL(urma_import_jetty_ex);
    DL_API_SET_NULL(urma_unimport_jetty);
    DL_API_SET_NULL(urma_advise_jetty);
    DL_API_SET_NULL(urma_unadvise_jetty);
    DL_API_SET_NULL(urma_bind_jetty);
    DL_API_SET_NULL(urma_bind_jetty_ex);
    DL_API_SET_NULL(urma_unbind_jetty);
    DL_API_SET_NULL(urma_flush_jetty);
    DL_API_SET_NULL(urma_import_jetty_async);
    DL_API_SET_NULL(urma_unimport_jetty_async);
    DL_API_SET_NULL(urma_bind_jetty_async);
    DL_API_SET_NULL(urma_unbind_jetty_async);
    DL_API_SET_NULL(urma_create_notifier);
    DL_API_SET_NULL(urma_delete_notifier);
    DL_API_SET_NULL(urma_alloc_jetty);
    DL_API_SET_NULL(urma_set_jetty_opt);
    DL_API_SET_NULL(urma_active_jetty);
    DL_API_SET_NULL(urma_get_jetty_opt);
    DL_API_SET_NULL(urma_deactive_jetty);
    DL_API_SET_NULL(urma_free_jetty);
    DL_API_SET_NULL(urma_wait_notify);
    DL_API_SET_NULL(urma_ack_notify);

    DL_API_SET_NULL(urma_create_jetty_grp);
    DL_API_SET_NULL(urma_delete_jetty_grp);

    DL_API_SET_NULL(urma_create_jfce);
    DL_API_SET_NULL(urma_delete_jfce);
    DL_API_SET_NULL(urma_get_async_event);
    DL_API_SET_NULL(urma_ack_async_event);
    DL_API_SET_NULL(urma_alloc_token_id);
    DL_API_SET_NULL(urma_alloc_token_id_ex);
    DL_API_SET_NULL(urma_free_token_id);

    DL_API_SET_NULL(urma_register_seg);
    DL_API_SET_NULL(urma_unregister_seg);
    DL_API_SET_NULL(urma_import_seg);
    DL_API_SET_NULL(urma_unimport_seg);

    DL_API_SET_NULL(urma_post_jfs_wr);
    DL_API_SET_NULL(urma_post_jfr_wr);
    DL_API_SET_NULL(urma_post_jetty_send_wr);
    DL_API_SET_NULL(urma_post_jetty_recv_wr);

    DL_API_SET_NULL(urma_write);
    DL_API_SET_NULL(urma_read);
    DL_API_SET_NULL(urma_send);
    DL_API_SET_NULL(urma_recv);

    DL_API_SET_NULL(urma_poll_jfc);
    DL_API_SET_NULL(urma_rearm_jfc);
    DL_API_SET_NULL(urma_wait_jfc);
    DL_API_SET_NULL(urma_ack_jfc);

    DL_API_SET_NULL(urma_get_uasid);
    DL_API_SET_NULL(urma_user_ctl);

    DL_API_SET_NULL(urma_register_log_func);
    DL_API_SET_NULL(urma_unregister_log_func);
    DL_API_SET_NULL(urma_log_get_level);
    DL_API_SET_NULL(urma_log_set_level);
    DL_API_SET_NULL(urma_log_get_thread_tag);
    DL_API_SET_NULL(urma_log_set_thread_tag);

    DL_API_SET_NULL(urma_get_tpn);
    DL_API_SET_NULL(urma_get_net_addr_list);
    DL_API_SET_NULL(urma_free_net_addr_list);
    DL_API_SET_NULL(urma_modify_tp);
    DL_API_SET_NULL(urma_get_tp_list);
    DL_API_SET_NULL(urma_set_tp_attr);
    DL_API_SET_NULL(urma_get_tp_attr);
    DL_API_SET_NULL(urma_get_eid_by_ip);
    DL_API_SET_NULL(urma_get_ip_by_eid);
    DL_API_SET_NULL(urma_get_smac);
    DL_API_SET_NULL(urma_get_dmac);
    DL_API_SET_NULL(urma_get_perf_info);
}

} // namespace openDCC
} // namespace ock