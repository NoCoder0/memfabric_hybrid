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
import importlib.util
import multiprocessing as mp
import os.path

import torch
import torch.distributed as dist
import torch_npu
import memfabric_hybrid as mf
from memfabric_hybrid import offload


def _load_timing_utils():
    """Load ../timing_utils.py by file path, avoiding sys.path modification (G.PSL.03)."""
    utils_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "timing_utils.py")
    spec = importlib.util.spec_from_file_location("timing_utils", utils_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_timing = _load_timing_utils()
report_timing = _timing.report_timing
time_with_npu_event = _timing.time_with_npu_event


ONE_GIB = 1 << 30
DEFAULT_WORLD_SIZE = 4
DEFAULT_INSTANCE_COUNT = 2
MAX_INSTANCE_COUNT = 63
CHIP_MAX_WORLD_SIZE = {"A3": 16, "A5": 8}
FALLBACK_MAX_WORLD_SIZE = 8
DEFAULT_PORT = 23456
DEFAULT_STORE_PORT = 8500
MAX_PORT = 65535
K_DIM = 512
V_DIM = 64
WARMUP_ITERS = 5


def _run_instance(rank_id: int, instance_id: int, pool):
    expected_value = instance_id + 1
    data = {
        'cpu': {'keys': [], 'values': [], 'key_ptrs': [], 'value_ptrs': []},
        'npu': {'keys': [], 'values': [], 'key_ptrs': [], 'value_ptrs': []},
        'len': {'keys': [], 'values': []},
    }

    elem_type = torch.bfloat16
    tokens = 4 * 2048  # batch * tokens_per_req
    for _ in range(tokens):
        if rank_id == 0:
            cpu_key = pool.empty([K_DIM, 1], dtype=elem_type).fill_(expected_value)
            cpu_value = pool.empty([V_DIM, 1], dtype=elem_type).fill_(expected_value)
        else:
            cpu_key = torch.ones([K_DIM, 1], dtype=elem_type)
            cpu_value = torch.ones([V_DIM, 1], dtype=elem_type)
        npu_key = torch.zeros(K_DIM, dtype=elem_type).npu()
        npu_value = torch.zeros(V_DIM, dtype=elem_type).npu()

        data['cpu']['keys'].append(cpu_key)
        data['cpu']['values'].append(cpu_value)
        data['cpu']['key_ptrs'].append(cpu_key.data_ptr())
        data['cpu']['value_ptrs'].append(cpu_value.data_ptr())

        data['npu']['keys'].append(npu_key)
        data['npu']['values'].append(npu_value)
        data['npu']['key_ptrs'].append(npu_key.data_ptr())
        data['npu']['value_ptrs'].append(npu_value.data_ptr())

        data['len']['keys'].append(cpu_key.numel() * elem_type.itemsize)
        data['len']['values'].append(cpu_value.numel() * elem_type.itemsize)

    size = len(data['cpu']['key_ptrs'] + data['cpu']['value_ptrs'])
    src_ptrs = torch.tensor(data['cpu']['key_ptrs'] + data['cpu']['value_ptrs'], dtype=torch.int64).npu()
    dst_ptrs = torch.tensor(data['npu']['key_ptrs'] + data['npu']['value_ptrs'], dtype=torch.int64).npu()
    len_ptrs = torch.tensor(data['len']['keys'] + data['len']['values'], dtype=torch.int32).npu()
    size_ptr = torch.tensor(size, dtype=torch.int32).npu()
    device = data['npu']['keys'][0].device

    dist.broadcast(src_ptrs, src=0)

    total_bytes = sum(data['len']['keys']) + sum(data['len']['values'])

    def copy_fn():
        assert pool.sparse_copy(src_ptrs, dst_ptrs, len_ptrs, size_ptr, device) == 0, (
            f"rank_id:{rank_id} instance:{instance_id} sparse_copy failed"
        )

    for _ in range(WARMUP_ITERS):
        copy_fn()
    torch.npu.synchronize()

    report_timing(rank_id, f"instance:{instance_id} npu_event", time_with_npu_event(copy_fn), total_bytes)

    dst_tensors = data['npu']['keys'] + data['npu']['values']
    for dst_tensor in dst_tensors:
        assert torch.all(dst_tensor == expected_value).item(), (
            f"rank_id:{rank_id} instance:{instance_id} dst tensor values not correct, expected:{expected_value}"
        )


def _rank_main(
    rank_id: int, device_id: int, world_size: int, port: int, store_port: int, instance_count: int, sync: mp.Barrier
):
    mf.set_log_level(3)
    torch.npu.set_device(device_id)

    config = offload.OffloadConfig()
    config.device_id = device_id
    config.reserve_size = ONE_GIB
    config.alloc_size = ONE_GIB if rank_id == 0 else 0
    config.world_size = world_size
    config.rank_id = rank_id
    config.scene = offload.Scene.SHARED

    pools = [offload.Offload() for _ in range(instance_count)]
    for instance_id, pool in enumerate(pools):
        config.store_url = f"tcp://127.0.0.1:{store_port + instance_id}"
        assert pool.initialize(config) == 0, f"rank_id:{rank_id} instance:{instance_id} initialize failed"

    dist.init_process_group("hccl", init_method=f'tcp://127.0.0.1:{port}', rank=rank_id, world_size=world_size)
    for instance_id, pool in enumerate(pools):
        _run_instance(rank_id, instance_id, pool)
        torch.npu.synchronize()
        sync.wait()
        pool.uninitialize()
        sync.wait()
    dist.destroy_process_group()


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


def _parse_args() -> tuple:
    parser = argparse.ArgumentParser(description="shared_dram_offload_multi: independent shared DRAM pools")
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
        help=f"TCP port for hccl rendezvous (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--store_port",
        type=int,
        default=DEFAULT_STORE_PORT,
        help=f"first offload Store port; instance i uses this port + i (default: {DEFAULT_STORE_PORT})",
    )
    parser.add_argument(
        "--instances",
        type=int,
        default=DEFAULT_INSTANCE_COUNT,
        help=f"number of offload instances per rank, in [1, {MAX_INSTANCE_COUNT}] (default: {DEFAULT_INSTANCE_COUNT})",
    )
    args = parser.parse_args()
    if not 1 <= args.instances <= MAX_INSTANCE_COUNT:
        raise ValueError(f"instances={args.instances} must be in [1, {MAX_INSTANCE_COUNT}]")
    if not 1 <= args.store_port <= MAX_PORT - args.instances + 1:
        raise ValueError(f"store_port={args.store_port} leaves insufficient ports for instances={args.instances}")
    if args.store_port <= args.port < args.store_port + args.instances:
        raise ValueError(f"HCCL port={args.port} overlaps the offload Store port range")

    chip_type = _detect_chip_type()
    chip_max = CHIP_MAX_WORLD_SIZE.get(chip_type, FALLBACK_MAX_WORLD_SIZE)
    if args.world_size < 2 or args.world_size > chip_max or (args.world_size & (args.world_size - 1)) != 0:
        raise ValueError(
            f"world_size={args.world_size} must be a power of 2 in [2, {chip_max}] on chip {chip_type or 'unknown'}"
        )
    return args.world_size, args.port, args.store_port, args.instances


def main():
    world_size, port, store_port, instance_count = _parse_args()

    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(world_size)

    procs = []
    for rank_id in range(world_size):
        p = mp.Process(
            target=_rank_main, args=(rank_id, rank_id + 4, world_size, port, store_port, instance_count, sync)
        )
        procs.append(p)
        p.start()

    for p in procs:
        p.join()

    if any(p.exitcode != 0 for p in procs):
        codes = [p.exitcode for p in procs]
        raise RuntimeError(f"child rank failed: {codes}")
    print(f"shared_dram_offload_multi: all ranks OK, instances={instance_count}", flush=True)


if __name__ == "__main__":
    main()
