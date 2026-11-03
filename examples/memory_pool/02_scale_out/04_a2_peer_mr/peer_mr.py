# SPDX-License-Identifier: MulanPSL-2.0
"""Start one Python process per A2 card. Gloo only synchronizes the three processes."""
import argparse
import ctypes
import os
from datetime import timedelta

import torch.distributed as dist
import memfabric_hybrid as mf
from memfabric_hybrid import bm

POOL = 32 << 20
SIZE = 4096


def check(rc, operation):
    if rc != 0:
        raise RuntimeError(f"{operation}: rc={rc}, {mf.get_last_err_msg()}")


def exercise(pool, rank, host_va):
    # Each peer publishes a distinct byte pattern directly into its own DRAM.
    if not host_va:
        raise RuntimeError(f"rank {rank}: no local host address for DATA buffer")
    ctypes.memset(host_va, rank, SIZE)
    dist.barrier()
    error = [None]
    if rank == 0:
        try:
            for peer in (1, 2):
                remote = pool.peer_rank_ptr(peer, bm.BmMemType.HOST)
                buf = (ctypes.c_ubyte * SIZE)()
                print(f"READ rank 0 <- {peer}, {SIZE} bytes", flush=True)
                check(pool.copy_data(remote, ctypes.addressof(buf), SIZE, bm.BmCopyType.GH2H), f"READ peer {peer}")
                if bytes(buf) != bytes([peer]) * SIZE:
                    raise RuntimeError(f"READ peer {peer}: payload mismatch")
                ctypes.memset(buf, 0x80 + peer, SIZE)
                print(f"WRITE rank 0 -> {peer}, {SIZE} bytes", flush=True)
                check(pool.copy_data(ctypes.addressof(buf), remote, SIZE, bm.BmCopyType.H2GH), f"WRITE peer {peer}")
        except Exception as exc:
            error[0] = str(exc)
    dist.broadcast_object_list(error, src=0)
    if error[0]:
        raise RuntimeError(error[0])
    actual = ctypes.string_at(host_va, SIZE)
    expected = 0 if rank == 0 else 0x80 + rank
    ok = [None] * 3
    dist.all_gather_object(ok, actual == bytes([expected]) * SIZE)
    if not all(ok):
        raise RuntimeError(f"Owner-side write verification failed: {ok}")
    if rank == 0:
        print("DATA PASS: both peer reads and both owner-side write checks passed", flush=True)


def run(rank, device):
    cfg = bm.BmConfig()
    cfg.auto_ranking = False
    cfg.rank_id = rank
    cfg.start_store = rank == 0
    cfg.set_nic(os.environ["BM_NIC"].format(rank=rank))
    check(bm.initialize(os.environ["BM_STORE"], 3, device, cfg), f"BM init rank {rank} device {device}")
    pool = None
    joined = False
    try:
        pool = bm.create2(id=0, local_dram_size=POOL, max_dram_size=POOL, enable_56bits_gva=True,
                          data_op_type=bm.BmDataOpType.DEVICE_RDMA)
        if pool is None:
            raise RuntimeError(f"BM create rank {rank}: {mf.get_last_err_msg()}")
        gva = pool.peer_rank_ptr(rank, bm.BmMemType.HOST)
        hva = pool.gva_to_va(gva, bm.BmMemType.LOCAL_HOST)
        dva = pool.gva_to_va(gva, bm.BmMemType.LOCAL_DEVICE)
        print(f"rank={rank} device={device} pool GVA={gva:#x} HVA={hva:#x} registered DVA={dva:#x}", flush=True)
        dist.barrier()
        # Both peers are already present when rank 0 creates its channels.
        for entrant in (1, 2, 0):
            if rank == entrant:
                print(f"rank={rank} joining BM", flush=True)
                check(pool.join(), f"join rank {rank}")
                joined = True
            dist.barrier()
        exercise(pool, rank, hva)
        # Keep rank 0's config store alive until both other ranks have left.
        for departing in (2, 1, 0):
            if rank == departing:
                joined = False
                check(pool.leave(), f"leave rank {rank}")
            dist.barrier()
    finally:
        try:
            if joined:
                check(pool.leave(), f"leave rank {rank}")
        finally:
            if pool is not None:
                pool.destroy()
            bm.uninitialize()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rank", type=int, required=True, choices=(0, 1, 2))
    parser.add_argument("--device", type=int, required=True, help="process-visible CANN device index")
    parser.add_argument("--sync-port", type=int, default=29501, help="shared loopback port for Gloo")
    args = parser.parse_args()
    if args.device < 0 or not 1 <= args.sync_port <= 65535:
        parser.error("device must be nonnegative and sync-port must be between 1 and 65535")
    if os.getenv("MF_HYBM_RDMA_USE_HCOMM") != "1" or os.getenv("MF_HYBM_RDMA_SWAP_SPACE_SIZE") != "64":
        raise SystemExit("Set MF_HYBM_RDMA_USE_HCOMM=1 and MF_HYBM_RDMA_SWAP_SPACE_SIZE=64 before launch.")
    sync_url = f"tcp://127.0.0.1:{args.sync_port}"
    print(f"rank={args.rank} device={args.device} waiting for three processes at {sync_url}", flush=True)
    dist.init_process_group("gloo", init_method=sync_url, rank=args.rank, world_size=3, timeout=timedelta(minutes=3))
    try:
        print(f"rank={args.rank} Gloo ready, initializing MemFabric", flush=True)
        check(mf.initialize(), "MemFabric init")
        try:
            mf.set_log_level(0)
            run(args.rank, args.device)
        finally:
            mf.uninitialize()
    finally:
        dist.destroy_process_group()
