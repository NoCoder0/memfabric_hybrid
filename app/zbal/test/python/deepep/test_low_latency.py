import argparse
import os
import logging
import random
import time
from functools import partial

import torch
import torch.distributed as dist
import torch_npu
import zbal
from zbal import Buffer, Config, zbal_uninit
from utils import (
    bench,
    bench_kineto,
    calc_diff,
    calculate_avg_stats,
    hash_tensor,
    init_dist,
    per_token_cast_back,
)


def test(
    num_tokens: int,
    hidden: int,
    num_experts: int,
    num_topk: int,
    rank: int,
    num_ranks: int,
    group: dist.ProcessGroup,
    buffer: Buffer,
    drop_percent: float,
    seed: int = 0,
):
    torch.manual_seed(seed + rank)
    random.seed(seed + rank)

    assert num_experts % num_ranks == 0
    num_local_experts = num_experts // num_ranks

    # NOTES: the integers greater than 256 exceeds the BF16 precision limit
    rank_offset = 0
    assert num_ranks - rank_offset < 257, "Too many ranks (exceeding test precision limit)"

    x = torch.ones((num_tokens, hidden), dtype=torch.bfloat16, device="npu") * (rank - rank_offset)
    scores = torch.randn((num_tokens, num_experts), dtype=torch.float32, device="npu").abs() + 1
    topk_idx = torch.topk(scores, num_topk, dim=-1, largest=True, sorted=True)[1]

    topk_idx = topk_idx.int()

    if drop_percent > 0:
        enable_neg_one = int(os.getenv("MOE_ENABLE_TOPK_NEG_ONE", 0))
        if enable_neg_one == 0:
            logging.error(
                "The kernel can't support drop_percent larger than 0 when "
                "MOE_ENABLE_TOPK_NEG_ONE was unset or 0. Please set to 1 and try again"
            )
            assert enable_neg_one == 1
        drop_mask = torch.rand_like(topk_idx, dtype=torch.float32) < drop_percent
        topk_idx = topk_idx.masked_fill(drop_mask, -1)
    topk_weights = torch.randn((num_tokens, num_topk), dtype=torch.float32, device="npu").abs()
    topk_weights = topk_weights / topk_weights.sum(dim=-1, keepdim=True)

    x_cpu = x.cpu()
    topk_idx_cpu = topk_idx.cpu()
    topk_weights_cpu = topk_weights.cpu()

    # Check dispatch correctness
    do_check = True
    return_recv_hook = False
    hash_value, num_times = 0, 0

    cumulative_local_expert_recv_stats = torch.zeros((num_local_experts,), dtype=torch.int, device="npu")

    dispatch_use_int8 = os.getenv("DEEP_LOW_LATENCY_MODE_USE_INT8_QUANT") == "1"

    logging.info(f"Rank {rank}: x shape={x.shape}, device={x.device}")
    logging.info(f"Rank {rank}: topk_idx shape={topk_idx.shape}, device={topk_idx.device}")
    logging.info(f"Rank {rank}: topk_idx.shape = {topk_idx.shape}")
    logging.info(f"Rank {rank}: quantization mode = {dispatch_use_int8=}")

    for i in range(100):
        packed_recv_x, packed_recv_count, handle, event, hook = buffer.low_latency_dispatch(
            x,
            topk_idx,
            num_tokens,
            num_experts,
            use_fp8=dispatch_use_int8,
            round_scale=False,
            use_ue8m0=False,
            cumulative_local_expert_recv_stats=cumulative_local_expert_recv_stats,
            async_finish=not return_recv_hook,
            return_recv_hook=return_recv_hook,
        )
        if dispatch_use_int8:
            quant_stream = torch.npu.Stream()
            with torch.npu.Stream(quant_stream):
                recv_x_int8_cpu = packed_recv_x[0].cpu()
                recv_x_scales_cpu = packed_recv_x[1].cpu()
            quant_stream.synchronize()
            simulated_gemm_x = per_token_cast_back(recv_x_int8_cpu, recv_x_scales_cpu).to("npu")
        else:
            simulated_gemm_x = packed_recv_x

        out = torch.empty((num_tokens, hidden), dtype=torch.bfloat16, device="npu")
        combined_x, event, hook = buffer.low_latency_combine(
            simulated_gemm_x,
            topk_idx,
            topk_weights,
            handle,
            async_finish=not return_recv_hook,
            zero_copy=False,
            return_recv_hook=return_recv_hook,
            out=out,
        )

    quant_stream = torch.npu.Stream()
    with torch.npu.Stream(quant_stream):
        combined_x_cpu = combined_x.cpu()
    quant_stream.synchronize()

    logging.info(f"Rank {rank}: combined_x shape={combined_x_cpu.shape}, device={combined_x_cpu[0].device}")
    logging.info(f"Rank {rank}: combined_x ={combined_x_cpu[:100, 0]}")

    if do_check:
        diff = calc_diff(
            x_cpu * topk_weights_cpu.masked_fill(topk_idx_cpu == -1, 0).sum(dim=1).view(-1, 1),
            combined_x_cpu,
        )
        assert torch.isnan(combined_x_cpu).sum().item() == 0
        if dispatch_use_int8:
            assert diff < 1e-4, f"Error: {diff=}"
        else:
            assert diff < 1e-5, f"Error: {diff=}"
        hash_value ^= hash_tensor(combined_x_cpu)

        logging.info(f"[rank {rank}] PASSED")

    logging.info(f"Calling Bench")

    out = torch.empty((num_tokens, hidden), dtype=torch.bfloat16, device="npu")

    def test_func(zero_copy: bool, return_recv_hook: bool):
        packed_recv_x, packed_recv_count, handle, event, hook = buffer.low_latency_dispatch(
            x,
            topk_idx,
            num_tokens,
            num_experts,
            use_fp8=dispatch_use_int8,
            round_scale=False,
            use_ue8m0=False,
            cumulative_local_expert_recv_stats=cumulative_local_expert_recv_stats,
            async_finish=not return_recv_hook,
            return_recv_hook=return_recv_hook,
        )
        combined_x, event, hook = buffer.low_latency_combine(
            simulated_gemm_x,
            topk_idx,
            topk_weights,
            handle,
            async_finish=not return_recv_hook,
            zero_copy=False,
            return_recv_hook=return_recv_hook,
            out=out,
        )

    # Calculate bandwidth
    num_int8_bytes, num_bf16_bytes = (hidden + hidden // 128 * 4 + 16), hidden * 2
    if dispatch_use_int8:
        num_dispatch_bytes_per_sel = num_int8_bytes
    else:
        num_dispatch_bytes_per_sel = num_bf16_bytes
    num_combine_bytes_per_sel = num_bf16_bytes
    num_dispatch_comm_bytes, num_combine_comm_bytes = 0, 0
    for i in range(num_tokens):
        num_selections = (topk_idx_cpu[i] != -1).sum().item()
        num_dispatch_comm_bytes += num_dispatch_bytes_per_sel * num_selections
        num_combine_comm_bytes += num_combine_bytes_per_sel * num_selections

    # Dispatch + combine testing
    avg_t, min_t, max_t = bench(partial(test_func, zero_copy=False, return_recv_hook=False))
    logging.info(
        f"[rank {rank}] Dispatch + combine bandwidth: "
        f"{(num_dispatch_comm_bytes + num_combine_comm_bytes) / 1e9 / avg_t:.2f} GB/s, "
        f"avg_t={avg_t * 1e6:.2f} us, min_t={min_t * 1e6:.2f} us, "
        f"max_t={max_t * 1e6:.2f} us"
    )

    # Separate profiling
    # return_recv_hook=True is not supported now
    for return_recv_hook in (False,):
        enable_neg_one = int(os.getenv("MOE_ENABLE_TOPK_NEG_ONE", 0))
        dist.barrier()

        dispatch_t, combine_t = bench_kineto(
            partial(test_func, zero_copy=False, return_recv_hook=return_recv_hook),
            kernel_names=(
                "dispatch_low_latency_",
                "combine_low_latency_",
            ),
            barrier_comm_profiling=True,
            suppress_kineto_output=True,
            num_kernels_per_period=2 if return_recv_hook else 1,
            trace_path=None,
        )

        if not return_recv_hook:
            logging.info(
                f"[rank {rank}] Dispatch bandwidth: "
                f"{num_dispatch_comm_bytes / 1e9 / dispatch_t:.2f} GB/s, "
                f"avg_t={dispatch_t * 1e6:.2f} us | "
                f"Combine bandwidth: {num_combine_comm_bytes / 1e9 / combine_t:.2f} GB/s, "
                f"avg_t={combine_t * 1e6:.2f} us"
            )
            calculate_avg_stats(
                dispatch_t=dispatch_t,
                num_dispatch_comm_bytes=num_dispatch_comm_bytes,
                combine_t=combine_t,
                num_combine_comm_bytes=num_combine_comm_bytes,
                rank=rank,
                num_ranks=num_ranks,
                root_rank=0,
            )

        else:
            logging.info(
                f"[rank {rank}] Dispatch send/recv time: "
                f"{dispatch_t[0] * 1e6:.2f} + {dispatch_t[1] * 1e6:.2f} us | "
                f"Combine send/recv time: {combine_t[0] * 1e6:.2f} + "
                f"{combine_t[1] * 1e6:.2f} us"
            )
    return hash_value


def test_loop(local_rank: int, num_local_ranks: int, args: argparse.Namespace):
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        force=True,
    )
    rank, num_ranks, group = init_dist(local_rank, num_local_ranks)
    shared_expert_rank_num = int(os.getenv("MOE_SHARED_EXPERT_RANK_NUM", 0))
    num_tokens, hidden = args.num_tokens, args.hidden
    num_topk, num_experts = args.num_topk, args.num_experts
    use_experts = num_experts if shared_expert_rank_num == 0 else (num_experts - 1)
    use_ranks = num_ranks - shared_expert_rank_num
    drop_percent = args.drop_percent
    num_rdma_bytes = Buffer.get_low_latency_rdma_size_hint(num_tokens, hidden, num_ranks, num_experts)
    buffer = Buffer(
        group,
        num_rdma_bytes=num_rdma_bytes,
        low_latency_mode=True,
        num_qps_per_rank=use_experts // use_ranks if use_ranks > 0 else 1,
    )

    test(
        num_tokens,
        hidden,
        use_experts,
        num_topk,
        rank,
        use_ranks,
        group,
        buffer,
        drop_percent,
        seed=1,
    )

    dist.barrier()
    del buffer
    torch.npu.synchronize()
    dist.destroy_process_group()
    if not zbal_uninit():
        logging.error("zbal_uninit failed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test intranode EP kernels")
    parser.add_argument(
        "--num-processes",
        type=int,
        default=16,
        help="Number of processes to spawn (default: 16)",
    )
    parser.add_argument(
        "--num-tokens",
        type=int,
        default=8,
        help="Number of tokens (default: 256)",
    )
    parser.add_argument(
        "--hidden",
        type=int,
        default=7168,
        help="Hidden dimension size (default: 7168)",
    )
    parser.add_argument(
        "--num-topk",
        type=int,
        default=8,
        help="Number of top-k experts (default: 8)",
    )
    parser.add_argument(
        "--num-experts",
        type=int,
        default=256,
        help="Number of experts (default: 256)",
    )
    parser.add_argument(
        "--pressure-test",
        action="store_true",
        help="Whether to do pressure test",
    )
    parser.add_argument(
        "--drop-percent",
        type=float,
        default=0.0,
        help="Percentage of dropping an individual top-k index (set to -1). ",
    )
    args = parser.parse_args()

    num_processes = args.num_processes
    torch.multiprocessing.spawn(test_loop, args=(num_processes, args), nprocs=num_processes)
