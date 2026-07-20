/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef ZBAL_DEEPEP_H_
#define ZBAL_DEEPEP_H_

#include <optional>
#include <tuple>
#include <vector>

#include <ATen/Tensor.h>
#include <torch/python.h>
#include <torch/types.h>

#include "aclnn/opdev/platform.h"
#include "dl_cann_api.h"

#include "zbal_def.h"
#include "zbal_deepep_config.h"
#include "zbal_deep_event.h"

namespace zbal {
namespace adaptor {
namespace deep_ep {

struct Buffer {
public:
    Buffer(int rank, int num_ranks, int64_t num_nvl_bytes, int64_t num_rdma_bytes, bool low_latency_mode,
           std::string moe_group_name);

    ~Buffer() noexcept(false);

    bool is_available() const;

    bool is_internode_available() const;

    int get_num_rdma_ranks() const;

    int get_rdma_rank() const;

    void clean_low_latency_buffer(int num_max_dispatch_tokens_per_rank, int hidden, int num_experts);

    at::Tensor get_send_token_idx() const;

    std::tuple<torch::Tensor, std::optional<torch::Tensor>, torch::Tensor, torch::Tensor, std::optional<EventHandle>>
    get_dispatch_layout(const torch::Tensor &topk_idx, int num_experts, std::optional<EventHandle> &previous_event,
                        bool async, bool allocate_on_comm_stream);

    std::tuple<at::Tensor, std::optional<at::Tensor>, std::optional<at::Tensor>, std::optional<at::Tensor>,
               std::vector<int>, at::Tensor, at::Tensor, std::optional<EventHandle>>
    intranode_dispatch(const at::Tensor &x, const std::optional<at::Tensor> &x_scales,
                       const std::optional<at::Tensor> &topk_idx, const std::optional<at::Tensor> &topk_weights,
                       const std::optional<at::Tensor> &num_tokens_per_rank, const at::Tensor &is_token_in_rank,
                       const std::optional<at::Tensor> &num_tokens_per_expert, int num_worst_tokens,
                       const Config &config, std::optional<EventHandle> &previous_event, bool async,
                       bool allocate_on_comm_stream, bool use_quant);

    std::tuple<torch::Tensor, std::optional<torch::Tensor>, std::optional<EventHandle>>
    intranode_combine(const torch::Tensor &x, const torch::Tensor &topk_idx,
                      const std::optional<torch::Tensor> &topk_weights, const torch::Tensor &put_offset,
                      const torch::Tensor &balance_matrix, std::optional<EventHandle> &previous_event, bool async,
                      bool allocate_on_comm_stream);

    std::tuple<at::Tensor, std::optional<at::Tensor>, at::Tensor, at::Tensor, at::Tensor, std::optional<EventHandle>,
               std::optional<std::function<void()>>>
    low_latency_dispatch(const at::Tensor &x, const at::Tensor &topk_idx,
                         const std::optional<at::Tensor> &cumulative_local_expert_recv_stats,
                         int64_t num_max_dispatch_tokens_per_rank, int64_t num_experts, bool use_fp8, bool round_scale,
                         bool use_ue8m0, bool async, bool return_recv_hook);

    std::tuple<at::Tensor, std::optional<EventHandle>, std::optional<std::function<void()>>>
    low_latency_combine(const at::Tensor &x, const at::Tensor &topk_idx, const at::Tensor &topk_weights,
                        const at::Tensor &src_info, const at::Tensor &layout_range,
                        int64_t num_max_dispatch_tokens_per_rank, int64_t num_experts,
                        const at::Tensor &packed_recv_count, bool zero_copy, bool async, bool return_recv_hook,
                        const std::optional<at::Tensor> &out);

#if defined(ZBAL_ASCEND_NPU_A3) && defined(ZBAL_FUSED_DEEP_MOE_ENABLED)
    std::tuple<at::Tensor, at::Tensor, at::Tensor> fused_deep_moe(
        const at::Tensor &x, const at::Tensor &expert_ids, const at::Tensor &gmm1_weight, const at::Tensor &gmm1_scale,
        const at::Tensor &gmm2_weight, const at::Tensor &gmm2_scale, const at::Tensor &expert_scales,
        int64_t moe_expert_num, int64_t gmm1_h_len, const std::optional<at::Tensor> &expert_smooth_scales,
        const std::optional<at::Tensor> &share_gmm1_weight, const std::optional<at::Tensor> &share_gmm1_scale,
        const std::optional<at::Tensor> &share_gmm2_weight, const std::optional<at::Tensor> &share_gmm2_scale,
        const std::optional<at::Tensor> &share_smooth_scales, const std::optional<at::Tensor> &x_active_mask,
        int64_t quant_mode, int64_t global_bs, int64_t share_gmm1_h_len, bool is_tensor_list);
#endif // ZBAL_ASCEND_NPU_A3 && ZBAL_FUSED_DEEP_MOE_ENABLED

private:
    int device_id;
    int rank, rdma_rank, nvl_rank;
    int num_ranks, num_rdma_ranks, num_nvl_ranks;
    op::SocVersion soc_version;
    int num_max_hccs_peers;
    int num_aiv_cores_{0};

    int64_t num_nvl_bytes;
    int64_t num_rdma_bytes;

    bool low_latency_mode = false;

    std::string moe_group_name;
    zbal_comm_t comm_{nullptr};
    aclrtStream comm_stream_;

    bool available = false;
    bool is_padding = false;
    int padding_cnt = 0;

    at::Tensor send_token_idx;
    at::Tensor new_topk_idx;
    at::Tensor ori_x;
    at::Tensor new_scales;
#if defined(ZBAL_ASCEND_NPU_A3) && defined(ZBAL_FUSED_DEEP_MOE_ENABLED)
    // Cached output tensors to avoid per-call at::zeros (triggers InplaceZero, pollutes L2 cache)
    at::Tensor cached_fdm_output_;
    at::Tensor cached_fdm_expert_token_nums_;
    at::Tensor cached_fdm_share_output_;
#endif
};
} // namespace deep_ep
} // namespace adaptor
} // namespace zbal

#endif // ZBAL_DEEPEP_H_
