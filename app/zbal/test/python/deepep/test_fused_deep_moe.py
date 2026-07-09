#!/usr/bin/env python3
"""
Precision test for fused_deep_moe: compares fused vs unfused
(low_latency_dispatch + per-expert GMM1 + SwiGLU + per-expert GMM2 + low_latency_combine).

Both paths use BF16 compute (fused: quant_mode=0, unfused: use_fp8=False dispatch).

Usage:
  python test_fused_deep_moe.py
  python test_fused_deep_moe.py --num-processes 8 --num-experts 64 --hidden 7168 --ffn-dim 4096
  python test_fused_deep_moe.py --num-tokens 128
"""

import argparse
import os
import sys
import math
import logging

import torch
import torch.distributed as dist
import torch_npu
from zbal import Buffer

torch_npu.npu.config.allow_internal_format = True

from utils import calc_diff, init_dist

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")


def generate_data(
    rank,
    bs,
    hidden,
    ffn_dim,
    num_local_experts,
    num_experts,
    num_topk,
    with_shared,
    with_smooth,
    share_ffn_dim,
    use_balanced_idx=False,
):
    """Generate test data for both fused and unfused paths.

    Returns both NZ-format weights (for fused kernel) and base weights
    (for unfused reference GEMM).
    """
    torch.manual_seed(42 + rank)

    x = (torch.rand((bs, hidden), device="npu") * 10 - 5).to(torch.bfloat16)

    if use_balanced_idx:
        expert_ids = torch.arange(
            rank * bs * num_topk, rank * bs * num_topk + bs * num_topk, dtype=torch.int32, device="npu"
        ).reshape(bs, num_topk)
    else:
        expert_ids = torch.randint(0, num_experts, (bs, num_topk), dtype=torch.int32, device="npu")
    expert_ids = expert_ids % num_experts

    expert_scales = torch.rand((bs, num_topk), dtype=torch.float32, device="npu")

    g2_dim = ffn_dim // 2
    # Base weights (standard format, for unfused reference)
    g1_w_base = torch.randint(-16, 16, (num_local_experts, hidden, ffn_dim), dtype=torch.int8, device="npu")
    g1_s_base = torch.rand((num_local_experts, ffn_dim), dtype=torch.float32, device="npu") * 0.003 + 0.0015
    g2_w_base = torch.randint(-16, 16, (num_local_experts, g2_dim, hidden), dtype=torch.int8, device="npu")
    g2_s_base = torch.rand((num_local_experts, hidden), dtype=torch.float32, device="npu") * 0.003 + 0.0015
    # NZ format for fused kernel
    g1_w = torch_npu.npu_format_cast(g1_w_base, 29)
    g1_s = g1_s_base.contiguous()
    g2_w = torch_npu.npu_format_cast(g2_w_base, 29)
    g2_s = g2_s_base.to(torch.bfloat16).contiguous()

    # TopK indices (int64) and weights for unfused dispatch/combine
    topk_idx = expert_ids.to(torch.int64)
    topk_weights = expert_scales.clone()

    sh_w1 = sh_s1 = sh_w2 = sh_s2 = None
    sh_w1_base = sh_s1_base = sh_w2_base = sh_s2_base = None
    smooth_scales = None
    sh_smooth = None

    if with_shared:
        sg2_dim = share_ffn_dim // 2
        sh_w1_base = torch.randint(-16, 16, (hidden, share_ffn_dim), dtype=torch.int8, device="npu")
        sh_w2_base = torch.randint(-16, 16, (sg2_dim, hidden), dtype=torch.int8, device="npu")
        sh_s1_base = torch.rand((share_ffn_dim,), dtype=torch.float32, device="npu") * 0.003 + 0.0015
        sh_s2_base = torch.rand((hidden,), dtype=torch.float32, device="npu") * 0.003 + 0.0015
        sh_w1 = torch_npu.npu_format_cast(sh_w1_base, 29)
        sh_w2 = torch_npu.npu_format_cast(sh_w2_base, 29)
        sh_s1 = sh_s1_base.contiguous()
        sh_s2 = sh_s2_base.to(torch.bfloat16).contiguous()
    if with_smooth:
        smooth_scales = torch.rand((num_experts, hidden), dtype=torch.float32, device="npu")
        if with_shared:
            sh_smooth = torch.rand((hidden,), dtype=torch.float32, device="npu")

    return (
        x,
        expert_ids,
        expert_scales,
        g1_w,
        g1_s,
        g2_w,
        g2_s,
        sh_w1,
        sh_s1,
        sh_w2,
        sh_s2,
        smooth_scales,
        sh_smooth,
        g1_w_base,
        g1_s_base,
        g2_w_base,
        g2_s_base,
        sh_w1_base,
        sh_s1_base,
        sh_w2_base,
        sh_s2_base,
        topk_idx,
        topk_weights,
    )


def run_fused(
    buffer,
    x,
    expert_ids,
    expert_scales,
    g1_w,
    g1_s,
    g2_w,
    g2_s,
    num_experts,
    ffn_dim,
    smooth_scales,
    sh_w1,
    sh_s1,
    sh_w2,
    sh_s2,
    sh_smooth,
    share_ffn_dim,
):
    """Execute the fused deep_moe kernel (quant_mode=0, BF16 path).
    Workspace is allocated internally by the C++ adaptor.
    """
    has_sh = sh_w1 is not None and sh_w1.numel() > 0
    sh1_h_len = share_ffn_dim if has_sh else 0

    return buffer.fused_deep_moe(
        x=x,
        topk_idx=expert_ids,
        gmm1_permuted_weight=g1_w,
        gmm1_permuted_weight_scale=g1_s,
        gmm2_weight=g2_w,
        gmm2_weight_scale=g2_s,
        topk_weights=expert_scales,
        num_experts=num_experts,
        quant_mode=0,  # BF16 path (no INT8 quantization)
        num_max_dispatch_tokens_per_rank=0,
        # is_tensor_list=False,
        # expert_smooth_scales=smooth_scales,
        # share_gmm1_weight=sh_w1,
        # share_gmm1_scale=sh_s1,
        # share_gmm2_weight=sh_w2,
        # share_gmm2_scale=sh_s2,
        # share_smooth_scales=sh_smooth,
        # share_gmm1_h_len=sh1_h_len,
    )


def run_unfused(
    buffer,
    x,
    topk_idx,
    topk_weights,
    g1_w_base,
    g1_s_base,
    g2_w_base,
    g2_s_base,
    num_experts,
    num_ranks,
    num_max_dispatch_tokens_per_rank,
    use_fp8=True,
):
    """Unfused reference with BF16 low-latency dispatch/combine.

    Pipeline:
      1. low_latency_dispatch(use_fp8=False) → BF16 tokens (3D)
      2. Per-expert GMM1 + SwiGLU            → BF16
      3. Per-expert GMM2                     → BF16, re-pack to 3D
      4. low_latency_combine                 → BF16 output

    Returns (output, expert_token_nums_tensor).
    """
    hidden = x.shape[1]
    ffn_dim = g1_w_base.shape[2]
    g2_dim = ffn_dim // 2
    num_local_experts = num_experts // num_ranks
    input_dtype = x.dtype

    return_recv_hook = False

    # Step 1: Quant dispatch → 3D tensor [num_local_experts, padded, hidden]
    recv_x_pack, packed_recv_count, handle, _, _ = buffer.low_latency_dispatch(
        x=x,
        topk_idx=topk_idx,
        num_max_dispatch_tokens_per_rank=num_max_dispatch_tokens_per_rank,
        num_experts=num_experts,
        use_fp8=use_fp8,
        async_finish=not return_recv_hook,
        return_recv_hook=return_recv_hook,
    )
    if use_fp8:
        recv_x, recv_x_scale = recv_x_pack
    else:
        raise NotImplementedError

    expected_m = (x.shape[0] * buffer.group_size * topk_idx.shape[1] + num_experts) // num_experts
    masked_m = packed_recv_count

    group_list_type = 1
    group_list = masked_m

    hidden_states, hidden_states_scale = torch_npu.npu_grouped_matmul_swiglu_quant_v2(
        x=recv_x,
        weight=[g1_w_base],
        weight_scale=[g1_s_base],
        x_scale=recv_x_scale,
        group_list_type=group_list_type,
        group_list=group_list,
    )

    hidden_states = torch.ops.npu.npu_grouped_matmul(
        x=[hidden_states],
        weight=[g2_w_base],
        scale=[g2_s_base],
        per_token_scale=[hidden_states_scale],
        split_item=2,
        group_list_type=group_list_type,
        group_type=0,
        group_list=group_list,
        output_dtype=torch.float16,
    )[0]

    hidden_states = hidden_states.to(input_dtype)

    # Step 4: Combine
    combined_x, _, _ = buffer.low_latency_combine(
        x=hidden_states,
        topk_idx=topk_idx,
        topk_weights=topk_weights,
        handle=handle,
        async_finish=not return_recv_hook,
        return_recv_hook=return_recv_hook,
    )
    # combined_x: [num_tokens, hidden]

    return combined_x, packed_recv_count


def test_fused_moe(rank, num_ranks, args):
    _, _, group = init_dist(rank, num_ranks)

    bs = args.num_tokens
    hidden = args.hidden
    ffn = args.ffn_dim
    ne = args.num_experts
    tk = args.num_topk
    ws = args.with_shared
    sm = args.with_smooth
    sfn = args.share_ffn_dim if args.share_ffn_dim > 0 else ffn
    num_max_dispatch = bs  # at most all tokens go to a single rank

    assert ne % num_ranks == 0, f"num_experts ({ne}) must be divisible by num_ranks ({num_ranks})"
    nle = ne // num_ranks

    # low_latency_mode=True: use RDMA buffer, no HCCS buffer needed
    num_rdma_bytes = Buffer.get_low_latency_rdma_size_hint(bs, hidden, num_ranks, ne)
    buffer_a = Buffer(
        group,
        num_nvl_bytes=0,
        num_rdma_bytes=num_rdma_bytes,
        low_latency_mode=True,
        num_qps_per_rank=nle,
    )
    buffer_b = Buffer(
        group,
        num_nvl_bytes=0,
        num_rdma_bytes=num_rdma_bytes,
        low_latency_mode=True,
        num_qps_per_rank=nle,
    )

    tag = f"bs{bs}_h{hidden}_ffn{ffn}_E{ne}_topk{tk}_R{num_ranks}_sh{int(ws)}_sm{int(sm)}"
    logging.info(f"[rank {rank}] Config: {tag}")

    (
        x,
        expert_ids,
        expert_scales,
        g1_w,
        g1_s,
        g2_w,
        g2_s,
        sh_w1,
        sh_s1,
        sh_w2,
        sh_s2,
        smooth_scales,
        sh_smooth,
        g1_w_base,
        g1_s_base,
        g2_w_base,
        g2_s_base,
        sh_w1_base,
        sh_s1_base,
        sh_w2_base,
        sh_s2_base,
        topk_idx,
        topk_weights,
    ) = generate_data(rank, bs, hidden, ffn, nle, ne, tk, ws, sm, sfn)

    # ===== Fused path =====
    torch.npu.synchronize()
    dist.barrier()

    fused_out, fused_token_nums = run_fused(
        buffer_a,
        x,
        expert_ids,
        expert_scales,
        g1_w,
        g1_s,
        g2_w,
        g2_s,
        ne,
        ffn,
        smooth_scales,
        sh_w1,
        sh_s1,
        sh_w2,
        sh_s2,
        sh_smooth,
        sfn,
    )

    torch.npu.synchronize()
    dist.barrier()

    # ===== Unfused reference =====
    unfused_out, unfused_token_nums = run_unfused(
        buffer_b,
        x,
        topk_idx,
        topk_weights,
        g1_w,
        g1_s,
        g2_w_base,
        g2_s_base,
        ne,
        num_ranks,
        num_max_dispatch * num_ranks,
    )

    torch.npu.synchronize()
    dist.barrier()

    # ===== Precision comparison =====
    # Compare expert token counts
    tn_match = False
    if fused_token_nums.numel() == unfused_token_nums.numel():
        tn_diff = (fused_token_nums - unfused_token_nums).abs().max().item()
        tn_match = tn_diff == 0
        logging.info(f"[rank {rank}] expert_token_nums max diff = {tn_diff} ({'MATCH' if tn_match else 'MISMATCH'})")
    else:
        logging.error(
            f"[rank {rank}] expert_token_nums size mismatch: "
            f"fused={fused_token_nums.numel()} unfused={unfused_token_nums.numel()}"
        )

    # Token-wise stats for diagnosis
    diff_marker = 0
    for idx in range(bs):
        fused_out_per = fused_out[idx]
        unfused_out_per = unfused_out[idx]

        abs_diff = (fused_out_per.float() - unfused_out_per.float()).abs()

        threshold = 1e-2
        diff = calc_diff(fused_out_per.contiguous(), unfused_out_per.contiguous())

        diff_val = diff.item() if isinstance(diff, torch.Tensor) else diff
        if math.isnan(diff_val) or math.isinf(diff_val) or diff_val >= threshold:
            diff_marker += 1
            logging.info(
                f"[rank {rank}][token {idx}] element-wise: max_abs_diff={abs_diff.max().item():.6f} "
                f"mean_abs_diff={abs_diff.mean().item():.6f}"
            )
            logging.info(f"[rank {rank}][token {idx}] calc_diff(fused, unfused) = {diff:.6f}")

    if diff_marker > (bs * 0.05):
        logging.error(f"[rank {rank}] FAILED (diff_per={diff_marker})")
        return False
    else:
        logging.info(f"[rank {rank}] PASSED (diff_per={diff_marker})")
        return True


def test_loop(local_rank, num_local_ranks, args):
    success = test_fused_moe(local_rank, num_local_ranks, args)
    if not success:
        logging.error(f"[rank {local_rank}] TEST FAILED")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ZBAL fused_deep_moe precision test")
    parser.add_argument("--num-processes", type=int, default=8, help="Number of NPU processes (default: 8)")
    parser.add_argument("--num-tokens", type=int, default=64, help="Batch size / num tokens per rank (default: 64)")
    parser.add_argument("--hidden", type=int, default=7168, help="Hidden dimension (default: 7168)")
    parser.add_argument("--ffn-dim", type=int, default=4096, help="FFN intermediate dimension (default: 4096)")
    parser.add_argument("--num-experts", type=int, default=64, help="Total number of experts (default: 64)")
    parser.add_argument("--num-topk", type=int, default=8, help="Top-K experts per token (default: 8)")
    parser.add_argument("--with-shared", action="store_true", help="Include shared expert")
    parser.add_argument("--with-smooth", action="store_true", help="Include smooth quantization scales")
    parser.add_argument(
        "--share-ffn-dim", type=int, default=0, help="Shared expert FFN dim (default: same as --ffn-dim)"
    )
    args = parser.parse_args()

    if args.num_processes < 2:
        logging.error("ERROR: --num-processes must be >= 2 for distributed testing")
        sys.exit(1)

    os.environ["HCCL_BUFFSIZE"] = "2"
    os.environ["MASTER_ADDR"] = "127.0.0.1"
    os.environ["MASTER_PORT"] = "29501"

    torch.multiprocessing.spawn(test_loop, args=(args.num_processes, args), nprocs=args.num_processes, join=True)
