#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.

import argparse
import ctypes
import os
import socket
import struct
import threading
import time
from concurrent.futures import ThreadPoolExecutor

os.environ.setdefault("MF_HYBM_RDMA_SWAP_SPACE_SIZE", "1024")

import memfabric_hybrid as mf
from memfabric_hybrid import bm


DEFAULT_PACKET_SIZES = (576, 576 * 2)
DEFAULT_PACKET_COUNTS = (2 * 1024, 4 * 1024, 8 * 1024, 16 * 1024, 32 * 1024)
DEFAULT_CONCURRENCY = (1, 2, 4, 8)
DEFAULT_STRIDE = 4096
MIN_POOL_SIZE = 256 << 20


def parse_count(value):
    text = value.strip().lower()
    multiplier = 1024 if text.endswith("k") else 1
    number = text[:-1] if multiplier != 1 else text
    count = int(number) * multiplier
    if count <= 0:
        raise argparse.ArgumentTypeError(f"packet count must be positive: {value}")
    return count


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError(f"control connection closed after {len(data)} of {size} bytes")
        data.extend(chunk)
    return bytes(data)


def send_message(conn, message):
    encoded = message.encode()
    conn.sendall(struct.pack("!I", len(encoded)) + encoded)


def recv_message(conn):
    length = struct.unpack("!I", recv_exact(conn, 4))[0]
    return recv_exact(conn, length).decode()


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def initialize_bm(args):
    config = bm.BmConfig()
    config.rank_id = args.rank
    config.start_store = args.rank == 0
    config.set_nic(f"tcp://{args.head_ip}:{args.data_port}")
    store_url = f"tcp://{args.head_ip}:{args.store_port}"
    ret = bm.initialize(store_url, 2, 0, config)
    if ret != 0:
        raise RuntimeError(f"bm.initialize failed, rank={args.rank}, store={store_url}, ret={ret}")


def create_bm_handle(args):
    handle = bm.create2(
        id=0,
        local_dram_size=args.pool_size,
        max_dram_size=args.pool_size,
        data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
    )
    ret = handle.join()
    if ret != 0:
        handle.destroy()
        raise RuntimeError(f"BM join failed, rank={args.rank}, ret={ret}")
    return handle


def build_worker_batches(src_base, dst_base, packet_size, packet_count, concurrency, stride):
    offsets = [index * stride for index in range(packet_count)]
    sources = [src_base + offset for offset in offsets]
    sizes = [packet_size] * packet_count
    batches = []
    for worker_id in range(concurrency):
        worker_base = dst_base + worker_id * packet_count * stride
        destinations = [worker_base + offset for offset in offsets]
        batches.append((worker_id, sources, destinations, sizes))
    return batches


def execute_worker(handle, batch, loops, barrier):
    worker_id, sources, destinations, sizes = batch
    count = len(sources)
    barrier.wait()
    for _ in range(loops):
        ret = handle.copy_data_batch(sources, destinations, sizes, count, bm.BmCopyType.H2G, 0)
        if ret != 0:
            raise RuntimeError(
                f"copy_data_batch failed, worker={worker_id}, count={count}, packet_size={sizes[0]}, ret={ret}"
            )


def execute_concurrent(handle, batches, loops, executor):
    barrier = threading.Barrier(len(batches) + 1)
    futures = [executor.submit(execute_worker, handle, batch, loops, barrier) for batch in batches]
    start_ns = time.perf_counter_ns()
    barrier.wait()
    for future in futures:
        future.result()
    return (time.perf_counter_ns() - start_ns) / 1_000_000_000


def fill_packets(src_base, packet_size, packet_count, stride):
    for index in range(packet_count):
        ctypes.memset(src_base + index * stride, (index % 251) + 1, packet_size)


def verify_packets(handle, remote_base, packet_size, packet_count, concurrency, stride):
    samples = sorted({0, packet_count // 2, packet_count - 1})
    output = ctypes.create_string_buffer(packet_size)
    for worker_id in range(concurrency):
        worker_base = remote_base + worker_id * packet_count * stride
        for index in samples:
            ret = handle.copy_data(
                worker_base + index * stride,
                ctypes.addressof(output),
                packet_size,
                bm.BmCopyType.G2H,
                0,
            )
            if ret != 0:
                raise RuntimeError(f"verification read failed, worker={worker_id}, packet={index}, ret={ret}")
            expected = bytes([(index % 251) + 1]) * packet_size
            if output.raw != expected:
                raise RuntimeError(
                    f"verification mismatch, worker={worker_id}, packet={index}, packet_size={packet_size}"
                )


def measure_case(handle, src_base, remote_base, packet_size, packet_count, concurrency, args):
    batches = build_worker_batches(src_base, remote_base, packet_size, packet_count, concurrency, args.stride)
    with ThreadPoolExecutor(max_workers=concurrency, thread_name_prefix="urma_bw") as executor:
        for _ in range(args.warmup):
            execute_concurrent(handle, batches, 1, executor)
        elapsed_values = [execute_concurrent(handle, batches, args.loops, executor) for _ in range(args.repeats)]
    elapsed = min(elapsed_values)
    total_packets = concurrency * packet_count * args.loops
    total_bytes = total_packets * packet_size
    return {
        "packet_size": packet_size,
        "packet_count": packet_count,
        "concurrency": concurrency,
        "total_packets": total_packets,
        "seconds": elapsed,
        "gb_per_second": total_bytes / elapsed / 1_000_000_000,
        "gbps": total_bytes * 8 / elapsed / 1_000_000_000,
        "mpps": total_packets / elapsed / 1_000_000,
    }


def print_result(result):
    print(
        f"size={result['packet_size']:4d} per_worker={result['packet_count']:5d} "
        f"workers={result['concurrency']:3d} total={result['total_packets']:8d} "
        f"time={result['seconds']:.6f}s "
        f"bandwidth={result['gb_per_second']:.3f} GB/s {result['gbps']:.3f} Gbps "
        f"rate={result['mpps']:.3f} Mpps",
        flush=True,
    )


def run_benchmark(handle, args):
    remote_base = handle.peer_rank_ptr(0, bm.BmMemType.HOST)
    if remote_base == 0:
        raise RuntimeError("peer_rank_ptr returned 0 for rank 0 host memory")
    source = ctypes.create_string_buffer(args.buffer_size)
    src_base = ctypes.addressof(source)
    results = []
    for packet_size in args.packet_sizes:
        fill_packets(src_base, packet_size, max(args.packet_counts), args.stride)
        for packet_count in args.packet_counts:
            for concurrency in args.concurrency:
                result = measure_case(handle, src_base, remote_base, packet_size, packet_count, concurrency, args)
                results.append(result)
                print_result(result)
                if args.verify:
                    verify_packets(handle, remote_base, packet_size, packet_count, concurrency, args.stride)
    print("\nMaximum bandwidth for each packet size and packet count:", flush=True)
    for packet_size in args.packet_sizes:
        for packet_count in args.packet_counts:
            candidates = [
                item for item in results if item["packet_size"] == packet_size and item["packet_count"] == packet_count
            ]
            print_result(max(candidates, key=lambda item: item["gb_per_second"]))


def run_rank0(args):
    handle = create_bm_handle(args)
    try:
        with socket.create_server(("0.0.0.0", args.control_port)) as server:
            server.listen(1)
            conn, _ = server.accept()
            with conn:
                send_message(conn, "READY")
                message = recv_message(conn)
                if message != "BENCHMARK_DONE":
                    raise RuntimeError(f"unexpected control message: {message}")
                send_message(conn, "ACK")
    finally:
        handle.leave()
        handle.destroy()


def run_rank1(args):
    handle = create_bm_handle(args)
    try:
        with socket.create_connection((args.head_ip, args.control_port)) as conn:
            if recv_message(conn) != "READY":
                raise RuntimeError("rank 0 did not become ready")
            run_benchmark(handle, args)
            send_message(conn, "BENCHMARK_DONE")
            if recv_message(conn) != "ACK":
                raise RuntimeError("rank 0 did not acknowledge completion")
    finally:
        handle.leave()
        handle.destroy()


def validate_args(args):
    if min(args.packet_sizes) <= 0:
        raise ValueError("packet sizes must be positive")
    if args.stride < max(args.packet_sizes):
        raise ValueError(f"stride {args.stride} is smaller than maximum packet size {max(args.packet_sizes)}")
    if args.loops <= 0 or args.repeats <= 0 or args.warmup < 0:
        raise ValueError("loops/repeats must be positive and warmup must not be negative")
    if min(args.concurrency) <= 0:
        raise ValueError("concurrency values must be positive")
    args.buffer_size = max(args.packet_counts) * args.stride
    remote_buffer_size = max(args.concurrency) * args.buffer_size
    args.pool_size = max(MIN_POOL_SIZE, align_up(remote_buffer_size, 2 << 20))


def parse_args():
    parser = argparse.ArgumentParser(description="Host URMA discrete small-packet write bandwidth test")
    parser.add_argument("--rank", type=int, required=True, choices=(0, 1))
    parser.add_argument("--head-ip", required=True)
    parser.add_argument("--eid", required=True, help="32-character local Host URMA EID")
    parser.add_argument("--packet-sizes", type=int, nargs="+", default=list(DEFAULT_PACKET_SIZES))
    parser.add_argument(
        "--packet-counts",
        type=parse_count,
        nargs="+",
        default=list(DEFAULT_PACKET_COUNTS),
        help="packet count submitted by each worker",
    )
    parser.add_argument(
        "--concurrency",
        type=int,
        nargs="+",
        default=list(DEFAULT_CONCURRENCY),
        help="concurrent copy_data_batch worker counts",
    )
    parser.add_argument("--stride", type=int, default=DEFAULT_STRIDE)
    parser.add_argument("--loops", type=int, default=10, help="full packet-set loops in each timed repeat")
    parser.add_argument("--repeats", type=int, default=3, help="timed repeats; maximum bandwidth is reported")
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--no-verify", dest="verify", action="store_false")
    parser.set_defaults(verify=True)
    parser.add_argument("--store-port", type=int, default=8572)
    parser.add_argument("--data-port", type=int, default=10005)
    parser.add_argument("--control-port", type=int, default=9876)
    return parser.parse_args()


def main():
    args = parse_args()
    validate_args(args)
    os.environ["MF_HOST_URMA_EID"] = args.eid
    mf.set_log_level(3)
    ret = mf.initialize()
    if ret != 0:
        raise RuntimeError(f"mf.initialize failed, rank={args.rank}, ret={ret}")
    bm_initialized = False
    try:
        initialize_bm(args)
        bm_initialized = True
        (run_rank0 if args.rank == 0 else run_rank1)(args)
    finally:
        if bm_initialized:
            bm.uninitialize(0)
        mf.uninitialize()


if __name__ == "__main__":
    main()
