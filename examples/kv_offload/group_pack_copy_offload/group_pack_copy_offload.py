#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import argparse
import multiprocessing as mp
import sys
import numpy as np
import torch
import torch_npu
import memfabric_hybrid as mf
from memfabric_hybrid import offload


ONE_GIB = 1 << 30
WORLD_SIZE = 4
RANK_0, DEVICE_0 = 0, 0
RANK_1, DEVICE_1 = 1, 1
RANK_2, DEVICE_2 = 2, 2
RANK_3, DEVICE_3 = 3, 3
NUM_LOCAL_EXPERTS = 8  # N: common length of inputs/outputs/lens/groupList arrays
DEFAULT_NUM_TOKENS = 512  # rows of each expert input/output tensor
DEFAULT_HIDDEN = 1  # columns of each expert input/output tensor

# Case tags for per-rank group_list patterns. Together they cover all kernel paths:
#   SPARSE  : even indices non-zero       -> M = N/2 (compacted copy, multi-entry dispatch)
#   FULL    : all indices non-zero        -> M = N   (no skip, full compaction)
#   EMPTY   : all zero                    -> M = 0   (totalElements=0, packedGroupList untouched)
#   SINGLE  : only index 0 non-zero       -> M = 1   (single large entry sliced across cores)
CASE_SPARSE, CASE_FULL, CASE_EMPTY, CASE_SINGLE = 0, 1, 2, 3


def bench(fn, num_warmups: int = 50, num_tests: int = 50):
    """Benchmark function execution time with warmup and L2 cache flush."""
    device = torch.device("npu")
    torch.npu.synchronize()

    # Flush L2 cache with 256 MB data
    cache = torch.empty(int(256e6 // 4), dtype=torch.int32, device=device)

    # Warmup
    for _ in range(num_warmups):
        fn()

    # Flush L2 cache
    cache.zero_()
    torch.npu.synchronize()

    # Timing
    times = []
    for _ in range(num_tests):
        torch.npu.synchronize()
        start = torch.npu.Event(enable_timing=True)
        end = torch.npu.Event(enable_timing=True)

        start.record()
        fn()
        end.record()

        torch.npu.synchronize()
        elapsed_time = start.elapsed_time(end) / 1e3  # ms -> s
        times.append(elapsed_time)

    times = np.array(times[1:])  # Remove the first timing
    return np.average(times), np.min(times), np.max(times)


def _build_group_list(rank_id):
    # Per-rank pattern covers distinct M values and kernel branches.
    mode = rank_id % 4
    group_list = [0] * NUM_LOCAL_EXPERTS
    if mode == CASE_SPARSE:
        for i in range(NUM_LOCAL_EXPERTS):
            if i % 2 == 0:
                group_list[i] = (i + 1) * 2
    elif mode == CASE_FULL:
        for i in range(NUM_LOCAL_EXPERTS):
            group_list[i] = (i + 1) * 2
    elif mode == CASE_EMPTY:
        pass  # all zero, M = 0
    else:  # CASE_SINGLE
        group_list[0] = 2  # single non-zero at index 0, M = 1
    return group_list


def _rank_main(rank_id: int, device_id: int, sync: mp.Barrier, result_queue: mp.Queue, num_tokens: int, hidden: int):
    mf.set_log_level(3)
    torch.npu.set_device(device_id)

    config = offload.OffloadConfig()
    config.device_id = device_id
    config.reserve_size = ONE_GIB
    config.alloc_size = ONE_GIB
    assert offload.initialize(config) == 0, "offload.initialize failed"

    group_list_host = _build_group_list(rank_id)
    n_nonzero = sum(1 for v in group_list_host if v != 0)  # M
    case_name = ["SPARSE", "FULL", "EMPTY", "SINGLE"][rank_id % 4]

    # inputs[i]: host DRAM tensor (zeros) allocated from the offload pool.
    # Keep host_tensors references alive so data_ptr() stays valid through the copy.
    # NOTE: lens is the BYTE count (kernel is instantiated as uint8_t byte copy, so callers
    # pass tensor.numel() * tensor.element_size()).
    host_tensors = []
    src_ptrs = []
    lens = []
    for _ in range(NUM_LOCAL_EXPERTS):
        host_t = offload.empty([num_tokens, hidden], dtype=torch.bfloat16).zero_()
        host_tensors.append(host_t)
        src_ptrs.append(host_t.data_ptr())
        lens.append(host_t.numel() * host_t.element_size())

    # outputs[j]: NPU tensor (ones) pre-sized to [num_tokens, hidden], one per index j.
    # The kernel compactly writes the M non-zero entries to outputs[0..M); outputs[M..N)
    # are left untouched and stay as ones.
    dst_tensors = []
    dst_ptrs = []
    for _ in range(NUM_LOCAL_EXPERTS):
        dev_t = torch.ones(num_tokens, hidden, dtype=torch.bfloat16).npu()
        dst_tensors.append(dev_t)
        dst_ptrs.append(dev_t.data_ptr())

    src_ptrs_t = torch.tensor(src_ptrs, dtype=torch.int64).npu()
    dst_ptrs_t = torch.tensor(dst_ptrs, dtype=torch.int64).npu()
    lens_t = torch.tensor(lens, dtype=torch.int32).npu()
    num_local_expert_t = torch.tensor(NUM_LOCAL_EXPERTS, dtype=torch.int32).npu()
    group_list_t = torch.tensor(group_list_host, dtype=torch.int64).npu()
    # packed_group_list is over-allocated to length N; only [0, M) is written by the kernel.
    packed_group_list_t = torch.zeros(NUM_LOCAL_EXPERTS, dtype=torch.int64).npu()
    device = dst_tensors[0].device

    # Calculate total bytes to transfer (only non-zero group_list entries)
    total_bytes = sum(lens[i] for i, v in enumerate(group_list_host) if v != 0)

    # Benchmark group_pack_copy
    def run_copy():
        assert (
            offload.group_pack_copy(
                src_ptrs_t, dst_ptrs_t, lens_t, num_local_expert_t, group_list_t, packed_group_list_t, device
            )
            == 0
        ), f"rank_id:{rank_id} ({case_name}) offload.group_pack_copy failed"
        torch.npu.synchronize()

    avg_t, min_t, max_t = bench(run_copy)

    # Calculate and print bandwidth
    bandwidth_avg = 0.0
    if total_bytes > 0:
        bandwidth_avg = total_bytes / 1e9 / avg_t  # GB/s
        bandwidth_min = total_bytes / 1e9 / max_t  # GB/s (min bandwidth from max time)
        bandwidth_max = total_bytes / 1e9 / min_t  # GB/s (max bandwidth from min time)
        print(
            f"rank_id:{rank_id} ({case_name}) "
            f"H2D bandwidth_avg={bandwidth_avg:.2f} GB/s, "
            f"bandwidth_range=[{bandwidth_min:.2f}, {bandwidth_max:.2f}] GB/s, "
            f"avg_t={avg_t * 1e6:.2f} us, "
            f"total_bytes={total_bytes / 1e9:.3f} GB"
        )
    else:
        print(f"rank_id:{rank_id} ({case_name}) no data transferred (total_bytes=0)")

    # Send result to queue for aggregation
    result_queue.put((rank_id, bandwidth_avg, total_bytes, avg_t))

    # Verify compacted copy: outputs[0..M) zeroed (host zeros copied); outputs[M..N) stay ones.
    ones_sum = float(num_tokens * hidden)
    for j, dev_t in enumerate(dst_tensors):
        got = dev_t.sum().item()
        if j < n_nonzero:
            assert got == 0, f"rank_id:{rank_id} ({case_name}) output[{j}] (packed slot) not zeroed, got {got}"
        else:
            assert got == ones_sum, (
                f"rank_id:{rank_id} ({case_name}) output[{j}] (tail) should stay ones, got {got} expected {ones_sum}"
            )

    # Verify packed_group_list: first M entries are non-zero groupList values in order; rest 0.
    expected_packed = [v for v in group_list_host if v != 0]
    actual_packed = packed_group_list_t.cpu().tolist()
    assert actual_packed[:n_nonzero] == expected_packed, (
        f"rank_id:{rank_id} ({case_name}) packed_group_list[:M] mismatch: "
        f"got {actual_packed[:n_nonzero]}, expected {expected_packed}"
    )
    assert all(v == 0 for v in actual_packed[n_nonzero:]), (
        f"rank_id:{rank_id} ({case_name}) packed_group_list[M:] should stay 0, got {actual_packed[n_nonzero:]}"
    )

    offload.uninitialize()
    sync.wait()


def _print_help():
    print("=" * 60)
    print("group_pack_copy_offload — Test H2D offload copy bandwidth")
    print("=" * 60)
    print()
    print("Usage:")
    print("  python group_pack_copy_offload.py [options]")
    print()
    print("Options:")
    print("  --num_tokens N   Number of rows (tokens) per expert tensor (default: 512)")
    print("  --hidden N       Number of columns (hidden size) per expert tensor (default: 1)")
    print("  -h, --help       Show this help message and exit")
    print()
    print("Examples:")
    print("  python group_pack_copy_offload.py")
    print("  python group_pack_copy_offload.py --num_tokens 1024 --hidden 512")
    print("  python group_pack_copy_offload.py -h")
    print()


def main():
    if len(sys.argv) <= 1 or "--help" in sys.argv or "-h" in sys.argv:
        _print_help()
        sys.exit(0)

    parser = argparse.ArgumentParser(
        description="Test group_pack_copy offload with configurable tensor sizes", add_help=False
    )
    parser.add_argument(
        "--num_tokens",
        type=int,
        default=DEFAULT_NUM_TOKENS,
        help="Number of rows (tokens) per expert tensor",
    )
    parser.add_argument(
        "--hidden",
        type=int,
        default=DEFAULT_HIDDEN,
        help="Number of columns (hidden size) per expert tensor",
    )
    args = parser.parse_args()

    num_tokens = args.num_tokens
    hidden = args.hidden
    total_bytes_per_expert = num_tokens * hidden * 2  # bfloat16 = 2 bytes
    print(f"Config: num_tokens={num_tokens}, hidden={hidden}, bytes_per_expert={total_bytes_per_expert}")

    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(WORLD_SIZE)
    result_queue = mp.Queue()

    procs = []
    for rank_id, device_id in [(RANK_0, DEVICE_0), (RANK_1, DEVICE_1), (RANK_2, DEVICE_2), (RANK_3, DEVICE_3)]:
        p = mp.Process(target=_rank_main, args=(rank_id, device_id, sync, result_queue, num_tokens, hidden))
        procs.append(p)
        p.start()

    for p in procs:
        p.join()

    if any(p.exitcode != 0 for p in procs):
        codes = [p.exitcode for p in procs]
        raise RuntimeError(f"child rank failed: {codes}")

    # Collect results and calculate average bandwidth
    results = []
    for _ in range(WORLD_SIZE):
        results.append(result_queue.get())

    # Sort by rank_id
    results.sort(key=lambda x: x[0])

    # Calculate average bandwidth (only for ranks with data transfer)
    bandwidths = [r[1] for r in results if r[2] > 0]  # r[2] = total_bytes
    if bandwidths:
        avg_bandwidth = sum(bandwidths) / len(bandwidths)
        print(f"\n{'=' * 60}")
        print(f"Final Average H2D Bandwidth: {avg_bandwidth:.2f} GB/s")
        print(f"{'=' * 60}\n")
    else:
        print("\nNo data transferred across all ranks\n")

    print("group_pack_copy_offload: all ranks OK", flush=True)


if __name__ == "__main__":
    main()
