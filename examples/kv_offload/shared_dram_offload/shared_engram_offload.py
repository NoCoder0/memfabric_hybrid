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
"""shared_engram_offload — SHARED DRAM pool random-entry gather micro-benchmark.

Exercises the fused offload.entry_gather operator: every rank gathers N random
256-byte entries over the WHOLE pool GVA range (all ranks' slots) selected by
device-resident int64 ids; the kernel derives every source GVA internally and
packs the entries contiguously into a 1 GiB HBM window. Each rank pre-fills its
own slot with an absolute-address pattern, so cross-rank reads stay verifiable;
NPU-event timing reports per-round time and bandwidth. The copies are tiny and
launch-latency bound — raise --reads for bandwidth shapes.

Usage (contiguous npu ids of one world-size block, e.g. 0..3 of 4):
  python shared_engram_offload.py                # 4 ranks, 24 x 256B entries
  python shared_engram_offload.py --npu-id 0,1   # 2 ranks
  python shared_engram_offload.py --reads 24     # entry count per round
"""

import argparse
import ctypes
import multiprocessing as mp
import random
import sys

import numpy as np
import torch
import torch_npu  # noqa: F401  # required: registers the npu backend

from memfabric_hybrid import offload, set_log_level

ONE_GIB = 1 << 30
POOL_MIB = 32 * 1024  # per-rank slot size (GB-aligned by the entry)
HBM_WINDOW_BYTES = ONE_GIB  # all gather results stay inside this window
ENTRY_BYTES = 256  # entry size the pool pattern is laid out on
FILL_CHUNK = 8 << 20  # slot pre-fill chunk (bounds the numpy temporaries)
RULE_LIGHT = "-" * 78
RESULTS = []  # (tag, entries, totalBytes, elapsedS, bwGBs) rows of THIS process


def print_results(rank_id, device_id, lock=None):
    """Dump this process' rows; `lock` (created in the parent before spawn) serializes ranks."""
    if not RESULTS:
        return
    if lock is not None:
        lock.acquire()
    try:
        print(f"\n[rank {rank_id} / npu {device_id}] results:")
        print(f"  {'config':<52}  {'entries':>7}  {'total(MB)':>9}  {'time(us)':>9}  {'GB/s':>8}")
        for tag, n, total, elapsed, bw in RESULTS:
            print(f"  {tag:<52}  {n:>7}  {total / (1 << 20):>9.3f}  {elapsed * 1e6:>9.2f}  {bw:>8.3f}")
    finally:
        if lock is not None:
            lock.release()


def bench_npu(fn, num_warmups, num_tests, flush_l2=True):
    """Mean NPU-event time over `num_tests` rounds; the L2 flush stays outside
    the timed window."""
    cache = torch.empty(int(256e6 // 4), dtype=torch.int32, device="npu") if flush_l2 else None
    for _ in range(num_warmups):
        fn()
    torch.npu.synchronize()
    times = []
    for _ in range(num_tests):
        if cache is not None:
            cache.zero_()
        torch.npu.synchronize()
        start = torch.npu.Event(enable_timing=True)
        end = torch.npu.Event(enable_timing=True)
        start.record()
        fn()
        end.record()
        torch.npu.synchronize()
        times.append(start.elapsed_time(end) / 1e3)
    return sum(times) / len(times)


def init_shared_pool(rank_id, device_id, world_size, slot_bytes):
    """Plain SHARED (vmm) pool; returns (hva, pool_base, total_gva).
    The whole pool is world_size contiguous slots, so pool_base is the local
    slot shifted back by rank_id slots (used only to plan and verify reads)."""
    cfg = offload.OffloadConfig()
    cfg.device_id = device_id
    cfg.reserve_size = slot_bytes
    cfg.alloc_size = slot_bytes
    cfg.world_size = world_size
    cfg.rank_id = rank_id
    cfg.scene = offload.Scene.SHARED
    assert offload.initialize(cfg) == 0, f"rank {rank_id}: offload.initialize failed"
    hva = offload.malloc(slot_bytes, 0)
    assert hva != 0, f"rank {rank_id}: offload.malloc failed"
    pool_base = hva - rank_id * slot_bytes
    total_gva = world_size * slot_bytes
    print(
        f"[rank {rank_id}] slot hva=0x{hva:x}, pool gva base=0x{pool_base:x}, "
        f"entry table span {total_gva >> 30}GiB ({world_size} slots x {slot_bytes >> 30}GiB)"
    )
    # Degenerate layout: the uniform grid addressing equals a flat whole-pool entry grid.
    ret = offload.register_entry_table(ENTRY_BYTES, slot_bytes // ENTRY_BYTES)
    assert ret == 0, f"rank {rank_id}: offload.register_entry_table failed: {ret}"
    return hva, pool_base, total_gva


def gen_random_ids(total_gva, args, rng):
    """N random 256B-entry ids over the whole pool GVA; ids may repeat (read-only)."""
    total_entries = total_gva // ENTRY_BYTES
    return [rng.randrange(total_entries) for _ in range(args.reads)]


def fill_slot(hva, slot_bytes):
    """Pre-fill this rank's own slot with the absolute-address pattern
    byte(a) = (a * 31 + 17) % 251, chunked to bound the numpy temporaries."""
    for off in range(0, slot_bytes, FILL_CHUNK):
        n = min(FILL_CHUNK, slot_bytes - off)
        addrs = np.arange(hva + off, hva + off + n, dtype=np.uint64)
        arr = ((addrs * 31 + 17) % 251).astype(np.uint8)
        ctypes.memmove(hva + off, arr.tobytes(), n)


def expected_pattern(src, ln):
    """Reader-side pattern of a segment: derived from absolute GVA alone."""
    addrs = np.arange(src, src + ln, dtype=np.uint64)
    return ((addrs * 31 + 17) % 251).astype(np.uint8)


def check_dst(hbm_np, pool_base, ids):
    """Compare every packed HBM entry (window base + i * 256) against its
    source entry's pattern (pool_base + id * 256)."""
    bad = 0
    for i, entry in enumerate(ids):
        src = pool_base + entry * ENTRY_BYTES
        off = i * ENTRY_BYTES
        if not np.array_equal(hbm_np[off : off + ENTRY_BYTES], expected_pattern(src, ENTRY_BYTES)):
            bad += 1
    return bad


def prepare_gather_args(ids, dev):
    """Build the id/count device tensors once, reused by every round (the count
    lives in device memory, so the launch is graph-capture safe)."""
    ids_t = torch.tensor(ids, dtype=torch.int64).to(dev)
    count_t = torch.tensor([len(ids)], dtype=torch.int32).to(dev)
    return ids_t, count_t


def submit_gather(dst, ids_t, count_t, dev):
    ret = offload.entry_gather(dst, ids_t, count_t, dev)
    assert ret == 0, f"offload.entry_gather failed: {ret}"


def rank_main(rank_id, device_id, world_size, args, sync, lock=None):
    """Process entry: pool init -> (pre-fill + accuracy check, skipped by
    --no-verify) -> timed rounds -> cleanup. The cyclic `sync` waits once after
    every slot is filled and once before teardown (rank 0 hosts the store)."""
    dev = torch.device("npu", device_id)
    torch.npu.set_device(device_id)

    slot_bytes = ((args.pool_mib << 20) + ONE_GIB - 1) // ONE_GIB * ONE_GIB  # entry aligns up to 1 GiB
    hva, pool_base, total_gva = init_shared_pool(rank_id, device_id, world_size, slot_bytes)
    hbm_buf = torch.zeros(HBM_WINDOW_BYTES, dtype=torch.uint8, device="npu")
    # verification indexes the window from its allocation base, so dst must equal it
    hbm_base = (hbm_buf.data_ptr() + ENTRY_BYTES - 1) & ~(ENTRY_BYTES - 1)
    assert hbm_base == hbm_buf.data_ptr(), "HBM window base must stay 256B aligned"
    assert args.reads * ENTRY_BYTES <= HBM_WINDOW_BYTES, "HBM window too small for the requested read count"

    if not args.no_verify:
        fill_slot(hva, slot_bytes)
        sync.wait()  # nobody reads until every rank's slot holds its pattern
    else:
        print(f"[rank {rank_id}] verify disabled: skipping pre-fill and accuracy check")

    rng = random.Random(args.seed + rank_id)  # per-rank stream, reproducible
    ids = gen_random_ids(total_gva, args, rng)
    remote = sum(1 for e in ids if (e * ENTRY_BYTES) // slot_bytes != rank_id)
    print(
        f"[rank {rank_id}] reads: {args.reads} total, {remote} from remote slots, "
        f"dst window {HBM_WINDOW_BYTES >> 30}GiB"
    )

    ids_t, count_t = prepare_gather_args(ids, dev)
    if not args.no_verify:
        submit_gather(hbm_base, ids_t, count_t, dev)
        torch.npu.synchronize()
        bad = check_dst(hbm_buf.cpu().numpy(), pool_base, ids)
        print(
            f"[rank {rank_id}] accuracy: {args.reads} x {ENTRY_BYTES}B random entries, "
            f"result: {'OK' if bad == 0 else f'FAILED({bad} segments)'}"
        )
        assert bad == 0, f"rank {rank_id}: random-entry accuracy check failed"

    def fn():
        submit_gather(hbm_base, ids_t, count_t, dev)

    elapsed = bench_npu(fn, args.warmup, args.rounds, flush_l2=not args.no_flush)
    total = args.reads * ENTRY_BYTES
    bw = total / elapsed / 1e9
    tag = f"entry gather {args.reads} x {ENTRY_BYTES}B over {total_gva >> 30}GiB GVA (fused id->GVA)"
    RESULTS.append((tag, args.reads, total, elapsed, bw))

    sync.wait()  # nobody uninitializes before every rank is done (rank 0 hosts the store)
    offload.free(hva, 0)
    offload.uninitialize()
    print_results(rank_id, device_id, lock)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--npu-id", default="0,1,2,3", help="contiguous NPU ids of one world-size block, e.g. 0,1,2,3")
    parser.add_argument("--reads", type=int, default=24, help="random entry count per round (default 24)")
    parser.add_argument("--pool-mib", type=int, default=POOL_MIB, help="per-rank DRAM slot size in MiB (default 32768)")
    parser.add_argument("--rounds", type=int, default=20, help="timed rounds; raise for less jitter (default 20)")
    parser.add_argument("--warmup", type=int, default=5, help="warmup rounds before timing")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--no-flush", action="store_true", help="skip the per-round L2 flush")
    parser.add_argument(
        "--no-verify", action="store_true", help="skip the pattern pre-fill and accuracy check (perf only)"
    )
    parser.add_argument("--log-level", type=int, default=2, choices=[0, 1, 2, 3])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    npu_ids = [int(x) for x in args.npu_id.split(",") if x.strip() != ""]
    assert npu_ids, "--npu-id got no valid id"
    world_size = len(npu_ids)
    # store port = 8500 + device_id // world_size, so every rank must resolve
    # the same port: the ids have to be contiguous in one world_size block
    assert len({i // world_size for i in npu_ids}) == 1, (
        f"npu ids {npu_ids} must be contiguous within one world_size={world_size} block"
    )
    assert args.reads > 0, "reads must be positive"

    set_log_level(args.log_level)

    print(RULE_LIGHT)
    print(
        f" shared_engram_offload (SHARED whole-GVA entry gather): npu {npu_ids}, "
        f"{args.reads} x {ENTRY_BYTES}B entries, rounds {args.rounds}"
    )
    print(RULE_LIGHT)

    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(world_size)
    lock = mp.Lock()  # created BEFORE spawn so children share it
    procs = [
        mp.Process(target=rank_main, args=(rank, nid, world_size, args, sync, lock)) for rank, nid in enumerate(npu_ids)
    ]
    for p in procs:
        p.start()
    for p in procs:
        p.join()
    bad = [p.exitcode for p in procs if p.exitcode != 0]
    if bad:
        print(f"[ERROR] rank process(es) failed: exitcodes {bad}")
        return 1
    print("\n[done] shared_engram_offload finished")
    return 0


if __name__ == "__main__":
    sys.exit(main())
