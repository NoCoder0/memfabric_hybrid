#!/usr/bin/env python3
"""
Standalone performance test for fused_deep_moe (zbal backend).

Mirrors UMDK's fused_deep_moe_sample.py approach:
  - One config per run, no accuracy check
  - Loop kernel N times for warmup + measurement
  - Use msprof externally to capture kernel timing:
      msprof --output=profs python perf_fused_deep_moe.py ...

Usage:
  python perf_fused_deep_moe.py
  python perf_fused_deep_moe.py --num-processes 16 --num-experts 64 --with-shared --with-smooth
  msprof --output=profs python perf_fused_deep_moe.py --num-processes 16 ...
"""

import argparse
import os
import sys
from pathlib import Path
import logging

import torch
import torch.distributed as dist
import torch_npu
import zbal
from zbal import Buffer

torch_npu.npu.config.allow_internal_format = True

from utils import init_dist

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")


def generate_data(
    rank, bs, hidden, ffn_dim, num_local_experts, num_experts, num_topk, with_shared, with_smooth, share_ffn_dim
):
    torch.manual_seed(42 + rank)

    x = (torch.rand((bs, hidden), device="npu") * 10 - 5).to(torch.bfloat16)

    expert_ids = torch.arange(
        rank * bs * num_topk, rank * bs * num_topk + bs * num_topk, dtype=torch.int32, device="npu"
    ).reshape(bs, num_topk)
    expert_ids = expert_ids % num_experts

    expert_scales = torch.rand((bs, num_topk), dtype=torch.float32, device="npu")

    g2_dim = ffn_dim // 2
    # Base weights (pre-NZ-cast, saved for cross-framework comparison)
    g1_w_base = torch.randint(-16, 16, (num_local_experts, hidden, ffn_dim), dtype=torch.int8, device="npu")
    g1_s_base = torch.rand((num_local_experts, ffn_dim), dtype=torch.float32, device="npu") * 0.003 + 0.0015
    g2_w_base = torch.randint(-16, 16, (num_local_experts, g2_dim, hidden), dtype=torch.int8, device="npu")
    g2_s_base = torch.rand((num_local_experts, hidden), dtype=torch.float32, device="npu") * 0.003 + 0.0015
    # NZ format for kernel
    g1_w = torch_npu.npu_format_cast(g1_w_base, 29)
    g1_s = g1_s_base.contiguous()
    g2_w = torch_npu.npu_format_cast(g2_w_base, 29)
    g2_s = g2_s_base.to(torch.bfloat16).contiguous()

    sh_w1_base = sh_s1_base = sh_w2_base = sh_s2_base = None
    sh_w1 = sh_s1 = sh_w2 = sh_s2 = None
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
    )


def run_perf(rank, num_ranks, args):
    redirect = getattr(args, 'redirect', False)
    if redirect:
        d = Path("perf_logs")
        d.mkdir(exist_ok=True)
        fd = os.open(str(d / f"rank{rank:02d}.log"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        os.dup2(fd, 1)
        os.dup2(fd, 2)
        os.close(fd)

    _, _, group = init_dist(rank, num_ranks)
    buffer = Buffer(group, num_nvl_bytes=0, num_rdma_bytes=0, low_latency_mode=False)

    bs = args.num_tokens
    hidden = args.hidden
    ffn = args.ffn_dim
    ne = args.num_experts
    tk = args.num_topk
    ws = args.with_shared
    sm = args.with_smooth
    sfn = args.share_ffn_dim if args.share_ffn_dim > 0 else ffn

    if ne % num_ranks != 0:
        raise ValueError("num of pe can not divided by num of ranks")
    nle = ne // num_ranks

    tag = f"bs{bs}_h{hidden}_ffn{ffn}_E{ne}_topk{tk}_R{num_ranks}_sh{int(ws)}_sm{int(sm)}"
    logging.info(f"[rank {rank}] {tag}")

    (
        x,
        eid,
        esc,
        g1w,
        g1s,
        g2w,
        g2s,
        sh_w1,
        sh_s1,
        sh_w2,
        sh_s2,
        smooth,
        sh_smooth,
        g1w_base,
        g1s_base,
        g2w_base,
        g2s_base,
        sh_w1_base,
        sh_s1_base,
        sh_w2_base,
        sh_s2_base,
    ) = generate_data(rank, bs, hidden, ffn, nle, ne, tk, ws, sm, sfn)

    # ============================================ Cross-framework save / load
    _to_cpu = lambda t: t.cpu() if t is not None else None
    ref_output_loaded = None

    if getattr(args, 'save_data_dir', None) is not None:
        rank_dir = os.path.join(args.save_data_dir, f"rank_{rank}")
        os.makedirs(rank_dir, exist_ok=True)
        save_inputs = {
            "x": _to_cpu(x),
            "expert_ids": _to_cpu(eid),
            "expert_scales": _to_cpu(esc),
            "x_active_mask": None,
            "gmm1_weight": _to_cpu(g1w_base),
            "gmm1_weight_scale": _to_cpu(g1s_base),
            "gmm2_weight": _to_cpu(g2w_base),
            "gmm2_weight_scale": _to_cpu(g2s_base),
            "smooth_scales": _to_cpu(smooth),
            "share_gmm1_weight": _to_cpu(sh_w1_base),
            "share_gmm1_weight_scale": _to_cpu(sh_s1_base),
            "share_gmm2_weight": _to_cpu(sh_w2_base),
            "share_gmm2_weight_scale": _to_cpu(sh_s2_base),
            "share_smooth_scales": _to_cpu(sh_smooth),
        }
        torch.save(save_inputs, os.path.join(rank_dir, "inputs.pt"))
        logging.info(f"[rank {rank}] Saved inputs to {rank_dir}")

    if getattr(args, 'load_data_dir', None) is not None:
        rank_dir = os.path.join(args.load_data_dir, f"rank_{rank}")
        inputs = torch.load(os.path.join(rank_dir, "inputs.pt"), map_location="cpu", weights_only=True)
        ref_saved = torch.load(os.path.join(rank_dir, "output.pt"), map_location="cpu", weights_only=True)
        ref_output_loaded = (ref_saved["token_output"], ref_saved["share_output"], ref_saved["expert_token_nums"])

        x = inputs["x"].to(device="npu", dtype=torch.bfloat16)
        eid = inputs["expert_ids"].to(device="npu")
        esc = inputs["expert_scales"].to(device="npu")
        g1w_base = inputs["gmm1_weight"].to(device="npu")
        g1s_base = inputs["gmm1_weight_scale"].to(device="npu")
        g2w_base = inputs["gmm2_weight"].to(device="npu")
        g2s_base = inputs["gmm2_weight_scale"].to(device="npu")
        smooth = inputs["smooth_scales"].to(device="npu") if inputs["smooth_scales"] is not None else None

        has_sh = inputs["share_gmm1_weight"] is not None
        if has_sh:
            sh_w1_base = inputs["share_gmm1_weight"].to(device="npu")
            sh_s1_base = inputs["share_gmm1_weight_scale"].to(device="npu")
            sh_w2_base = inputs["share_gmm2_weight"].to(device="npu")
            sh_s2_base = inputs["share_gmm2_weight_scale"].to(device="npu")
            sh_smooth = (
                inputs["share_smooth_scales"].to(device="npu") if inputs["share_smooth_scales"] is not None else None
            )

        # Re-apply NZ cast for loaded weights
        g1w = torch_npu.npu_format_cast(g1w_base, 29)
        g1s = g1s_base.contiguous()
        g2w = torch_npu.npu_format_cast(g2w_base, 29)
        g2s = g2s_base.to(torch.bfloat16).contiguous()
        sh_w1 = sh_s1 = sh_w2 = sh_s2 = None
        if has_sh:
            sh_w1 = torch_npu.npu_format_cast(sh_w1_base, 29)
            sh_w2 = torch_npu.npu_format_cast(sh_w2_base, 29)
            sh_s1 = sh_s1_base.contiguous()
            sh_s2 = sh_s2_base.to(torch.bfloat16).contiguous()
            ws = True
            sm = smooth is not None
            sfn = sh_w1_base.shape[1]
        else:
            ws = False

        bs = x.shape[0]
        nle = ne // num_ranks
        logging.info(f"[rank {rank}] Loaded data from {rank_dir}, bs={bs}")

    has_sh = ws
    sh1_h_len = sfn if has_sh else 0

    def run_kernel():
        return buffer.fused_deep_moe(
            x=x,
            topk_idx=eid,
            gmm1_permuted_weight=g1w,
            gmm1_permuted_weight_scale=g1s,
            gmm2_weight=g2w,
            gmm2_weight_scale=g2s,
            topk_weights=esc,
            num_experts=ne,
            quant_mode=0,
            num_max_dispatch_tokens_per_rank=0,
            is_tensor_list=False,
            expert_smooth_scales=smooth,
            share_gmm1_weight=sh_w1,
            share_gmm1_scale=sh_s1,
            share_gmm2_weight=sh_w2,
            share_gmm2_scale=sh_s2,
            share_smooth_scales=sh_smooth,
            share_gmm1_h_len=sh1_h_len,
        )

    # Ensure async ops done, then barrier
    torch.npu.synchronize()
    dist.barrier()

    nw = args.num_warmups
    ni = args.num_iters
    logging.info(f"[rank {rank}] warmup={nw} iters={ni}")

    # Warmup
    for _ in range(nw):
        run_kernel()
        torch.npu.synchronize()
    dist.barrier()

    # ============================================= Save output / cross-compare
    save_data_dir = getattr(args, 'save_data_dir', None)
    load_data_dir = getattr(args, 'load_data_dir', None)
    if save_data_dir is not None or ref_output_loaded is not None:
        hccl_out, hccl_counts = run_kernel()
        torch.npu.synchronize()

        if save_data_dir is not None:
            rank_dir = os.path.join(save_data_dir, f"rank_{rank}")
            save_outputs = {
                "token_output": hccl_out.cpu(),
                "expert_token_nums": hccl_counts.cpu(),
            }
            torch.save(save_outputs, os.path.join(rank_dir, "output.pt"))
            logging.info(f"[rank {rank}] Saved output to {rank_dir}")

        if ref_output_loaded is not None:
            from utils import calc_diff

            ref_token, _ref_share, ref_counts = ref_output_loaded
            ref_token = ref_token.to(device="npu", dtype=hccl_out.dtype)
            ref_counts = ref_counts.to(device="npu", dtype=hccl_counts.dtype)
            torch.npu.synchronize()
            diff = calc_diff(ref_token, hccl_out.contiguous())
            logging.info(f"[rank {rank}] Cross-compare token_output diff = {diff:.6f}")
            if diff < 1e-2:
                logging.info(f"[rank {rank}] token_output PASSED")
            else:
                logging.error(f"[rank {rank}] token_output WARNING: diff={diff:.6f} exceeds threshold")

            counts_diff = calc_diff(ref_counts, hccl_counts.contiguous())
            logging.info(f"[rank {rank}] Cross-compare expert_token_nums diff = {counts_diff:.6f}")
            logging.info(f"[rank {rank}] ref_counts: {ref_counts.tolist()}")
            logging.info(f"[rank {rank}] hccl_counts: {hccl_counts.tolist()}")
            if counts_diff < 1e-2:
                logging.info(f"[rank {rank}] expert_token_nums PASSED")
            else:
                logging.error(f"[rank {rank}] expert_token_nums FAILED: diff={counts_diff:.6f}")

    # Timing loop — msprof captures kernel times from NPU hardware
    # Also record wall-clock per iter via torch.npu.Event for P0-P95
    # times = []
    for i in range(ni):
        run_kernel()

    dist.barrier()


def test_loop(local_rank, num_local_ranks, args):
    run_perf(local_rank, num_local_ranks, args)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ZBAL fused_deep_moe perf (msprof)")
    parser.add_argument("--num-processes", type=int, default=8)
    parser.add_argument("--num-tokens", type=int, default=64)
    parser.add_argument("--hidden", type=int, default=7168)
    parser.add_argument("--ffn-dim", type=int, default=4096)
    parser.add_argument("--num-experts", type=int, default=64)
    parser.add_argument("--num-topk", type=int, default=8)
    parser.add_argument("--with-shared", action="store_true")
    parser.add_argument("--with-smooth", action="store_true")
    parser.add_argument("--share-ffn-dim", type=int, default=0)
    parser.add_argument("--num-warmups", type=int, default=50)
    parser.add_argument("--num-iters", type=int, default=100)
    parser.add_argument("--redirect", action="store_true")
    parser.add_argument(
        "--save-data-dir", type=str, default=None, help="Save inputs and output for cross-framework comparison"
    )
    parser.add_argument(
        "--load-data-dir", type=str, default=None, help="Load inputs and compare with saved reference output"
    )
    args = parser.parse_args()

    os.environ["HCCL_BUFFSIZE"] = "2"  # DEEP_FUSE: data window unused, state window needs ~1 MB
    os.environ["MASTER_ADDR"] = "127.0.0.1"
    os.environ["MASTER_PORT"] = "29501"

    torch.multiprocessing.spawn(test_loop, args=(args.num_processes, args), nprocs=args.num_processes, join=True)
