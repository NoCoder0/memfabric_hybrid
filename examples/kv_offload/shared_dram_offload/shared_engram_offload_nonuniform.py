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
"""shared_engram_offload_nonuniform — BM round-trip + offload entry_gather in a
non-uniform 4-rank deployment.

Mirrors a real 4-node inference scenario: prefill nodes (ranks 0/1) run BM +
offload shared in the same process, decode nodes (ranks 2/3) run offload
shared only.  The offload VA region is isolated in the last 4 TB of the
regular GVM area, so BM's VA reservation on ranks 0/1 does not shift the
offload GVA base — cross-rank symmetry holds without negotiation.

BM verification follows 01_single_node_multi_device_dram (H2G/G2G/G2H
round-trip between ranks 0 and 1).  Offload verification follows
shared_engram_offload: every rank gathers N random 256-byte entries over
the WHOLE pool GVA range via offload.entry_gather and verifies them against
an absolute-address pattern.

Usage:
  python shared_engram_offload_nonuniform.py
  python shared_engram_offload_nonuniform.py --reads 48
"""

import argparse
import ctypes
import multiprocessing as mp
import random
import sys

import numpy as np
import torch
import torch_npu  # noqa: F401  # required: registers the npu backend

import memfabric_hybrid as mf
from memfabric_hybrid import bm, offload

# ── constants ────────────────────────────────────────────────────────────
ONE_GIB = 1 << 30
DEFAULT_WORLD_SIZE = 4
BM_WORLD_SIZE = 2  # ranks 0/1 form the BM group
BM_STORE_URL = "tcp://127.0.0.1:8572"
OFFLOAD_STORE_URL = "tcp://127.0.0.1:8501"  # distinct from BM store
BM_NIC_URL = "tcp://127.0.0.1:10005"

COPY_BYTES = 4 * 1024 * 1024  # 4 MB int32 payload for BM round-trip
SLOT_BYTES = ONE_GIB  # per-rank offload slot = 1 GiB (1 GiB-aligned)
HBM_WINDOW_BYTES = ONE_GIB  # HBM destination window
ENTRY_BYTES = 256  # entry size the pool pattern is laid out on
FILL_CHUNK = 8 << 20  # slot pre-fill chunk (bounds numpy temporaries)
DEFAULT_READS = 24  # random entry count per round
WARMUP_ROUNDS = 5
TIMED_ROUNDS = 20
RULE_LIGHT = "-" * 78

# Per-process results: (tag, entries, totalBytes, elapsedS, bwGBs)
RESULTS = []


def print_results(rank_id, device_id, lock=None):
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


# ── BM phase (ranks 0/1 only) ──────────────────────────────────────────
def _bm_phase(rank_id, device_id, sync):
    """BM init + H2G/G2G/G2H round-trip between ranks 0 and 1 (mirrors
    01_single_node_multi_device_dram).  Ranks 2/3 skip but hit every sync."""
    use_bm = rank_id < BM_WORLD_SIZE
    bm_handle = None
    bm_inited = False
    mf_inited = False

    if use_bm:
        assert mf.initialize() == 0, f"rank {rank_id}: mf.initialize failed"
        mf_inited = True

        cfg = bm.BmConfig()
        cfg.rank_id = rank_id
        cfg.start_store = rank_id == 0
        cfg.set_nic(BM_NIC_URL)
        assert bm.initialize(BM_STORE_URL, BM_WORLD_SIZE, device_id, cfg) == 0, f"rank {rank_id}: bm.initialize failed"
        bm_inited = True

        bm_handle = bm.create2(
            id=0,
            local_dram_size=ONE_GIB,
            max_dram_size=ONE_GIB,
            data_op_type=bm.BmDataOpType.SDMA,
        )
        print(f"[rank {rank_id}] BM initialized (device_id={device_id}, store={BM_STORE_URL})", flush=True)
        assert bm_handle.join() == 0, f"rank {rank_id}: bm join failed"

    sync.wait()  # ranks 0/1 finished BM init; ranks 2/3 passthrough

    if use_bm:
        peer = 1 - rank_id
        gva_me = bm_handle.peer_rank_ptr(rank_id, bm.BmMemType.HOST)
        gva_peer = bm_handle.peer_rank_ptr(peer, bm.BmMemType.HOST)
        assert gva_me != 0 and gva_peer != 0, f"rank {rank_id}: peer_rank_ptr HOST"

        # Phase A: r0 H2G → G2G to r1 → r1 G2H verify
        if rank_id == 0:
            src = torch.arange(COPY_BYTES // 4, dtype=torch.int32).contiguous()
            assert bm_handle.copy_data(src.data_ptr(), gva_me, COPY_BYTES, bm.BmCopyType.H2G, 0) == 0, "H2G r0"
    sync.wait()

    if use_bm and rank_id == 0:
        peer = 1
        gva_me = bm_handle.peer_rank_ptr(rank_id, bm.BmMemType.HOST)
        gva_peer = bm_handle.peer_rank_ptr(peer, bm.BmMemType.HOST)
        assert bm_handle.copy_data(gva_me, gva_peer, COPY_BYTES, bm.BmCopyType.G2G, 0) == 0, "G2G 0→1"
    sync.wait()

    if use_bm and rank_id == 1:
        exp = torch.arange(COPY_BYTES // 4, dtype=torch.int32).contiguous()
        got = torch.empty(COPY_BYTES // 4, dtype=torch.int32)
        gva_me = bm_handle.peer_rank_ptr(rank_id, bm.BmMemType.HOST)
        assert bm_handle.copy_data(gva_me, got.data_ptr(), COPY_BYTES, bm.BmCopyType.G2H, 0) == 0, "G2H r1"
        assert torch.equal(got, exp), f"rank {rank_id}: BM round-trip r0→r1"
        print(f"[rank {rank_id}] BM Phase A OK (r0→r1)", flush=True)
    sync.wait()

    # Phase B: symmetric r1 → r0
    if use_bm and rank_id == 1:
        src = (torch.arange(COPY_BYTES // 4, dtype=torch.int32) * 17 + 3).contiguous()
        gva_me = bm_handle.peer_rank_ptr(rank_id, bm.BmMemType.HOST)
        assert bm_handle.copy_data(src.data_ptr(), gva_me, COPY_BYTES, bm.BmCopyType.H2G, 0) == 0, "H2G r1"
    sync.wait()

    if use_bm and rank_id == 1:
        peer = 0
        gva_me = bm_handle.peer_rank_ptr(rank_id, bm.BmMemType.HOST)
        gva_peer = bm_handle.peer_rank_ptr(peer, bm.BmMemType.HOST)
        assert bm_handle.copy_data(gva_me, gva_peer, COPY_BYTES, bm.BmCopyType.G2G, 0) == 0, "G2G 1→0"
    sync.wait()

    if use_bm and rank_id == 0:
        exp = (torch.arange(COPY_BYTES // 4, dtype=torch.int32) * 17 + 3).contiguous()
        got = torch.empty(COPY_BYTES // 4, dtype=torch.int32)
        gva_me = bm_handle.peer_rank_ptr(rank_id, bm.BmMemType.HOST)
        assert bm_handle.copy_data(gva_me, got.data_ptr(), COPY_BYTES, bm.BmCopyType.G2H, 0) == 0, "G2H r0"
        assert torch.equal(got, exp), f"rank {rank_id}: BM round-trip r1→r0"
        print(f"[rank {rank_id}] BM Phase B OK (r1→r0)", flush=True)
    sync.wait()

    return bm_handle, bm_inited, mf_inited


# ── offload entry_gather phase (all 4 ranks) ─────────────────────────────
def _fill_slot(hva, slot_bytes):
    """Pre-fill this rank's own slot with the absolute-address pattern
    byte(a) = (a * 31 + 17) % 251, chunked to bound numpy temporaries."""
    for off in range(0, slot_bytes, FILL_CHUNK):
        n = min(FILL_CHUNK, slot_bytes - off)
        addrs = np.arange(hva + off, hva + off + n, dtype=np.uint64)
        arr = ((addrs * 31 + 17) % 251).astype(np.uint8)
        ctypes.memmove(hva + off, arr.tobytes(), n)


def _expected_pattern(src, ln):
    addrs = np.arange(src, src + ln, dtype=np.uint64)
    return ((addrs * 31 + 17) % 251).astype(np.uint8)


def _check_dst(hbm_np, pool_base, ids):
    bad = 0
    for i, entry in enumerate(ids):
        src = pool_base + entry * ENTRY_BYTES
        off = i * ENTRY_BYTES
        if not np.array_equal(hbm_np[off : off + ENTRY_BYTES], _expected_pattern(src, ENTRY_BYTES)):
            bad += 1
    return bad


def _bench_npu(fn, num_warmups, num_tests, flush_l2=True):
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


def _offload_entry_gather_phase(rank_id, device_id, world_size, reads, sync, lock):
    """Offload shared pool init + entry_gather (mirrors shared_engram_offload).
    All 4 ranks participate.  The isolated offload VA region guarantees
    cross-rank GVA symmetry regardless of BM's prior VA reservation."""
    dev = torch.device("npu", device_id)

    # 1. pool init
    cfg = offload.OffloadConfig()
    cfg.device_id = device_id
    cfg.reserve_size = SLOT_BYTES
    cfg.alloc_size = SLOT_BYTES
    cfg.world_size = world_size
    cfg.rank_id = rank_id
    cfg.scene = offload.Scene.SHARED
    cfg.store_url = OFFLOAD_STORE_URL
    assert offload.initialize(cfg) == 0, f"rank {rank_id}: offload.initialize failed"

    hva = offload.malloc(SLOT_BYTES, 0)
    assert hva != 0, f"rank {rank_id}: offload.malloc failed"
    pool_base = hva - rank_id * SLOT_BYTES
    total_gva = world_size * SLOT_BYTES
    print(
        f"[rank {rank_id}] offload slot hva=0x{hva:x}, pool gva base=0x{pool_base:x}, "
        f"entry table span {total_gva >> 30}GiB ({world_size} slots x {SLOT_BYTES >> 30}GiB)"
    )
    ret = offload.register_entry_table(ENTRY_BYTES, SLOT_BYTES // ENTRY_BYTES)
    assert ret == 0, f"rank {rank_id}: offload.register_entry_table failed: {ret}"

    # 2. HBM destination window
    hbm_buf = torch.zeros(HBM_WINDOW_BYTES, dtype=torch.uint8, device="npu")
    hbm_base = (hbm_buf.data_ptr() + ENTRY_BYTES - 1) & ~(ENTRY_BYTES - 1)
    assert hbm_base == hbm_buf.data_ptr(), "HBM window base must stay 256B aligned"
    assert reads * ENTRY_BYTES <= HBM_WINDOW_BYTES, "HBM window too small for the requested read count"

    # 3. pre-fill slot with absolute-address pattern
    _fill_slot(hva, SLOT_BYTES)
    sync.wait()  # nobody reads until every rank's slot holds its pattern

    # 4. random entry ids over the whole pool GVA
    rng = random.Random(42 + rank_id)
    total_entries = total_gva // ENTRY_BYTES
    ids = [rng.randrange(total_entries) for _ in range(reads)]
    remote = sum(1 for e in ids if (e * ENTRY_BYTES) // SLOT_BYTES != rank_id)
    print(f"[rank {rank_id}] reads: {reads} total, {remote} from remote slots, dst window {HBM_WINDOW_BYTES >> 30}GiB")

    ids_t = torch.tensor(ids, dtype=torch.int64).to(dev)
    count_t = torch.tensor([len(ids)], dtype=torch.int32).to(dev)

    # 5. accuracy check
    ret = offload.entry_gather(hbm_base, ids_t, count_t, dev)
    assert ret == 0, f"rank {rank_id}: offload.entry_gather failed: {ret}"
    torch.npu.synchronize()
    bad = _check_dst(hbm_buf.cpu().numpy(), pool_base, ids)
    print(
        f"[rank {rank_id}] accuracy: {reads} x {ENTRY_BYTES}B random entries, "
        f"result: {'OK' if bad == 0 else f'FAILED({bad} segments)'}"
    )
    assert bad == 0, f"rank {rank_id}: random-entry accuracy check failed"

    # 6. timed rounds
    def fn():
        ret = offload.entry_gather(hbm_base, ids_t, count_t, dev)
        assert ret == 0, f"rank {rank_id}: offload.entry_gather failed: {ret}"

    elapsed = _bench_npu(fn, WARMUP_ROUNDS, TIMED_ROUNDS, flush_l2=True)
    total = reads * ENTRY_BYTES
    bw = total / elapsed / 1e9
    tag = f"entry gather {reads} x {ENTRY_BYTES}B over {total_gva >> 30}GiB GVA (fused id->GVA)"
    RESULTS.append((tag, reads, total, elapsed, bw))

    sync.wait()  # nobody uninitializes before every rank is done
    offload.free(hva, 0)
    offload.uninitialize()
    print_results(rank_id, device_id, lock)


# ── rank entry point ────────────────────────────────────────────────────
def _rank_main(rank_id, device_id, world_size, reads, sync, lock):
    mf.set_log_level(3)
    torch.npu.set_device(device_id)

    # Phase 1: BM init + round-trip (ranks 0/1 only; ranks 2/3 passthrough)
    bm_handle, bm_inited, mf_inited = _bm_phase(rank_id, device_id, sync)

    # Phase 2: offload shared pool + entry_gather (all 4 ranks)
    # The isolated offload VA region (last 4 TB of regular GVM) guarantees
    # that offload's GVA base is the same on all ranks, regardless of BM's
    # prior VA reservation on ranks 0/1.
    _offload_entry_gather_phase(rank_id, device_id, world_size, reads, sync, lock)

    # Phase 3: BM cleanup (ranks 0/1 only) — offload already torn down above
    if bm_handle is not None:
        assert bm_handle.leave() == 0, f"rank {rank_id}: bm leave failed"
        bm_handle.destroy()
    if bm_inited:
        bm.uninitialize(0)
    if mf_inited:
        mf.uninitialize()
    sync.wait()


# ── CLI / main ──────────────────────────────────────────────────────────
def _parse_args():
    parser = argparse.ArgumentParser(
        description="shared_engram_offload_nonuniform: BM round-trip + offload entry_gather (non-uniform 4-rank)"
    )
    parser.add_argument(
        "--reads", type=int, default=DEFAULT_READS, help=f"random entry count per round (default {DEFAULT_READS})"
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    assert args.reads > 0, "reads must be positive"
    world_size = DEFAULT_WORLD_SIZE

    print(RULE_LIGHT)
    print(
        f" shared_engram_offload_nonuniform: {world_size} ranks "
        f"(ranks 0/1 BM+offload, ranks 2/3 offload only), "
        f"{args.reads} x {ENTRY_BYTES}B entries, rounds {TIMED_ROUNDS}"
    )
    print(RULE_LIGHT)

    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(world_size)
    lock = mp.Lock()  # created before spawn so children share it
    procs = [
        mp.Process(target=_rank_main, args=(rank, rank, world_size, args.reads, sync, lock))
        for rank in range(world_size)
    ]
    for p in procs:
        p.start()
    for p in procs:
        p.join()
    bad = [p.exitcode for p in procs if p.exitcode != 0]
    if bad:
        print(f"[ERROR] rank process(es) failed: exitcodes {bad}")
        return 1
    print("\n[done] shared_engram_offload_nonuniform finished")
    return 0


if __name__ == "__main__":
    sys.exit(main())
