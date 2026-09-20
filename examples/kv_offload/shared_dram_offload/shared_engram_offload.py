#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the Mulan PSL v2 for
# more details.
"""shared_engram_offload — SHARED DRAM pool random-entry gather micro-benchmark.

Every rank gathers N random 256-byte entries over the whole-pool GVA range (all
ranks' slots, any machine) via the fused offload.entry_gather operator, into a
1 GiB HBM window. Each rank pre-fills its own slot with an absolute-address
pattern so cross-rank/cross-machine reads stay verifiable; NPU-event timing
reports per-round time and bandwidth (copies are tiny and launch-latency bound
— raise --reads for bandwidth shapes).

Pool layout: slot = global rank, machine_rank = global_rank // local_world_size.
Multi-machine pools are A3 (Ascend910C) only — other SoCs fail at init.

Two ways to drive several machines:
1. SIMULATION (default, no --node-index): --nodes splits the ranks into virtual
   machines on this host — exercises the multi-node code paths only.
2. REAL multi-node (--node-index): every machine runs the same command with its
   own --node-index/--npu-id and one shared --store-url (machine 0's ip:port);
   there is no cross-node barrier, so the script settles a fixed internal wait
   after the local pre-fill and before teardown.

Usage:
  python shared_engram_offload.py                    # 1 machine, 4 ranks
  python shared_engram_offload.py --nodes 2          # simulate 2 machines
  machine0$ python shared_engram_offload.py --nodes 2 --node-index 0 \
             --npu-id 0,1,2,3 --store-url tcp://<machine0-ip>:8600
  machine1$ same command with --node-index 1 (same --store-url)
"""

import argparse
import ctypes
import dataclasses
import multiprocessing as mp
import random
import sys
import time
from typing import Optional

import numpy as np
import torch
import torch_npu  # noqa: F401  # required: registers the npu backend

from memfabric_hybrid import offload, set_log_level

ONE_GIB = 1 << 30
POOL_MIB = 1024  # per-rank slot size in MiB (1 GiB = the 1GB huge-page minimum the entry allocates)
HBM_WINDOW_BYTES = ONE_GIB  # all gather results stay inside this window
ENTRY_BYTES = 256  # entry size the pool pattern is laid out on
FILL_CHUNK = 8 << 20  # slot pre-fill chunk (bounds the numpy temporaries)
RULE_LIGHT = "-" * 78
DEFAULT_STORE_PORT = 8600  # default port of the simulated cluster's store
SETTLE_SEC = 5.0  # real multi-node settle wait: after the local fill / before the store host tears down
RESULTS = []  # (tag, entries, totalBytes, elapsedS, bwGBs) rows of THIS process


@dataclasses.dataclass(frozen=True)
class PoolContext:
    """One engram weight pool. `store_url` is the rendezvous store every rank of
    the pool configures (the pool's rank 0 hosts it): a real multi-node pool
    points it at machine 0's reachable ip:port; None keeps the C++ derived-port
    path (single machine only)."""

    world: int  # global rank total of the pool
    local_world: int  # ranks per machine
    store_url: Optional[str]


def pool_store_desc(pool):
    return pool.store_url if pool.store_url is not None else "derived(8500+dev//world)"


def pool_desc(pool):
    return f"world={pool.world} local_world={pool.local_world} store={pool_store_desc(pool)}"


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


def init_shared_pool(rank_id, device_id, pool, slot_bytes):
    """SHARED (vmm) pool over the pool's GLOBAL ranks; returns (hva, pool_base, total_gva).
    The whole pool is pool.world contiguous slots, so pool_base is the local slot
    shifted back by rank_id slots (used only to plan and verify reads).
    Equal-size scenario: reads below span the WHOLE pool, so every rank passes
    reserve_size = alloc_size (SHARED also allows per-rank alloc_size as long as
    each fits its reserve_size slot; scenarios needing equal sizes guarantee it
    on the caller side)."""
    cfg = offload.OffloadConfig()
    cfg.device_id = device_id
    cfg.reserve_size = slot_bytes
    cfg.alloc_size = slot_bytes
    cfg.world_size = pool.world
    cfg.rank_id = rank_id
    cfg.local_world_size = pool.local_world
    if pool.store_url is not None:
        cfg.store_url = pool.store_url
    cfg.scene = offload.Scene.SHARED
    assert offload.initialize(cfg) == 0, f"rank {rank_id}: offload.initialize failed"
    hva = offload.malloc(slot_bytes, 0)
    assert hva != 0, f"rank {rank_id}: offload.malloc failed"
    pool_base = hva - rank_id * slot_bytes
    total_gva = pool.world * slot_bytes
    who = f"rank {rank_id} / machine {rank_id // pool.local_world}.{rank_id % pool.local_world} / npu {device_id}"
    print(
        f"[{who}] slot hva=0x{hva:x}, pool gva base=0x{pool_base:x}, "
        f"entry table span {total_gva >> 30}GiB ({pool.world} slots x {slot_bytes >> 30}GiB), "
        f"store {pool_store_desc(pool)}"
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


def report_read_mix(rank_id, ids, slot_bytes, pool):
    """Print how the random ids spread over local / same-machine / cross-machine
    slots (the cross-machine share is the multi-node observability signal)."""
    slots = [(e * ENTRY_BYTES) // slot_bytes for e in ids]
    local = sum(1 for s in slots if s == rank_id)
    my_machine = rank_id // pool.local_world
    cross = sum(1 for s in slots if s // pool.local_world != my_machine)
    remote = len(slots) - local
    print(
        f"[rank {rank_id}] reads: {len(ids)} total, {remote} from remote slots "
        f"({cross} cross-machine), dst window {HBM_WINDOW_BYTES >> 30}GiB"
    )


def rank_main(rank_id, device_id, pool, args, sync, lock=None):
    """Process entry: pool init -> (pre-fill + accuracy check, skipped by
    --no-verify) -> timed rounds -> cleanup. The local `sync` waits once after
    every local rank's slot is filled and once before teardown. In a real
    multi-node launch (--node-index) there is no user-facing cross-node
    barrier: after the local fill every rank waits SETTLE_SEC for the remote
    machines' parallel fills, and the store-hosting machine (index 0) waits
    SETTLE_SEC before teardown so remote ranks uninitialize against a live
    store first."""
    set_log_level(args.log_level)  # spawn starts a fresh C++ logger per child: set it HERE, not only in the parent
    dev = torch.device("npu", device_id)
    torch.npu.set_device(device_id)

    slot_bytes = ((args.pool_mib << 20) + ONE_GIB - 1) // ONE_GIB * ONE_GIB  # entry aligns up to 1 GiB
    hva, pool_base, total_gva = init_shared_pool(rank_id, device_id, pool, slot_bytes)
    hbm_buf = torch.zeros(HBM_WINDOW_BYTES, dtype=torch.uint8, device="npu")
    # verification indexes the window from its allocation base, so dst must equal it
    hbm_base = (hbm_buf.data_ptr() + ENTRY_BYTES - 1) & ~(ENTRY_BYTES - 1)
    assert hbm_base == hbm_buf.data_ptr(), "HBM window base must stay 256B aligned"
    assert args.reads * ENTRY_BYTES <= HBM_WINDOW_BYTES, "HBM window too small for the requested read count"

    if not args.no_verify:
        fill_slot(hva, slot_bytes)
        sync.wait()  # nobody reads until every local rank's slot holds its pattern
        if args.node_index is not None:
            time.sleep(SETTLE_SEC)  # remote machines fill their slots in parallel
    else:
        print(f"[rank {rank_id}] verify disabled: skipping pre-fill and accuracy check")

    rng = random.Random(args.seed + rank_id)  # per-rank stream, reproducible
    ids = gen_random_ids(total_gva, args, rng)
    report_read_mix(rank_id, ids, slot_bytes, pool)

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
    tag = f"gather {args.reads} x {ENTRY_BYTES}B over {total_gva >> 30}GiB GVA"
    RESULTS.append((tag, args.reads, total, elapsed, bw))

    sync.wait()  # nobody uninitializes before every local rank is done
    if args.node_index == 0:
        time.sleep(SETTLE_SEC)  # store host outlives the remote ranks' uninitialize
    offload.free(hva, 0)
    offload.uninitialize()
    print_results(rank_id, device_id, lock)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--npu-id", default="0,1,2,3", help="NPU ids backing the ranks (shared by virtual machines)")
    parser.add_argument(
        "--nodes",
        type=int,
        default=1,
        help="machines of the pool, each owning an equal rank block"
        " (one-host simulation without --node-index; multi-machine pools are A3-only — runs on other SoCs"
        " fail at pool init; default 1 = legacy single machine)",
    )
    parser.add_argument(
        "--node-index",
        type=int,
        default=None,
        help="REAL multi-node launch: run only THIS machine's rank block on this host; every machine of the"
        " pool runs the same command with its own index and --npu-id (default None = one-host simulation)",
    )
    parser.add_argument(
        "--store-url",
        default=None,
        help="full store url configured by every rank of the pool, e.g. tcp://<machine0-ip>:8600 (required"
        " with --node-index; machine 0's rank 0 hosts the store server)",
    )
    parser.add_argument(
        "--store-port",
        type=int,
        default=None,
        help=f"loopback store port of a simulated multi-machine pool (default: {DEFAULT_STORE_PORT};"
        " legacy derived port otherwise)",
    )
    parser.add_argument("--reads", type=int, default=24, help="random entry count per round (default 24)")
    parser.add_argument(
        "--pool-mib",
        type=int,
        default=POOL_MIB,
        help="per-rank DRAM slot size in MiB (default 1024; the entry aligns up to 1 GiB — 1GB huge-page granularity)",
    )
    parser.add_argument("--rounds", type=int, default=20, help="timed rounds; raise for less jitter (default 20)")
    parser.add_argument("--warmup", type=int, default=5, help="warmup rounds before timing")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--no-flush", action="store_true", help="skip the per-round L2 flush")
    parser.add_argument(
        "--no-verify", action="store_true", help="skip the pattern pre-fill and accuracy check (perf only)"
    )
    parser.add_argument("--log-level", type=int, default=2, choices=[0, 1, 2, 3])
    return parser.parse_args()


def is_legacy_single_node(args):
    """True only for the pure single-machine default: no virtual machines, no
    explicit store — the C++ side then derives the store port as
    8500 + device_id // world_size, which needs contiguous npu ids."""
    return args.nodes == 1 and args.store_port is None and args.store_url is None


def plan_real_machines(args, npu_ids):
    """REAL multi-node plan: this host runs only machine `--node-index`'s
    ranks; every machine runs the same command with its own index/--npu-id and
    the SAME --store-url (hosted by machine 0). Returns (rank assignments,
    pools) where a rank assignment is (global rank, local npu id)."""
    assert 0 <= args.node_index < args.nodes, f"--node-index {args.node_index} must be in [0, {args.nodes})"
    assert args.store_url is not None and args.store_url.startswith("tcp://"), (
        "--node-index needs --store-url tcp://<machine0-ip>:<port>, identical on every machine"
    )
    ranks_per_machine = len(npu_ids)
    world = args.nodes * ranks_per_machine
    pool = PoolContext(world, ranks_per_machine, args.store_url)
    base_rank = args.node_index * ranks_per_machine
    ranks = [(base_rank + k, nid) for k, nid in enumerate(npu_ids)]
    return ranks, [pool]


def plan_simulated(args, npu_ids):
    """One-host simulation plan: all global ranks run on this host; --nodes
    splits them into virtual machines sharing the npus and the loopback store."""
    world = len(npu_ids)
    assert world % args.nodes == 0, f"npu-id count {world} must divide evenly by --nodes {args.nodes}"
    local_world = world // args.nodes
    base_port = args.store_port if args.store_port is not None else DEFAULT_STORE_PORT
    if is_legacy_single_node(args):
        pools = [PoolContext(world, local_world, None)]
    else:
        url = args.store_url if args.store_url is not None else f"tcp://127.0.0.1:{base_port}"
        pools = [PoolContext(world, local_world, url)]
    return list(enumerate(npu_ids)), pools


def main() -> int:
    args = parse_args()
    npu_ids = [int(x) for x in args.npu_id.split(",") if x.strip() != ""]
    assert npu_ids, "--npu-id got no valid id"
    assert args.nodes >= 1, "--nodes must be >= 1"
    assert args.reads > 0, "reads must be positive"

    if args.node_index is not None:
        ranks, pools = plan_real_machines(args, npu_ids)
        mode = (
            f"real {args.nodes}-machine launch, this host = machine {args.node_index} "
            f"(global ranks {ranks[0][0]}..{ranks[-1][0]})"
        )
    else:
        ranks, pools = plan_simulated(args, npu_ids)
        world = len(npu_ids)
        mode = f"one-host simulation, {args.nodes} simulated machine(s) x {world // args.nodes} rank(s)"
        if is_legacy_single_node(args):
            # store port = 8500 + device_id // world_size, so every rank must resolve
            # the same port: the ids have to be contiguous in one world_size block
            assert len({i // world for i in npu_ids}) == 1, (
                f"npu ids {npu_ids} must be contiguous within one world_size={world} block"
            )
    # NOTE: the C++ log level is per-process; rank_main sets it in every spawned child
    print(RULE_LIGHT)
    print(
        f" shared_engram_offload (SHARED whole-GVA entry gather): npu {npu_ids}, {mode}, "
        f"{args.reads} x {ENTRY_BYTES}B entries, rounds {args.rounds}"
    )
    for pool in pools:
        print(f"   pool: {pool_desc(pool)}")
    print(RULE_LIGHT)

    mp.set_start_method("spawn", force=True)
    lock = mp.Lock()  # created BEFORE spawn so children share it
    procs = []
    for pool in pools:
        sync = mp.Barrier(len(ranks))  # one barrier per pool over THIS host's ranks only
        procs += [mp.Process(target=rank_main, args=(rank, nid, pool, args, sync, lock)) for rank, nid in ranks]
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
