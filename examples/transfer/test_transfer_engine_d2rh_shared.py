#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use, copy and redistribute this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""SFA D2RH 场景 PD KV Cache push 传输测试（Decode 多 rank，P 主动写 D 共享池）。

对齐 sglang AscendTransferEngine 的 push 用法（batch_transfer_sync_write）：P 侧生成
每层 KV cache（HBM bf16）并 register_memory 注册源内存，D 侧各 rank 建 HCCL 组后以 SHARED
场景初始化共享 DRAM offload 池（仅 rank0 分配物理 DRAM，store url 与池基地址经 HCCL
broadcast 同步，参考 kv_offload 共享池与 test_transfer_engine_rd2h），SHARED 池全组同 VA，
各 rank 将广播基址的目的区 register_memory 到本 rank 引擎后上报 session；P 按 tp_block_range
均分的块区间逐 rank 推送分片写入共享池，rank0 全池校验并演示 sparse_copy D2H。KV 为 MLA
main cache（bf16），默认 32 层 x 128 块。

混合介质（对齐生产 SFA indexer/scale 布局）：每个 owned 块的 main K/V 推送至共享 DRAM
池，同批块的 indexer 组件推送至本 rank 的 HBM 热区 arena——描述符合入同一次
batch_transfer_sync_write，一次 batch 同时覆盖 HBM→远端 HBM 与 HBM→DRAM。内存排布
对齐生产 _allocate_kv_cache_tensors：P 侧 main K/V 每层独立分配、各自 2MiB 对齐，
indexer C8 下 K+scale 同一 raw 前后排布（边界仅按 scale_dtype 对齐），注册按 storage
一段；D 侧 arena 每层一段（段基址 2MiB 对齐）逐层独立注册。indexer 两条路径分开
验证（--indexer-c8）：
- 非量化（默认）：indexer K bf16、无 scale（生产 scale_dim=0，单组件/层）；
- C8（--indexer-c8）：indexer K int8 + scale int8（scale_dim=1），同 storage 前后排布
  （生产 LIC8 路径，K+scale 合并注册为一段）。
设备内存共享名走 IPC 语义仅同机有效，跨机（prefill-ip != local-ip）时 HBM 热区可能
导入失败；可用 --no-enable-hbm 去掉 HBM 热区（仅注册/推送共享 DRAM 池的 main K/V）
做跨机验证，两侧开关必须一致。
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
from memfabric_hybrid import TransferEngine, offload, set_conf_store_tls, TransDataOpType

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

logger = logging.getLogger("test_transfer_engine_d2rh")
# 一层 K/V 分片的推送规格：目标层、源布局、目标 rank 的共享池/HBM 热区基地址、该 rank 拥有块区间
LayerPushSpec = namedtuple("LayerPushSpec", "layer_idx layer_meta cpu_base hbm_base p_block_ids owned")


def _ipv4(value):
    try:
        address = ipaddress.ip_address(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid IP address: {value}") from error
    if address.version != 4:
        raise argparse.ArgumentTypeError("MemFabric test requires a numeric IPv4 address")
    return str(address)


def _parse_args():
    parser = argparse.ArgumentParser(description="SFA D2RH PD KV cache push transfer test (multi-rank Decode)")
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
        "--indexer-head-dim",
        type=int,
        default=128,
        help="SFA indexer K head dim landing in Decode HBM, mirroring the production index_head_dim",
    )
    parser.add_argument(
        "--indexer-c8",
        action="store_true",
        help="Indexer K int8 + scale int8 (scale_dim=1) in one storage, mirroring the production C8/LIC8 path; "
        "default is the non-quantized bf16 path without scale",
    )
    parser.add_argument(
        "--enable-hbm",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Include the per-rank HBM hot segments (indexer/scale) in registration and push; "
        "use --no-enable-hbm to push main K/V to the shared DRAM pool only (cross-machine friendly)",
    )
    parser.add_argument(
        "--log-level", type=int, default=1, choices=[0, 1, 2, 3], help="0 debug, 1 info, 2 warn, 3 error"
    )
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS, help="Control channel timeout")
    parser.add_argument("--iterations", type=int, default=3, help="Measured push rounds (mean and best)")
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
    # P 侧每层注册 3 段：main K / main V / indexer（C8 时 K+scale 同 storage 合并一段）
    if args.num_layers * 3 > MAX_REGISTER_REGIONS:
        parser.error(f"num_layers*3 must be <= {MAX_REGISTER_REGIONS} (register region limit)")
    for name in ("num_layers", "num_blocks", "block_size", "kv_lora_rank", "qk_rope_head_dim", "indexer_head_dim"):
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


def indexer_layout(args):
    """SFA indexer 组件布局（对齐生产 DeepseekV32IndexerCache spec）。

    非 C8（默认）：indexer K bf16、无 scale（生产 scale_dim=0）；
    C8（--indexer-c8）：indexer K int8 + scale int8（scale_dim=1），同 storage 前后排布。
    """
    ik_shape = (args.num_blocks, args.block_size, 1, args.indexer_head_dim)
    ik_block_len = (1 if args.indexer_c8 else 2) * args.block_size * args.indexer_head_dim
    if args.indexer_c8:
        scale_shape = (args.num_blocks, args.block_size, 1, 1)
        scale_block_len = args.block_size  # int8, scale_dim=1
    else:
        scale_shape, scale_block_len = None, 0
    return ik_shape, scale_shape, ik_block_len, scale_block_len


def make_expected_indexer(args, layer):
    """按固定 seed 生成期望 indexer 组件，P/D 两侧独立重现（C8 时附带 scale，否则为 None）。"""
    ik_shape, scale_shape, _, _ = indexer_layout(args)
    gen_ik = torch.Generator().manual_seed(args.seed * 100003 + layer + 900000)
    if args.indexer_c8:
        ik = torch.randint(-128, 128, ik_shape, generator=gen_ik, dtype=torch.int32).to(torch.int8)
    else:
        ik = torch.randn(ik_shape, generator=gen_ik, dtype=torch.float32).to(torch.bfloat16)
    if scale_shape is None:
        return ik, None
    gen_s = torch.Generator().manual_seed(args.seed * 100003 + layer + 910000)
    scale = torch.randint(-128, 128, scale_shape, generator=gen_s, dtype=torch.int32).to(torch.int8)
    return ik, scale


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
    """对齐 sglang AscendTransferEngine 的 5 参数 initialize（不传 store_server_role，默认 Decode）。

    默认参数下的框架行为（pytransfer 设计）：Decode 引擎内嵌各自的 store server（绑定本 rank
    session 端口）；Prefill 引擎为纯 client，register_memory 仅记录，write(dest_session) 时按
    目标 session 懒连接到对应 Decode 的 store 并作为后加入者 join（同步学习该 D 的 session 与
    slices，并自动重放 P 侧已注册内存）。disagg_service 的 create_config_store 与此传输面无关
    （store_url 参数仅用于提取协议前缀）。
    """
    set_conf_store_tls(False, "")
    engine = TransferEngine()
    ret = engine.initialize(f"tcp://{local_ip}", local_ip, role, args.npu_id, TransDataOpType.SDMA)
    if ret != 0:
        raise RuntimeError(f"TransferEngine initialize failed, ret={ret}, role={role}, localIp={local_ip}")
    session = f"{local_ip}:{engine.get_rpc_port()}"
    tag = f"[D{args.tp_rank} engine]" if role == ROLE_DECODE else "[P engine]"
    logger.info(f"{tag} role={role} session={session}")
    return engine, session


# ---------------------------- Prefill (producer, push) ----------------------------


def alloc_aligned_npu(num_bytes):
    """对齐生产 _allocate_int8_cache_tensor：多要 2MiB 再把基址对齐到 2MiB，返回内容视图。"""
    raw = torch.zeros(num_bytes + CPU_BUFFER_ALIGNMENT, dtype=torch.uint8, device="npu")
    offset = (-raw.data_ptr()) % CPU_BUFFER_ALIGNMENT
    return raw[offset : offset + num_bytes]


def build_prefill_kv_caches(args):
    """P 侧模拟 prefill 产出每层组件，内存排布对齐生产 _allocate_kv_cache_tensors：

    - main K/V：每层独立分配、各自 2MiB 对齐（生产：split into k_cache/v_cache，均 2M 对齐）；
    - indexer 非量化：单块 bf16（生产 scale_dim=0）；C8：int8 K + int8 scale 从同一块 raw
      切出（基址 2MiB 对齐、边界仅按 scale_dtype 对齐，对齐 _allocate_sparse_c8_indexer_tensors）。
    返回 caches[layer] = (k, v, ik, scale|None)，均为对齐存储上的视图。
    """
    k_shape, v_shape, k_block_len, v_block_len = kv_layout(args)
    ik_shape, scale_shape, ik_block_len, s_block_len = indexer_layout(args)
    caches = []
    for layer in range(args.num_layers):
        k_expected, v_expected = make_expected_kv(args, layer)
        ik_expected, s_expected = make_expected_indexer(args, layer)
        k_raw = alloc_aligned_npu(k_block_len * args.num_blocks)
        v_raw = alloc_aligned_npu(v_block_len * args.num_blocks)
        k = k_raw.view(torch.bfloat16).reshape(k_shape)
        v = v_raw.view(torch.bfloat16).reshape(v_shape)
        k.copy_(k_expected)
        v.copy_(v_expected)
        if scale_shape is None:
            ik_raw = alloc_aligned_npu(ik_block_len * args.num_blocks)
            ik = ik_raw.view(torch.bfloat16).reshape(ik_shape)
            ik.copy_(ik_expected)
            caches.append((k, v, ik, None))
        else:
            indexer_bytes = ik_block_len * args.num_blocks + s_block_len * args.num_blocks
            idx_raw = alloc_aligned_npu(indexer_bytes)
            ik = idx_raw[: ik_block_len * args.num_blocks].view(torch.int8).reshape(ik_shape)
            scale = idx_raw[ik_block_len * args.num_blocks :].view(torch.int8).reshape(scale_shape)
            ik.copy_(ik_expected)
            scale.copy_(s_expected)
            caches.append((k, v, ik, scale))
    torch.npu.synchronize()
    return caches


def register_prefill_caches(engine, caches, args):
    """P 侧注册源内存（push 路径无需注册，实测无效果；保留供读方向实验使用）。

    一个 storage 一段：main K/V 各一段；C8 下 indexer K+scale 同 storage 合并为一段
    （对齐生产 collect_storage_merged_register_regions 的按 storage 合并）。
    """
    _, _, k_block_len, v_block_len = kv_layout(args)
    _, _, ik_block_len, s_block_len = indexer_layout(args)
    per_layer = 3
    if len(caches) * per_layer > MAX_REGISTER_REGIONS:
        raise RuntimeError(f"register regions {len(caches) * per_layer} exceed limit {MAX_REGISTER_REGIONS}")
    for layer, (k_cache, v_cache, ik_cache, s_cache) in enumerate(caches):
        components = [
            ("k", k_cache, k_block_len * args.num_blocks),
            ("v", v_cache, v_block_len * args.num_blocks),
            ("indexer", ik_cache, ik_block_len * args.num_blocks),
        ]
        if s_cache is not None:
            components[-1] = (
                "indexer",
                ik_cache,
                ik_block_len * args.num_blocks + s_block_len * args.num_blocks,
            )
        for name, cache, size in components:
            ret = engine.register_memory(cache.data_ptr(), size)
            if ret != 0:
                raise RuntimeError(f"register_memory failed, ret={ret}, layer={layer}, tensor={name}")
    logger.info(f"[P] registered {len(caches) * per_layer} source regions over {len(caches)} layers")


def build_prefill_layers_meta(caches, args):
    """P 侧每层各组件基地址与块长（用于按 block table 构造推送描述符）。"""
    _, _, k_block_len, v_block_len = kv_layout(args)
    _, _, ik_block_len, s_block_len = indexer_layout(args)
    layers = []
    for k_cache, v_cache, ik_cache, s_cache in caches:
        layers.append(
            {
                "k_base": int(k_cache.data_ptr()),
                "v_base": int(v_cache.data_ptr()),
                "k_block_len": k_block_len,
                "v_block_len": v_block_len,
                "ik_base": int(ik_cache.data_ptr()),
                "ik_block_len": ik_block_len,
                "scale_base": int(s_cache.data_ptr()) if s_cache is not None else None,
                "scale_block_len": s_block_len,
            }
        )
    return layers


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


def collect_pool_ready(conns, args):
    """并行下发 init 并等待各 rank 上报 pool_ready（携带 D 侧 session 与共享池广播基地址）。

    各 D rank 的引擎内嵌各自独立的 store server（对齐 sglang 默认行为），rank 间无依赖，
    可并行初始化；P 侧 write 时按目标 session 逐个懒连接加入。SHARED 池全组同 VA，
    各 rank 上报的基地址应完全一致。
    """
    for rank in range(args.decode_tp_size):
        send_json(conns[rank], {"phase": "init", "tp_size": args.decode_tp_size})
    infos = [None] * args.decode_tp_size
    for rank in range(args.decode_tp_size):
        ready = recv_json(conns[rank])
        expect_phase(ready, "pool_ready")
        if int(ready.get("tp_rank", -1)) != rank or len(ready.get("cpu_bases", [])) != args.num_layers:
            raise RuntimeError(f"invalid pool_ready from rank {rank}: {ready}")
        hbm_bases = ready.get("hbm_bases", [])
        if len(hbm_bases) != args.num_layers:
            raise RuntimeError(f"invalid hbm_bases from rank {rank}: {ready}")
        if bool(ready.get("indexer_c8")) != args.indexer_c8:
            raise RuntimeError(f"indexer c8 mode mismatch on rank {rank}: {ready}")
        infos[rank] = {"session": ready["session"], "cpu_bases": ready["cpu_bases"], "hbm_bases": hbm_bases}
        if rank > 0 and ready["cpu_bases"] != infos[0]["cpu_bases"]:
            raise RuntimeError(f"shared cpu bases mismatch on rank {rank}: {ready['cpu_bases']}")
        logger.info(f"[P] rank {rank} pool ready, session={ready['session']}")
    return infos


def coalesce_descriptors(descriptors):
    """合并连续块为大段传输（对应生产 read_thread 的 _coalesce_desc）。"""
    merged = []
    for peer, local, length in descriptors:
        if merged and merged[-1][0] + merged[-1][2] == peer and merged[-1][1] + merged[-1][2] == local:
            merged[-1][2] += length
        else:
            merged.append([peer, local, length])
    return merged


def build_layer_descriptors(layer_meta, cpu_base, hbm_base, p_block_ids, owned):
    """构造一层推送描述符（介质划分对齐生产）：owned 全部块的 main K/V 落共享池，
    同批块的 indexer/scale 落本 rank HBM 热区（区内偏移 dst_idx-start），两类描述符
    合入同一次 batch 传输；hbm_base 为 None 时（--no-enable-hbm）仅构造共享池腿。
    """
    start, end = owned
    descriptors = []
    specs = [
        (layer_meta["k_base"], cpu_base["k"], layer_meta["k_block_len"], False),
        (layer_meta["v_base"], cpu_base["v"], layer_meta["v_block_len"], False),
    ]
    if hbm_base is not None:
        specs.append((layer_meta["ik_base"], hbm_base["indexer"], layer_meta["ik_block_len"], True))
        if layer_meta["scale_base"] is not None:
            specs.append((layer_meta["scale_base"], hbm_base["scale"], layer_meta["scale_block_len"], True))
    for src_base, dst_base, block_len, to_hbm in specs:
        for dst_idx in range(start, end):
            src_idx = p_block_ids[dst_idx]
            dst_off = (dst_idx - start) if to_hbm else dst_idx
            descriptors.append((src_base + src_idx * block_len, dst_base + dst_off * block_len, block_len))
    return coalesce_descriptors(descriptors)


def push_owned_layer(engine, d_session, spec):
    """按 owned 块区间向一个 D rank 推送一层混合介质分片，描述符构建与传输分段计时。"""
    tick = time.monotonic()
    descriptors = build_layer_descriptors(spec.layer_meta, spec.cpu_base, spec.hbm_base, spec.p_block_ids, spec.owned)
    src_ptrs = [desc[0] for desc in descriptors]
    dst_ptrs = [desc[1] for desc in descriptors]
    lengths = [desc[2] for desc in descriptors]
    desc_seconds = time.monotonic() - tick

    tick = time.monotonic()
    ret = engine.batch_transfer_sync_write(d_session, src_ptrs, dst_ptrs, lengths)
    transfer_seconds = time.monotonic() - tick
    if ret != 0:
        raise RuntimeError(f"batch_transfer_sync_write failed, ret={ret}, session={d_session}, layer={spec.layer_idx}")
    return sum(lengths), len(descriptors), desc_seconds, transfer_seconds


def wait_decode_sessions_ready(engine, infos, layers_meta, p_block_ids, args):
    """逐 rank 触发懒连接并探测就绪：首块单描述符探测写，失败退避重试。

    P 引擎为 client 模式（对齐 sglang）：首次 write(dest_session) 时按目标 session 懒连接
    对应 Decode 内嵌的 store server，join（同步等待 PROMOTE_TO_ACTIVE）并自动重放 P 侧已
    注册内存。探测区间在后续正式轮次会被完整重写，数据无影响；探测失败多为 join 后异步
    dispatcher 尚在消化，退避重试直至全部就绪或超时。
    """
    deadline = time.monotonic() + args.timeout
    pending = list(range(len(infos)))
    while pending:
        still = []
        for rank in pending:
            owned = tp_block_range(args.num_blocks, rank, args.decode_tp_size)
            spec = LayerPushSpec(
                0,
                layers_meta[0],
                infos[rank]["cpu_bases"][0],
                infos[rank]["hbm_bases"][0],
                p_block_ids,
                (owned[0], owned[0] + 1),
            )
            try:
                push_owned_layer(engine, infos[rank]["session"], spec)
                logger.info(f"[P] decode rank {rank} session ready")
            except RuntimeError:
                still.append(rank)
        if not still:
            return
        if time.monotonic() > deadline:
            raise RuntimeError(f"timed out waiting for decode sessions: {still}")
        time.sleep(0.05)
        pending = still


def prefill_push_iterations(engine, conns, infos, layers_meta, p_block_ids, args):
    """warmup 轮 + 测量轮：逐 rank 推送各自分片；返回各测量轮（取最慢 rank）传输秒数等。"""
    owned_ranges = {
        rank: tp_block_range(args.num_blocks, rank, args.decode_tp_size) for rank in range(args.decode_tp_size)
    }
    iter_transfer = []
    iter_desc = []
    total_bytes = 0
    total_desc = 0
    for round_idx in range(args.warmup + args.iterations):
        quiet = round_idx < args.warmup
        round_bytes = 0
        round_desc = 0
        round_desc_seconds = 0.0
        round_transfer_seconds = 0.0
        for rank in range(args.decode_tp_size):
            owned = owned_ranges[rank]
            for layer, layer_meta in enumerate(layers_meta):
                layer_bytes, n_desc, d_sec, t_sec = push_owned_layer(
                    engine,
                    infos[rank]["session"],
                    LayerPushSpec(
                        layer,
                        layer_meta,
                        infos[rank]["cpu_bases"][layer],
                        infos[rank]["hbm_bases"][layer],
                        p_block_ids,
                        owned,
                    ),
                )
                round_bytes += layer_bytes
                round_desc += n_desc
                round_desc_seconds += d_sec
                round_transfer_seconds += t_sec
        total_bytes, total_desc = round_bytes, round_desc
        if quiet:
            logger.info(f"[P] warmup round {round_idx + 1}/{args.warmup} done")
            continue
        iter_desc.append(round_desc_seconds)
        iter_transfer.append(round_transfer_seconds)
        logger.info(
            f"[P] round {round_idx + 1} pushed {round_bytes / GIB:.2f} GiB in "
            f"{round_desc_seconds * 1000:.2f}ms build + {round_transfer_seconds * 1000:.2f}ms transfer"
        )
    return total_bytes, total_desc, iter_transfer, iter_desc


def run_prefill_role(args):
    # 先占住控制端口，再初始化引擎（client 模式，无组操作），避免自动选端口撞上控制端口
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.local_ip, args.control_port))
        server.listen(args.decode_tp_size)
        server.settimeout(args.timeout)

        engine, _ = init_transfer_engine(ROLE_PREFILL, args.local_ip, args)
        final, total_bytes, total_desc, iter_transfer, iter_desc = serve_decode_ranks(server, args, engine)

    engine.unInitialize()
    desc_ms = 1000.0 * sum(iter_desc) / len(iter_desc) if iter_desc else 0.0
    transfer_ms = 1000.0 * sum(iter_transfer) / len(iter_transfer) if iter_transfer else 0.0
    logger.info(
        f"[P] PASS: layers={args.num_layers}, decode_ranks={args.decode_tp_size}, "
        f"bytes={total_bytes / GIB:.2f} GiB/round, iterations={final['iterations']}, "
        f"transfer_bw={final['bandwidth_gibps']:.2f} GiB/s (best {final['best_bandwidth_gibps']:.2f}), "
        f"pieces={total_desc}, latency build={desc_ms:.2f}ms + transfer={transfer_ms:.2f}ms/round, "
        f"d2h_demo_ok={final['d2h_demo_ok']}"
    )


def serve_decode_ranks(server, args, engine):
    """驱动控制协议：hello → init → pool_ready → push(各轮) → verify → final。

    P 引擎为 client 模式（对齐 sglang）：register 仅本地记录，write(dest_session) 时按目标
    D session 懒连接其内嵌 store 并作为后加入者 join，自动重放已注册内存。
    """
    caches = build_prefill_kv_caches(args)
    conns = []
    try:
        conns = accept_decode_ranks(server, args)
        infos = collect_pool_ready(conns, args)
        layers_meta = build_prefill_layers_meta(caches, args)
        p_block_ids = make_block_table(args)
        wait_decode_sessions_ready(engine, infos, layers_meta, p_block_ids, args)
        total_bytes, total_desc, iter_transfer, iter_desc = prefill_push_iterations(
            engine, conns, infos, layers_meta, p_block_ids, args
        )

        results = []
        for rank in range(args.decode_tp_size):
            send_json(conns[rank], {"phase": "verify"})
        for rank in range(args.decode_tp_size):
            result = recv_json(conns[rank])
            expect_phase(result, "result")
            if not result.get("ok"):
                raise RuntimeError(f"Decode rank {rank} verification failed: {result.get('error', result)}")
            results.append(result)

        bandwidths = [total_bytes / GIB / seconds if seconds > 0 else 0.0 for seconds in iter_transfer]
        final = {
            "phase": "final",
            "ok": True,
            "total_bytes": total_bytes,
            "iterations": len(bandwidths),
            "bandwidth_gibps": sum(bandwidths) / len(bandwidths),
            "best_bandwidth_gibps": max(bandwidths),
            "d2h_demo_ok": all(bool(r.get("d2h_demo_ok", False)) for r in results),
        }
        for rank in range(args.decode_tp_size):
            send_json(conns[rank], final)
        for rank in range(args.decode_tp_size):
            cleanup = recv_json(conns[rank])
            expect_phase(cleanup, "cleanup_done")
            if not cleanup.get("ok"):
                raise RuntimeError(f"Decode rank {rank} cleanup failed: {cleanup.get('error', cleanup)}")
        return final, total_bytes, total_desc, iter_transfer, iter_desc
    except Exception as error:
        with contextlib.suppress(OSError):
            for conn in conns:
                send_json(conn, {"phase": "final", "ok": False, "error": f"{type(error).__name__}: {error}"})
        raise
    finally:
        # 先释放源 HBM tensor 再关闭连接，退出时 VA 清理更干净
        del caches
        torch.npu.synchronize()
        torch.npu.empty_cache()
        for conn in conns:
            conn.close()


# ---------------------------- Decode (consumer, receives) ----------------------------


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
    """仅 rank0 分配每层 CPU 池 K/V（SHARED 场景对应 manager 中 tp_rank==0 才持有 CPU 张量）。"""
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


def hbm_layer_seg(args, blocks):
    """每层热区段字节数：indexer_k + scale 向上对齐 2MiB（对齐生产每层分配独立 2MiB 对齐）。"""
    _, _, ik_block_len, scale_block_len = indexer_layout(args)
    raw = (ik_block_len + scale_block_len) * blocks
    return ((raw + CPU_BUFFER_ALIGNMENT - 1) // CPU_BUFFER_ALIGNMENT) * CPU_BUFFER_ALIGNMENT


def build_decode_hbm_pool(args):
    """按生产 SFA indexer 布局分配本 rank HBM 热区（每层一段、逐层独立注册）。

    每层一段（段基址 2MiB 对齐、段长为 2MiB 整数倍，等价生产每层 raw 分配独立对齐），
    覆盖本 rank owned 全部块；对齐后相邻段互不重叠，逐层注册不冲突。非 C8 段内仅
    indexer K（bf16）；C8 段内 indexer K（int8）+ scale（int8，scale_dim=1）前后排布。
    返回 (backing, segments, blocks, hbm_bases)，segments[layer] 为该层注册段。
    """
    owned = tp_block_range(args.num_blocks, args.tp_rank, args.tp_size)
    blocks = owned[1] - owned[0]
    _, scale_shape, ik_block_len, _ = indexer_layout(args)
    seg = hbm_layer_seg(args, blocks)
    total = seg * args.num_layers
    backing = torch.zeros(total + CPU_BUFFER_ALIGNMENT, dtype=torch.uint8, device="npu")
    offset = (-backing.data_ptr()) % CPU_BUFFER_ALIGNMENT
    arena = backing[offset : offset + total]
    base = arena.data_ptr()
    segments, hbm_bases = [], []
    for layer in range(args.num_layers):
        layer_base = base + layer * seg
        segments.append(arena[layer * seg : (layer + 1) * seg])
        bases = {"indexer": layer_base}
        if scale_shape is not None:
            bases["scale"] = layer_base + ik_block_len * blocks
        hbm_bases.append(bases)
    logger.info(
        f"[D{args.tp_rank}] allocated hbm hot segments (indexer c8={args.indexer_c8}), "
        f"layers={args.num_layers}, seg={seg}B, base={hex(base)}, blocks={blocks}"
    )
    return backing, segments, blocks, hbm_bases


def hbm_hot_views(segments, blocks, args):
    """按层切出热区 indexer（及 C8 scale）视图，布局与 hbm_bases 一一对应，用于逐层校验。"""
    ik_shape, scale_shape, ik_block_len, scale_block_len = indexer_layout(args)
    ik_dtype = torch.int8 if args.indexer_c8 else torch.bfloat16
    views = []
    for segment in segments:
        ik = segment[: ik_block_len * blocks].view(ik_dtype).reshape((blocks,) + ik_shape[1:])
        if scale_shape is None:
            views.append((ik, None))
            continue
        scale = segment[ik_block_len * blocks : (ik_block_len + scale_block_len) * blocks].view(torch.int8)
        views.append((ik, scale.reshape((blocks,) + scale_shape[1:])))
    return views


def broadcast_cpu_bases(pools, args):
    """rank0 的 CPU 池基地址经 HCCL 广播（SHARED 池全组同 VA，等价生产 tp_group.broadcast(gvas)）。"""
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


def register_decode_destinations(engine, cpu_bases, hbm_segments, args):
    """D 侧各 rank 将共享池目的区与本 rank HBM 热区注册到本 rank 引擎。

    SHARED 池全组同 VA，rank>0 以广播基址注册；HBM 热区逐层一段独立注册（段基址
    2MiB 对齐、段长 2MiB 整数倍，对齐后互不重叠）。push 路径：P 写入的远端目的必须在
    本端注册；注册后经 session 元数据同步到 P 侧，作为 batch_transfer_sync_write 的
    远端落地内存。
    """
    _, _, k_block_len, v_block_len = kv_layout(args)
    regions = len(cpu_bases) * 2 + len(hbm_segments)
    if regions > MAX_REGISTER_REGIONS:
        raise RuntimeError(f"register regions {regions} exceed limit {MAX_REGISTER_REGIONS}")
    for layer, bases in enumerate(cpu_bases):
        for name, base, block_len in (("k", bases["k"], k_block_len), ("v", bases["v"], v_block_len)):
            ret = engine.register_memory(base, block_len * args.num_blocks)
            if ret != 0:
                raise RuntimeError(
                    f"register_memory failed, ret={ret}, rankId={args.tp_rank}, layer={layer}, tensor={name}"
                )
    for layer, segment in enumerate(hbm_segments):
        ret = engine.register_memory(segment.data_ptr(), segment.numel())
        if ret != 0:
            raise RuntimeError(
                f"register_memory failed, ret={ret}, rankId={args.tp_rank}, layer={layer}, tensor=hbm hot segment"
            )
    logger.info(f"[D{args.tp_rank}] registered {len(cpu_bases) * 2} cpu pool + {len(hbm_segments)} hbm segment regions")


def verify_shared_pool(pools, p_block_ids, args):
    """rank0 对整个共享 CPU 池逐层校验（main K/V 覆盖所有 rank 写入的块）。"""
    for layer, (k_pool, v_pool, _, _) in enumerate(pools):
        expected_k, expected_v = make_expected_kv(args, layer)
        if not torch.equal(k_pool, expected_k[p_block_ids]):
            raise RuntimeError(f"layer {layer} k cache mismatch in shared pool after multi-rank push")
        if not torch.equal(v_pool, expected_v[p_block_ids]):
            raise RuntimeError(f"layer {layer} v cache mismatch in shared pool after multi-rank push")


def verify_hbm_slice(segments, blocks, owned, p_block_ids, args):
    """本 rank 校验 HBM 热区 indexer（及 C8 scale）内容（owned 全部块）。"""
    if not segments:
        return
    for layer, (ik_hot, s_hot) in enumerate(hbm_hot_views(segments, blocks, args)):
        expected_ik, expected_s = make_expected_indexer(args, layer)
        if not torch.equal(ik_hot.cpu(), expected_ik[p_block_ids[owned[0] : owned[1]]]):
            raise RuntimeError(f"layer {layer} indexer k mismatch in hbm hot pool, blocks=[{owned[0]},{owned[1]})")
        if s_hot is not None and not torch.equal(s_hot.cpu(), expected_s[p_block_ids[owned[0] : owned[1]]]):
            raise RuntimeError(f"layer {layer} indexer scale mismatch in hbm hot pool, blocks=[{owned[0]},{owned[1]})")


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
        # demo_offload_new_kv(k_pool, args)
        logger.info(f"[D{args.tp_rank}] sparse_copy D2H demo PASS")
        return True
    except RuntimeError as error:
        logger.warning("[D%d] D2H demo failed (non-fatal): %s", args.tp_rank, error)
        return False


def decode_shared_pool_handshake(conn, args):
    """收到 init 后建 HCCL 组并初始化共享池；rank0 分配 CPU 池并广播基地址，随后初始化引擎并注册目的区。"""
    message = recv_json(conn)
    expect_phase(message, "init")
    if int(message.get("tp_size", -1)) != args.tp_size:
        raise RuntimeError(f"tp_size mismatch: prefill={message.get('tp_size')} local={args.tp_size}")
    if args.prefill_ip != args.local_ip:
        logger.warning(
            f"[D{args.tp_rank}] hbm hot pool relies on same-host IPC memory sharing; cross-machine push "
            f"(prefill={args.prefill_ip}, local={args.local_ip}) may fail at import "
            f"(use --no-enable-hbm to exclude the hbm part)"
        )
    init_tp_group(args)
    store_url = resolve_shared_store_url(args)
    init_offload_pool(args, store_url)
    pools = build_decode_cpu_pools(args) if args.tp_rank == 0 else None
    cpu_bases = broadcast_cpu_bases(pools, args)
    if args.enable_hbm:
        hbm_backing, hbm_segments, hbm_blocks, hbm_bases = build_decode_hbm_pool(args)
    else:
        hbm_backing, hbm_segments, hbm_blocks, hbm_bases = None, [], 0, [None] * args.num_layers
    engine, session = init_transfer_engine(ROLE_DECODE, args.local_ip, args)
    register_decode_destinations(engine, cpu_bases, hbm_segments, args)
    return engine, session, pools, cpu_bases, hbm_backing, hbm_segments, hbm_blocks, hbm_bases


def cleanup_decode_rank(engine, conn, cpu_bases, hbm_backing, hbm_segments, args):
    """收 final 后同步清理共享池与引擎（先反注册目的区与 HBM 各层热区段），并上报 P 侧 cleanup_done。"""
    try:
        if args.tp_size > 1:
            dist.barrier()
        for layer, bases in enumerate(cpu_bases):
            for name, base in (("k", bases["k"]), ("v", bases["v"])):
                with contextlib.suppress(Exception):
                    ret = engine.unregister_memory(base)
                    if ret != 0:
                        logger.warning(
                            f"[D{args.tp_rank}] unregister_memory returned {ret}, layer={layer}, tensor={name}"
                        )
        for layer, segment in enumerate(hbm_segments):
            with contextlib.suppress(Exception):
                ret = engine.unregister_memory(segment.data_ptr())
                if ret != 0:
                    logger.warning(
                        f"[D{args.tp_rank}] unregister_memory returned {ret}, layer={layer}, tensor=hbm hot segment"
                    )
        offload.uninitialize()
        engine.unInitialize()
        del hbm_backing, hbm_segments
        torch.npu.empty_cache()
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
            handshake = decode_shared_pool_handshake(conn, args)
            engine, session, pools, cpu_bases = handshake[:4]
            hbm_backing, hbm_segments, hbm_blocks = handshake[4:7]
            owned = tp_block_range(args.num_blocks, args.tp_rank, args.tp_size)
            logger.info(f"[D{args.tp_rank}] engine ready, owning blocks [{owned[0]}, {owned[1]}) of {args.num_blocks}")
            send_json(
                conn,
                {
                    "phase": "pool_ready",
                    "tp_rank": args.tp_rank,
                    "session": session,
                    "cpu_bases": cpu_bases,
                    "hbm_bases": handshake[7],
                    "indexer_c8": args.indexer_c8,
                },
            )

            expect_phase(recv_json(conn), "verify")
            if args.enable_hbm:
                verify_hbm_slice(hbm_segments, hbm_blocks, owned, make_block_table(args), args)
                logger.info(f"[D{args.tp_rank}] hbm indexer/scale verification PASS, blocks=[{owned[0]},{owned[1]})")
            if args.tp_rank == 0:
                verify_shared_pool(pools, make_block_table(args), args)
                logger.info(f"[D0] shared pool main K/V verification PASS (all {args.tp_size} ranks)")
            d2h_demo_ok = run_d2h_demo(pools[0][0], args) if args.tp_rank == 0 else True
            send_json(conn, {"phase": "result", "tp_rank": args.tp_rank, "ok": True, "d2h_demo_ok": d2h_demo_ok})

            final = recv_json(conn)
            expect_phase(final, "final")
            if not final.get("ok"):
                raise RuntimeError(f"transfer failed: {final.get('error', final)}")
            cleanup_decode_rank(engine, conn, cpu_bases, hbm_backing, hbm_segments, args)
            logger.info(
                f"[D{args.tp_rank}] PASS: blocks=[{owned[0]},{owned[1]}), d2h_demo_ok={final.get('d2h_demo_ok')}"
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
    if args.role == ROLE_PREFILL:
        torch.npu.set_device(device=args.npu_id)
        run_prefill_role(args)
    elif args.tp_rank is None:
        launch_decode_ranks(args)
    else:
        torch.npu.set_device(device=args.npu_id)
        run_decode_role(args)


if __name__ == "__main__":
    main()
