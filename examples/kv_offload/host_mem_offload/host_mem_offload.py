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
import ctypes
import multiprocessing as mp

import torch
import torch.distributed as dist
import torch_npu  # noqa: F401  # required: registers the npu backend
import memfabric_hybrid as mf
from memfabric_hybrid import offload

ONE_GIB = 1 << 30
TWO_MIB = 2 << 20
EIGHT_MIB = 8 << 20
SIXTEEN_MIB = 16 << 20
THIRTY_TWO_MIB = 32 << 20
SIXTY_FOUR_MIB = 64 << 20
NINETY_SIX_MIB = 96 << 20
ONE_HUNDRED_SIXTY_MIB = 160 << 20
SEVEN_HUNDRED_MIB = 700 << 20
THREE_HUNDRED_MIB = 300 << 20
MIN_RESERVE_MIB = 1024
DEFAULT_WORLD_SIZE = 4
DEFAULT_INSTANCE_COUNT = 1
MAX_INSTANCE_COUNT = 8
CHIP_MAX_WORLD_SIZE = {"A3": 16, "A5": 8}
FALLBACK_MAX_WORLD_SIZE = 8
DEFAULT_PORT = 23456
DEFAULT_STORE_PORT = 8531


def _align_up(value, align):
    return (value + align - 1) & ~(align - 1)


def _block_seed(rank_id, block_idx):
    return 0x484D0000 + rank_id * 0x100 + block_idx


def _block_tensor(ptr, size):
    """Wrap one raw host block as a writable int32 tensor (same trick as offload.empty)."""
    buf = (ctypes.c_int8 * size).from_address(ptr)
    return torch.frombuffer(buf, dtype=torch.int32)


def _fill_block(ptr, size, seed):
    _block_tensor(ptr, size).fill_(seed)


def _verify_block(rank_id, tag, ptr, size, seed):
    tensor = _block_tensor(ptr, size)
    assert bool((tensor == seed).all().item()), (
        f"rank_id:{rank_id} {tag} block content mismatch, ptr:{ptr}, size:{size}, seed:{seed}"
    )


def _local_block_sizes():
    return [SIXTY_FOUR_MIB, SIXTEEN_MIB]


def _shared_block_sizes(rank_id):
    """Per-rank non-uniform base sizes: ranks may malloc different sizes in one pool."""
    return [SIXTY_FOUR_MIB + TWO_MIB * rank_id, THIRTY_TWO_MIB + TWO_MIB * rank_id]


def _build_config(scene, device_id, rank_id, world_size, store_port, giant_page, reserve_size):
    config = offload.OffloadConfig()
    config.device_id = device_id
    config.reserve_size = reserve_size
    config.alloc_size = reserve_size
    config.scene = offload.Scene.SHARED if scene == "shared" else offload.Scene.LOCAL
    config.flags = 0
    if giant_page:
        config.flags |= offload.OFFLOAD_FLAG_GIANT_PAGE
    if scene == "shared":
        config.world_size = world_size
        config.rank_id = rank_id
        config.store_url = f"tcp://127.0.0.1:{store_port}"
    return config


def _malloc(rank_id, pool, tag, size):
    ptr = pool.malloc(size)
    assert ptr != 0, f"rank_id:{rank_id} {tag} pool.malloc failed, size:{size}"
    assert ptr % 16 == 0, f"rank_id:{rank_id} {tag} not 16B aligned, ptr:{ptr}"
    return ptr


def _exercise_lifecycle(rank_id, pool, base_sizes, giant_page, reserve_size):
    """Whole-pool lifecycle on the up-front backed pool: tight 16B bump (no per-block
    1G/2M rounding), DVA valid without register (the pool is registered at initialize),
    idempotent register, free-then-reuse, adjacent-free merge-reuse, no cross-block
    stitching, exact capacity exhaustion and recovery. Returns the trailing symmetry
    blocks as (tag, ptr, size, seed, poolOffset, dva) tuples."""
    align = 16

    # phase A: tight bump + pre-registered DVA
    a = _malloc(rank_id, pool, "base0", base_sizes[0])
    _fill_block(a, base_sizes[0], _block_seed(rank_id, 0))
    b = _malloc(rank_id, pool, "base1", base_sizes[1])
    assert b - a == _align_up(base_sizes[0], align), (
        f"rank_id:{rank_id} tight bump mismatch, delta:{b - a}, expected:{_align_up(base_sizes[0], align)}"
    )
    dva_a = pool.host_get_device_pointer(a)
    assert dva_a != 0, f"rank_id:{rank_id} dva unavailable before register (pool must be pre-registered)"
    if not giant_page:
        assert dva_a == a, f"rank_id:{rank_id} dva:{dva_a} != ptr:{a} on the vmm form"
    sub_dva = pool.host_get_device_pointer(a + EIGHT_MIB)
    assert sub_dva == dva_a + EIGHT_MIB, (
        f"rank_id:{rank_id} intra-pool dva not linear, sub:{sub_dva}, expected:{dva_a + EIGHT_MIB}"
    )
    assert pool.host_register(a) == 0, f"rank_id:{rank_id} host_register failed on a pre-registered pool"
    assert pool.host_register(a) == 0, f"rank_id:{rank_id} repeated host_register not idempotent"
    _verify_block(rank_id, "base0", a, base_sizes[0], _block_seed(rank_id, 0))

    # phase B: free-then-reuse of the just-freed hole
    assert pool.free(b) == 0, f"rank_id:{rank_id} free of an unregistered block failed"
    scratch = _malloc(rank_id, pool, "scratch", SIXTEEN_MIB)
    assert scratch == b, f"rank_id:{rank_id} freed hole not reused, scratch:{scratch}, hole:{b}"
    assert pool.free(scratch) == 0, f"rank_id:{rank_id} free of scratch failed"
    re_b = _malloc(rank_id, pool, "restore", base_sizes[1])
    assert re_b == b, f"rank_id:{rank_id} hole not restored, re:{re_b}, hole:{b}"
    assert pool.free(re_b) == 0, f"rank_id:{rank_id} free of the restored block failed"

    # phase C: merge-reuse of adjacent frees; a request larger than the hole is
    # allocated beyond it, never stitched across blocks
    c1 = _malloc(rank_id, pool, "frag0", SIXTY_FOUR_MIB)
    assert c1 == b, f"rank_id:{rank_id} hole+tail merge not reused, c1:{c1}, expected:{b}"
    c2 = _malloc(rank_id, pool, "frag1", SIXTY_FOUR_MIB)
    assert c2 == c1 + SIXTY_FOUR_MIB, f"rank_id:{rank_id} frag blocks not adjacent, c1:{c1}, c2:{c2}"
    assert pool.free(c1) == 0, f"rank_id:{rank_id} free of frag0 failed"
    c3 = _malloc(rank_id, pool, "nosplit", NINETY_SIX_MIB)
    assert c3 == c2 + SIXTY_FOUR_MIB, f"rank_id:{rank_id} oversized request stitched into the hole, c3:{c3}, hole:{c1}"
    assert pool.free(c3) == 0, f"rank_id:{rank_id} free of nosplit failed"
    assert pool.free(c2) == 0, f"rank_id:{rank_id} free of frag1 failed"
    c4 = _malloc(rank_id, pool, "merged", ONE_HUNDRED_SIXTY_MIB)
    assert c4 == c1, f"rank_id:{rank_id} merged hole not reused from its start, c4:{c4}, expected:{c1}"
    assert pool.free(c4) == 0, f"rank_id:{rank_id} free of merged failed"

    # phase D: exact capacity — 700M + 300M fit into one 1G pool (the legacy per-block
    # 1G rounding would already fail the second malloc), then exhaustion and recovery
    assert pool.free(a) == 0, f"rank_id:{rank_id} free of base0 failed"
    d0 = _malloc(rank_id, pool, "exact0", SEVEN_HUNDRED_MIB)
    assert d0 == a, f"rank_id:{rank_id} drained pool not reused from offset 0, d0:{d0}, expected:{a}"
    d1 = _malloc(rank_id, pool, "exact1", THREE_HUNDRED_MIB)
    assert d1 == d0 + SEVEN_HUNDRED_MIB, f"rank_id:{rank_id} exact1 not adjacent, d0:{d0}, d1:{d1}"
    exhaust = _align_up(reserve_size, ONE_GIB) - SEVEN_HUNDRED_MIB - THREE_HUNDRED_MIB + (1 << 20)
    assert pool.malloc(exhaust) == 0, (
        f"rank_id:{rank_id} malloc beyond the exact pool capacity unexpectedly succeeded, size:{exhaust}"
    )
    assert pool.free(d1) == 0, f"rank_id:{rank_id} free of exact1 failed"
    d3 = _malloc(rank_id, pool, "recover", SIXTY_FOUR_MIB)
    assert d3 == d1, f"rank_id:{rank_id} freed 300M hole not reused, d3:{d3}, hole:{d1}"
    assert pool.free(d3) == 0, f"rank_id:{rank_id} free of recover failed"
    assert pool.free(d0) == 0, f"rank_id:{rank_id} free of exact0 failed"

    # phase E: trailing symmetry blocks, again from pool offset 0 after the drain
    blocks = []
    cursor = 0
    for tag, size, seed in (
        ("sym0", SIXTY_FOUR_MIB, _block_seed(rank_id, 30)),
        ("sym1", THIRTY_TWO_MIB, _block_seed(rank_id, 31)),
    ):
        ptr = _malloc(rank_id, pool, tag, size)
        if blocks:
            assert ptr - blocks[0][1] == cursor, (
                f"rank_id:{rank_id} {tag} sits off the pool restart base, ptr:{ptr}, offset:{cursor}"
            )
        else:
            assert ptr == a, f"rank_id:{rank_id} drained pool not restarted at offset 0, ptr:{ptr}, expected:{a}"
        _fill_block(ptr, size, seed)
        dva = pool.host_get_device_pointer(ptr)
        assert dva != 0, f"rank_id:{rank_id} {tag} dva unavailable"
        blocks.append((tag, ptr, size, seed, cursor, dva))
        cursor += _align_up(size, align)
    for tag, ptr, size, seed, _, _ in blocks:
        _verify_block(rank_id, tag, ptr, size, seed)
    return blocks


def _assert_shared_symmetry(rank_id, world_size, registered, reserve_size):
    """Slot symmetry: dva minus the block's own slot offset equals pool_base + stride * rank,
    identical for every registered block on every rank.
    """
    count = len(registered)
    mine = torch.tensor([[block[5], block[4]] for block in registered], dtype=torch.int64).npu()
    gathered = [torch.zeros(count, 2, dtype=torch.int64).npu() for _ in range(world_size)]
    dist.all_gather(gathered, mine)
    table = [t.cpu().tolist() for t in gathered]
    bases = [row[0][0] - row[0][1] for row in table]
    stride = _align_up(reserve_size, ONE_GIB)
    for peer in range(1, world_size):
        assert bases[peer] - bases[0] == stride * peer, (
            f"rank_id:{rank_id} slot symmetry broken, peer:{peer}, "
            f"delta:{bases[peer] - bases[0]}, expected:{stride * peer}"
        )
    for row_idx, row in enumerate(table):
        for block_idx in range(1, count):
            assert row[block_idx][0] - row[block_idx][1] == bases[row_idx], (
                f"rank_id:{rank_id} rank{row_idx} block{block_idx} sits off its slot base"
            )


def _assert_cross_instance_isolation(rank_id, pools, registered_per_pool):
    """Cross-instance isolation: pool 0's block must not resolve or free through any other pool."""
    ptr, dva = registered_per_pool[0][0][1], registered_per_pool[0][0][5]
    for instance_id, pool in enumerate(pools[1:], start=1):
        assert pool.host_get_device_pointer(ptr) == 0, (
            f"rank_id:{rank_id} instance:{instance_id} host_get_device_pointer succeeded on a foreign block"
        )
        assert pool.free(ptr) != 0, (
            f"rank_id:{rank_id} instance:{instance_id} free of a foreign block unexpectedly succeeded"
        )
    assert pools[0].host_get_device_pointer(ptr) == dva, (
        f"rank_id:{rank_id} owner pool no longer resolves its own block after cross-instance calls"
    )


def _rank_main(rank_id, device_id, scene, world_size, port, store_port, giant_page, reserve_size, instance_count, sync):
    mf.set_log_level(3)
    torch.npu.set_device(device_id)

    pools = []
    for instance_id in range(instance_count):
        # SHARED instances require distinct store URLs; peers use the same port for instance i.
        config = _build_config(
            scene, device_id, rank_id, world_size, store_port + instance_id, giant_page, reserve_size
        )
        pool = offload.Offload()
        assert pool.initialize(config) == 0, f"rank_id:{rank_id} instance:{instance_id} initialize failed"
        pools.append(pool)

    sizes = _shared_block_sizes(rank_id) if scene == "shared" else _local_block_sizes()
    registered_per_pool = [_exercise_lifecycle(rank_id, pool, sizes, giant_page, reserve_size) for pool in pools]
    if len(pools) > 1:
        _assert_cross_instance_isolation(rank_id, pools, registered_per_pool)

    if scene == "shared":
        dist.init_process_group("hccl", init_method=f'tcp://127.0.0.1:{port}', rank=rank_id, world_size=world_size)
        for registered in registered_per_pool:
            _assert_shared_symmetry(rank_id, world_size, registered, reserve_size)

    for instance_id, registered in enumerate(registered_per_pool):
        print(
            f"host_mem_offload rank_id:{rank_id} scene:{scene} instance:{instance_id}/{instance_count} ok, "
            f"blocks:{len(registered)}, ptr0:0x{registered[0][1]:x}, dva0:0x{registered[0][5]:x}",
            flush=True,
        )
    for pool in reversed(pools):
        pool.uninitialize()
    sync.wait()


def _detect_chip_type() -> str:
    """Detect chip type via acl: Ascend950*/Ascend910_95* -> A5, Ascend910B* -> A2, Ascend910* -> A3; '' if unknown."""
    try:
        import acl

        chip_name = acl.get_soc_name()
    except Exception:
        return ""
    if not chip_name:
        return ""
    if "Ascend950" in chip_name or "Ascend910_95" in chip_name:
        return "A5"
    if "Ascend910B" in chip_name:
        return "A2"
    if "Ascend910" in chip_name:
        return "A3"
    return ""


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="host_mem_offload: LOCAL/SHARED pool lifecycle (up-front allocated pool, in-pool "
        "malloc/free, pre-registered DVA); single or multiple offload instances per rank"
    )
    parser.add_argument(
        "--scene",
        choices=["local", "shared"],
        default="local",
        help="offload pool scene (default: local)",
    )
    parser.add_argument(
        "--instances",
        type=int,
        default=DEFAULT_INSTANCE_COUNT,
        help=f"host-mem offload instances per rank, in [1, {MAX_INSTANCE_COUNT}] "
        f"(default: {DEFAULT_INSTANCE_COUNT}); shared instance i uses store_port + i",
    )
    parser.add_argument(
        "--world_size",
        type=int,
        default=DEFAULT_WORLD_SIZE,
        help=f"number of ranks, power of 2 in [2, chip max] (default: {DEFAULT_WORLD_SIZE})",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"TCP port for hccl rendezvous, shared scene only (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--store_port",
        type=int,
        default=DEFAULT_STORE_PORT,
        help=f"TCP port of the offload config store, shared scene only (default: {DEFAULT_STORE_PORT})",
    )
    parser.add_argument(
        "--giant_page",
        action="store_true",
        help="local scene only: giant-page form (A5 routes to the conn-based segment, A3 stays vmm)",
    )
    parser.add_argument(
        "--device_offset",
        type=int,
        default=0,
        help="first NPU device id for the ranks (default: 0); rank i runs on device offset+i, "
        "e.g. 4 to pin 4 ranks onto the last four cards of a shared machine",
    )
    parser.add_argument(
        "--reserve_mib",
        type=int,
        default=1024,
        help="per-rank pool reserve size in MiB; the whole reserve is backed at initialize (per instance), "
        f"and the exact-capacity phase needs at least {MIN_RESERVE_MIB} MiB (default: 1024)",
    )
    args = parser.parse_args()

    if args.giant_page and args.scene == "shared":
        raise ValueError("--giant_page is local only: shared uses the vmm pool")
    if args.reserve_mib < MIN_RESERVE_MIB:
        raise ValueError(f"reserve_mib={args.reserve_mib} must be >= {MIN_RESERVE_MIB} for the capacity phase")
    chip_type = _detect_chip_type()
    chip_max = CHIP_MAX_WORLD_SIZE.get(chip_type, FALLBACK_MAX_WORLD_SIZE)
    if args.world_size < 2 or args.world_size > chip_max or (args.world_size & (args.world_size - 1)) != 0:
        raise ValueError(
            f"world_size={args.world_size} must be a power of 2 in [2, {chip_max}] on chip {chip_type or 'unknown'}"
        )
    if args.instances < 1 or args.instances > MAX_INSTANCE_COUNT:
        raise ValueError(f"instances={args.instances} must be in [1, {MAX_INSTANCE_COUNT}]")
    if args.scene == "shared" and not 0 < args.store_port <= 65535 - args.instances + 1:
        raise ValueError("store_port through store_port + instances - 1 must be valid TCP ports")
    if args.device_offset < 0 or args.device_offset + args.world_size > chip_max:
        raise ValueError(
            f"device_offset={args.device_offset} + world_size={args.world_size} must stay within "
            f"[0, {chip_max}] devices on chip {chip_type or 'unknown'}"
        )
    args.chip_type = chip_type
    return args


def main():
    args = _parse_args()
    # Preserve the LOCAL pool's allocation flags on both A3 and A5.
    giant_page = args.giant_page

    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(args.world_size)

    procs = []
    for rank_id in range(args.world_size):
        p = mp.Process(
            target=_rank_main,
            args=(
                rank_id,
                args.device_offset + rank_id,
                args.scene,
                args.world_size,
                args.port,
                args.store_port,
                giant_page,
                args.reserve_mib << 20,
                args.instances,
                sync,
            ),
        )
        procs.append(p)
        p.start()

    for p in procs:
        p.join()

    if any(p.exitcode != 0 for p in procs):
        codes = [p.exitcode for p in procs]
        raise RuntimeError(f"child rank failed: {codes}")
    print(f"host_mem_offload: all ranks OK, scene: {args.scene}", flush=True)


if __name__ == "__main__":
    main()
