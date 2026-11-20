#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
"""多卡 AICPU 图模式：按字节数和记录数测试 H2D/D2H 性能与正确性。"""

import argparse
import importlib.util
import logging
import os.path

import torch
import torch.multiprocessing as mp
import torch_npu  # noqa: F401

import memfabric_hybrid as mf
from memfabric_hybrid import offload


def _load_timing_utils():
    utils_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "timing_utils.py")
    spec = importlib.util.spec_from_file_location("timing_utils", utils_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_timing = _load_timing_utils()
report_timing = _timing.report_timing
time_with_npu_event = _timing.time_with_npu_event
logger = logging.getLogger(__name__)

ONE_GIB = 1 << 30
WARMUP_ITERS = 1


def _verify_graph(graph, stream, source, destination, expected, generator, record_bytes, label):
    expected.random_(0, 256, generator=generator)
    source.copy_(expected)
    destination.copy_(expected.bitwise_not())
    graph.replay()
    stream.synchronize()
    actual = destination.cpu()
    if not torch.equal(actual, expected):
        offset = (actual != expected).to(torch.uint8).argmax().item()
        raise AssertionError(
            f"{label} mismatch: record={offset // record_bytes} byte={offset % record_bytes} "
            f"expected={expected[offset].item()} actual={actual[offset].item()}"
        )


def run_graph(pool, device, record_bytes, args, sync):
    total_bytes = record_bytes * args.records
    cpu_data = pool.empty([total_bytes], dtype=torch.uint8).zero_()
    npu_data = torch.empty(total_bytes, dtype=torch.uint8, device=device)
    expected = torch.empty(total_bytes, dtype=torch.uint8)
    generator = torch.Generator().manual_seed(device.index)
    offsets = torch.arange(args.records, dtype=torch.int64, device=device) * record_bytes
    host_addresses = offsets + cpu_data.data_ptr()
    device_addresses = offsets + npu_data.data_ptr()
    src_ptrs = host_addresses.clone()
    dst_ptrs = device_addresses.clone()
    lengths = torch.full((args.records,), record_bytes, dtype=torch.int32, device=device)
    count_ptr = torch.tensor([args.records], dtype=torch.int32, device=device)

    def copy_once():
        ret = pool.sparse_copy(src_ptrs, dst_ptrs, lengths, count_ptr, device)
        if ret != 0:
            raise RuntimeError(f"sparse_copy failed: ret={ret}")

    def replay_many():
        for _ in range(args.iters):
            graph.replay()

    stream = torch.npu.Stream(device=device)
    stream.wait_stream(torch.npu.current_stream())
    graph = torch.npu.NPUGraph()
    try:
        with torch.npu.stream(stream):
            for _ in range(args.warmup_iters):
                copy_once()
            stream.synchronize()
            with torch.npu.graph(graph, stream=stream, capture_error_mode="global"):
                copy_once()
            stream.synchronize()

            for label in ("H2D", "D2H"):
                source, destination = (cpu_data, npu_data) if label == "H2D" else (npu_data, cpu_data)
                src_ptrs.copy_(host_addresses if label == "H2D" else device_addresses)
                dst_ptrs.copy_(device_addresses if label == "H2D" else host_addresses)
                for _ in range(args.warmup_iters):
                    graph.replay()
                stream.synchronize()
                _verify_graph(graph, stream, source, destination, expected, generator, record_bytes, label)
                sync.wait()
                elapsed = time_with_npu_event(replay_many)
                sync.wait()
                _verify_graph(graph, stream, source, destination, expected, generator, record_bytes, label)
                report_timing(
                    device.index - args.device,
                    f"graph_copy device={device.index} {label} bytes={record_bytes} "
                    f"records={args.records} iters={args.iters} PASS",
                    elapsed / args.iters,
                    total_bytes,
                )
    finally:
        stream.synchronize()
        graph.reset()
        ret = pool.free(cpu_data.data_ptr())
        if ret != 0:
            raise RuntimeError(f"pool.free failed: ptr={cpu_data.data_ptr()} bytes={total_bytes} ret={ret}")


def _run_device(rank_id, args, sync):
    device_id = args.device + rank_id
    record_bytes = None
    try:
        torch.npu.set_device(device_id)
        mf.set_log_level(3)
        max_total_bytes = max(args.record_bytes) * args.records
        pool_bytes = ((max_total_bytes + ONE_GIB - 1) // ONE_GIB) * ONE_GIB
        logger.info("device=%s pool=%.2f GiB", device_id, pool_bytes / ONE_GIB)
        config = offload.OffloadConfig()
        config.device_id = device_id
        config.reserve_size = pool_bytes
        config.alloc_size = pool_bytes
        config.scene = offload.Scene.LOCAL
        config.executor = offload.Executor.AICPU
        pool = offload.Offload()
        ret = pool.initialize(config)
        if ret != 0:
            raise RuntimeError(f"pool.initialize failed: bytes={pool_bytes} ret={ret}")
        try:
            for record_bytes in args.record_bytes:
                logger.info(
                    "device=%s bytes=%s records=%s data=%.2f MiB",
                    device_id,
                    record_bytes,
                    args.records,
                    record_bytes * args.records / (1 << 20),
                )
                run_graph(pool, torch.device(f"npu:{device_id}"), record_bytes, args, sync)
                sync.wait()
        finally:
            torch.npu.synchronize()
            pool.uninitialize()
    except Exception:
        logger.exception(
            "AICPU graph failed: device=%s bytes=%s records=%s warmup_iters=%s iters=%s",
            device_id,
            record_bytes,
            args.records,
            args.warmup_iters,
            args.iters,
        )
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", type=int, default=0, help="first logical NPU device ID (default: 0)")
    parser.add_argument("--num-devices", type=int, default=4, help="number of consecutive NPU devices (default: 4)")
    parser.add_argument(
        "--bytes",
        dest="record_bytes",
        type=int,
        nargs="+",
        default=[256, 2048, 4096, 16384, 524288, 20971520],
        help="bytes per record, tested in order (default: 256 2048 4096 16384 524288 20971520)",
    )
    parser.add_argument("--records", type=int, default=1000, help="records per graph replay per device (default: 1000)")
    parser.add_argument("--iters", type=int, default=1, help="timed graph replays per direction (default: 1)")
    parser.add_argument(
        "--warmup-iters",
        type=int,
        default=WARMUP_ITERS,
        help=f"eager and graph warmup count; 0 captures without sparse-copy warmup (default: {WARMUP_ITERS})",
    )
    args = parser.parse_args()
    error = None
    if any(not 1 <= size <= (1 << 31) - 1 for size in args.record_bytes):
        error = f"each --bytes value must be in [1, 2147483647], got {args.record_bytes}"
    elif not 1 <= args.records <= 65536:
        error = f"--records must be in [1, 65536], got {args.records}"
    elif args.device < 0 or args.num_devices < 1:
        error = f"--device must be non-negative and --num-devices positive, got {args.device}, {args.num_devices}"
    elif args.warmup_iters < 0 or args.iters < 1:
        error = f"--warmup-iters must be non-negative and --iters positive, got {args.warmup_iters}, {args.iters}"
    else:
        device_count = torch.npu.device_count()
        if args.device + args.num_devices > device_count:
            error = (
                f"requested devices [{args.device}, {args.device + args.num_devices}) "
                f"exceed available device count {device_count}"
            )
    if error is not None:
        logger.error(error)
        parser.error(error)

    sync = mp.get_context("spawn").Barrier(args.num_devices)
    mp.spawn(_run_device, args=(args, sync), nprocs=args.num_devices, join=True)
    print(f"AICPU graph: all {args.num_devices} devices passed H2D and D2H", flush=True)


if __name__ == "__main__":
    main()
