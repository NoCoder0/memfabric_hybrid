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
#ifndef MF_GENETTY_DL_URMA_API_H
#define MF_GENETTY_DL_URMA_API_H

#include <ostream>
#include "under_api/genetty_dl_api.h"
#include "urma_types.h"

namespace ock {
namespace openDCC {
using urma_init_api = urma_status_t (*)(urma_init_attr_t *conf);
using urma_uninit_api = urma_status_t (*)(void);
using urma_get_device_list_api = urma_device_t **(*)(int *num_devices);
using urma_free_device_list_api = void (*)(urma_device_t **device_list);
using urma_get_eid_list_api = urma_eid_info_t *(*)(urma_device_t * dev, uint32_t *cnt);
using urma_free_eid_list_api = void (*)(urma_eid_info_t *eid_list);
using urma_get_device_by_name_api = urma_device_t *(*)(char *dev_name);
using urma_get_device_by_eid_api = urma_device_t *(*)(urma_eid_t eid, urma_transport_type_t type);
using urma_query_device_api = urma_status_t (*)(urma_device_t *dev, urma_device_attr_t *dev_attr);
using urma_create_context_api = urma_context_t *(*)(urma_device_t * dev, uint32_t eid_index);
using urma_delete_context_api = urma_status_t (*)(urma_context_t *ctx);
using urma_set_context_opt_api = urma_status_t (*)(urma_context_t *ctx, urma_opt_name_t opt_name, const void *opt_value,
                                                   size_t opt_len);

using urma_create_jfc_api = urma_jfc_t *(*)(urma_context_t * ctx, urma_jfc_cfg_t *jfc_cfg);
using urma_modify_jfc_api = urma_status_t (*)(urma_jfc_t *jfc, urma_jfc_attr_t *attr);
using urma_delete_jfc_api = urma_status_t (*)(urma_jfc_t *jfc);
using urma_alloc_jfc_api = urma_status_t (*)(urma_context_t *urma_ctx, urma_jfc_cfg_t *cfg, urma_jfc_t **jfc);
using urma_set_jfc_opt_api = urma_status_t (*)(urma_jfc_t *jfc, uint64_t opt, void *buf, uint32_t len);
using urma_active_jfc_api = urma_status_t (*)(urma_jfc_t *jfc);
using urma_get_jfc_opt_api = urma_status_t (*)(urma_jfc_t *jfc, uint64_t opt, void *buf, uint32_t len);
using urma_deactive_jfc_api = urma_status_t (*)(urma_jfc_t *jfc);
using urma_free_jfc_api = urma_status_t (*)(urma_jfc_t *jfc);
using urma_delete_jfc_batch_api = urma_status_t (*)(urma_jfc_t **jfc_arr, int jfc_num, urma_jfc_t **bad_jfc);

using urma_create_jfs_api = urma_jfs_t *(*)(urma_context_t * ctx, urma_jfs_cfg_t *jfs_cfg);
using urma_modify_jfs_api = urma_status_t (*)(urma_jfs_t *jfs, urma_jfs_attr_t *attr);
using urma_query_jfs_api = urma_status_t (*)(urma_jfs_t *jfs, urma_jfs_cfg_t *cfg, urma_jfs_attr_t *attr);
using urma_delete_jfs_api = urma_status_t (*)(urma_jfs_t *jfs);
using urma_delete_jfs_batch_api = urma_status_t (*)(urma_jfs_t **jfs_arr, int jfs_num, urma_jfs_t **bad_jfs);
using urma_flush_jfs_api = int (*)(urma_jfs_t *jfs, int cr_cnt, urma_cr_t *cr);
using urma_alloc_jfs_api = urma_status_t (*)(urma_context_t *urma_ctx, urma_jfs_cfg_t *cfg, urma_jfs_t **jfs);
using urma_set_jfs_opt_api = urma_status_t (*)(urma_jfs_t *jfs, uint64_t opt, void *buf, uint32_t len);
using urma_active_jfs_api = urma_status_t (*)(urma_jfs_t *jfs);
using urma_get_jfs_opt_api = urma_status_t (*)(urma_jfs_t *jfs, uint64_t opt, void *buf, uint32_t len);
using urma_deactive_jfs_api = urma_status_t (*)(urma_jfs_t *jfs);
using urma_free_jfs_api = urma_status_t (*)(urma_jfs_t *jfs);

using urma_create_jfr_api = urma_jfr_t *(*)(urma_context_t * ctx, urma_jfr_cfg_t *jfr_cfg);
using urma_modify_jfr_api = urma_status_t (*)(urma_jfr_t *jfr, urma_jfr_attr_t *attr);
using urma_query_jfr_api = urma_status_t (*)(urma_jfr_t *jfr, urma_jfr_cfg_t *cfg, urma_jfr_attr_t *attr);
using urma_delete_jfr_api = urma_status_t (*)(urma_jfr_t *jfr);
using urma_delete_jfr_batch_api = urma_status_t (*)(urma_jfr_t **jfr_arr, int jfr_num, urma_jfr_t **bad_jfr);
using urma_import_jfr_api = urma_target_jetty_t *(*)(urma_context_t * ctx, urma_rjfr_t *rjfr,
                                                     urma_token_t *token_value);
using urma_import_jfr_ex_api = urma_target_jetty_t *(*)(urma_context_t * ctx, urma_rjfr_t *rjfr,
                                                        urma_token_t *token_value, urma_import_jfr_ex_cfg_t *cfg);
using urma_unimport_jfr_api = urma_status_t (*)(urma_target_jetty_t *target_jfr);
using urma_advise_jfr_api = urma_status_t (*)(urma_jfs_t *jfs, urma_target_jetty_t *tjfr);
using urma_advise_jfr_async_api = urma_status_t (*)(urma_jfs_t *jfs, urma_target_jetty_t *tjfr,
                                                    urma_advise_async_cb_func cb_fun, void *cb_arg);
using urma_unadvise_jfr_api = urma_status_t (*)(urma_jfs_t *jfs, urma_target_jetty_t *tjfr);
using urma_alloc_jfr_api = urma_status_t (*)(urma_context_t *urma_ctx, urma_jfr_cfg_t *cfg, urma_jfr_t **jfr);
using urma_set_jfr_opt_api = urma_status_t (*)(urma_jfr_t *jfr, uint64_t opt, void *buf, uint32_t len);
using urma_active_jfr_api = urma_status_t (*)(urma_jfr_t *jfr);
using urma_get_jfr_opt_api = urma_status_t (*)(urma_jfr_t *jfr, uint64_t opt, void *buf, uint32_t len);
using urma_deactive_jfr_api = urma_status_t (*)(urma_jfr_t *jfr);
using urma_free_jfr_api = urma_status_t (*)(urma_jfr_t *jfr);

using urma_create_jetty_api = urma_jetty_t *(*)(urma_context_t * ctx, urma_jetty_cfg_t *jetty_cfg);
using urma_modify_jetty_api = urma_status_t (*)(urma_jetty_t *jetty, urma_jetty_attr_t *attr);
using urma_query_jetty_api = urma_status_t (*)(urma_jetty_t *jetty, urma_jetty_cfg_t *cfg, urma_jetty_attr_t *attr);
using urma_delete_jetty_api = urma_status_t (*)(urma_jetty_t *jetty);
using urma_delete_jetty_batch_api = urma_status_t (*)(urma_jetty_t **jetty_arr, int jetty_num,
                                                      urma_jetty_t **bad_jetty);
using urma_import_jetty_api = urma_target_jetty_t *(*)(urma_context_t * ctx, urma_rjetty_t *rjetty,
                                                       urma_token_t *token_value);
using urma_import_jetty_ex_api = urma_target_jetty_t *(*)(urma_context_t * ctx, urma_rjetty_t *rjetty,
                                                          urma_token_t *token_value, urma_import_jetty_ex_cfg_t *cfg);
using urma_unimport_jetty_api = urma_status_t (*)(urma_target_jetty_t *tjetty);
using urma_advise_jetty_api = urma_status_t (*)(urma_jetty_t *jetty, urma_target_jetty_t *tjetty);
using urma_unadvise_jetty_api = urma_status_t (*)(urma_jetty_t *jetty, urma_target_jetty_t *tjetty);
using urma_bind_jetty_api = urma_status_t (*)(urma_jetty_t *jetty, urma_target_jetty_t *tjetty);
using urma_bind_jetty_ex_api = urma_status_t (*)(urma_jetty_t *jetty, urma_target_jetty_t *tjetty,
                                                 urma_bind_jetty_ex_cfg_t *cfg);
using urma_unbind_jetty_api = urma_status_t (*)(urma_jetty_t *jetty);
using urma_flush_jetty_api = int (*)(urma_jetty_t *jetty, int cr_cnt, urma_cr_t *cr);
using urma_import_jetty_async_api = urma_target_jetty_t *(*)(urma_notifier_t * notifier, const urma_rjetty_t *rjetty,
                                                             const urma_token_t *token_value, uint64_t user_ctx,
                                                             int timeout);
using urma_unimport_jetty_async_api = urma_status_t (*)(urma_target_jetty_t *tjetty);
using urma_bind_jetty_async_api = urma_status_t (*)(urma_notifier_t *notifier, urma_jetty_t *jetty,
                                                    urma_target_jetty_t *tjetty, uint64_t user_ctx, int timeout);
using urma_unbind_jetty_async_api = urma_status_t (*)(urma_jetty_t *jetty);
using urma_create_notifier_api = urma_notifier_t *(*)(urma_context_t * ctx);
using urma_delete_notifier_api = urma_status_t (*)(urma_notifier_t *notifier);
using urma_alloc_jetty_api = urma_status_t (*)(urma_context_t *urma_ctx, urma_jetty_cfg_t *cfg, urma_jetty_t **jetty);
using urma_set_jetty_opt_api = urma_status_t (*)(urma_jetty_t *jetty, uint64_t opt, void *buf, uint32_t len);
using urma_active_jetty_api = urma_status_t (*)(urma_jetty_t *jetty);
using urma_get_jetty_opt_api = urma_status_t (*)(urma_jetty_t *jetty, uint64_t opt, void *buf, uint32_t len);
using urma_deactive_jetty_api = urma_status_t (*)(urma_jetty_t *jetty);
using urma_free_jetty_api = urma_status_t (*)(urma_jetty_t *jetty);
using urma_wait_notify_api = int (*)(urma_notifier_t *notifier, uint32_t cnt, urma_notify_t *notify, int timeout);
using urma_ack_notify_api = urma_status_t (*)(urma_context_t *ctx, uint32_t cnt, urma_notify_t *notify);

using urma_create_jetty_grp_api = urma_jetty_grp_t *(*)(urma_context_t * ctx, urma_jetty_grp_cfg_t *cfg);
using urma_delete_jetty_grp_api = urma_status_t (*)(urma_jetty_grp_t *jetty_grp);

using urma_create_jfce_api = urma_jfce_t *(*)(urma_context_t * ctx);
using urma_delete_jfce_api = urma_status_t (*)(urma_jfce_t *jfce);
using urma_get_async_event_api = urma_status_t (*)(urma_context_t *ctx, urma_async_event_t *event);
using urma_ack_async_event_api = void (*)(urma_async_event_t *event);
using urma_alloc_token_id_api = urma_token_id_t *(*)(urma_context_t * ctx);
using urma_alloc_token_id_ex_api = urma_token_id_t *(*)(urma_context_t * ctx, urma_token_id_flag_t flag);
using urma_free_token_id_api = urma_status_t (*)(urma_token_id_t *token_id);

using urma_register_seg_api = urma_target_seg_t *(*)(urma_context_t * ctx, urma_seg_cfg_t *seg_cfg);
using urma_unregister_seg_api = urma_status_t (*)(urma_target_seg_t *target_seg);
using urma_import_seg_api = urma_target_seg_t *(*)(urma_context_t * ctx, urma_seg_t *seg, urma_token_t *token_value,
                                                   uint64_t addr, urma_import_seg_flag_t flag);
using urma_unimport_seg_api = urma_status_t (*)(urma_target_seg_t *tseg);

using urma_post_jfs_wr_api = urma_status_t (*)(urma_jfs_t *jfs, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr);
using urma_post_jfr_wr_api = urma_status_t (*)(urma_jfr_t *jfr, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr);
using urma_post_jetty_send_wr_api = urma_status_t (*)(urma_jetty_t *jetty, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr);
using urma_post_jetty_recv_wr_api = urma_status_t (*)(urma_jetty_t *jetty, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr);

using urma_write_api = urma_status_t (*)(urma_jfs_t *jfs, urma_target_jetty_t *target_jfr, urma_target_seg_t *dst_tseg,
                                         urma_target_seg_t *src_tseg, uint64_t dst, uint64_t src, uint32_t len,
                                         urma_jfs_wr_flag_t flag, uint64_t user_ctx);
using urma_read_api = urma_status_t (*)(urma_jfs_t *jfs, urma_target_jetty_t *target_jfr, urma_target_seg_t *dst_tseg,
                                        urma_target_seg_t *src_tseg, uint64_t dst, uint64_t src, uint32_t len,
                                        urma_jfs_wr_flag_t flag, uint64_t user_ctx);
using urma_send_api = urma_status_t (*)(urma_jfs_t *jfs, urma_target_jetty_t *target_jfr, urma_target_seg_t *src_tseg,
                                        uint64_t src, uint32_t len, urma_jfs_wr_flag_t flag, uint64_t user_ctx);
using urma_recv_api = urma_status_t (*)(urma_jfr_t *jfr, urma_target_seg_t *recv_tseg, uint64_t buf, uint32_t len,
                                        uint64_t user_ctx);

using urma_poll_jfc_api = int (*)(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr);
using urma_rearm_jfc_api = urma_status_t (*)(urma_jfc_t *jfc, bool solicited_only);
using urma_wait_jfc_api = int (*)(urma_jfce_t *jfce, uint32_t jfc_cnt, int time_out, urma_jfc_t *jfc[]);
using urma_ack_jfc_api = void (*)(urma_jfc_t *jfc[], uint32_t nevents[], uint32_t jfc_cnt);

using urma_get_uasid_api = urma_status_t (*)(uint32_t *uasid);
using urma_user_ctl_api = urma_status_t (*)(urma_context_t *ctx, urma_user_ctl_in_t *in, urma_user_ctl_out_t *out);

using urma_register_log_func_api = urma_status_t (*)(urma_log_cb_t func);
using urma_unregister_log_func_api = urma_status_t (*)(void);
using urma_log_get_level_api = urma_vlog_level_t (*)(void);
using urma_log_set_level_api = void (*)(urma_vlog_level_t level);
using urma_log_get_thread_tag_api = const char *(*)(void);
using urma_log_set_thread_tag_api = void (*)(const char *tag);

using urma_get_tpn_api = int (*)(urma_jetty_t *jetty);
using urma_get_net_addr_list_api = urma_net_addr_info_t *(*)(urma_context_t * ctx, uint32_t *cnt);
using urma_free_net_addr_list_api = void (*)(urma_net_addr_info_t *net_addr_list);
using urma_modify_tp_api = int (*)(urma_context_t *ctx, uint32_t tpn, urma_tp_cfg_t *cfg, urma_tp_attr_t *attr,
                                   urma_tp_attr_mask_t mask);
using urma_get_tp_list_api = urma_status_t (*)(urma_context_t *ctx, urma_get_tp_cfg_t *cfg, uint32_t *tp_cnt,
                                               urma_tp_info_t *tp_list);
using urma_set_tp_attr_api = urma_status_t (*)(const urma_context_t *ctx, const uint64_t tp_handle,
                                               const uint8_t tp_attr_cnt, const uint32_t tp_attr_bitmap,
                                               const urma_tp_attr_value_t *tp_attr);
using urma_get_tp_attr_api = urma_status_t (*)(const urma_context_t *ctx, const uint64_t tp_handle,
                                               uint8_t *tp_attr_cnt, uint32_t *tp_attr_bitmap,
                                               urma_tp_attr_value_t *tp_attr);
using urma_get_eid_by_ip_api = urma_status_t (*)(const urma_context_t *ctx, const urma_net_addr_t *net_addr,
                                                 urma_eid_t *eid);
using urma_get_ip_by_eid_api = urma_status_t (*)(const urma_context_t *ctx, const urma_eid_t *eid,
                                                 urma_net_addr_t *net_addr);
using urma_get_smac_api = urma_status_t (*)(const urma_context_t *ctx, uint8_t *mac);
using urma_get_dmac_api = urma_status_t (*)(const urma_context_t *ctx, const urma_net_addr_t *net_addr, uint8_t *mac);

using urma_get_perf_info_api = urma_status_t (*)(char *perf_buf, uint32_t *length);

class UrmaApi {
public:
    static urma_status_t urma_init(urma_init_attr_t *conf)
    {
        return urma_init_ptr(conf);
    }

    static urma_status_t urma_uninit(void)
    {
        return urma_uninit_ptr();
    }

    static urma_device_t **urma_get_device_list(int *num_devices)
    {
        return urma_get_device_list_ptr(num_devices);
    }

    static void urma_free_device_list(urma_device_t **device_list)
    {
        urma_free_device_list_ptr(device_list);
    }

    static urma_eid_info_t *urma_get_eid_list(urma_device_t *dev, uint32_t *cnt)
    {
        return urma_get_eid_list_ptr(dev, cnt);
    }

    static void urma_free_eid_list(urma_eid_info_t *eid_list)
    {
        urma_free_eid_list_ptr(eid_list);
    }

    static urma_device_t *urma_get_device_by_name(char *dev_name)
    {
        return urma_get_device_by_name_ptr(dev_name);
    }

    static urma_device_t *urma_get_device_by_eid(urma_eid_t eid, urma_transport_type_t type)
    {
        return urma_get_device_by_eid_ptr(eid, type);
    }

    static urma_status_t urma_query_device(urma_device_t *dev, urma_device_attr_t *dev_attr)
    {
        return urma_query_device_ptr(dev, dev_attr);
    }

    static urma_context_t *urma_create_context(urma_device_t *dev, uint32_t eid_index)
    {
        return urma_create_context_ptr(dev, eid_index);
    }

    static urma_status_t urma_delete_context(urma_context_t *ctx)
    {
        return urma_delete_context_ptr(ctx);
    }

    static urma_status_t urma_set_context_opt(urma_context_t *ctx, urma_opt_name_t opt_name, const void *opt_value,
                                              size_t opt_len)
    {
        return urma_set_context_opt_ptr(ctx, opt_name, opt_value, opt_len);
    }

    static urma_jfc_t *urma_create_jfc(urma_context_t *ctx, urma_jfc_cfg_t *jfc_cfg)
    {
        return urma_create_jfc_ptr(ctx, jfc_cfg);
    }

    static urma_status_t urma_modify_jfc(urma_jfc_t *jfc, urma_jfc_attr_t *attr)
    {
        return urma_modify_jfc_ptr(jfc, attr);
    }

    static urma_status_t urma_delete_jfc(urma_jfc_t *jfc)
    {
        return urma_delete_jfc_ptr(jfc);
    }

    static urma_status_t urma_alloc_jfc(urma_context_t *urma_ctx, urma_jfc_cfg_t *cfg, urma_jfc_t **jfc)
    {
        return urma_alloc_jfc_ptr(urma_ctx, cfg, jfc);
    }

    static urma_status_t urma_set_jfc_opt(urma_jfc_t *jfc, uint64_t opt, void *buf, uint32_t len)
    {
        return urma_set_jfc_opt_ptr(jfc, opt, buf, len);
    }

    static urma_status_t urma_active_jfc(urma_jfc_t *jfc)
    {
        return urma_active_jfc_ptr(jfc);
    }

    static urma_status_t urma_get_jfc_opt(urma_jfc_t *jfc, uint64_t opt, void *buf, uint32_t len)
    {
        return urma_get_jfc_opt_ptr(jfc, opt, buf, len);
    }

    static urma_status_t urma_deactive_jfc(urma_jfc_t *jfc)
    {
        return urma_deactive_jfc_ptr(jfc);
    }

    static urma_status_t urma_free_jfc(urma_jfc_t *jfc)
    {
        return urma_free_jfc_ptr(jfc);
    }

    static urma_status_t urma_delete_jfc_batch(urma_jfc_t **jfc_arr, int jfc_num, urma_jfc_t **bad_jfc)
    {
        return urma_delete_jfc_batch_ptr(jfc_arr, jfc_num, bad_jfc);
    }

    static urma_jfs_t *urma_create_jfs(urma_context_t *ctx, urma_jfs_cfg_t *jfs_cfg)
    {
        return urma_create_jfs_ptr(ctx, jfs_cfg);
    }

    static urma_status_t urma_modify_jfs(urma_jfs_t *jfs, urma_jfs_attr_t *attr)
    {
        return urma_modify_jfs_ptr(jfs, attr);
    }

    static urma_status_t urma_query_jfs(urma_jfs_t *jfs, urma_jfs_cfg_t *cfg, urma_jfs_attr_t *attr)
    {
        return urma_query_jfs_ptr(jfs, cfg, attr);
    }

    static urma_status_t urma_delete_jfs(urma_jfs_t *jfs)
    {
        return urma_delete_jfs_ptr(jfs);
    }

    static urma_status_t urma_delete_jfs_batch(urma_jfs_t **jfs_arr, int jfs_num, urma_jfs_t **bad_jfs)
    {
        return urma_delete_jfs_batch_ptr(jfs_arr, jfs_num, bad_jfs);
    }

    static int urma_flush_jfs(urma_jfs_t *jfs, int cr_cnt, urma_cr_t *cr)
    {
        return urma_flush_jfs_ptr(jfs, cr_cnt, cr);
    }

    static urma_status_t urma_alloc_jfs(urma_context_t *urma_ctx, urma_jfs_cfg_t *cfg, urma_jfs_t **jfs)
    {
        return urma_alloc_jfs_ptr(urma_ctx, cfg, jfs);
    }

    static urma_status_t urma_set_jfs_opt(urma_jfs_t *jfs, uint64_t opt, void *buf, uint32_t len)
    {
        return urma_set_jfs_opt_ptr(jfs, opt, buf, len);
    }

    static urma_status_t urma_active_jfs(urma_jfs_t *jfs)
    {
        return urma_active_jfs_ptr(jfs);
    }

    static urma_status_t urma_get_jfs_opt(urma_jfs_t *jfs, uint64_t opt, void *buf, uint32_t len)
    {
        return urma_get_jfs_opt_ptr(jfs, opt, buf, len);
    }

    static urma_status_t urma_deactive_jfs(urma_jfs_t *jfs)
    {
        return urma_deactive_jfs_ptr(jfs);
    }

    static urma_status_t urma_free_jfs(urma_jfs_t *jfs)
    {
        return urma_free_jfs_ptr(jfs);
    }

    static urma_jfr_t *urma_create_jfr(urma_context_t *ctx, urma_jfr_cfg_t *jfr_cfg)
    {
        return urma_create_jfr_ptr(ctx, jfr_cfg);
    }

    static urma_status_t urma_modify_jfr(urma_jfr_t *jfr, urma_jfr_attr_t *attr)
    {
        return urma_modify_jfr_ptr(jfr, attr);
    }

    static urma_status_t urma_query_jfr(urma_jfr_t *jfr, urma_jfr_cfg_t *cfg, urma_jfr_attr_t *attr)
    {
        return urma_query_jfr_ptr(jfr, cfg, attr);
    }

    static urma_status_t urma_delete_jfr(urma_jfr_t *jfr)
    {
        return urma_delete_jfr_ptr(jfr);
    }

    static urma_status_t urma_delete_jfr_batch(urma_jfr_t **jfr_arr, int jfr_num, urma_jfr_t **bad_jfr)
    {
        return urma_delete_jfr_batch_ptr(jfr_arr, jfr_num, bad_jfr);
    }

    static urma_target_jetty_t *urma_import_jfr(urma_context_t *ctx, urma_rjfr_t *rjfr, urma_token_t *token_value)
    {
        return urma_import_jfr_ptr(ctx, rjfr, token_value);
    }

    static urma_target_jetty_t *urma_import_jfr_ex(urma_context_t *ctx, urma_rjfr_t *rjfr, urma_token_t *token_value,
                                                   urma_import_jfr_ex_cfg_t *cfg)
    {
        return urma_import_jfr_ex_ptr(ctx, rjfr, token_value, cfg);
    }

    static urma_status_t urma_unimport_jfr(urma_target_jetty_t *target_jfr)
    {
        return urma_unimport_jfr_ptr(target_jfr);
    }

    static urma_status_t urma_advise_jfr(urma_jfs_t *jfs, urma_target_jetty_t *tjfr)
    {
        return urma_advise_jfr_ptr(jfs, tjfr);
    }

    static urma_status_t urma_advise_jfr_async(urma_jfs_t *jfs, urma_target_jetty_t *tjfr,
                                               urma_advise_async_cb_func cb_fun, void *cb_arg)
    {
        return urma_advise_jfr_async_ptr(jfs, tjfr, cb_fun, cb_arg);
    }

    static urma_status_t urma_unadvise_jfr(urma_jfs_t *jfs, urma_target_jetty_t *tjfr)
    {
        return urma_unadvise_jfr_ptr(jfs, tjfr);
    }

    static urma_status_t urma_alloc_jfr(urma_context_t *urma_ctx, urma_jfr_cfg_t *cfg, urma_jfr_t **jfr)
    {
        return urma_alloc_jfr_ptr(urma_ctx, cfg, jfr);
    }

    static urma_status_t urma_set_jfr_opt(urma_jfr_t *jfr, uint64_t opt, void *buf, uint32_t len)
    {
        return urma_set_jfr_opt_ptr(jfr, opt, buf, len);
    }

    static urma_status_t urma_active_jfr(urma_jfr_t *jfr)
    {
        return urma_active_jfr_ptr(jfr);
    }

    static urma_status_t urma_get_jfr_opt(urma_jfr_t *jfr, uint64_t opt, void *buf, uint32_t len)
    {
        return urma_get_jfr_opt_ptr(jfr, opt, buf, len);
    }

    static urma_status_t urma_deactive_jfr(urma_jfr_t *jfr)
    {
        return urma_deactive_jfr_ptr(jfr);
    }

    static urma_status_t urma_free_jfr(urma_jfr_t *jfr)
    {
        return urma_free_jfr_ptr(jfr);
    }

    static urma_jetty_t *urma_create_jetty(urma_context_t *ctx, urma_jetty_cfg_t *jetty_cfg)
    {
        return urma_create_jetty_ptr(ctx, jetty_cfg);
    }

    static urma_status_t urma_modify_jetty(urma_jetty_t *jetty, urma_jetty_attr_t *attr)
    {
        return urma_modify_jetty_ptr(jetty, attr);
    }

    static urma_status_t urma_query_jetty(urma_jetty_t *jetty, urma_jetty_cfg_t *cfg, urma_jetty_attr_t *attr)
    {
        return urma_query_jetty_ptr(jetty, cfg, attr);
    }

    static urma_status_t urma_delete_jetty(urma_jetty_t *jetty)
    {
        return urma_delete_jetty_ptr(jetty);
    }

    static urma_status_t urma_delete_jetty_batch(urma_jetty_t **jetty_arr, int jetty_num, urma_jetty_t **bad_jetty)
    {
        return urma_delete_jetty_batch_ptr(jetty_arr, jetty_num, bad_jetty);
    }

    static urma_target_jetty_t *urma_import_jetty(urma_context_t *ctx, urma_rjetty_t *rjetty, urma_token_t *token_value)
    {
        return urma_import_jetty_ptr(ctx, rjetty, token_value);
    }

    static urma_target_jetty_t *urma_import_jetty_ex(urma_context_t *ctx, urma_rjetty_t *rjetty,
                                                     urma_token_t *token_value, urma_import_jetty_ex_cfg_t *cfg)
    {
        return urma_import_jetty_ex_ptr(ctx, rjetty, token_value, cfg);
    }

    static urma_status_t urma_unimport_jetty(urma_target_jetty_t *tjetty)
    {
        return urma_unimport_jetty_ptr(tjetty);
    }

    static urma_status_t urma_advise_jetty(urma_jetty_t *jetty, urma_target_jetty_t *tjetty)
    {
        return urma_advise_jetty_ptr(jetty, tjetty);
    }

    static urma_status_t urma_unadvise_jetty(urma_jetty_t *jetty, urma_target_jetty_t *tjetty)
    {
        return urma_unadvise_jetty_ptr(jetty, tjetty);
    }

    static urma_status_t urma_bind_jetty(urma_jetty_t *jetty, urma_target_jetty_t *tjetty)
    {
        return urma_bind_jetty_ptr(jetty, tjetty);
    }

    static urma_status_t urma_bind_jetty_ex(urma_jetty_t *jetty, urma_target_jetty_t *tjetty,
                                            urma_bind_jetty_ex_cfg_t *cfg)
    {
        return urma_bind_jetty_ex_ptr(jetty, tjetty, cfg);
    }

    static urma_status_t urma_unbind_jetty(urma_jetty_t *jetty)
    {
        return urma_unbind_jetty_ptr(jetty);
    }

    static int urma_flush_jetty(urma_jetty_t *jetty, int cr_cnt, urma_cr_t *cr)
    {
        return urma_flush_jetty_ptr(jetty, cr_cnt, cr);
    }

    static urma_target_jetty_t *urma_import_jetty_async(urma_notifier_t *notifier, const urma_rjetty_t *rjetty,
                                                        const urma_token_t *token_value, uint64_t user_ctx, int timeout)
    {
        return urma_import_jetty_async_ptr(notifier, rjetty, token_value, user_ctx, timeout);
    }

    static urma_status_t urma_unimport_jetty_async(urma_target_jetty_t *tjetty)
    {
        return urma_unimport_jetty_async_ptr(tjetty);
    }

    static urma_status_t urma_bind_jetty_async(urma_notifier_t *notifier, urma_jetty_t *jetty,
                                               urma_target_jetty_t *tjetty, uint64_t user_ctx, int timeout)
    {
        return urma_bind_jetty_async_ptr(notifier, jetty, tjetty, user_ctx, timeout);
    }

    static urma_status_t urma_unbind_jetty_async(urma_jetty_t *jetty)
    {
        return urma_unbind_jetty_async_ptr(jetty);
    }

    static urma_notifier_t *urma_create_notifier(urma_context_t *ctx)
    {
        return urma_create_notifier_ptr(ctx);
    }

    static urma_status_t urma_delete_notifier(urma_notifier_t *notifier)
    {
        return urma_delete_notifier_ptr(notifier);
    }

    static urma_status_t urma_alloc_jetty(urma_context_t *urma_ctx, urma_jetty_cfg_t *cfg, urma_jetty_t **jetty)
    {
        return urma_alloc_jetty_ptr(urma_ctx, cfg, jetty);
    }

    static urma_status_t urma_set_jetty_opt(urma_jetty_t *jetty, uint64_t opt, void *buf, uint32_t len)
    {
        return urma_set_jetty_opt_ptr(jetty, opt, buf, len);
    }

    static urma_status_t urma_active_jetty(urma_jetty_t *jetty)
    {
        return urma_active_jetty_ptr(jetty);
    }

    static urma_status_t urma_get_jetty_opt(urma_jetty_t *jetty, uint64_t opt, void *buf, uint32_t len)
    {
        return urma_get_jetty_opt_ptr(jetty, opt, buf, len);
    }

    static urma_status_t urma_deactive_jetty(urma_jetty_t *jetty)
    {
        return urma_deactive_jetty_ptr(jetty);
    }

    static urma_status_t urma_free_jetty(urma_jetty_t *jetty)
    {
        return urma_free_jetty_ptr(jetty);
    }

    static int urma_wait_notify(urma_notifier_t *notifier, uint32_t cnt, urma_notify_t *notify, int timeout)
    {
        return urma_wait_notify_ptr(notifier, cnt, notify, timeout);
    }

    static urma_status_t urma_ack_notify(urma_context_t *ctx, uint32_t cnt, urma_notify_t *notify)
    {
        return urma_ack_notify_ptr(ctx, cnt, notify);
    }

    static urma_jetty_grp_t *urma_create_jetty_grp(urma_context_t *ctx, urma_jetty_grp_cfg_t *cfg)
    {
        return urma_create_jetty_grp_ptr(ctx, cfg);
    }

    static urma_status_t urma_delete_jetty_grp(urma_jetty_grp_t *jetty_grp)
    {
        return urma_delete_jetty_grp_ptr(jetty_grp);
    }

    static urma_jfce_t *urma_create_jfce(urma_context_t *ctx)
    {
        return urma_create_jfce_ptr(ctx);
    }

    static urma_status_t urma_delete_jfce(urma_jfce_t *jfce)
    {
        return urma_delete_jfce_ptr(jfce);
    }

    static urma_status_t urma_get_async_event(urma_context_t *ctx, urma_async_event_t *event)
    {
        return urma_get_async_event_ptr(ctx, event);
    }

    static void urma_ack_async_event(urma_async_event_t *event)
    {
        urma_ack_async_event_ptr(event);
    }

    static urma_token_id_t *urma_alloc_token_id(urma_context_t *ctx)
    {
        return urma_alloc_token_id_ptr(ctx);
    }

    static urma_token_id_t *urma_alloc_token_id_ex(urma_context_t *ctx, urma_token_id_flag_t flag)
    {
        return urma_alloc_token_id_ex_ptr(ctx, flag);
    }

    static urma_status_t urma_free_token_id(urma_token_id_t *token_id)
    {
        return urma_free_token_id_ptr(token_id);
    }

    static urma_target_seg_t *urma_register_seg(urma_context_t *ctx, urma_seg_cfg_t *seg_cfg)
    {
        return urma_register_seg_ptr(ctx, seg_cfg);
    }

    static urma_status_t urma_unregister_seg(urma_target_seg_t *target_seg)
    {
        return urma_unregister_seg_ptr(target_seg);
    }

    static urma_target_seg_t *urma_import_seg(urma_context_t *ctx, urma_seg_t *seg, urma_token_t *token_value,
                                              uint64_t addr, urma_import_seg_flag_t flag)
    {
        return urma_import_seg_ptr(ctx, seg, token_value, addr, flag);
    }

    static urma_status_t urma_unimport_seg(urma_target_seg_t *tseg)
    {
        return urma_unimport_seg_ptr(tseg);
    }

    static urma_status_t urma_post_jfs_wr(urma_jfs_t *jfs, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr)
    {
        return urma_post_jfs_wr_ptr(jfs, wr, bad_wr);
    }

    static urma_status_t urma_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr)
    {
        return urma_post_jfr_wr_ptr(jfr, wr, bad_wr);
    }

    static urma_status_t urma_post_jetty_send_wr(urma_jetty_t *jetty, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr)
    {
        return urma_post_jetty_send_wr_ptr(jetty, wr, bad_wr);
    }

    static urma_status_t urma_post_jetty_recv_wr(urma_jetty_t *jetty, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr)
    {
        return urma_post_jetty_recv_wr_ptr(jetty, wr, bad_wr);
    }

    static urma_status_t urma_write(urma_jfs_t *jfs, urma_target_jetty_t *target_jfr, urma_target_seg_t *dst_tseg,
                                    urma_target_seg_t *src_tseg, uint64_t dst, uint64_t src, uint32_t len,
                                    urma_jfs_wr_flag_t flag, uint64_t user_ctx)
    {
        return urma_write_ptr(jfs, target_jfr, dst_tseg, src_tseg, dst, src, len, flag, user_ctx);
    }

    static urma_status_t urma_read(urma_jfs_t *jfs, urma_target_jetty_t *target_jfr, urma_target_seg_t *dst_tseg,
                                   urma_target_seg_t *src_tseg, uint64_t dst, uint64_t src, uint32_t len,
                                   urma_jfs_wr_flag_t flag, uint64_t user_ctx)
    {
        return urma_read_ptr(jfs, target_jfr, dst_tseg, src_tseg, dst, src, len, flag, user_ctx);
    }

    static urma_status_t urma_send(urma_jfs_t *jfs, urma_target_jetty_t *target_jfr, urma_target_seg_t *src_tseg,
                                   uint64_t src, uint32_t len, urma_jfs_wr_flag_t flag, uint64_t user_ctx)
    {
        return urma_send_ptr(jfs, target_jfr, src_tseg, src, len, flag, user_ctx);
    }

    static urma_status_t urma_recv(urma_jfr_t *jfr, urma_target_seg_t *recv_tseg, uint64_t buf, uint32_t len,
                                   uint64_t user_ctx)
    {
        return urma_recv_ptr(jfr, recv_tseg, buf, len, user_ctx);
    }

    static int urma_poll_jfc(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr)
    {
        return urma_poll_jfc_ptr(jfc, cr_cnt, cr);
    }

    static urma_status_t urma_rearm_jfc(urma_jfc_t *jfc, bool solicited_only)
    {
        return urma_rearm_jfc_ptr(jfc, solicited_only);
    }

    static int urma_wait_jfc(urma_jfce_t *jfce, uint32_t jfc_cnt, int time_out, urma_jfc_t *jfc[])
    {
        return urma_wait_jfc_ptr(jfce, jfc_cnt, time_out, jfc);
    }

    static void urma_ack_jfc(urma_jfc_t *jfc[], uint32_t nevents[], uint32_t jfc_cnt)
    {
        urma_ack_jfc_ptr(jfc, nevents, jfc_cnt);
    }

    static urma_status_t urma_get_uasid(uint32_t *uasid)
    {
        return urma_get_uasid_ptr(uasid);
    }

    static urma_status_t urma_user_ctl(urma_context_t *ctx, urma_user_ctl_in_t *in, urma_user_ctl_out_t *out)
    {
        return urma_user_ctl_ptr(ctx, in, out);
    }

    static urma_status_t urma_register_log_func(urma_log_cb_t func)
    {
        return urma_register_log_func_ptr(func);
    }

    static urma_status_t urma_unregister_log_func(void)
    {
        return urma_unregister_log_func_ptr();
    }

    static urma_vlog_level_t urma_log_get_level(void)
    {
        return urma_log_get_level_ptr();
    }

    static void urma_log_set_level(urma_vlog_level_t level)
    {
        urma_log_set_level_ptr(level);
    }

    static const char *urma_log_get_thread_tag(void)
    {
        return urma_log_get_thread_tag_ptr();
    }

    static void urma_log_set_thread_tag(const char *tag)
    {
        urma_log_set_thread_tag_ptr(tag);
    }

    static int urma_get_tpn(urma_jetty_t *jetty)
    {
        return urma_get_tpn_ptr(jetty);
    }

    static urma_net_addr_info_t *urma_get_net_addr_list(urma_context_t *ctx, uint32_t *cnt)
    {
        return urma_get_net_addr_list_ptr(ctx, cnt);
    }

    static void urma_free_net_addr_list(urma_net_addr_info_t *net_addr_list)
    {
        urma_free_net_addr_list_ptr(net_addr_list);
    }

    static int urma_modify_tp(urma_context_t *ctx, uint32_t tpn, urma_tp_cfg_t *cfg, urma_tp_attr_t *attr,
                              urma_tp_attr_mask_t mask)
    {
        return urma_modify_tp_ptr(ctx, tpn, cfg, attr, mask);
    }

    static urma_status_t urma_get_tp_list(urma_context_t *ctx, urma_get_tp_cfg_t *cfg, uint32_t *tp_cnt,
                                          urma_tp_info_t *tp_list)
    {
        return urma_get_tp_list_ptr(ctx, cfg, tp_cnt, tp_list);
    }

    static urma_status_t urma_set_tp_attr(const urma_context_t *ctx, const uint64_t tp_handle,
                                          const uint8_t tp_attr_cnt, const uint32_t tp_attr_bitmap,
                                          const urma_tp_attr_value_t *tp_attr)
    {
        return urma_set_tp_attr_ptr(ctx, tp_handle, tp_attr_cnt, tp_attr_bitmap, tp_attr);
    }

    static urma_status_t urma_get_tp_attr(const urma_context_t *ctx, const uint64_t tp_handle, uint8_t *tp_attr_cnt,
                                          uint32_t *tp_attr_bitmap, urma_tp_attr_value_t *tp_attr)
    {
        return urma_get_tp_attr_ptr(ctx, tp_handle, tp_attr_cnt, tp_attr_bitmap, tp_attr);
    }

    static urma_status_t urma_get_eid_by_ip(const urma_context_t *ctx, const urma_net_addr_t *net_addr, urma_eid_t *eid)
    {
        return urma_get_eid_by_ip_ptr(ctx, net_addr, eid);
    }

    static urma_status_t urma_get_ip_by_eid(const urma_context_t *ctx, const urma_eid_t *eid, urma_net_addr_t *net_addr)
    {
        return urma_get_ip_by_eid_ptr(ctx, eid, net_addr);
    }

    static urma_status_t urma_get_smac(const urma_context_t *ctx, uint8_t *mac)
    {
        return urma_get_smac_ptr(ctx, mac);
    }

    static urma_status_t urma_get_dmac(const urma_context_t *ctx, const urma_net_addr_t *net_addr, uint8_t *mac)
    {
        return urma_get_dmac_ptr(ctx, net_addr, mac);
    }

    static urma_status_t urma_get_perf_info(char *perf_buf, uint32_t *length)
    {
        return urma_get_perf_info_ptr(perf_buf, length);
    }

    static Result Load() noexcept;
    static void UnLoad() noexcept;

    static bool IsLoaded() noexcept;

private:
    DL_API_DECLARE(urma_init);
    DL_API_DECLARE(urma_uninit);
    DL_API_DECLARE(urma_get_device_list);
    DL_API_DECLARE(urma_free_device_list);
    DL_API_DECLARE(urma_get_eid_list);
    DL_API_DECLARE(urma_free_eid_list);
    DL_API_DECLARE(urma_get_device_by_name);
    DL_API_DECLARE(urma_get_device_by_eid);
    DL_API_DECLARE(urma_query_device);
    DL_API_DECLARE(urma_create_context);
    DL_API_DECLARE(urma_delete_context);
    DL_API_DECLARE(urma_set_context_opt);

    DL_API_DECLARE(urma_create_jfc);
    DL_API_DECLARE(urma_modify_jfc);
    DL_API_DECLARE(urma_delete_jfc);
    DL_API_DECLARE(urma_alloc_jfc);
    DL_API_DECLARE(urma_set_jfc_opt);
    DL_API_DECLARE(urma_active_jfc);
    DL_API_DECLARE(urma_get_jfc_opt);
    DL_API_DECLARE(urma_deactive_jfc);
    DL_API_DECLARE(urma_free_jfc);
    DL_API_DECLARE(urma_delete_jfc_batch);

    DL_API_DECLARE(urma_create_jfs);
    DL_API_DECLARE(urma_modify_jfs);
    DL_API_DECLARE(urma_query_jfs);
    DL_API_DECLARE(urma_delete_jfs);
    DL_API_DECLARE(urma_delete_jfs_batch);
    DL_API_DECLARE(urma_flush_jfs);
    DL_API_DECLARE(urma_alloc_jfs);
    DL_API_DECLARE(urma_set_jfs_opt);
    DL_API_DECLARE(urma_active_jfs);
    DL_API_DECLARE(urma_get_jfs_opt);
    DL_API_DECLARE(urma_deactive_jfs);
    DL_API_DECLARE(urma_free_jfs);

    DL_API_DECLARE(urma_create_jfr);
    DL_API_DECLARE(urma_modify_jfr);
    DL_API_DECLARE(urma_query_jfr);
    DL_API_DECLARE(urma_delete_jfr);
    DL_API_DECLARE(urma_delete_jfr_batch);
    DL_API_DECLARE(urma_import_jfr);
    DL_API_DECLARE(urma_import_jfr_ex);
    DL_API_DECLARE(urma_unimport_jfr);
    DL_API_DECLARE(urma_advise_jfr);
    DL_API_DECLARE(urma_advise_jfr_async);
    DL_API_DECLARE(urma_unadvise_jfr);
    DL_API_DECLARE(urma_alloc_jfr);
    DL_API_DECLARE(urma_set_jfr_opt);
    DL_API_DECLARE(urma_active_jfr);
    DL_API_DECLARE(urma_get_jfr_opt);
    DL_API_DECLARE(urma_deactive_jfr);
    DL_API_DECLARE(urma_free_jfr);

    DL_API_DECLARE(urma_create_jetty);
    DL_API_DECLARE(urma_modify_jetty);
    DL_API_DECLARE(urma_query_jetty);
    DL_API_DECLARE(urma_delete_jetty);
    DL_API_DECLARE(urma_delete_jetty_batch);
    DL_API_DECLARE(urma_import_jetty);
    DL_API_DECLARE(urma_import_jetty_ex);
    DL_API_DECLARE(urma_unimport_jetty);
    DL_API_DECLARE(urma_advise_jetty);
    DL_API_DECLARE(urma_unadvise_jetty);
    DL_API_DECLARE(urma_bind_jetty);
    DL_API_DECLARE(urma_bind_jetty_ex);
    DL_API_DECLARE(urma_unbind_jetty);
    DL_API_DECLARE(urma_flush_jetty);
    DL_API_DECLARE(urma_import_jetty_async);
    DL_API_DECLARE(urma_unimport_jetty_async);
    DL_API_DECLARE(urma_bind_jetty_async);
    DL_API_DECLARE(urma_unbind_jetty_async);
    DL_API_DECLARE(urma_create_notifier);
    DL_API_DECLARE(urma_delete_notifier);
    DL_API_DECLARE(urma_alloc_jetty);
    DL_API_DECLARE(urma_set_jetty_opt);
    DL_API_DECLARE(urma_active_jetty);
    DL_API_DECLARE(urma_get_jetty_opt);
    DL_API_DECLARE(urma_deactive_jetty);
    DL_API_DECLARE(urma_free_jetty);
    DL_API_DECLARE(urma_wait_notify);
    DL_API_DECLARE(urma_ack_notify);

    DL_API_DECLARE(urma_create_jetty_grp);
    DL_API_DECLARE(urma_delete_jetty_grp);

    DL_API_DECLARE(urma_create_jfce);
    DL_API_DECLARE(urma_delete_jfce);
    DL_API_DECLARE(urma_get_async_event);
    DL_API_DECLARE(urma_ack_async_event);
    DL_API_DECLARE(urma_alloc_token_id);
    DL_API_DECLARE(urma_alloc_token_id_ex);
    DL_API_DECLARE(urma_free_token_id);

    DL_API_DECLARE(urma_register_seg);
    DL_API_DECLARE(urma_unregister_seg);
    DL_API_DECLARE(urma_import_seg);
    DL_API_DECLARE(urma_unimport_seg);

    DL_API_DECLARE(urma_post_jfs_wr);
    DL_API_DECLARE(urma_post_jfr_wr);
    DL_API_DECLARE(urma_post_jetty_send_wr);
    DL_API_DECLARE(urma_post_jetty_recv_wr);

    DL_API_DECLARE(urma_write);
    DL_API_DECLARE(urma_read);
    DL_API_DECLARE(urma_send);
    DL_API_DECLARE(urma_recv);

    DL_API_DECLARE(urma_poll_jfc);
    DL_API_DECLARE(urma_rearm_jfc);
    DL_API_DECLARE(urma_wait_jfc);
    DL_API_DECLARE(urma_ack_jfc);

    DL_API_DECLARE(urma_get_uasid);
    DL_API_DECLARE(urma_user_ctl);

    DL_API_DECLARE(urma_register_log_func);
    DL_API_DECLARE(urma_unregister_log_func);
    DL_API_DECLARE(urma_log_get_level);
    DL_API_DECLARE(urma_log_set_level);
    DL_API_DECLARE(urma_log_get_thread_tag);
    DL_API_DECLARE(urma_log_set_thread_tag);

    DL_API_DECLARE(urma_get_tpn);
    DL_API_DECLARE(urma_get_net_addr_list);
    DL_API_DECLARE(urma_free_net_addr_list);
    DL_API_DECLARE(urma_modify_tp);
    DL_API_DECLARE(urma_get_tp_list);
    DL_API_DECLARE(urma_set_tp_attr);
    DL_API_DECLARE(urma_get_tp_attr);
    DL_API_DECLARE(urma_get_eid_by_ip);
    DL_API_DECLARE(urma_get_ip_by_eid);
    DL_API_DECLARE(urma_get_smac);
    DL_API_DECLARE(urma_get_dmac);
    DL_API_DECLARE(urma_get_perf_info);

private:
    static void UnLoadInner() noexcept;
    static std::mutex LOAD_MUTEX;
    static bool LOADED;
};

inline bool UrmaApi::IsLoaded() noexcept
{
    return LOADED;
}
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_DL_URMA_API_H
