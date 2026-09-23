#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
"""MF prepares and polls; offload copies. Pure CPU HOST_RDMA sparse-copy benchmark."""

import argparse
import ctypes
import json
import math
import multiprocessing as mp
import os
from pathlib import Path
import select
import shlex
import socket
import sys
import time

DEFAULT_COUNTS = [100, 200, 300, 400, 600, 800, 1200, 1600, 2400, 3200, 4800, 6400, 9600, 12800, 19200, 25600]
DEFAULT_SIZES = [656, 1024]
MODES = ("baseline", "cont", "gather")
MIB = 1024 * 1024


def align(value, boundary=4096):
    return (value + boundary - 1) // boundary * boundary


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--role", choices=("local", "remote"), required=True)
    p.add_argument("--store-url", required=True, help="same MF config store URL on both hosts")
    p.add_argument("--hcom-url", required=True, help="this host's RDMA URL(s), separated by semicolons")
    p.add_argument("--control-host", required=True, help="local rank 0 control address reachable by remote rank 1")
    p.add_argument("--control-port", type=int, default=18581)
    p.add_argument("--counts", "--segments", nargs="+", type=int, help=f"default: {DEFAULT_COUNTS}")
    p.add_argument("--sizes", "--segment-bytes", nargs="+", type=int, help="default: 656, 1024 bytes")
    p.add_argument("--matrix", action="store_true", help="compatibility flag; the default already runs the matrix")
    p.add_argument("--stride", type=int, help="default: twice each case's block size")
    p.add_argument("--rounds", type=int, default=1000)
    p.add_argument("--warmup", "--warmup-rounds", type=int, default=10)
    p.add_argument("--chunk", type=int, default=128, help="cont watermark interval in blocks per link")
    p.add_argument("--gather-threads", type=int, default=16)
    p.add_argument("--gather-cpus", help="remote gather worker CPUs, e.g. 0-15 or 0-7,16-23; default: auto-select")
    p.add_argument("--scatter-threads", type=int, default=6)
    p.add_argument("--dram-mb", type=int, help="per-rank pool capacity; otherwise calculated automatically")
    p.add_argument("--poll-timeout-ms", type=int, default=100, help="remote busy-poll idle deadline; 0 checks once")
    p.add_argument("--timeout", type=float, default=600, help="whole suite watchdog, seconds")
    p.add_argument("--round-timeout", type=float, default=30, help="copy protocol wait timeout, seconds")
    p.add_argument("--log-level", type=int, choices=range(5), default=3)
    p.add_argument("--env-file", type=Path, help="literal KEY=VALUE lines; no shell evaluation")
    p.add_argument("--stats-file", type=Path, help="default: host_rdma_<role>.json")
    a = p.parse_args(argv)
    a.counts = sorted(set(a.counts or DEFAULT_COUNTS))
    a.sizes = sorted(set(a.sizes or DEFAULT_SIZES))
    a.links = len(a.hcom_url.split(";"))
    validate_args(p, a)
    return a


def validate_args(parser, a):
    if min(a.counts) < 1 or max(a.counts) > 2**32 - 1 or min(a.sizes) < 4:
        parser.error("counts must be in [1, UINT32_MAX]; sizes must be >= 4 for identity verification")
    if a.rounds < 1 or a.warmup < 0 or not 1 <= a.chunk <= 2**32 - 1:
        parser.error("rounds/chunk must be positive; warmup must be nonnegative")
    if not all(1 <= n <= 64 for n in (a.gather_threads, a.scatter_threads, a.links)):
        parser.error("gather/scatter threads and links must be in [1, 64]")
    if a.stride is not None and a.stride < max(a.sizes):
        parser.error("stride must be >= the largest block size")
    if a.dram_mb is not None and (a.dram_mb <= 0 or a.dram_mb % 2):
        parser.error("dram-mb must be a positive multiple of 2")
    if not all(math.isfinite(t) and t > 0 for t in (a.timeout, a.round_timeout)):
        parser.error("timeouts must be finite and positive")
    if a.round_timeout * 1000 > 2**32 - 1 or not 0 <= a.poll_timeout_ms <= 60000:
        parser.error("round-timeout exceeds UINT32_MAX ms or poll-timeout-ms is outside [0, 60000]")
    if not 1 <= a.control_port <= 65535 or not all(url.strip() for url in a.hcom_url.split(";")):
        parser.error("invalid control port or empty HCOM URL")


def load_env(path):
    if path is not None:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            parts = shlex.split(line, comments=True)
            if not parts:
                continue
            if parts[0] == "export":
                parts = parts[1:]
            if len(parts) != 1 or "=" not in parts[0]:
                raise ValueError(f"invalid env assignment at {path}:{number}")
            key, value = parts[0].split("=", 1)
            if not key.isidentifier():
                raise ValueError(f"invalid env name at {path}:{number}")
            os.environ[key] = value
    os.environ.setdefault("MF_HYBM_ENABLE_4K_PAGE", "1")
    mode = os.environ.get("MF_HOST_RDMA_SPARSE_MODE")
    if mode not in MODES:
        raise ValueError("set MF_HOST_RDMA_SPARSE_MODE=baseline|cont|gather on both hosts")
    return mode


def connect_control(a):
    if a.role == "local":
        with socket.create_server((a.control_host, a.control_port)) as listener:
            listener.settimeout(a.timeout)
            conn, _ = listener.accept()
    else:
        deadline = time.monotonic() + a.timeout
        while True:
            try:
                conn = socket.create_connection((a.control_host, a.control_port), timeout=2)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError("cannot connect to local control listener") from None
                time.sleep(0.1)
    conn.settimeout(a.timeout)
    return conn


class Control:
    """Preserve partial TCP messages while interleaving receive(0) with MF polling."""

    def __init__(self, conn, timeout):
        self.conn, self.timeout, self.pending = conn, timeout, bytearray()

    def send(self, payload):
        self.conn.sendall(json.dumps(payload).encode() + b"\n")

    def receive(self, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while b"\n" not in self.pending:
            readable, _, _ = select.select([self.conn], [], [], max(0, deadline - time.monotonic()))
            if not readable:
                return None
            chunk = self.conn.recv(65536)
            if not chunk:
                raise RuntimeError("peer disconnected before the control handshake completed")
            self.pending.extend(chunk)
            if len(self.pending) > MIB:
                raise RuntimeError("control message exceeds 1 MiB")
            if b"\n" not in self.pending and time.monotonic() >= deadline:
                return None
        line, _, rest = self.pending.partition(b"\n")
        self.pending = bytearray(rest)
        payload = json.loads(line)
        if not isinstance(payload, dict):
            raise RuntimeError("invalid control message")
        return payload

    def exchange(self, payload):
        self.send(payload)
        received = self.receive()
        if received != payload:
            raise RuntimeError(f"control handshake mismatch/timeout: expected {payload}, received {received}")


def checked(result, operation):
    if result != 0:
        raise RuntimeError(f"{operation} failed: ret={result}")


def pattern(index, size):
    return (index + 1).to_bytes(4, "little") + bytes([(index + 1) & 255]) * (size - 4)


def verify_blocks(base, count, size, stride):
    for index in range(count):
        if ctypes.string_at(base + index * stride, size) != pattern(index, size):
            raise RuntimeError(f"VERIFY FAIL: block={index}, size={size}, stride={stride}")


def summarize(values):
    values = sorted(value / 1000 for value in values)
    return {"avg_us": sum(values) / len(values), "min_us": values[0], "max_us": values[-1],
            **{f"p{p}_us": values[math.ceil(len(values) * p / 100) - 1] for p in (50, 95, 99)},
            "samples": len(values)}


class Benchmark:
    def __init__(self, a, handle, bm, offload, workspace_offset, workspace_bytes):
        self.a, self.handle, self.operation = a, handle, offload.sparse_copy_host_rdma
        self.read_timing = offload.host_rdma_sparse_last_timing
        self.sequence = 0
        rank = 0 if a.role == "local" else 1
        self.local = handle.peer_rank_ptr(rank, bm.BmMemType.HOST)
        self.peer = handle.peer_rank_ptr(1 - rank, bm.BmMemType.HOST)
        self.va = handle.gva_to_va(self.local, bm.BmMemType.LOCAL_HOST)
        if not all((self.local, self.peer, self.va)):
            raise RuntimeError("cannot resolve rank DRAM addresses")
        checked(handle.prepare_host_rdma_sparse(
            self.local + workspace_offset, workspace_bytes, max(a.counts), max(a.sizes),
            progress_interval=a.chunk, timeout_ms=math.ceil(a.round_timeout * 1000),
            gather_threads=a.gather_threads, scatter_threads=a.scatter_threads), "prepare_host_rdma_sparse")

    def timing(self):
        result = self.read_timing(self.handle)
        self.sequence += 1
        if result["sequence"] != self.sequence:
            raise RuntimeError("sparse timing sequence does not match the completed request")
        return result

    def prepare_data(self, case):
        self.sources = [self.peer + i * case["stride"] for i in range(case["count"])]
        self.targets = [self.local + i * case["stride"] for i in range(case["count"])]
        checked(self.handle.prepare_host_rdma_sparse_case(
            self.targets if self.a.role == "local" else [], case["count"], case["size"]),
            "prepare_host_rdma_sparse_case")
        for index in range(case["count"]):
            address = self.va + index * case["stride"]
            if self.a.role == "remote":
                ctypes.memmove(address, pattern(index, case["size"]), case["size"])
            else:
                ctypes.memset(address, 0xA5, case["size"])

    def copy_case(self, case):
        count, size, stride = case["count"], case["size"], case["stride"]
        samples = {name: [] for name in ("e2e", "request", "scatter")}
        for iteration in range(self.a.warmup + self.a.rounds):
            begin = time.perf_counter_ns()
            checked(self.operation(self.handle, self.sources, self.targets, size), "offload.sparse_copy_host_rdma")
            elapsed = time.perf_counter_ns() - begin
            timing = self.timing()  # Query outside E2E timing, before the next request.
            if iteration >= self.a.warmup:
                samples["e2e"].append(elapsed)
                samples["request"].append(timing["request_ns"])
                samples["scatter"].append(timing["scatter_ns"])
        verify_blocks(self.va, count, size, stride)  # Original benchmark: validate after all rounds.
        return {name: summarize(values) for name, values in samples.items()}

    def serve_case(self, case, control):
        processed, idle_polls = 0, 0
        samples = {name: [] for name in ("gather", "write")}
        while True:
            message = control.receive(0)
            if message is not None:
                if message.get("case_done") != case or not isinstance(message.get("metrics"), dict):
                    raise RuntimeError("unexpected control message while serving a case")
                if processed != self.a.warmup + self.a.rounds:
                    raise RuntimeError(f"request count mismatch: processed={processed}")
                remote = {"processed_requests": processed, "idle_polls": idle_polls}
                remote["timing"] = {name: summarize(values) for name, values in samples.items()}
                control.send({"case_done_ack": case, "remote": remote})
                return message["metrics"], remote
            # The application calls MF explicitly; no hidden polling thread exists.
            result = self.handle.poll_host_rdma_sparse(timeout_ms=self.a.poll_timeout_ms)
            if result not in (0, 1):
                raise RuntimeError(f"poll_host_rdma_sparse failed: ret={result}")
            processed += result
            idle_polls += result == 0
            if result == 1:
                timing = self.timing()
                if processed > self.a.warmup:
                    for name in samples:
                        samples[name].append(timing[f"{name}_ns"])


def suite_config(a):
    keys = ("mode", "counts", "sizes", "stride", "rounds", "warmup", "chunk", "links", "dram_mb", "store_url")
    return {"protocol": 3, "benchmark": "post-loop-verify-v1", **{key: getattr(a, key) for key in keys}}


def run_cases(a, bench, control):
    rows = []
    for size in a.sizes:
        for count in a.counts:
            case = {"mode": a.mode, "count": count, "size": size, "stride": a.stride or 2 * size}
            bench.prepare_data(case)
            control.exchange({"ready": case})
            if a.role == "local":
                local = bench.copy_case(case)
                control.send({"case_done": case, "metrics": local})
                ack = control.receive()
                if ack is None or ack.get("case_done_ack") != case:
                    raise RuntimeError("case completion acknowledgment missing")
                remote = ack["remote"]
                if remote["processed_requests"] != a.rounds + a.warmup:
                    raise RuntimeError("remote request count mismatch")
            else:
                local, remote = bench.serve_case(case, control)
            row = {**case, "local": local, "remote": remote, "verify": "OK"}
            rows.append(row)
            print(f"{a.mode:8} size={size:6} count={count:6} verify=OK "
                  f"E2E(us)={local['e2e']['avg_us']:.3f} p99={local['e2e']['p99_us']:.3f}", flush=True)
            write_results(a, rows)
    return rows


def format_table(title, headers, rows):
    cells = [[str(value) for value in row] for row in rows]
    widths = [max([len(header), *[len(row[i]) for row in cells]]) for i, header in enumerate(headers)]
    border = "+" + "+".join("-" * (width + 2) for width in widths) + "+"

    def line(values):
        return "| " + " | ".join(value.rjust(width) for value, width in zip(values, widths)) + " |"

    return title + "\n" + "\n".join([border, line(headers), border, *[line(row) for row in cells], border])


def print_summary(a, rows):
    averages, details = [], []
    keys = ("avg_us", "min_us", "max_us", "p50_us", "p95_us", "p99_us")
    for row in rows:
        local, remote = row["local"], row["remote"]["timing"]
        stages = [("E2E", local["e2e"]), ("request", local["request"]),
                  ("host gather", remote["gather"] if a.mode == "gather" else None),
                  ("host write", remote["write"]),
                  ("scatter", local["scatter"] if a.mode != "baseline" else None)]
        averages.append([row["size"], row["count"],
                         *[f"{metrics['avg_us']:.3f}" if metrics is not None else "-" for _, metrics in stages]])
        for name, metrics in stages:
            if metrics is not None:
                details.append([row["size"], row["count"], name, *[f"{metrics[key]:.3f}" for key in keys]])
    title = f"{a.mode} copy summary (E2E = Python/offload synchronous call; verify=PASS)"
    print("\n" + format_table(title, ("bytes/pkt", "packets", "E2E(us)", "request(us)",
                                     "host gather(us)", "host write(us)", "scatter(us)"), averages), flush=True)
    descriptions = (
        "Metric descriptions (overlapping stages must not be added as E2E):\n"
        "  E2E         : requester Python/offload call, including validation and pybind; "
        "excludes result verification.\n"
        "  request     : build and publish the address message and request doorbell.\n"
        "  host gather : original GatherAddresses call only, including dispatch and worker completion wait.\n"
        "  host write  : synchronous MF data copy/batch call; cont includes progress publishing.\n"
        "  scatter     : original Scatter call only; cont sums ready-chunk CPU copies only.\n"
        "  -           : stage does not apply to this mode. Warmup samples are excluded.\n"
        "  Stage clocks run on their own hosts; cont write/scatter overlap. "
        "E2E also includes unlisted waits/overhead.\n"
        "  Timing queries are outside E2E; native stage clock reads are inside the operation.\n"
    )
    path = Path(str(a.stats_file or Path(f"host_rdma_{a.role}.json")) + ".txt")
    path.write_text(format_table(title, ("bytes/pkt", "packets", "stage", "avg(us)", "min(us)", "max(us)",
                                        "P50(us)", "P95(us)", "P99(us)"), details)
                    + "\n\n" + descriptions, encoding="utf-8")
    print(f"Detailed latency statistics: {path.resolve()}", flush=True)


def write_results(a, rows, status="running"):
    path = a.stats_file or Path(f"host_rdma_{a.role}.json")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"config": suite_config(a), "role": a.role, "status": status,
                               "gather_threads": a.gather_threads, "scatter_threads": a.scatter_threads,
                               "poll_timeout_ms": a.poll_timeout_ms,
                               "timing": "Python/native offload E2E including pybind; validation excluded",
                               "results": rows}, indent=2) + "\n", encoding="utf-8")


def memory_layout(a, bm):
    # One application data region followed by one exclusive MF workspace; no hand-written protocol layout.
    data_bytes = (max(a.counts) - 1) * (a.stride or 2 * max(a.sizes)) + max(a.sizes)
    offset = align(data_bytes)
    workspace = bm.host_rdma_sparse_workspace_size(max(a.counts), max(a.sizes), a.links, a.chunk)
    if workspace == 0:
        raise ValueError("invalid workspace dimensions")
    required = align(offset + workspace, 2 * MIB)
    capacity = a.dram_mb * MIB if a.dram_mb else max(16 * MIB, required)
    if capacity < required or capacity > 2**64 - 1:
        raise ValueError(f"invalid dram-mb; need at least {required // MIB} MiB per rank")
    return offset, workspace, capacity


def pin_main_thread():
    value = os.environ.get("MF_BENCH_APP_CPU", "")
    if not value:
        return
    try:
        cpu = int(value)
        if cpu < 0:
            raise ValueError("CPU must be nonnegative")
        os.sched_setaffinity(0, {cpu})
        print(f"[bench] main thread pinned to cpu {cpu}", flush=True)
    except (ValueError, OSError) as error:
        print(f"[bench] cannot pin main thread with MF_BENCH_APP_CPU={value}: {error}; ignored", flush=True)


def run_connected(a, control):
    import memfabric_hybrid as mf
    from memfabric_hybrid import bm, offload

    offset, workspace, capacity = memory_layout(a, bm)
    rank = 0 if a.role == "local" else 1
    config = bm.BmConfig()
    config.auto_ranking, config.rank_id, config.start_store = False, rank, rank == 0
    config.set_nic(a.hcom_url)
    mf.set_log_level(a.log_level)
    checked(bm.initialize(a.store_url, 2, 0, config), "bm.initialize")
    handle = None
    try:
        handle = bm.create2(id=0, local_dram_size=capacity, max_dram_size=capacity,
                            data_op_type=bm.BmDataOpType.HOST_RDMA, enable_56bits_gva=False)
        checked(handle.join(), "join")
        bench = Benchmark(a, handle, bm, offload, offset, workspace)
        pin_main_thread()  # MF/HCOM threads already exist; per-case copy pools are created afterwards.
        print(f"rank={rank} mode={a.mode} HOST_RDMA ready; capacity={capacity // MIB} MiB, links={a.links}",
              flush=True)
        rows = run_cases(a, bench, control)
        control.exchange({"finished": True})  # All copies and poll calls have returned before either BM is destroyed.
        write_results(a, rows, "complete")
        print_summary(a, rows)
    finally:
        if handle is not None:
            handle.destroy()
        bm.uninitialize()


def worker(a):
    try:
        with connect_control(a) as conn:
            control = Control(conn, a.timeout)
            control.exchange(suite_config(a))
            run_connected(a, control)
    except Exception as error:
        print(f"[ERROR] {a.role}: {error}", file=sys.stderr, flush=True)
        raise SystemExit(1) from error


def main():
    a = parse_args()
    a.mode = load_env(a.env_file)
    if a.gather_cpus is not None:
        os.environ["MF_HOST_RDMA_GATHER_CPUS"] = a.gather_cpus
    write_results(a, [])
    process = mp.get_context("spawn").Process(target=worker, args=(a,))
    process.start()
    succeeded = False
    try:
        process.join(a.timeout)
        if process.is_alive():
            print(f"[ERROR] suite exceeded {a.timeout}s", file=sys.stderr, flush=True)
            return 1
        succeeded = process.exitcode == 0
        if not succeeded:
            print(f"[ERROR] {a.role} worker exited with code {process.exitcode}", file=sys.stderr, flush=True)
        return process.exitcode or 0
    finally:
        if process.is_alive():
            process.terminate()
            process.join(5)
        if process.is_alive():
            process.kill()
            process.join()
        if not succeeded:
            path = a.stats_file or Path(f"host_rdma_{a.role}.json")
            result = json.loads(path.read_text(encoding="utf-8"))
            result["status"] = "failed"
            path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("[ERROR] interrupted", file=sys.stderr)
        sys.exit(130)
