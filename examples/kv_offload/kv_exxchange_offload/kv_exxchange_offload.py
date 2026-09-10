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
import os

import torch
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
CHIP_MAX_WORLD_SIZE = {"A3": 16, "A5": 8}
FALLBACK_MAX_WORLD_SIZE = 8

# Backend of the unified offload.kv_exchange_copy API, selected by --mode and
# routed internally through the MF_OFFLOAD_MODE environment variable:
#   aiv   AIV MTE kernel; meta/indices live on device, host_base is rewritten
#         to DVA before upload, offload pool runs in LOCAL + URMU_POOL scene.
MODE_AIV = "aiv"

# KV cache geometry defaults. The tiny default (< 1 MiB) suits precision runs;
# pass --data_mibs (and optionally --num_layers/--k_dim/--v_dim/--page_size) to
# scale the data volume for bandwidth benchmarking. NUM_PAGES is derived per
# data size. Each page holds PAGE_SIZE token rows and one (page, layer) copy
# row covers all of them, mirroring sglang's host pool geometry.
BFLOAT16_BYTES = 2
NUM_PAGES = 3200
PAGE_SIZE = 128
NUM_LAYERS = 1
K_DIM = 656 // BFLOAT16_BYTES
V_DIM = 128 // BFLOAT16_BYTES
NUM_COMPONENTS = 2  # K and V
# meta layout: 8 header int64 + 9 int64 per component, sized for the kernel's
# maximum of 4 components (unused component slots stay zero).
KV_EXCHANGE_META_HEADER = 8
KV_EXCHANGE_META_STRIDE = 9
KV_EXCHANGE_MAX_COMPONENTS = 4
KV_EXCHANGE_META_SIZE = KV_EXCHANGE_META_HEADER + KV_EXCHANGE_META_STRIDE * KV_EXCHANGE_MAX_COMPONENTS
HOST_LAYOUT_PAGE_FIRST = 0
HOST_LAYOUT_LAYER_FIRST = 1
DIRECTION_H2D = 1
DIRECTION_D2H = 2
WARMUP_ITERS = 3
# Each timed round repeats the copy TIMED_ITERS times so small data sizes get a
# stable average; the reported bandwidth divides total bytes by total time.
TIMED_ITERS = 10
# Precision checks build full-size comparison patterns on the CPU, so skip them
# above this size and only benchmark bandwidth.
PRECISION_CHECK_MAX_MIB = 64


def _total_bytes() -> int:
    """Total KV bytes moved by one copy call: pages * layers * page_size * (K + V) * bf16."""
    return NUM_PAGES * NUM_LAYERS * PAGE_SIZE * (K_DIM + V_DIM) * BFLOAT16_BYTES


def _pages_for_total_mib(total_mib: float, num_layers: int, k_dim: int, v_dim: int) -> int:
    """Derive NUM_PAGES so the moved bytes match total_mib (rounded down, min 1)."""
    row_bytes = num_layers * PAGE_SIZE * (k_dim + v_dim) * BFLOAT16_BYTES
    return max(int(total_mib * (1 << 20) / row_bytes), 1)


def _host_shape(dim: int, host_layout_mode: int) -> list:
    """Host buffer shape for one K/V component in the given layout mode.

    Mirrors sglang's host pool: each page holds PAGE_SIZE token rows, i.e.
    page_first [page, layer, page_size, dim] or layer_first
    [layer, page, page_size, dim].
    """
    if host_layout_mode == HOST_LAYOUT_LAYER_FIRST:
        return [NUM_LAYERS, NUM_PAGES, PAGE_SIZE, dim]
    return [NUM_PAGES, NUM_LAYERS, PAGE_SIZE, dim]


def _relayout(tensor: torch.Tensor, host_layout_mode: int) -> torch.Tensor:
    """Convert between device layout [layer, page, page_size, dim] and the host layout.

    A page_first host is [page, layer, page_size, dim] so the conversion
    transposes the first two dims; a layer_first host is already
    [layer, page, page_size, dim] (identity).
    """
    if host_layout_mode == HOST_LAYOUT_LAYER_FIRST:
        return tensor
    return tensor.permute(1, 0, 2, 3).contiguous()


def _make_host_pattern(dim: int, host_layout_mode: int) -> torch.Tensor:
    """Distinct per-(page, layer, token, elem) pattern in the host layout of the mode.

    Uses arange so every slot has a unique pre-bf16 value; after casting to
    bfloat16 some adjacent values may collapse to the same bf16 bit pattern,
    but that is fine: the copy kernel is byte-level, so we compare bit-exact via
    torch.equal rather than relying on float arithmetic.
    """
    total = NUM_PAGES * NUM_LAYERS * PAGE_SIZE * dim
    shape = _host_shape(dim, host_layout_mode)
    return torch.arange(total, dtype=torch.float32).view(*shape).to(torch.bfloat16)


def _make_device_pattern(pages: int, layers: int, dim: int) -> torch.Tensor:
    """Distinct per-(page, layer, token, elem) pattern in layer_first layout
    [layers, pages, page_size, dim].

    Uses a different arithmetic base than _make_host_pattern so H2D-residual and
    D2H-residual are both detectable (a stale buffer would not match the expected
    pattern even if it happened to come from the other direction).
    """
    layer_idx = torch.arange(layers, dtype=torch.float32).view(layers, 1, 1, 1)
    page_idx = torch.arange(pages, dtype=torch.float32).view(1, pages, 1, 1)
    token_idx = torch.arange(PAGE_SIZE, dtype=torch.float32).view(1, 1, PAGE_SIZE, 1)
    elem_idx = torch.arange(dim, dtype=torch.float32).view(1, 1, 1, dim)
    return (page_idx * 70000.0 + token_idx * 7000.0 + layer_idx * 300.0 + elem_idx).to(torch.bfloat16)


def _component_fields(base_dev: int, base_host: int, dim: int, host_layout_mode: int) -> list:
    """Per-component 9-value meta block for one K/V component.

    device is layer_first [layer, page, page_size, dim] contiguous. host is
    either page_first [page, layer, page_size, dim] or layer_first
    [layer, page, page_size, dim] contiguous, selected by host_layout_mode
    (meta[6]). All pitches/widths are in bytes.

    Mirrors sglang's comp_meta: one (page, layer) row covers all PAGE_SIZE
    tokens of the page, so width = PAGE_SIZE * dim * itemsize and the page
    pitches advance by one full page slot. base_dev/base_host are the raw
    data_ptr() values; in aiv mode _build_meta rewrites host_base from HVA to
    DVA before uploading the meta.
    """
    width = PAGE_SIZE * dim * BFLOAT16_BYTES
    if host_layout_mode == HOST_LAYOUT_LAYER_FIRST:
        host_page_pitch, host_layer_pitch = width, NUM_PAGES * width
    else:
        host_page_pitch, host_layer_pitch = NUM_LAYERS * width, width
    return [
        base_dev,  # device_base
        base_host,  # host_base (rewritten to DVA in aiv mode)
        NUM_PAGES * width,  # device_layer_pitch
        width,  # device_page_pitch
        host_page_pitch,  # host_page_pitch
        host_layer_pitch,  # host_layer_pitch
        width,  # width (bytes per (page, layer) row = PAGE_SIZE tokens)
        0,  # layer_lo
        NUM_LAYERS,  # layer_hi
    ]


def _rewrite_host_base_to_dva(meta: list) -> list:
    """Rewrite host_base HVA->DVA in the CPU-side meta list before uploading.

    The AIV kernel needs the DVA of conn-based DRAM pools (HalHostRegister
    device mapping); for vmm pools get_dva returns dva == hva (no-op).
    Per-component host_base sits at KV_EXCHANGE_META_HEADER + 9*c + 1
    (see acc_offload_kv_exchange.h).
    """
    for c in range(NUM_COMPONENTS):
        idx = KV_EXCHANGE_META_HEADER + KV_EXCHANGE_META_STRIDE * c + 1
        hva = int(meta[idx])
        if hva == 0:
            continue
        dva = offload.get_dva(hva)
        if dva == 0:
            raise ValueError(f"kv_exchange: get_dva failed for host_base 0x{hva:x}")
        meta[idx] = dva
    return meta


def _build_meta(buf: dict, direction: int) -> None:
    """Fill the int64 meta array per the layout both backends expect.

    See src/acc_offload/csrc/operators/acc_offload_kv_exchange.h for field semantics.
    In aiv mode the meta is uploaded to device with host_base rewritten to DVA;
    """
    # host_base carries the HVA from data_ptr(); convert it to the DVA on the
    # CPU list before uploading. host_indices is an npu tensor, its data_ptr
    # is already device-visible.
    mode = buf["host_layout_mode"]
    meta = [
        NUM_COMPONENTS,  # [0] num_components
        NUM_PAGES,  # [1] num_pages
        PAGE_SIZE,  # [2] page_size
        direction,  # [3] direction: 1=H2D, 2=D2H
        buf["dev_indices"].data_ptr(),  # [4] device_indices_ptr
        buf["host_indices"].data_ptr(),  # [5] host_indices_ptr
        mode,  # [6] host_layout_mode: 0=page_first, 1=layer_first
        0,  # [7] reserved (must be 0)
    ]
    meta += _component_fields(buf["dev_k"].data_ptr(), buf["host_k"].data_ptr(), K_DIM, mode)
    meta += _component_fields(buf["dev_v"].data_ptr(), buf["host_v"].data_ptr(), V_DIM, mode)
    meta += [0] * (KV_EXCHANGE_META_SIZE - len(meta))
    assert len(meta) == KV_EXCHANGE_META_SIZE, f"meta size mismatch: {len(meta)} != {KV_EXCHANGE_META_SIZE}"
    if buf["mode"] == MODE_AIV:
        _rewrite_host_base_to_dva(meta)
        buf["meta"].copy_(torch.tensor(meta, dtype=torch.int64).npu())
    else:
        buf["meta"].copy_(torch.tensor(meta, dtype=torch.int64))


def _verify_bit_exact(actual: torch.Tensor, expected: torch.Tensor, tag: str, rank_id: int) -> None:
    """Bit-exact comparison; the copy is byte-level so any mismatch indicates addressing or data corruption."""
    if not torch.equal(actual, expected):
        max_abs = (actual.to(torch.float32) - expected.to(torch.float32)).abs().max().item()
        mismatch_count = int((actual != expected).sum().item())
        raise AssertionError(
            f"rank_id:{rank_id} {tag}: precision mismatch, max_abs={max_abs}, mismatch_count={mismatch_count}"
        )


def _exchange(buf: dict, direction: int, rank_id: int) -> None:
    """Build meta for the given direction, launch kv_exchange_copy and synchronize."""
    _build_meta(buf, direction)
    ret = offload.kv_exchange_copy(buf["meta"], buf["device"])
    assert ret == 0, f"rank_id:{rank_id} kv_exchange_copy (direction={direction}) failed, ret={ret}"
    torch.npu.synchronize()


def _alloc_buffers(mode: str, host_layout_mode: int) -> dict:
    """Allocate host (offload pool) and device (layer_first) KV buffers plus identity indices.

    Host KV buffers come from the offload DRAM pool; their layout follows
    host_layout_mode: page_first [page, layer, page_size, dim] or layer_first
    [layer, page, page_size, dim]. Device KV buffers are always layer_first
    [layer, page, page_size, dim].
    In aiv mode the meta and the token indices live on device (the kernel reads
    them through GM);
    """
    host_k = offload.empty(_host_shape(K_DIM, host_layout_mode), dtype=torch.bfloat16)
    host_v = offload.empty(_host_shape(V_DIM, host_layout_mode), dtype=torch.bfloat16)
    # Device KV buffers in layer_first layout [layer, page, page_size, dim].
    dev_k = torch.zeros([NUM_LAYERS, NUM_PAGES, PAGE_SIZE, K_DIM], dtype=torch.bfloat16).npu()
    dev_v = torch.zeros([NUM_LAYERS, NUM_PAGES, PAGE_SIZE, V_DIM], dtype=torch.bfloat16).npu()
    # Identity indices: indices[p * PAGE_SIZE] = p * PAGE_SIZE, so for page p,
    # devicePage_ = hostPage_ = p (no permutation).
    indices = torch.arange(NUM_PAGES * PAGE_SIZE, dtype=torch.int64).npu()
    meta = torch.zeros(KV_EXCHANGE_META_SIZE, dtype=torch.int64).npu()

    return {
        "mode": mode,
        "host_k": host_k,
        "host_v": host_v,
        "dev_k": dev_k,
        "dev_v": dev_v,
        "dev_indices": indices,
        "host_indices": indices,
        "meta": meta,
        "device": dev_k.device,
        "host_layout_mode": host_layout_mode,
    }


def _check_h2d(buf: dict, rank_id: int) -> None:
    """H2D precision check: fill host with unique pattern, zero device, verify device equals host re-laid."""
    mode = buf["host_layout_mode"]
    host_k_pattern = _make_host_pattern(K_DIM, mode)
    host_v_pattern = _make_host_pattern(V_DIM, mode)
    buf["host_k"].copy_(host_k_pattern)
    buf["host_v"].copy_(host_v_pattern)
    buf["dev_k"].zero_()
    buf["dev_v"].zero_()

    _exchange(buf, DIRECTION_H2D, rank_id)

    # Device is layer_first [l, p, e]; expected device is the host pattern re-laid to [l, p, e].
    _verify_bit_exact(buf["dev_k"].cpu(), _relayout(host_k_pattern, mode), "H2D K", rank_id)
    _verify_bit_exact(buf["dev_v"].cpu(), _relayout(host_v_pattern, mode), "H2D V", rank_id)


def _check_d2h(buf: dict, rank_id: int) -> None:
    """D2H precision check: fill device with a different pattern, zero host, verify host equals device re-laid."""
    mode = buf["host_layout_mode"]
    dev_k_pattern = _make_device_pattern(NUM_PAGES, NUM_LAYERS, K_DIM)
    dev_v_pattern = _make_device_pattern(NUM_PAGES, NUM_LAYERS, V_DIM)
    buf["dev_k"].copy_(dev_k_pattern)
    buf["dev_v"].copy_(dev_v_pattern)
    buf["host_k"].zero_()
    buf["host_v"].zero_()

    _exchange(buf, DIRECTION_D2H, rank_id)

    # Expected host: device pattern re-laid from [l, p, e] to the host layout of the mode.
    _verify_bit_exact(buf["host_k"].cpu(), _relayout(dev_k_pattern, mode), "D2H K", rank_id)
    _verify_bit_exact(buf["host_v"].cpu(), _relayout(dev_v_pattern, mode), "D2H V", rank_id)


def _report_bandwidth(buf: dict, rank_id: int) -> None:
    """Informational bandwidth report; meta is built once so only the copy itself is timed."""
    _build_meta(buf, DIRECTION_H2D)

    def copy_fn():
        ret = offload.kv_exchange_copy(buf["meta"], buf["device"])
        assert ret == 0, f"rank_id:{rank_id} kv_exchange_copy (timed) failed, ret={ret}"

    for _ in range(WARMUP_ITERS):
        copy_fn()
    torch.npu.synchronize()

    def timed_fn():
        for _ in range(TIMED_ITERS):
            copy_fn()

    report_timing(rank_id, "npu_event", time_with_npu_event(timed_fn), _total_bytes() * TIMED_ITERS)


def _rank_main(
    rank_id: int,
    device_id: int,
    mode: str,
    num_layers: int,
    k_dim: int,
    v_dim: int,
    num_pages_list: list,
    pool_size: int,
    sync: mp.Barrier,
) -> None:
    global NUM_LAYERS, K_DIM, V_DIM, NUM_PAGES
    mf.set_log_level(3)
    torch.npu.set_device(device_id)
    NUM_LAYERS, K_DIM, V_DIM = num_layers, k_dim, v_dim

    config = offload.OffloadConfig()
    config.device_id = device_id
    config.reserve_size = pool_size
    config.alloc_size = pool_size
    config.flags = offload.OFFLOAD_FLAG_GIANT_PAGE
    config.scene = offload.Scene.LOCAL

    assert offload.initialize(config) == 0, f"rank_id:{rank_id} offload.initialize failed"

    for pages in num_pages_list:
        NUM_PAGES = pages
        total_mib = _total_bytes() / (1 << 20)
        for layout_mode in (HOST_LAYOUT_PAGE_FIRST, HOST_LAYOUT_LAYER_FIRST):
            layout_name = "layer_first" if layout_mode == HOST_LAYOUT_LAYER_FIRST else "page_first"
            print(
                f"rank_id:{rank_id} === mode:{mode} layout:{layout_name} pages:{NUM_PAGES} "
                f"layers:{NUM_LAYERS} page_size:{PAGE_SIZE} k_dim:{K_DIM} v_dim:{V_DIM} "
                f"data:{total_mib:.2f} MiB ===",
                flush=True,
            )
            buf = _alloc_buffers(mode, layout_mode)
            if total_mib <= PRECISION_CHECK_MAX_MIB:
                _check_h2d(buf, rank_id)
                _check_d2h(buf, rank_id)
            else:
                print(
                    f"rank_id:{rank_id} skip precision check ({total_mib:.2f} MiB > "
                    f"{PRECISION_CHECK_MAX_MIB} MiB), bandwidth only",
                    flush=True,
                )
            _report_bandwidth(buf, rank_id)
            del buf

    offload.uninitialize()
    sync.wait()
    print(f"rank_id:{rank_id} kv_exchange_offload[{mode}]: precision OK (page_first + layer_first)", flush=True)


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
        description="kv_exchange_offload: precision verification and bandwidth benchmark of H2D/D2H KV exchange "
        "in page_first and layer_first host layouts"
    )
    parser.add_argument(
        "--mode",
        type=str,
        choices=[MODE_AIV],
        default=MODE_AIV,
        help="kv_exchange_copy backend: aiv (AIV MTE kernel, default)",
    )
    parser.add_argument(
        "--world_size",
        type=int,
        default=DEFAULT_WORLD_SIZE,
        help=f"number of ranks, range [1, chip max] (default: {DEFAULT_WORLD_SIZE})",
    )
    parser.add_argument(
        "--data_mibs",
        type=str,
        default="1",
        help="comma-separated total data sizes in MiB to benchmark, e.g. 1,16,64,256,1024 (default: 1)",
    )
    parser.add_argument(
        "--num_layers",
        type=int,
        default=NUM_LAYERS,
        help=f"number of KV layers (default: {NUM_LAYERS})",
    )
    parser.add_argument(
        "--k_dim",
        type=int,
        default=K_DIM,
        help=f"K component head dim (default: {K_DIM})",
    )
    parser.add_argument(
        "--v_dim",
        type=int,
        default=V_DIM,
        help=f"V component head dim (default: {V_DIM})",
    )
    args = parser.parse_args()

    try:
        args.data_mibs = [float(m) for m in args.data_mibs.split(",")]
    except ValueError:
        raise ValueError(f"--data_mibs must be comma-separated numbers, got: {args.data_mibs}")
    if not args.data_mibs or any(m <= 0 for m in args.data_mibs):
        raise ValueError(f"--data_mibs values must be positive, got: {args.data_mibs}")
    if args.num_layers <= 0 or args.k_dim <= 0 or args.v_dim <= 0:
        raise ValueError(
            f"geometry must be positive, num_layers: {args.num_layers}, k_dim: {args.k_dim}, v_dim: {args.v_dim}"
        )

    chip_type = _detect_chip_type()
    chip_max = CHIP_MAX_WORLD_SIZE.get(chip_type, FALLBACK_MAX_WORLD_SIZE)
    if not 1 <= args.world_size <= chip_max:
        raise ValueError(f"world_size={args.world_size} out of range [1, {chip_max}] on chip {chip_type or 'unknown'}")
    return args


def main():
    global PAGE_SIZE
    args = _parse_args()

    # Derive the page count per requested data size; the offload pool must hold
    # the host K/V buffers of the largest size (2x headroom over the raw bytes).
    num_pages_list = [_pages_for_total_mib(m, args.num_layers, args.k_dim, args.v_dim) for m in args.data_mibs]
    row_bytes = args.num_layers * PAGE_SIZE * (args.k_dim + args.v_dim) * BFLOAT16_BYTES
    pool_size = max(ONE_GIB, 2 * max(num_pages_list) * row_bytes)

    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(args.world_size)

    procs = []
    for rank_id in range(args.world_size):
        p = mp.Process(
            target=_rank_main,
            args=(
                rank_id,
                rank_id,
                args.mode,
                args.num_layers,
                args.k_dim,
                args.v_dim,
                num_pages_list,
                pool_size,
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
    print(f"kv_exchange_offload[{args.mode}]: all ranks OK", flush=True)


if __name__ == "__main__":
    main()
