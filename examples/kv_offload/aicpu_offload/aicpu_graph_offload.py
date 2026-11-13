#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
"""单卡 AICPU：预热、捕获一次，动态修改设备元数据并校验 H2D/D2H。"""

import argparse
import importlib.util
import os.path

import torch
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


ONE_GIB = 1 << 30
K_DIM = 512
V_DIM = 64
WARMUP_ITERS = 5


def run_graph(pool, cpu_tensors, npu_tensors, warmup_iters=WARMUP_ITERS):
    device = npu_tensors[0].device
    sources, destinations = cpu_tensors, npu_tensors

    # 1. 四个元数据 Tensor 都放在 NPU 上，捕获后保持地址不变。
    src_ptrs = torch.tensor([t.data_ptr() for t in sources], dtype=torch.int64, device=device)
    dst_ptrs = torch.tensor([t.data_ptr() for t in destinations], dtype=torch.int64, device=device)
    len_ptrs = torch.tensor([t.numel() * t.element_size() for t in sources], dtype=torch.int32, device=device)
    size_ptr = torch.tensor([len(sources)], dtype=torch.int32, device=device)
    host_tables = (src_ptrs.clone(), src_ptrs.flip(0))
    device_tables = (dst_ptrs.clone(), dst_ptrs.flip(0))
    length_tables = (len_ptrs.clone(), len_ptrs.flip(0))

    def copy_once():
        ret = pool.sparse_copy(src_ptrs, dst_ptrs, len_ptrs, size_ptr, device)
        if ret != 0:
            raise RuntimeError(f"sparse_copy failed: {ret}")

    stream = torch.npu.Stream(device=device)
    stream.wait_stream(torch.npu.current_stream())
    graph = torch.npu.NPUGraph()
    try:
        with torch.npu.stream(stream):
            # 2. 图外预热，等待初始化和拷贝完成。
            for _ in range(warmup_iters):
                copy_once()
            stream.synchronize()

            # 3. 进入 with 开始捕获，退出 with 结束捕获。
            with torch.npu.graph(graph, stream=stream, capture_error_mode="global"):
                copy_once()
            stream.synchronize()

            # 同一图和固定表地址跨 replay 切换 H2D/D2H，方向仅由表内容决定。
            for value, (h2d, reverse, count, partial) in enumerate(
                (
                    (True, 0, 0, False),
                    (True, 1, 1, False),
                    (False, 0, 2, True),
                    (True, 0, 2, False),
                    (False, 1, 2, False),
                ),
                start=1,
            ):
                sources, destinations = (cpu_tensors, npu_tensors) if h2d else (npu_tensors, cpu_tensors)
                src_table, dst_table = (host_tables, device_tables) if h2d else (device_tables, host_tables)
                label = "H2D" if h2d else "D2H"
                for index, (source, destination) in enumerate(zip(sources, destinations)):
                    source.fill_(value + index)
                    destination.zero_()
                src_ptrs.copy_(src_table[reverse])
                dst_ptrs.copy_(dst_table[reverse])
                len_ptrs.copy_(length_tables[reverse])
                size_ptr.fill_(count)
                if partial:
                    len_ptrs.fill_(sources[0].element_size())
                order = list(reversed(range(len(sources)))) if reverse else list(range(len(sources)))
                total_bytes = sum(
                    (1 if partial else sources[index].numel()) * sources[index].element_size()
                    for index in order[:count]
                )
                elapsed = time_with_npu_event(graph.replay)

                for index, (source, destination) in enumerate(zip(sources, destinations)):
                    expected = torch.zeros_like(source.cpu())
                    if index in order[:count]:
                        elements = 1 if partial else source.numel()
                        expected[:elements].copy_(source.cpu()[:elements])
                    if not torch.equal(destination.cpu(), expected):
                        raise AssertionError(f"{label} replay {value}: data mismatch")
                print(f"PASS {label} replay {value}", flush=True)
                report_timing(
                    0,
                    f"graph_copy device={device.index} {label} count={count} partial={partial}",
                    elapsed,
                    total_bytes,
                )
    finally:
        stream.synchronize()
        graph.reset()  # 销毁图后，才能释放它引用的 Tensor 和 offload 资源。


def run_large_graph(pool, device, records, elements_per_record=1, warmup_iters=WARMUP_ITERS):
    elements = records * elements_per_record
    cpu_data = pool.empty([elements], dtype=torch.int32).fill_(7)
    npu_data = torch.zeros(elements, dtype=torch.int32, device=device)
    observed = torch.empty_like(npu_data)
    record_bytes = elements_per_record * cpu_data.element_size()
    offsets = torch.arange(records, dtype=torch.int64, device=device) * record_bytes
    host_addresses = offsets + cpu_data.data_ptr()
    device_addresses = offsets + npu_data.data_ptr()
    src_ptrs = host_addresses.clone()
    dst_ptrs = device_addresses.clone()
    lengths = torch.full((records,), record_bytes, dtype=torch.int32, device=device)
    count_ptr = torch.tensor([records], dtype=torch.int32, device=device)
    src_input, dst_input, count_input = src_ptrs.clone(), dst_ptrs.clone(), count_ptr.clone()

    def copy_once():
        src_ptrs.copy_(src_input)
        dst_ptrs.copy_(dst_input)
        count_ptr.copy_(count_input)
        ret = pool.sparse_copy(src_ptrs, dst_ptrs, lengths, count_ptr, device)
        if ret != 0:
            raise RuntimeError(f"large sparse_copy failed: {ret}")

    stream = torch.npu.Stream(device=device)
    stream.wait_stream(torch.npu.current_stream())
    graph = torch.npu.NPUGraph()
    try:
        with torch.npu.stream(stream):
            for _ in range(warmup_iters):
                copy_once()
            stream.synchronize()
            with torch.npu.graph(graph, stream=stream, capture_error_mode="global"):
                copy_once()
                torch.add(npu_data, 1, out=observed)
            stream.synchronize()
            counts = sorted(
                {
                    0,
                    min(255, records),
                    min(256, records),
                    min(257, records),
                    min(1000, records),
                    min(1001, records),
                    min(8190, records),
                    min(8191, records),
                    records,
                }
            )
            for h2d in (True, False):
                for iteration, count in enumerate(counts, start=1):
                    source, destination = (cpu_data, npu_data) if h2d else (npu_data, cpu_data)
                    source.fill_(iteration)
                    destination.zero_()
                    src_input.copy_(host_addresses if h2d else device_addresses)
                    dst_input.copy_(device_addresses if h2d else host_addresses)
                    count_input.fill_(count)
                    elapsed = time_with_npu_event(graph.replay)
                    expected = torch.zeros(elements, dtype=torch.int32)
                    expected[: count * elements_per_record].fill_(iteration)
                    if not torch.equal(destination.cpu(), expected):
                        raise AssertionError(f"large copy mismatch, h2d={h2d}, count={count}")
                    if not torch.equal(observed.cpu(), npu_data.cpu() + 1):
                        raise AssertionError(f"same-stream consumer ran before copy completion, count={count}")
                    print(f"PASS large graph h2d={h2d} count={count} capacity={records}", flush=True)
                    label = "H2D" if h2d else "D2H"
                    report_timing(
                        0,
                        f"graph_e2e device={device.index} {label} count={count} record_bytes={record_bytes}",
                        elapsed,
                        count * record_bytes,
                    )
    finally:
        stream.synchronize()
        graph.reset()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", type=int, default=0, help="NPU device ID (default: 0)")
    parser.add_argument("--records", type=int, default=20000, help="large-copy table capacity (default: 20000)")
    parser.add_argument(
        "--warmup-iters",
        type=int,
        default=WARMUP_ITERS,
        help="eager warmup count; use 0 to capture without sparse-copy warmup",
    )
    args = parser.parse_args()
    if not 1 <= args.records <= 65536:
        parser.error("--records must be in [1, 65536]")
    if args.warmup_iters < 0:
        parser.error("--warmup-iters must be non-negative")
    torch.npu.set_device(args.device)
    mf.set_log_level(1)

    config = offload.OffloadConfig()
    config.device_id = args.device
    config.reserve_size = ONE_GIB
    config.alloc_size = ONE_GIB
    config.scene = offload.Scene.AICPU
    pool = offload.Offload()
    ret = pool.initialize(config)
    if ret != 0:
        raise RuntimeError(f"pool.initialize failed: {ret}")

    try:
        cpu_key = pool.empty([K_DIM], dtype=torch.bfloat16).zero_()
        cpu_value = pool.empty([V_DIM], dtype=torch.bfloat16).zero_()
        npu_key = torch.ones(K_DIM, dtype=torch.bfloat16, device=f"npu:{args.device}")
        npu_value = torch.ones(V_DIM, dtype=torch.bfloat16, device=f"npu:{args.device}")
        cpu_tensors = [cpu_key, cpu_value]
        npu_tensors = [npu_key, npu_value]

        run_graph(pool, cpu_tensors, npu_tensors, warmup_iters=args.warmup_iters)
        run_large_graph(pool, npu_key.device, args.records, warmup_iters=args.warmup_iters)
        run_large_graph(pool, npu_key.device, 1, (32 << 20) // 4 + 1, warmup_iters=args.warmup_iters)
        print("AICPU graph: H2D and D2H passed", flush=True)
    finally:
        torch.npu.synchronize()
        pool.uninitialize()


if __name__ == "__main__":
    main()
