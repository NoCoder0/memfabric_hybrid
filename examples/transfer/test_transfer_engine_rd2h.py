#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/mulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""SFA RD2H 场景 PD KV Cache pull 传输测试（Decode 多 rank）。

对齐 vLLM SfaRemoteD2HConnector + SparseKVOffloadManager：P 侧 register_memory 只发布
元数据，D 侧各 rank 按 tp_block_range 均分的块区间经 TransferEngine 拉取写入共享 CPU 池
（SHARED 仅 rank0 分配，store url 与池基地址经 HCCL broadcast 同步），rank0 全池校验并
演示 sparse_copy D2H。KV 为 MLA main cache（bf16），默认 32 层 x 128 块。
用法：先启 Prefill 再启 Decode（--tp-size N 一条命令拉起全部 rank 子进程，或 --tp-rank r
单独启动）；测量轮 warmup+iterations 可调。
"""

import argparse
import contextlib
import ipaddress
import json
import logging
import os
import socket
import subprocess
import sys
import time
from collections import namedtuple

import torch
import torch.distributed as dist
import torch_npu  # noqa: F401  # side-effect import: registers the torch NPU backend
from memfabric_hybrid import TransferEngine, offload, set_conf_store_tls

CONTROL_PORT = 29598
DIST_PORT = 29599
CPU_BUFFER_ALIGNMENT = 2 * 1024 * 1024
GIB = 1024 * 1024 * 1024
MAX_CONTROL_MESSAGE_BYTES = 64 * 1024
MAX_REGISTER_REGIONS = 256  # 与生产一致：HCCL 注册 region 上限
DEFAULT_TIMEOUT_SECONDS = 120.0
ROLE_PREFILL = "Prefill"
ROLE_DECODE = "Decode"
LOG_LEVELS = {0: logging.DEBUG, 1: logging.INFO, 2: logging.WARNING, 3: logging.ERROR}

logger = logging.getLogger("test_transfer_engine_rd2h")
# 一层 K/V 分片的拉取规格：目标层、源布局、本 rank 拥有块区间等强相关参数打包为一个具名元组
LayerPullSpec = namedtuple("LayerPullSpec", "layer_idx layer_meta cpu_base p_block_ids owned")


def _ipv4(value):
    try:
        address = ipaddress.ip_address(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid IP address: {value}") from error
    if address.version != 4:
        raise argparse.ArgumentTypeError("MemFabric test requires a numeric IPv4 address")
    return str(address)


def _parse_args():
    parser = argparse.ArgumentParser(description="SFA RD2H PD KV cache pull transfer test (multi-rank Decode)")
    parser.add_argument(
        "--role", type=str, required=True, choices=["Decode", "Prefill"], help="Decode (consumer) or Prefill (producer)"
    )
    parser.add_argument("--local-ip", type=_ipv4, default="127.0.0.1", help="Numeric IPv4 of this machine")
    parser.add_argument("--prefill-ip", type=_ipv4, default="127.0.0.1", help="Prefill machine IPv4")
    parser.add_argument(
        "--npu-id", type=int, default=None, help="NPU device ID; Decode rank r uses start+r, else the device"
    )
    parser.add_argument("--control-port", type=int, default=CONTROL_PORT, help="TCP metadata channel port")
    parser.add_argument(
        "--dist-port", type=int, default=DIST_PORT, help="TCPStore port of the Decode HCCL group (rank0 hosts)"
    )
    parser.add_argument(
        "--tp-rank", type=int, default=None, help="Decode TP rank; if omitted in Decode, spawn all ranks as children"
    )
    parser.add_argument("--tp-size", type=int, default=1, help="Decode TP rank count (Decode role only)")
    parser.add_argument("--decode-tp-size", type=int, default=1, help="Decode ranks to expect (Prefill role only)")
    parser.add_argument("--num-layers", type=int, default=32, help="Number of KV cache layers")
    parser.add_argument("--num-blocks", type=int, default=128, help="KV blocks per layer")
    parser.add_argument("--block-size", type=int, default=128, help="Tokens per block")
    parser.add_argument("--kv-lora-rank", type=int, default=512, help="MLA kv_lora_rank (k dim)")
    parser.add_argument("--qk-rope-head-dim", type=int, default=64, help="MLA qk_rope_head_dim (v dim)")
    parser.add_argument("--seed", type=int, default=2026, help="Seed of deterministic KV payload")
    parser.add_argument("--dram-pool-gb", type=int, default=2, help="Decode shared offload DRAM pool size in GiB")
    parser.add_argument(
        "--log-level", type=int, default=1, choices=[0, 1, 2, 3], help="0 debug, 1 info, 2 warn, 3 error"
    )
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS, help="Control channel timeout")
    parser.add_argument("--iterations", type=int, default=3, help="Measured pull rounds (mean and best)")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup rounds excluded from measurement")
    args = parser.parse_args()
    return _validate_args(parser, args)


def _validate_args(parser, args):
    """解析后的语义校验：角色参数组合、取值范围与资源上限。"""
    if args.npu_id is None:
        args.npu_id = args.tp_rank if args.role == ROLE_DECODE and args.tp_rank is not None else 0
    if args.role == ROLE_DECODE:
        if args.tp_rank is not None and not 0 <= args.tp_rank < args.tp_size:
            parser.error("--tp-rank must be in [0, --tp-size)")
    if args.role == ROLE_PREFILL and args.tp_rank is not None:
        parser.error("--tp-rank only applies to Decode role")
    if args.role == ROLE_PREFILL and args.decode_tp_size <= 0:
        parser.error("--decode-tp-size must be positive")
    if args.num_layers * 2 > MAX_REGISTER_REGIONS:
        parser.error(f"num_layers*2 must be <= {MAX_REGISTER_REGIONS} (HCCL register region limit)")
    for name in ("num_layers", "num_blocks", "block_size", "kv_lora_rank", "qk_rope_head_dim"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.iterations <= 0 or args.warmup < 0:
        parser.error("--iterations must be positive and --warmup must be non-negative")
    return args


def kv_layout(args):
    """MLA/SFA main KV cache 布局与单块字节数（bf16）。"""
    k_shape = (args.num_blocks, args.block_size, 1, args.kv_lora_rank)
    v_shape = (args.num_blocks, args.block_size, 1, args.qk_rope_head_dim)
    k_block_len = 2 * args.block_size * args.kv_lora_rank
    v_block_len = 2 * args.block_size * args.qk_rope_head_dim
    return k_shape, v_shape, k_block_len, v_block_len


def make_expected_kv(args, layer):
    """按固定 seed 在 CPU 上生成期望 KV，P/D 两侧可独立重现用于校验。"""
    k_shape, v_shape, _, _ = kv_layout(args)
    gen_k = torch.Generator().manual_seed(args.seed * 100003 + layer)
    gen_v = torch.Generator().manual_seed(args.seed * 100003 + layer + 500000)
    k = torch.randn(k_shape, generator=gen_k, dtype=torch.float32).to(torch.bfloat16)
    v = torch.randn(v_shape, generator=gen_v, dtype=torch.float32).to(torch.bfloat16)
    return k, v


def make_block_table(args):
    """模拟 vLLM block table：P 侧物理块 id 的非连续访问顺序（seed 确定）。"""
    gen = torch.Generator().manual_seed(args.seed + 7)
    return torch.randperm(args.num_blocks, generator=gen).tolist()


def send_json(sock, message):
    sock.sendall(json.dumps(message).encode() + b"\n")


def recv_json(sock):
    payload = bytearray()
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("control connection closed before a complete message")
        payload.extend(chunk)
        if len(payload) > MAX_CONTROL_MESSAGE_BYTES:
            raise ValueError("control message too large")
        newline = payload.find(b"\n")
        if newline >= 0:
            return json.loads(payload[:newline])


def expect_phase(message, phase):
    """校验控制消息 phase；对端报错时直接抛出对端错误。"""
    if isinstance(message, dict) and message.get("phase") == "error":
        raise RuntimeError(f"peer reported error: {message.get('error', message)}")
    got = message.get("phase") if isinstance(message, dict) else type(message).__name__
    if got != phase:
        raise RuntimeError(f"protocol error: expected phase {phase!r}, got {got!r}: {message}")


def connect_with_retry(host, port, timeout):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            remaining = max(deadline - time.monotonic(), 0.1)
            return socket.create_connection((host, port), timeout=min(5.0, remaining))
        except OSError as error:
            last_error = error
            time.sleep(0.5)
    raise TimeoutError(f"timed out connecting to {host}:{port}: {last_error}")


def tp_block_range(num_blocks, tp_rank, tp_size, start_block=0):
    """对应生产 read_thread._tp_block_range：按块均分，并按 start_block 轮转首 owner。"""
    logical_rank = (tp_rank - start_block % tp_size) % tp_size
    blocks_per_rank, remainder = divmod(num_blocks, tp_size)
    start = logical_rank * blocks_per_rank + min(logical_rank, remainder)
    count = blocks_per_rank + int(logical_rank < remainder)
    return start, start + count


def init_transfer_engine(role, local_ip, args):
    """对齐 vllm-ascend GlobalMemfabricTE：Prefill 侧承担 config store server，session 为 "{ip}:{port}"。"""
    set_conf_store_tls(False, "")
    engine = TransferEngine()
    ret = engine.initialize(f"tcp://{local_ip}", local_ip, role, args.npu_id, store_server_role=ROLE_PREFILL)
    if ret != 0:
        raise RuntimeError(f"TransferEngine initialize failed, ret={ret}, role={role}, localIp={local_ip}")
    session = f"{local_ip}:{engine.get_rpc_port()}"
    tag = f"[D{args.tp_rank} engine]" if role == ROLE_DECODE else "[P engine]"
    logger.info(f"{tag} role={role} session={session}")
    return engine, session


# ---------------------------- Prefill (producer) ----------------------------


def build_prefill_kv_caches(args):
    """P 侧模拟 prefill 产出每层 KV cache（HBM bf16）。"""
    caches = []
    for layer in range(args.num_layers):
        k_cpu, v_cpu = make_expected_kv(args, layer)
        caches.append((k_cpu.to("npu"), v_cpu.to("npu")))
    torch.npu.synchronize()
    return caches


def register_prefill_caches(engine, caches, args):
    """仅 P 侧注册源内存（生产 pull 路径：D 侧目的地为共享池，无需注册）。"""
    _, _, k_block_len, v_block_len = kv_layout(args)
    if len(caches) * 2 > MAX_REGISTER_REGIONS:
        raise RuntimeError(f"register regions {len(caches) * 2} exceed limit {MAX_REGISTER_REGIONS}")
    for layer, (k_cache, v_cache) in enumerate(caches):
        for name, cache, block_len in (("k", k_cache, k_block_len), ("v", v_cache, v_block_len)):
            ret = engine.register_memory(cache.data_ptr(), block_len * args.num_blocks)
            if ret != 0:
                raise RuntimeError(f"register_memory failed, ret={ret}, layer={layer}, tensor={name}")
    logger.info(f"[P] registered {len(caches) * 2} KV regions over {len(caches)} layers")


def build_prefill_meta(session, caches, args):
    """构建下发 D 侧的元数据（对应生产协议的 LayerMetadata + block table）。"""
    _, _, k_block_len, v_block_len = kv_layout(args)
    layers = []
    for k_cache, v_cache in caches:
        layers.append(
            {
                "k_base": int(k_cache.data_ptr()),
                "v_base": int(v_cache.data_ptr()),
                "k_block_len": k_block_len,
                "v_block_len": v_block_len,
            }
        )
    return {
        "session": session,
        "seed": args.seed,
        "num_layers": args.num_layers,
        "num_blocks": args.num_blocks,
        "block_size": args.block_size,
        "kv_lora_rank": args.kv_lora_rank,
        "qk_rope_head_dim": args.qk_rope_head_dim,
        "k_block_len": k_block_len,
        "v_block_len": v_block_len,
        "p_block_ids": make_block_table(args),
        "layers": layers,
    }


def accept_decode_ranks(server, args):
    """收集全部 Decode rank 的连接与 hello。"""
    conns = [None] * args.decode_tp_size
    for _ in range(args.decode_tp_size):
        conn, peer = server.accept()
        conn.settimeout(args.timeout)
        hello = recv_json(conn)
        rank = hello.get("tp_rank") if isinstance(hello, dict) else None
        valid = (
            isinstance(hello, dict)
            and hello.get("phase") == "hello"
            and isinstance(rank, int)
            and 0 <= rank < args.decode_tp_size
        )
        if not valid or conns[rank] is not None:
            conn.close()
            raise RuntimeError(f"invalid Decode hello from {peer}: {hello}")
        conns[rank] = conn
        logger.info(f"[P] Decode rank {rank} connected from {peer[0]}:{peer[1]}")
    if any(conn is None for conn in conns):
        raise RuntimeError(f"not all Decode ranks connected, expected 0..{args.decode_tp_size - 1}")
    return conns


def exchange_pool_and_meta(conns, meta, args):
    """各 rank 并行初始化共享池与 HCCL 组；rank0 上报 pool_ready 后下发 meta（池基地址经 D 侧 HCCL broadcast 同步）。"""
    for rank in range(args.decode_tp_size):
        send_json(conns[rank], {"phase": "init", "tp_size": args.decode_tp_size})
    expect_phase(recv_json(conns[0]), "pool_ready")
    meta["phase"] = "meta"
    for rank in range(args.decode_tp_size):
        send_json(conns[rank], meta)


def collect_pull_done(conns, args):
    """等待所有 rank 完成分片拉取，返回单轮总字节与各测量轮的最慢 rank 纯传输用时。"""
    total_bytes = 0
    per_rank_transfer = []
    for rank in range(args.decode_tp_size):
        done = recv_json(conns[rank])
        expect_phase(done, "pull_done")
        if not done.get("ok"):
            raise RuntimeError(f"Decode rank {rank} pull failed: {done.get('error', done)}")
        total_bytes += int(done["bytes"])
        transfer = [float(t) for t in done.get("transfer_seconds", [])]
        desc = [float(t) for t in done.get("desc_seconds", [])]
        per_rank_transfer.append(transfer)
        n_iter = len(transfer)
        desc_mean = sum(desc) / n_iter if n_iter else 0.0
        transfer_mean = sum(transfer) / n_iter if n_iter else 0.0
        seg_bytes = int(done["bytes"]) / int(done["descriptors"]) if int(done["descriptors"]) else 0
        merge_ratio = float(done.get("merge_ratio", 0.0))
        logger.info(
            f"[P] rank {rank} pulled {int(done['bytes']) / GIB:.2f} GiB/round, merge ratio {merge_ratio:.2f} "
            f"({done['descriptors']} pieces, avg {seg_bytes / 1024:.1f} KiB each, "
            f"build {desc_mean:.3f}s + transfer {transfer_mean:.3f}s per round)"
        )
    iterations = len(per_rank_transfer[0])
    iter_slowest = [max(times[i] for times in per_rank_transfer) for i in range(iterations)]
    return total_bytes, iter_slowest


def serve_decode_ranks(server, meta, args):
    """驱动完整控制协议：hello → init → pool_ready → meta → pull_done → verify → final。"""
    conns = []
    try:
        conns = accept_decode_ranks(server, args)
        exchange_pool_and_meta(conns, meta, args)
        total_bytes, iter_slowest = collect_pull_done(conns, args)

        send_json(conns[0], {"phase": "verify"})
        result = recv_json(conns[0])
        expect_phase(result, "result")
        if not result.get("ok"):
            raise RuntimeError(f"Decode rank 0 verification failed: {result.get('error', result)}")

        bandwidths = [total_bytes / GIB / seconds if seconds > 0 else 0.0 for seconds in iter_slowest]
        final = {
            "phase": "final",
            "ok": True,
            "total_bytes": total_bytes,
            "iterations": len(bandwidths),
            "bandwidth_gibps": sum(bandwidths) / len(bandwidths),
            "best_bandwidth_gibps": max(bandwidths),
            "slowest_transfer_seconds": max(iter_slowest),
            "d2h_demo_ok": bool(result.get("d2h_demo_ok", False)),
        }
        for rank in range(args.decode_tp_size):
            send_json(conns[rank], final)
        for rank in range(args.decode_tp_size):
            cleanup = recv_json(conns[rank])
            expect_phase(cleanup, "cleanup_done")
            if not cleanup.get("ok"):
                raise RuntimeError(f"Decode rank {rank} cleanup failed: {cleanup.get('error', cleanup)}")
        return final
    except Exception as error:
        with contextlib.suppress(OSError):
            for conn in conns:
                send_json(conn, {"phase": "final", "ok": False, "error": f"{type(error).__name__}: {error}"})
        raise
    finally:
        for conn in conns:
            conn.close()


def run_prefill_role(args):
    caches = build_prefill_kv_caches(args)
    # 先占住控制端口，再初始化引擎，避免引擎自动选端口时撞上控制端口
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.local_ip, args.control_port))
        server.listen(args.decode_tp_size)
        server.settimeout(args.timeout)

        engine, session = init_transfer_engine(ROLE_PREFILL, args.local_ip, args)
        register_prefill_caches(engine, caches, args)
        meta = build_prefill_meta(session, caches, args)
        logger.info(
            f"[P] ready: session={session}, control={args.local_ip}:{args.control_port}, "
            f"decode_tp_size={args.decode_tp_size}, waiting for Decode ranks..."
        )
        final = serve_decode_ranks(server, meta, args)

    # 先释放源 HBM tensor 并归还设备内存，再反初始化引擎，避免退出时 VA 清理告警
    del caches
    torch.npu.synchronize()
    torch.npu.empty_cache()

    engine.unInitialize()
    logger.info(
        f"[P] PASS: layers={args.num_layers}, decode_ranks={args.decode_tp_size}, "
        f"bytes={final['total_bytes'] / GIB:.2f} GiB/round, iterations={final['iterations']}, "
        f"transfer_bw={final['bandwidth_gibps']:.2f} GiB/s (best {final['best_bandwidth_gibps']:.2f}), "
        f"d2h_demo_ok={final['d2h_demo_ok']}"
    )


# ---------------------------- Decode (consumer) ----------------------------


def probe_free_port(host="127.0.0.1"):
    """让系统分配一个空闲 TCP 端口（bind 0 后读回，与 C++ AccFindAvailableTcpPort 同思路）。"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return sock.getsockname()[1]


def resolve_shared_store_url(args):
    """共享池 ConfigStore url：rank0 探测空闲端口经 HCCL broadcast 分发，绕开 C++ 默认端口推导，支持任意起始卡号。"""
    if args.tp_size == 1:
        return f"tcp://127.0.0.1:{probe_free_port()}"
    port = torch.zeros(1, dtype=torch.int64, device="npu")
    if args.tp_rank == 0:
        port.fill_(probe_free_port())
    dist.broadcast(port, src=0)
    return f"tcp://127.0.0.1:{int(port.item())}"


def init_offload_pool(args, store_url):
    """对齐 SparseKVOffloadManager：SHARED 场景仅 rank0 分配物理 DRAM，全组以显式 store_url 初始化（内部 GroupBarrier 会合）。"""
    config = offload.OffloadConfig()
    config.device_id = args.npu_id
    config.reserve_size = args.dram_pool_gb * GIB
    config.alloc_size = args.dram_pool_gb * GIB if args.tp_rank == 0 else 0
    config.world_size = args.tp_size
    config.rank_id = args.tp_rank
    config.scene = offload.Scene.SHARED
    config.store_url = store_url
    ret = offload.initialize(config)
    if ret != 0:
        raise RuntimeError(
            f"offload.initialize failed, ret={ret}, rankId={args.tp_rank}, "
            f"worldSize={args.tp_size}, storeUrl={store_url}"
        )
    logger.info(f"[D{args.tp_rank}] shared pool store: {store_url}")


def alloc_cpu_kv_pool(shape, num_bytes):
    """从共享池分配 2MiB 对齐的 CPU KV cache（对齐 manager 的对齐约定）。"""
    backing = offload.empty([num_bytes + CPU_BUFFER_ALIGNMENT], dtype=torch.uint8, pin_memory=True)
    offset = (-backing.data_ptr()) % CPU_BUFFER_ALIGNMENT
    pool = backing[offset:offset + num_bytes].view(torch.bfloat16).reshape(shape)  # fmt: skip
    if pool.data_ptr() % CPU_BUFFER_ALIGNMENT != 0:
        raise RuntimeError(f"failed to align offload buffer to {CPU_BUFFER_ALIGNMENT} bytes")
    return backing, pool


def build_decode_cpu_pools(args):
    """仅 rank0 分配每层 CPU 池 K/V（对应 manager 中 tp_rank==0 才持有 CPU 张量）。"""
    k_shape, v_shape, k_block_len, v_block_len = kv_layout(args)
    pools = []
    for layer in range(args.num_layers):
        backing_k, k_pool = alloc_cpu_kv_pool(k_shape, k_block_len * args.num_blocks)
        backing_v, v_pool = alloc_cpu_kv_pool(v_shape, v_block_len * args.num_blocks)
        pools.append((k_pool, v_pool, backing_k, backing_v))
    logger.info(
        f"[D{args.tp_rank}] allocated {args.num_layers} layer cpu pools, "
        f"k0={hex(pools[0][0].data_ptr())}, v0={hex(pools[0][1].data_ptr())}"
    )
    return pools


def validate_layout(args, meta):
    """模拟 read_thread 的布局一致性校验：P/D 的 KV 布局参数必须一致。"""
    _, _, k_block_len, v_block_len = kv_layout(args)
    checks = (
        ("num_layers", args.num_layers),
        ("num_blocks", args.num_blocks),
        ("block_size", args.block_size),
        ("kv_lora_rank", args.kv_lora_rank),
        ("qk_rope_head_dim", args.qk_rope_head_dim),
        ("k_block_len", k_block_len),
        ("v_block_len", v_block_len),
    )
    for key, expected in checks:
        if int(meta[key]) != expected:
            raise RuntimeError(f"KV layout mismatch on {key}: P={meta[key]} D={expected}")
    if len(meta["p_block_ids"]) != args.num_blocks:
        raise RuntimeError(f"block table size mismatch: P={len(meta['p_block_ids'])} D={args.num_blocks}")


def coalesce_descriptors(descriptors):
    """合并连续块为大段传输（对应生产 read_thread 的 _coalesce_desc）。"""
    merged = []
    for peer, local, length in descriptors:
        if merged and merged[-1][0] + merged[-1][2] == peer and merged[-1][1] + merged[-1][2] == local:
            merged[-1][2] += length
        else:
            merged.append([peer, local, length])
    return merged


def build_layer_descriptors(layer_meta, cpu_base, p_block_ids, owned):
    """构造一层 K/V 的读描述符，仅覆盖本 rank 拥有的块区间 owned=[start, end)。"""
    start, end = owned
    descriptors = []
    specs = (
        (layer_meta["k_base"], cpu_base["k"], layer_meta["k_block_len"]),
        (layer_meta["v_base"], cpu_base["v"], layer_meta["v_block_len"]),
    )
    for peer_base, local_base, block_len in specs:
        for dst_idx in range(start, end):
            src_idx = p_block_ids[dst_idx]
            descriptors.append((peer_base + src_idx * block_len, local_base + dst_idx * block_len, block_len))
    return coalesce_descriptors(descriptors)


def pull_owned_layer(engine, p_session, spec):
    """按 owned 块区间拉取一层 K/V 分片写入共享 CPU 池，描述符构建与传输分段计时。"""
    tick = time.monotonic()
    descriptors = build_layer_descriptors(spec.layer_meta, spec.cpu_base, spec.p_block_ids, spec.owned)
    local_ptrs = [desc[1] for desc in descriptors]
    peer_ptrs = [desc[0] for desc in descriptors]
    lengths = [desc[2] for desc in descriptors]
    desc_seconds = time.monotonic() - tick

    tick = time.monotonic()
    ret = engine.batch_transfer_sync_read(p_session, local_ptrs, peer_ptrs, lengths)
    transfer_seconds = time.monotonic() - tick
    if ret != 0:
        raise RuntimeError(f"batch_transfer_sync_read failed, ret={ret}, session={p_session}, layer={spec.layer_idx}")
    return sum(lengths), len(descriptors), desc_seconds, transfer_seconds


def init_tp_group(args):
    """对齐生产 tp_group：D ranks 间建立 HCCL 进程组（单 rank 时跳过）。"""
    if args.tp_size == 1:
        return
    dist.init_process_group(
        backend="hccl",
        init_method=f"tcp://{args.local_ip}:{args.dist_port}",
        world_size=args.tp_size,
        rank=args.tp_rank,
    )
    logger.info(f"[D{args.tp_rank}] hccl tp group ready: worldSize={args.tp_size}")


def broadcast_cpu_bases(pools, args):
    """rank0 的 CPU 池基地址经 HCCL 广播（等价生产 tp_group.broadcast(gvas)，单 rank 直接返回本地值）。"""
    if args.tp_size == 1:
        return [{"k": int(k.data_ptr()), "v": int(v.data_ptr())} for k, v, _, _ in pools]
    flat = torch.zeros(args.num_layers * 2, dtype=torch.int64, device="npu")
    if args.tp_rank == 0:
        for layer, (k_pool, v_pool, _, _) in enumerate(pools):
            flat[2 * layer] = int(k_pool.data_ptr())
            flat[2 * layer + 1] = int(v_pool.data_ptr())
    dist.broadcast(flat, src=0)
    values = flat.cpu().tolist()
    return [{"k": values[2 * layer], "v": values[2 * layer + 1]} for layer in range(args.num_layers)]


def decode_shared_pool_handshake(conn, args):
    """收到 init 后建 HCCL 组并初始化共享池；rank0 分配 CPU 池并广播基地址。"""
    message = recv_json(conn)
    expect_phase(message, "init")
    if int(message.get("tp_size", -1)) != args.tp_size:
        raise RuntimeError(f"tp_size mismatch: prefill={message.get('tp_size')} local={args.tp_size}")
    init_tp_group(args)
    store_url = resolve_shared_store_url(args)
    init_offload_pool(args, store_url)
    pools = build_decode_cpu_pools(args) if args.tp_rank == 0 else None
    cpu_bases = broadcast_cpu_bases(pools, args)
    if args.tp_rank == 0:
        send_json(conn, {"phase": "pool_ready"})
    return pools, cpu_bases


def decode_pull_owned_slices(engine, meta, cpu_bases, owned, args):
    """本 rank 单轮拉取 owned 块区间的所有层分片，返回 (bytes, 描述符数, 构建秒数, 传输秒数)。"""
    total_bytes = 0
    total_desc = 0
    desc_seconds = 0.0
    transfer_seconds = 0.0
    for layer, layer_meta in enumerate(meta["layers"]):
        layer_bytes, n_desc, d_sec, t_sec = pull_owned_layer(
            engine,
            meta["session"],
            LayerPullSpec(layer, layer_meta, cpu_bases[layer], meta["p_block_ids"], owned),
        )
        total_bytes += layer_bytes
        total_desc += n_desc
        desc_seconds += d_sec
        transfer_seconds += t_sec
    return total_bytes, total_desc, desc_seconds, transfer_seconds


def decode_pull_iterations(engine, meta, cpu_bases, owned, args):
    """warmup 轮 + 测量轮；返回 (单轮字节, 单轮描述符数, 各测量轮传输秒数, 各测量轮构建秒数)。"""
    iter_transfer = []
    iter_desc = []
    total_bytes = 0
    total_desc = 0
    for round_idx in range(args.warmup + args.iterations):
        quiet = round_idx < args.warmup
        total_bytes, total_desc, desc_seconds, transfer_seconds = decode_pull_owned_slices(
            engine, meta, cpu_bases, owned, args
        )
        if quiet:
            logger.info(f"[D{args.tp_rank}] warmup round {round_idx + 1}/{args.warmup} done")
            continue
        iter_desc.append(desc_seconds)
        iter_transfer.append(transfer_seconds)
        logger.info(
            f"[D{args.tp_rank}] round {round_idx + 1} pulled {total_bytes / GIB:.2f} GiB in "
            f"{desc_seconds * 1000:.2f}ms build + {transfer_seconds * 1000:.2f}ms transfer"
        )
    return total_bytes, total_desc, iter_transfer, iter_desc


def verify_shared_pool(pools, p_block_ids, args):
    """rank0 对整个共享 CPU 池逐层校验（覆盖所有 rank 写入的分片）。"""
    for layer, (k_pool, v_pool, _, _) in enumerate(pools):
        expected_k, expected_v = make_expected_kv(args, layer)
        if not torch.equal(k_pool, expected_k[p_block_ids]):
            raise RuntimeError(f"layer {layer} k cache mismatch in shared pool after multi-rank pull")
        if not torch.equal(v_pool, expected_v[p_block_ids]):
            raise RuntimeError(f"layer {layer} v cache mismatch in shared pool after multi-rank pull")


def demo_offload_new_kv(k_pool, args):
    """模拟 SparseKVOffloadManager.offload_new_kv：decode 新增 KV 经 sparse_copy 直写 CPU 池。"""
    num_new = min(4, args.block_size)
    token_bytes = 2 * args.kv_lora_rank
    gen = torch.Generator().manual_seed(args.seed + 99)
    new_k = torch.randn((num_new, args.kv_lora_rank), generator=gen, dtype=torch.float32).to(torch.bfloat16)
    new_k_npu = new_k.to("npu")

    src_ptrs = torch.arange(num_new, dtype=torch.int64, device="npu") * token_bytes + new_k_npu.data_ptr()
    dst_ptrs = torch.arange(num_new, dtype=torch.int64, device="npu") * token_bytes + k_pool.data_ptr()
    lengths = torch.full((num_new,), token_bytes, dtype=torch.int32, device="npu")
    counts = torch.tensor([num_new], dtype=torch.int32, device="npu")
    ret = offload.sparse_copy(src_ptrs, dst_ptrs, lengths, counts, torch.device("npu", args.npu_id))
    if ret != 0:
        raise RuntimeError(f"sparse_copy d2h failed, ret={ret}, deviceId={args.npu_id}")
    torch.npu.synchronize()

    flat = k_pool.reshape(-1, args.kv_lora_rank)
    if not torch.equal(flat[:num_new], new_k):
        raise RuntimeError("sparse_copy d2h verification mismatch on cpu kv pool")


def run_d2h_demo(k_pool, args):
    """运行 D2H 演示，失败仅告警不中断（演示性质）。"""
    try:
        demo_offload_new_kv(k_pool, args)
        logger.info("[D0] sparse_copy D2H demo PASS")
        return True
    except RuntimeError as error:
        logger.warning("[D0] D2H demo failed (non-fatal): %s", error)
        return False


def decode_rank0_verify(conn, pools, meta, args):
    """rank0 等待 verify 指令，对全池校验并运行 D2H 演示，返回 result 消息。"""
    expect_phase(recv_json(conn), "verify")
    verify_shared_pool(pools, meta["p_block_ids"], args)
    logger.info(f"[D0] shared pool verification PASS (all {args.tp_size} ranks)")
    d2h_demo_ok = run_d2h_demo(pools[0][0], args)
    return {"phase": "result", "ok": True, "d2h_demo_ok": d2h_demo_ok}


def cleanup_decode_rank(engine, conn, args):
    """收 final 后同步清理共享池与引擎，并上报 P 侧 cleanup_done。"""
    try:
        if args.tp_size > 1:
            dist.barrier()
        offload.uninitialize()
        engine.unInitialize()
        send_json(conn, {"phase": "cleanup_done", "tp_rank": args.tp_rank, "ok": True})
    except Exception as cleanup_error:
        with contextlib.suppress(OSError):
            send_json(
                conn,
                {
                    "phase": "cleanup_done",
                    "tp_rank": args.tp_rank,
                    "ok": False,
                    "error": f"{type(cleanup_error).__name__}: {cleanup_error}",
                },
            )
        raise
    finally:
        if args.tp_size > 1:
            dist.destroy_process_group()


def run_decode_role(args):
    with connect_with_retry(args.prefill_ip, args.control_port, args.timeout) as conn:
        conn.settimeout(args.timeout)
        send_json(conn, {"phase": "hello", "tp_rank": args.tp_rank})
        try:
            pools, cpu_bases = decode_shared_pool_handshake(conn, args)
            meta = recv_json(conn)
            expect_phase(meta, "meta")
            validate_layout(args, meta)

            engine, _ = init_transfer_engine(ROLE_DECODE, args.local_ip, args)
            owned = tp_block_range(args.num_blocks, args.tp_rank, args.tp_size)
            logger.info(f"[D{args.tp_rank}] engine ready, owning blocks [{owned[0]}, {owned[1]}) of {args.num_blocks}")

            total_bytes, total_desc, iter_transfer, iter_desc = decode_pull_iterations(
                engine, meta, cpu_bases, owned, args
            )
            max_pieces = args.num_layers * (owned[1] - owned[0]) * 2
            merge_ratio = max_pieces / total_desc if total_desc else 0.0
            send_json(
                conn,
                {
                    "phase": "pull_done",
                    "tp_rank": args.tp_rank,
                    "ok": True,
                    "bytes": total_bytes,
                    "descriptors": total_desc,
                    "merge_ratio": merge_ratio,
                    "iterations": args.iterations,
                    "warmup": args.warmup,
                    "transfer_seconds": iter_transfer,
                    "desc_seconds": iter_desc,
                },
            )

            if args.tp_rank == 0:
                result = decode_rank0_verify(conn, pools, meta, args)
                send_json(conn, result)
            final = recv_json(conn)
            expect_phase(final, "final")
            if not final.get("ok"):
                raise RuntimeError(f"transfer failed: {final.get('error', final)}")
            cleanup_decode_rank(engine, conn, args)
            own_bw = [total_bytes / GIB / t for t in iter_transfer if t > 0]
            bw_mean = sum(own_bw) / len(own_bw) if own_bw else 0.0
            bw_best = max(own_bw) if own_bw else 0.0
            desc_ms = 1000.0 * sum(iter_desc) / len(iter_desc) if iter_desc else 0.0
            transfer_ms = 1000.0 * sum(iter_transfer) / len(iter_transfer) if iter_transfer else 0.0
            round_gib = total_bytes / GIB
            logger.info(
                f"[D{args.tp_rank}] PASS: pulled {round_gib:.2f} GiB/round, "
                f"total {round_gib * args.iterations:.2f} GiB in {args.iterations} rounds, "
                f"blocks=[{owned[0]},{owned[1]}), "
                f"merge ratio {merge_ratio:.2f} ({total_desc} pieces), "
                f"bw={bw_mean:.2f} GiB/s (best {bw_best:.2f}), "
                f"latency build={desc_ms:.2f}ms + transfer={transfer_ms:.2f}ms/round, "
                f"d2h_demo_ok={final.get('d2h_demo_ok')}"
            )
        except Exception as error:
            with contextlib.suppress(OSError):
                send_json(conn, {"phase": "error", "error": f"{type(error).__name__}: {error}"})
            raise


def _rank_argv(rank, base_npu_id):
    """基于当前命令行构造 rank 子进程 argv：透传其余参数，注入 --tp-rank 与 --npu-id（起始卡号 + rank）。"""
    argv = []
    skip_next = False
    for item in sys.argv[1:]:
        if skip_next:
            skip_next = False
            continue
        if item in ("--tp-rank", "--npu-id"):
            skip_next = True
            continue
        if item.startswith("--tp-rank=") or item.startswith("--npu-id="):
            continue
        argv.append(item)
    return argv + ["--tp-rank", str(rank), "--npu-id", str(base_npu_id + rank)]


def launch_decode_ranks(args):
    """Decode 启动器：一条命令拉起全部 rank 子进程并聚合退出码。"""
    children = []
    for rank in range(args.tp_size):
        command = [sys.executable, os.path.abspath(__file__)] + _rank_argv(rank, args.npu_id)
        logger.info(f"[launcher] starting Decode rank {rank}")
        children.append((rank, subprocess.Popen(command)))
    failed = []
    for rank, child in children:
        code = child.wait()
        logger.info(f"[launcher] Decode rank {rank} exited with code {code}")
        if code != 0:
            failed.append((rank, code))
    if failed:
        raise RuntimeError(f"Decode ranks failed: {failed}")
    logger.info(f"[launcher] all {args.tp_size} Decode ranks passed")


def main():
    args = _parse_args()
    logging.basicConfig(level=LOG_LEVELS[args.log_level], format="%(asctime)s %(levelname)s %(message)s")
    if args.role == "Prefill":
        torch.npu.set_device(device=args.npu_id)
        run_prefill_role(args)
    elif args.tp_rank is None:
        launch_decode_ranks(args)
    else:
        torch.npu.set_device(device=args.npu_id)
        run_decode_role(args)


if __name__ == "__main__":
    main()
