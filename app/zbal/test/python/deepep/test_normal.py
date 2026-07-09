import argparse
import math
import os
import sys
import logging
from pathlib import Path
import random
from functools import partial
from typing import Optional

import zbal
from zbal import Buffer, Config, zbal_uninit
import numpy as np
import torch
import torch_npu
import torch.distributed as dist

from utils import (
    bench,
    bench_kineto,
    calc_diff,
    calculate_avg_stats,
    init_dist,
    inplace_unique,
    per_token_cast_back,
)

logger = logging.getLogger(__name__)


def redirect_io(rank, log_dir="./logs"):
    Path(log_dir).mkdir(parents=True, exist_ok=True)
    pid = os.getpid()
    log_path = f"{log_dir}/rank{rank:02d}_pid{pid}.log"

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        handlers=[
            logging.FileHandler(log_path, mode="w"),
            logging.StreamHandler(),
        ],
        force=True,
    )
    logging.info(f"[rank {rank}] logging to {log_path}")


# noinspection PyShadowingNames
def test_main(
    args: argparse.Namespace,
    num_local_ranks: int,
    local_rank: int,
    num_ranks: int,
    rank: int,
    buffer: zbal.Buffer,
    group: dist.ProcessGroup,
):
    # Settings
    num_tokens, hidden = args.num_tokens, args.hidden
    num_topk, num_experts = args.num_topk, args.num_experts
    num_servers = num_ranks // num_local_ranks
    expert_token_nums_type = int(os.getenv("MOE_EXPERT_TOKEN_NUMS_TYPE", 1))
    use_quant = os.getenv("DEEP_NORMAL_MODE_USE_INT8_QUANT") == "1"

    multi_list = [1] * num_ranks
    num_tokens = int(num_tokens * multi_list[rank])
    if num_tokens == 0:
        num_tokens = 1

    assert num_experts % num_ranks == 0
    logger.info(
        f"[rank {rank}] [config] num_tokens={num_tokens}, hidden={hidden}, num_topk={num_topk}, "
        f"num_experts={num_experts}, num_ranks={num_ranks}, multi_list={multi_list}"
    )

    experts_per_rank = num_experts // num_ranks
    # Default: random over all experts (original behavior)
    scores = torch.randn((num_tokens, num_experts), dtype=torch.float32, device="npu").abs() + 1
    # topk_idx = [[0, 1, 2, 3, 4, 5, 6, 7], [8, 9, 10, 11, 12, 13, 14, 15], ...]
    topk_idx = torch.zeros((num_tokens, num_topk), dtype=torch.int64, device='npu')
    for t in range(num_tokens):
        start = (t * num_topk) % num_experts
        for k in range(num_topk):
            topk_idx[t, k] = (start + k) % num_experts

    rank_idx = topk_idx // experts_per_rank
    rank_idx.masked_fill_(topk_idx == -1, -1)
    inplace_unique(rank_idx, num_ranks)

    # Expert meta
    num_tokens_per_expert = torch.zeros((num_experts,), dtype=torch.int, device="npu")
    for i in range(num_experts):
        num_tokens_per_expert[i] = (topk_idx == i).sum()
    gbl_num_tokens_per_expert = num_tokens_per_expert.clone()
    torch.npu.synchronize()

    # for all_reduce data sync
    dist.all_reduce(gbl_num_tokens_per_expert, group=group)
    gbl_num_tokens_per_expert = gbl_num_tokens_per_expert.cpu()

    # Rank layout meta
    num_tokens_per_rank = torch.empty((num_ranks,), dtype=torch.int, device="npu")
    token_idx_in_rank = torch.full((num_ranks, num_tokens), -1, dtype=torch.long, device="npu")
    for i in range(num_ranks):
        num_tokens_per_rank[i] = (rank_idx == i).sum()
        token_sel = (rank_idx == i).max(dim=-1)[0]
        count = token_sel.sum().item()
        tokens = torch.sort(token_sel.to(torch.int), descending=True)[1]
        tokens[:count] = torch.sort(tokens[:count])[0]
        token_idx_in_rank[i][tokens[:count]] = torch.arange(count, dtype=torch.long, device="npu")
    token_idx_in_rank = token_idx_in_rank.T.contiguous().to(torch.int)
    is_token_in_rank = (token_idx_in_rank >= 0).to(torch.int)
    gbl_num_tokens_per_rank = num_tokens_per_rank.clone()
    dist.all_reduce(gbl_num_tokens_per_rank, group=group)

    return_values = buffer.get_dispatch_layout(topk_idx, num_experts)
    (
        ref_num_tokens_per_rank,  # 1-dim, [token/rank, token/rank, ...]
        _,
        ref_num_tokens_per_expert,  # 1-dim, [token*topk/expert, token*topk/expert, ...]
        ref_is_token_in_rank,  # 2-dim, shape=(num_tokens, num_ranks), 1 if token in rank else 0
        _,
    ) = return_values
    send_token_idx = buffer.get_send_token_idx().clone().cpu()
    topk_idx_cpu = topk_idx.cpu()
    try:
        assert torch.allclose(ref_num_tokens_per_rank, num_tokens_per_rank), (
            f"Assertion num_tokens_per_rank failed on rank {rank}: "
            f"Expected {num_tokens_per_rank}, Actual {ref_num_tokens_per_rank}"
        )
        assert torch.allclose(ref_num_tokens_per_expert, num_tokens_per_expert), (
            f"Assertion num_tokens_per_expert failed on rank {rank}: "
            f"Expected {num_tokens_per_expert}, Actual {ref_num_tokens_per_expert}"
        )
        logger.info(f"[rank {rank}] [layout] Test passed.")
    except AssertionError as e:
        logger.error("[rank %d] happen error: %s", rank, e)
        raise

    # Config
    buffer_size = 256
    config = Config(24, 8, buffer_size)

    # Random data
    x = torch.ones((num_tokens, hidden), dtype=torch.bfloat16, device="npu") * rank
    x_pure_rand = torch.randn((num_tokens, hidden), dtype=torch.bfloat16, device="npu")
    topk_weights = torch.ones((num_tokens, num_topk), dtype=torch.float32, device="npu") * rank
    topk_weights_pure_rand = torch.randn((num_tokens, num_topk), dtype=torch.float32, device="npu")

    x_cpu = x.cpu()
    x_pure_rand_cpu = x_pure_rand.cpu()

    def get_num_tokens_per_expert_list(rank: int):
        local_expert_token = gbl_num_tokens_per_expert.view(num_ranks, -1)[rank]
        if expert_token_nums_type == 0:
            # 计算前缀和并转为 list
            local_expert_token_list = local_expert_token.cumsum(dim=0).tolist()
        else:
            local_expert_token_list = local_expert_token.tolist()
        return local_expert_token_list

    def verify_combine_output(check_x):
        """Verify combined_x correctness for deterministic input (x = ones * rank, weights = ones * rank).
        Expected: every token row = 8 * rank^2  (uniform across hidden dims).
        Returns True on success, False on failure after logging diagnostics."""
        cx_float = check_x.float()
        expected_val = float(num_topk) * float(rank) * float(rank)

        # 1. Row uniformity: all hidden dims in a row must be identical
        row_diffs = (cx_float[:, 1:] - cx_float[:, :1]).abs().max(dim=1)[0]
        max_col_diff, bad_idx = row_diffs.max().item(), row_diffs.argmax().item()
        non_uniform = (row_diffs > 5e-5).sum().item()

        # 2. First-column values vs expected
        vals = cx_float[:, 0]
        val_errs = (vals - expected_val).abs()
        max_val_err, worst_row = val_errs.max().item(), val_errs.argmax().item()
        wrong_rows = (val_errs > 5e-5).sum().item()

        ok = non_uniform == 0 and wrong_rows == 0
        if not ok:
            logger.error(
                f"[rank {rank}] combine: expected_val={expected_val:.1f}, "
                f"non_uniform_rows={non_uniform}, wrong_value_rows={wrong_rows}, "
                f"max_col_diff={max_col_diff:.4f} @ row{bad_idx}, "
                f"max_val_err={max_val_err:.4f} @ row{worst_row}(val={vals[worst_row]:.4f})"
            )
            for r in range(check_x.shape[0]):
                logger.warning(f"[rank {rank}] row{r} [:32] = {check_x[r, :32].tolist()}")
            wrong_mask = val_errs > 5e-5
            wrong_indices = wrong_mask.nonzero(as_tuple=True)[0]
            for wi in wrong_indices:
                logger.error(
                    f"[rank {rank}] row{wi.item()} val={vals[wi].item():.4f} all_hidden = {cx_float[wi].tolist()}"
                )
        return ok

    def dump_send_token_idx(send_token_idx):
        sti_cpu = send_token_idx.cpu()
        lines = []
        for t in range(sti_cpu.shape[0]):
            lines.append(f"[rank {rank}] send_token_idx row{t} = {sti_cpu[t].tolist()}")
        logger.warning("\n".join(lines))

    def dump_dispatch_output(recv_x, put_offset, balance_matrix):
        dump_send_token_idx(send_token_idx)

        # dump recv_x
        lines = []
        for r in range(recv_x.shape[0]):
            lines.append(f"[rank {rank}] recv_x row{r} [:32] = {recv_x[r, :32].tolist()}")
        logger.warning("\n".join(lines))

        logger.warning(f"[rank {rank}] put_offset = {put_offset.tolist()}")
        logger.warning(f"[rank {rank}] balance_matrix = {balance_matrix.tolist()}")

    def verify_send_token_idx(send_token_idx):
        """Verify send_token_idx: position of each (token, topk) within its expert's recv_x segment.

        Shape [num_tokens, num_topk], int32, computed by dispatch_normal_layout kernel.
        send_token_idx[t, k] = number of prior (t', k') pairs that route to the same
        expert as topk_idx[t, k]. Verified by simulating the sequential counter logic
        that the kernel uses in its per-block Phase 3.
        """
        sti_cpu = send_token_idx.cpu()
        topk_cpu = topk_idx_cpu
        expert_counter = [0] * num_experts
        for t in range(num_tokens):
            for k in range(num_topk):
                expert_id = int(topk_cpu[t, k].item())
                expected = expert_counter[expert_id]
                expert_counter[expert_id] += 1
                actual = int(sti_cpu[t, k].item())
                if expected != actual:
                    logger.error(f"[rank {rank}] send_token_idx[{t},{k}] expected={expected} actual={actual}")
                    dump_send_token_idx(send_token_idx)
                    return False
        logger.info(f"[rank {rank}] send_token_idx check passed, expert_counter[:8]={expert_counter[:8]}")
        return True

    def verify_recv_x(recv_x, put_offset, balance_matrix):
        """Verify dispatch output recv_x for deterministic input x = ones * rank.

        Checks (in order):
        1. Token count matches expected sum for this rank's local experts.
        2. Every row is uniform across hidden dims (x is broadcast, no row should vary).
        3. Block-based source rank pattern: entries_per_exp_src consecutive rows per
           (expert, src_rank) pair, alternating src_rank 0,1,0,1,...

        entries_per_exp_src = num_tokens * num_topk / num_ranks / experts_per_rank.
        For the default small test (64 tokens, 8 topk, 2 ranks, 256 experts):
        entries_per_exp_src = 64 * 8 / 2 / 128 = 2 rows per (expert, src_rank).
        """
        # 1. token count
        local_expert_sum_expected = gbl_num_tokens_per_expert.view(num_ranks, -1)[rank].sum().item()
        if recv_x.shape[0] != local_expert_sum_expected:
            logger.error(f"[rank {rank}] recv_x count {recv_x.shape[0]} != expected {local_expert_sum_expected}")
            dump_dispatch_output(recv_x, put_offset, balance_matrix)
            return False

        rx_float = recv_x.float()
        # 2. row uniformity: all hidden dims in a row must be identical
        row_diffs = (rx_float[:, 1:] - rx_float[:, :1]).abs().max(dim=1)[0]
        max_col_diff = row_diffs.max().item()
        if max_col_diff >= 5e-5:
            first_bad = (row_diffs > 5e-5).nonzero(as_tuple=True)[0]
            first_bad = first_bad[0].item() if len(first_bad) > 0 else row_diffs.argmax().item()
            logger.error(f"[rank {rank}] recv_x hidden first bad row {first_bad}: {rx_float[first_bad].tolist()}")
            dump_dispatch_output(recv_x, put_offset, balance_matrix)
            return False

        # 3. block-based source rank check
        entries_per_exp_src = num_tokens * num_topk // num_ranks // experts_per_rank
        vals = rx_float[:, 0]
        for i in range(0, vals.shape[0], entries_per_exp_src):
            expected = float((i // entries_per_exp_src) % num_ranks)
            block = vals[i : i + entries_per_exp_src]
            if (block - expected).abs().max().item() >= 5e-5:
                end = min(i + entries_per_exp_src, vals.shape[0])
                logger.error(f"[rank {rank}] recv_x rows [{i}:{end}] expected all ≈ {expected}, got {block.tolist()}")
                dump_dispatch_output(recv_x, put_offset, balance_matrix)
                return False
        return True

    def verify_put_offset(recv_x, put_offset, balance_matrix):
        """Verify put_offset: per-expert cumsum prefix for determining recv_x write positions.

        Shape [num_ranks, num_experts], int32, written by dispatch_normal_notify.
        Layout: flattened, each segment of size (experts_per_rank * num_ranks) is
        [0, step, 2*step, ..., (N-1)*step] repeated for each rank,
        where step = entries_per_exp_src.
        """
        entries_per_exp_src = num_tokens * num_topk // num_ranks // experts_per_rank
        po = put_offset.flatten().cpu()
        step = entries_per_exp_src
        seg_len = experts_per_rank * num_ranks
        for seg in range(num_ranks):
            start = seg * seg_len
            expected_seg = torch.arange(0, seg_len * step, step, dtype=torch.int32, device='cpu')
            if not torch.equal(po[start : start + seg_len], expected_seg):
                logger.error(f"[rank {rank}] put_offset segment {seg} mismatch")
                dump_dispatch_output(recv_x, put_offset, balance_matrix)
                return False
        return True

    def verify_balance_matrix(recv_x, put_offset, balance_matrix):
        """Verify balance_matrix: token range each rank is responsible for combining.

        Shape [num_ranks, num_ranks * 2], int32, written by dispatch_normal_notify.
        Identity pattern for non-balance mode:
        - Diagonal entries (col == seg): [0, num_tokens - 1] — rank handles all its own tokens.
        - Off-diagonal: [-1, -1] — no cross-rank combining.
        """
        bm = balance_matrix.flatten().cpu()
        stride = num_ranks * 2
        for seg in range(num_ranks):
            for col in range(num_ranks):
                v_start = bm[seg * stride + col * 2].item()
                v_end = bm[seg * stride + col * 2 + 1].item()
                if col == seg:
                    if v_start != 0 or v_end != num_tokens - 1:
                        logger.error(f"[rank {rank}] balance_matrix row{seg} col{col} mismatch")
                        dump_dispatch_output(recv_x, put_offset, balance_matrix)
                        return False
                else:
                    if v_start != -1 or v_end != -1:
                        logger.error(f"[rank {rank}] balance_matrix row{seg} col{col} mismatch")
                        dump_dispatch_output(recv_x, put_offset, balance_matrix)
                        return False
        return True

    def test_correctness():
        for current_x in filter(lambda elem: elem is not None, (x, x_pure_rand)):
            if local_rank == 0:
                tp = "INT8" if use_quant else "BF16"
                logger.info(f"[rank {rank}] [testing] Running with {tp}, with top-k {num_topk} ...")
            logger.info(f"[rank {rank}] begin running dispatch")
            # Test dispatch
            dispatch_args = {
                "x": current_x,
                "num_tokens_per_rank": ref_num_tokens_per_rank,
                "is_token_in_rank": ref_is_token_in_rank,
                "num_tokens_per_expert": ref_num_tokens_per_expert,
                "config": config,
                "topk_idx": topk_idx,
                "topk_weights": (topk_weights_pure_rand if current_x is x_pure_rand else topk_weights),
            }

            (
                recv_x,
                recv_topk_idx,
                recv_topk_weights,
                recv_num_tokens_per_expert_list,
                handle,
                event,
            ) = buffer.dispatch(**dispatch_args)
            if isinstance(recv_x, tuple):
                quant_stream = torch.npu.Stream()
                with torch.npu.Stream(quant_stream):
                    recv_x_int8_cpu = recv_x[0].cpu()
                    recv_x_scales_cpu = recv_x[1].cpu()
                    put_offset_cpu = handle[3].cpu()
                    balance_matrix_cpu = handle[4].cpu()
                    recv_topk_weights_cpu = handle[2].cpu()
                quant_stream.synchronize()
                recv_x_cpu = per_token_cast_back(recv_x_int8_cpu, recv_x_scales_cpu)
                recv_x = recv_x_cpu.to("npu")
            else:
                recv_x_cpu = recv_x.cpu()
                put_offset_cpu = handle[3].cpu()
                balance_matrix_cpu = handle[4].cpu()
                recv_topk_weights_cpu = handle[2].cpu()

            # Checks notify output
            local_expert_token_list = get_num_tokens_per_expert_list(rank)
            assert local_expert_token_list == recv_num_tokens_per_expert_list

            # Verify recv_x token count and content (deterministic input: x = ones * rank)
            if current_x is x:
                assert verify_send_token_idx(send_token_idx)
                assert verify_recv_x(recv_x_cpu, put_offset_cpu, balance_matrix_cpu)
                assert verify_put_offset(recv_x_cpu, put_offset_cpu, balance_matrix_cpu)
                assert verify_balance_matrix(recv_x_cpu, put_offset_cpu, balance_matrix_cpu)
            logger.info(f"[rank {rank}] [dispatch] Test passed.")

            # Test combine
            combine_args = {
                "x": recv_x,
                "handle": handle,
                "config": config,
                "async_finish": False,
                "topk_weights": handle[2],
            }
            combined_x, combined_topk_weights, event = buffer.combine(**combine_args)

            check_x = combined_x.cpu().float()
            ref_x = x_pure_rand_cpu if current_x is x_pure_rand else x_cpu
            ref_x_compute = ref_x.float() * recv_topk_weights_cpu.masked_fill(topk_idx_cpu == -1, 0).sum(dim=1).view(
                -1, 1
            )
            diff = calc_diff(check_x, ref_x_compute)
            if diff > 5e-5 or math.isnan(diff):
                if current_x is x:
                    assert verify_combine_output(check_x), f"[rank {rank}] combine verification failed"
                else:
                    assert False, f"[rank {rank}] combine diff={diff}"

            logger.info(f"[rank {rank}] [Combine] Test passed")

    def test_tuning():
        config = Config(24, 8, buffer_size)

        t = bench(lambda: buffer.get_dispatch_layout(topk_idx, num_experts))[0]
        logger.info(f"[rank {rank}] [layout] Kernel performance: {t * 1000:.3f} ms")

        current_x = x
        local_expert_token_list = get_num_tokens_per_expert_list(rank)
        real_recv_tokens = sum(local_expert_token_list)
        dispatch_bf16_recv_bytes = real_recv_tokens * hidden * 2
        combine_bf16_send_bytes = dispatch_bf16_recv_bytes

        # tuning dispatch
        recv_bytes = (dispatch_bf16_recv_bytes / 2) if use_quant else dispatch_bf16_recv_bytes
        tune_dispatch_args = {
            "x": current_x,
            "config": config,
            "num_tokens_per_rank": ref_num_tokens_per_rank,
            "is_token_in_rank": ref_is_token_in_rank,
            "num_tokens_per_expert": ref_num_tokens_per_expert,
            "topk_idx": topk_idx,
            "topk_weights": topk_weights,
        }
        dispatch_t = bench(lambda: buffer.dispatch(**tune_dispatch_args))[0]
        logger.info(
            f'[rank {rank}] [tuning] Dispatch ({"INT8" if use_quant else "BF16"}) '
            f'{recv_bytes / 1e9 / dispatch_t:.2f} GB/s (HCCS), '
            f'avg_t: {dispatch_t * 1e6:.2f} us'
        )

        dispatch_args = {
            "x": x,
            "config": config,
            "num_tokens_per_rank": ref_num_tokens_per_rank,
            "is_token_in_rank": ref_is_token_in_rank,
            "num_tokens_per_expert": ref_num_tokens_per_expert,
            "topk_idx": topk_idx,
            "topk_weights": topk_weights,
        }
        recv_x, _, _, _, handle, _ = buffer.dispatch(**dispatch_args)
        if isinstance(recv_x, tuple):
            quant_stream = torch.npu.Stream()
            with torch.npu.Stream(quant_stream):
                recv_x_int8_cpu = recv_x[0].cpu()
                recv_x_scales_cpu = recv_x[1].cpu()
            quant_stream.synchronize()
            recv_x = per_token_cast_back(recv_x_int8_cpu, recv_x_scales_cpu).to("npu")
        else:
            recv_x = recv_x
        # Tune combine performance
        tune_combine_args = {
            "x": recv_x,
            "handle": handle,
            "config": config,
            "async_finish": False,
            "topk_weights": handle[2],
        }
        combine_t = bench(lambda: buffer.combine(**tune_combine_args))[0]
        logger.info(
            f"[rank {rank}] [tuning] Combine {combine_bf16_send_bytes / 1e9 / combine_t:.2f} GB/s (HCCS), "
            f"avg_t: {combine_t * 1e6:.2f} us"
        )

        calculate_avg_stats(
            dispatch_t=dispatch_t,
            num_dispatch_comm_bytes=recv_bytes,
            combine_t=combine_t,
            num_combine_comm_bytes=combine_bf16_send_bytes,
            rank=rank,
            num_ranks=num_ranks,
            root_rank=0,
        )

    test_correctness()
    test_tuning()


# noinspection PyUnboundLocalVariable,PyShadowingNames
def test_loop(local_rank: int, num_local_ranks: int, args: argparse.Namespace):
    redirect_io(local_rank, "./logs")
    rank, num_ranks, group = init_dist(local_rank, num_local_ranks)
    logger.info(f"[rank {rank}] [group] group.rank={group.rank()}, group.size={group.size()}")

    logger.info(f"[rank {rank}] Initializing buffer...")
    buffer = zbal.Buffer(
        group,
        int(2e9),
        0,
        low_latency_mode=False,
    )
    logger.info(f"[rank {rank}] Buffer created OK.")
    torch.manual_seed(rank)

    test_main(args, num_local_ranks, local_rank, num_ranks, rank, buffer, group)

    dist.barrier()
    del buffer  # destroy MoE group before world group
    torch.npu.synchronize()
    dist.destroy_process_group()
    if not zbal_uninit():
        logger.error("zbal_uninit failed")


def print_help():
    """Print parameter usage and defaults, then exit."""
    print("=" * 60)
    print("test_normal.py — Test intranode EP kernels (normal mode)")
    print("=" * 60)
    print()
    params = [
        ("-h, --help", "Show this help message and exit", None),
        ("--num-processes", "Number of processes to spawn", 16),
        ("--num-tokens", "Number of tokens", 1024),
        ("--hidden", "Hidden dimension size", 7168),
        ("--num-topk", "Number of top-k experts", 8),
        ("--num-experts", "Number of experts", 256),
    ]
    for flags, help_text, default in params:
        print(f"  {flags}")
        print(f"    {help_text}")
        if default is not None:
            print(f"    Default: {default}")
        print()
    print("Example (all defaults):")
    print("  python test_normal.py --num-processes 16 --num-tokens 1024 --hidden 7168 --num-topk 8 --num-experts 256")


if __name__ == "__main__":
    # Handle -h/--help first, before argparse
    if len(sys.argv) >= 2 and sys.argv[1] in ("-h", "--help"):
        print_help()
        sys.exit(0)

    parser = argparse.ArgumentParser(description="Test intranode EP kernels (normal mode)")
    parser.add_argument(
        "--num-processes",
        type=int,
        default=16,
        help="Number of processes to spawn",
    )
    parser.add_argument(
        "--num-tokens",
        type=int,
        default=1024,
        help="Number of tokens",
    )
    parser.add_argument(
        "--hidden",
        type=int,
        default=7168,
        help="Hidden dimension size",
    )
    parser.add_argument(
        "--num-topk",
        type=int,
        default=8,
        help="Number of top-k experts",
    )
    parser.add_argument(
        "--num-experts",
        type=int,
        default=256,
        help="Number of experts",
    )
    args = parser.parse_args()

    num_processes = args.num_processes
    torch.multiprocessing.spawn(test_loop, args=(num_processes, args), nprocs=num_processes)
