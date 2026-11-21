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
#ifndef MF_GENETTY_DL_VERBS_API_H
#define MF_GENETTY_DL_VERBS_API_H

#include "verbs_types.h"

#include "under_api/genetty_dl_api.h"

namespace ock {
namespace openDCC {
using ibv_get_device_list_api = struct ibv_device **(*)(int *num_devices);
using ibv_fork_init_api = int (*)();
using ibv_open_device_api = struct ibv_context *(*)(struct ibv_device * device);
using ibv_alloc_pd_api = struct ibv_pd *(*)(struct ibv_context * context);
using ibv_query_port_api = int (*)(struct ibv_context *context, uint8_t port_num,
                                   struct _compat_ibv_port_attr *port_attr);
using ibv_free_device_list_api = void (*)(struct ibv_device **list);
using ibv_reg_mr_api = struct ibv_mr *(*)(struct ibv_pd * pd, void *addr, size_t length, int access);
using ibv_create_comp_channel_api = struct ibv_comp_channel *(*)(struct ibv_context * context);
using ibv_get_cq_event_api = int (*)(struct ibv_comp_channel *channel, struct ibv_cq **cq, void **cq_context);
using ibv_get_async_event_api = int (*)(struct ibv_context *context, struct ibv_async_event *event);
using ibv_ack_async_event_api = void (*)(struct ibv_async_event *event);
using ibv_create_qp_api = struct ibv_qp *(*)(struct ibv_pd * pd, struct ibv_qp_init_attr *qp_init_attr);
using ibv_close_device_api = int (*)(struct ibv_context *context);
using ibv_dealloc_pd_api = int (*)(struct ibv_pd *pd);
using ibv_create_cq_api = struct ibv_cq *(*)(struct ibv_context * context, int cqe, void *cq_context,
                                             struct ibv_comp_channel *channel, int comp_vector);
using ibv_destroy_comp_channel_api = int (*)(struct ibv_comp_channel *channel);
using ibv_destroy_cq_api = int (*)(struct ibv_cq *cq);
using ibv_ack_cq_events_api = void (*)(struct ibv_cq *cq, unsigned int nevents);
using ibv_destroy_qp_api = int (*)(struct ibv_qp *qp);
using ibv_modify_qp_api = int (*)(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask);
using ibv_dereg_mr_api = int (*)(struct ibv_mr *mr);
using ibv_query_gid_api = int (*)(struct ibv_context *context, uint8_t port_num, int index, union ibv_gid *gid);
using ibv_query_device_api = int (*)(struct ibv_context *context, struct ibv_device_attr *device_attr);
using ibv_reg_mr_iova2_api = struct ibv_mr *(*)(struct ibv_pd * pd, void *addr, size_t length, uint64_t iova,
                                                unsigned int access);
using ibv_port_state_str_api = const char *(*)(enum ibv_port_state port_state);

class VerbsApi {
public:
    static struct ibv_device **ibv_get_device_list(int *num_devices)
    {
        return ibv_get_device_list_ptr(num_devices);
    }

    static int ibv_fork_init()
    {
        return ibv_fork_init_ptr();
    }

    static struct ibv_context *ibv_open_device(struct ibv_device *device)
    {
        return ibv_open_device_ptr(device);
    }

    static struct ibv_pd *ibv_alloc_pd(struct ibv_context *context)
    {
        return ibv_alloc_pd_ptr(context);
    }

    static int ibv_query_port(struct ibv_context *context, uint8_t port_num, struct _compat_ibv_port_attr *port_attr)
    {
        return ibv_query_port_ptr(context, port_num, port_attr);
    }

    static void ibv_free_device_list(struct ibv_device **list)
    {
        ibv_free_device_list_ptr(list);
    }

    static struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access)
    {
        return ibv_reg_mr_ptr(pd, addr, length, access);
    }

    static struct ibv_comp_channel *ibv_create_comp_channel(struct ibv_context *context)
    {
        return ibv_create_comp_channel_ptr(context);
    }

    static int ibv_get_cq_event(struct ibv_comp_channel *channel, struct ibv_cq **cq, void **cq_context)
    {
        return ibv_get_cq_event_ptr(channel, cq, cq_context);
    }

    static int ibv_get_async_event(struct ibv_context *context, struct ibv_async_event *event)
    {
        return ibv_get_async_event_ptr(context, event);
    }

    static void ibv_ack_async_event(struct ibv_async_event *event)
    {
        ibv_ack_async_event_ptr(event);
    }

    static struct ibv_qp *ibv_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr)
    {
        return ibv_create_qp_ptr(pd, qp_init_attr);
    }

    static int ibv_close_device(struct ibv_context *context)
    {
        return ibv_close_device_ptr(context);
    }

    static int ibv_dealloc_pd(struct ibv_pd *pd)
    {
        return ibv_dealloc_pd_ptr(pd);
    }

    static struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe, void *cq_context,
                                        struct ibv_comp_channel *channel, int comp_vector)
    {
        return ibv_create_cq_ptr(context, cqe, cq_context, channel, comp_vector);
    }

    static int ibv_destroy_comp_channel(struct ibv_comp_channel *channel)
    {
        return ibv_destroy_comp_channel_ptr(channel);
    }

    static int ibv_destroy_cq(struct ibv_cq *cq)
    {
        return ibv_destroy_cq_ptr(cq);
    }

    static void ibv_ack_cq_events(struct ibv_cq *cq, unsigned int nevents)
    {
        ibv_ack_cq_events_ptr(cq, nevents);
    }

    static int ibv_destroy_qp(struct ibv_qp *qp)
    {
        return ibv_destroy_qp_ptr(qp);
    }

    static int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask)
    {
        return ibv_modify_qp_ptr(qp, attr, attr_mask);
    }

    static int ibv_dereg_mr(struct ibv_mr *mr)
    {
        return ibv_dereg_mr_ptr(mr);
    }

    static int ibv_query_gid(struct ibv_context *context, uint8_t port_num, int index, union ibv_gid *gid)
    {
        return ibv_query_gid_ptr(context, port_num, index, gid);
    }

    static int ibv_query_device(struct ibv_context *context, struct ibv_device_attr *device_attr)
    {
        return ibv_query_device_ptr(context, device_attr);
    }

    static struct ibv_mr *ibv_reg_mr_iova2(struct ibv_pd *pd, void *addr, size_t length, uint64_t iova,
                                           unsigned int access)
    {
        return ibv_reg_mr_iova2_ptr(pd, addr, length, iova, access);
    }

    static const char *ibv_port_state_str(enum ibv_port_state port_state)
    {
        return ibv_port_state_str_ptr(port_state);
    }

    /* Inline verbs APIs dispatch through the provider rather than exported library symbols. */
    static int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr)
    {
        return qp->context->ops.post_send(qp, wr, bad_wr);
    }

    static int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr)
    {
        return qp->context->ops.post_recv(qp, wr, bad_wr);
    }

    static int ibv_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc)
    {
        return cq->context->ops.poll_cq(cq, num_entries, wc);
    }

    static int ibv_req_notify_cq(struct ibv_cq *cq, int solicited_only)
    {
        return cq->context->ops.req_notify_cq(cq, solicited_only);
    }

public:
    static Result Load() noexcept;
    static void UnLoad() noexcept;

    static bool IsLoaded() noexcept;

private:
    static void UnLoadInner() noexcept;

private:
    DL_API_DECLARE(ibv_get_device_list);
    DL_API_DECLARE(ibv_fork_init);
    DL_API_DECLARE(ibv_open_device);
    DL_API_DECLARE(ibv_alloc_pd);
    DL_API_DECLARE(ibv_query_port);
    DL_API_DECLARE(ibv_free_device_list);
    DL_API_DECLARE(ibv_reg_mr);
    DL_API_DECLARE(ibv_create_comp_channel);
    DL_API_DECLARE(ibv_get_cq_event);
    DL_API_DECLARE(ibv_get_async_event);
    DL_API_DECLARE(ibv_ack_async_event);
    DL_API_DECLARE(ibv_create_qp);
    DL_API_DECLARE(ibv_close_device);
    DL_API_DECLARE(ibv_dealloc_pd);
    DL_API_DECLARE(ibv_create_cq);
    DL_API_DECLARE(ibv_destroy_comp_channel);
    DL_API_DECLARE(ibv_destroy_cq);
    DL_API_DECLARE(ibv_ack_cq_events);
    DL_API_DECLARE(ibv_destroy_qp);
    DL_API_DECLARE(ibv_modify_qp);
    DL_API_DECLARE(ibv_dereg_mr);
    DL_API_DECLARE(ibv_query_gid);
    DL_API_DECLARE(ibv_query_device);
    DL_API_DECLARE(ibv_reg_mr_iova2);
    DL_API_DECLARE(ibv_port_state_str);

    static std::mutex LOAD_MUTEX;
    static bool LOADED;
};

inline bool VerbsApi::IsLoaded() noexcept
{
    return LOADED;
}
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_DL_VERBS_API_H
