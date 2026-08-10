#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import ctypes
import os
import struct

from urma_example_common import (
    HOST_RANK,
    ITEM_BYTES,
    NPU_RANK,
    POOL_BYTES,
    DEFAULT_SEED,
    ValidationError,
    _accept_control_connection,
    _connect_control,
    _create_control_server,
    _fail,
    _log_debug,
    _log_info,
    _recv,
    _runtime_device_id,
    _send,
)


BYTE_ARRAY = ctypes.c_uint8 * POOL_BYTES


def _host_pattern(seed):
    return BYTE_ARRAY.from_buffer_copy(bytes((index * 131 + seed) & 0xFF for index in range(POOL_BYTES)))


def _fnv1a(data):
    checksum = 1469598103934665603
    for value in data:
        checksum ^= int(value)
        checksum = (checksum * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return checksum


def _first_mismatch(expected, actual):
    for index, (expected_value, actual_value) in enumerate(zip(expected, actual)):
        if expected_value != actual_value:
            return index
    return None if len(expected) == len(actual) else min(len(expected), len(actual))


def _expected_bytes(offset, size, seed):
    return bytes(((offset + index) * 131 + seed) & 0xFF for index in range(size))


def _checked_add(args, address, length, stage):
    if address < 0 or length < 0 or address + length > (1 << 64) - 1:
        _fail(args, stage, f"address addition overflow, address=0x{address:x} length={length}")
    return address + length


def _build_copy_inputs(args, host_gva, hbm_va, torch, count, item_bytes, offset):
    try:
        src_addresses = [host_gva + offset + index * item_bytes for index in range(count)]
        dst_addresses = [hbm_va + index * item_bytes for index in range(count)]
        src_ptrs = torch.tensor(src_addresses, dtype=torch.int64).npu()
        dst_ptrs = torch.tensor(dst_addresses, dtype=torch.int64).npu()
        len_ptrs = torch.full((count,), item_bytes, dtype=torch.int64).npu()
        device = torch.device(f"npu:{_runtime_device_id(args)}")
    except Exception as exc:
        _fail(args, "copy_prepare", f"address tensor construction failed, count={count} "
              f"item_bytes={item_bytes} offset={offset}: {exc}")
    return src_ptrs, dst_ptrs, len_ptrs, device


def _submit_sparse_copy(args, mf_acc_offload, torch, src_ptrs, dst_ptrs, len_ptrs, count, device):
    _log_debug(args, "sparse_copy_urma", f"submit count={count} runtime_device={_runtime_device_id(args)} "
                                      f"torch_device={device}")
    try:
        ret = mf_acc_offload.sparse_copy_urma(src_ptrs, dst_ptrs, len_ptrs, count, device)
    except Exception as exc:
        _fail(args, "sparse_copy_urma", f"copy raised, count={count} "
              f"runtime_device={_runtime_device_id(args)}: {exc}")
    _log_debug(args, "sparse_copy_urma", f"submit returned ret={ret} count={count}")
    if ret != 0:
        _fail(args, "sparse_copy_urma", f"copy failed, count={count} "
              f"runtime_device={_runtime_device_id(args)}", ret)
    try:
        torch.npu.synchronize()
    except Exception as exc:
        _fail(args, "copy_fence", f"NPU synchronize failed, count={count}: {exc}")
    _log_debug(args, "copy_fence", f"synchronize completed count={count}")


def _read_hbm_staging(args, handle, bm, hbm_gva, total_bytes):
    staging = (ctypes.c_uint8 * total_bytes)()
    try:
        ret = handle.copy_data(hbm_gva, ctypes.addressof(staging), total_bytes, bm.BmCopyType.G2H, 0)
    except Exception as exc:
        _fail(args, "g2h_verify", f"HBM staging read raised, gva=0x{hbm_gva:x} "
              f"bytes={total_bytes}: {exc}")
    if ret != 0:
        _fail(args, "g2h_verify", f"HBM staging read failed, gva=0x{hbm_gva:x} bytes={total_bytes}", ret)
    return bytes(staging)


def _copy_batch(args, handle, bm, host_gva, hbm_gva, hbm_va, torch, mf_acc_offload,
                count, item_bytes, offset, seed, round_id=None):
    total_bytes = count * item_bytes
    _log_debug(
        args,
        "copy_batch",
        f"start round={round_id} count={count} item_bytes={item_bytes} offset={offset} bytes={total_bytes} "
        f"host_gva=0x{host_gva:x} hbm_gva=0x{hbm_gva:x} hbm_va=0x{hbm_va:x}",
    )
    _checked_add(args, host_gva + offset, total_bytes, "copy_range")
    _checked_add(args, hbm_va, total_bytes, "destination_range")
    src_ptrs, dst_ptrs, len_ptrs, device = _build_copy_inputs(
        args, host_gva, hbm_va, torch, count, item_bytes, offset)
    _submit_sparse_copy(args, mf_acc_offload, torch, src_ptrs, dst_ptrs, len_ptrs, count, device)
    actual = _read_hbm_staging(args, handle, bm, hbm_gva, total_bytes)
    expected = _expected_bytes(offset, total_bytes, seed)
    mismatch = _first_mismatch(expected, actual)
    if mismatch is not None:
        _fail(args, "data_verify", f"data mismatch, count={count} item_bytes={item_bytes} offset={offset} "
              f"first_mismatch={mismatch}")
    _log_debug(
        args,
        "data_verify",
        f"passed bytes={total_bytes} expected_checksum={_fnv1a(expected)} actual_checksum={_fnv1a(actual)}",
    )
    result = {
        "count": count,
        "item_bytes": item_bytes,
        "offset": offset,
        "bytes": total_bytes,
        "expected_checksum": _fnv1a(expected),
        "actual_checksum": _fnv1a(actual),
        "first_mismatch": None,
    }
    if round_id is not None:
        result["round"] = round_id
    _log_info(args, f"sparse_copy_urma round-case count={count} item_bytes={item_bytes} offset={offset} "
                    f"bytes={total_bytes} ret=0 fence=complete")
    return result


def _run_local_cases(args, handle, bm, host_gva, hbm_gva, hbm_va, torch, mf_acc_offload):
    results = []
    for round_id in range(args.rounds):
        for size in args.sizes:
            results.append(_copy_batch(args, handle, bm, host_gva, hbm_gva, hbm_va, torch, mf_acc_offload,
                                       1, size, 0, DEFAULT_SEED, round_id))
        for count in args.batch_counts:
            if count == 1:
                offset = 2 * ITEM_BYTES
            elif count == 999:
                offset = ITEM_BYTES
            else:
                offset = POOL_BYTES - count * ITEM_BYTES
            result = _copy_batch(args, handle, bm, host_gva, hbm_gva, hbm_va, torch, mf_acc_offload,
                                 count, ITEM_BYTES, offset, DEFAULT_SEED, round_id)
            results.append(result)
    return results


def _summarize_cases(cases):
    expected = b"".join(struct.pack("!Q", case["expected_checksum"]) for case in cases)
    actual = b"".join(struct.pack("!Q", case["actual_checksum"]) for case in cases)
    return {
        "bytes": sum(case["bytes"] for case in cases),
        "expected_checksum": _fnv1a(expected),
        "actual_checksum": _fnv1a(actual),
        "first_mismatch": next((case["first_mismatch"] for case in cases if case["first_mismatch"] is not None), None),
        "hcomm_ret": 0,
    }


def _run_local_negative(args, handle, host_gva, hbm_gva, hbm_va, torch, mf_acc_offload):
    if args.negative == "overflow-len":
        try:
            _checked_add(args, (1 << 64) - 4, 8, "negative_range")
        except ValidationError:
            return {"negative": args.negative, "expected_failure": True, "hcomm_ret": "python_precheck"}
        _fail(args, "negative_test", "overflow-len precheck unexpectedly succeeded")
    if args.negative == "bad-gva":
        source = host_gva + POOL_BYTES
        length = 1
    elif args.negative == "cross-range":
        source = host_gva + POOL_BYTES - 1
        length = 2
    else:
        source = host_gva
        length = 1
    _checked_add(args, source, length, "negative_range")
    runtime_device_id = _runtime_device_id(args)
    target_device = runtime_device_id + 1 if args.negative == "wrong-device" else runtime_device_id
    try:
        src_ptrs = torch.tensor([source], dtype=torch.int64).npu()
        dst_ptrs = torch.tensor([hbm_va], dtype=torch.int64).npu()
        len_ptrs = torch.tensor([length], dtype=torch.int64).npu()
        device = torch.device(f"npu:{target_device}")
    except Exception as exc:
        _fail(args, "negative_prepare", f"negative input tensor construction failed, negative={args.negative} "
              f"source=0x{source:x} destination=0x{hbm_va:x} length={length}: {exc}")
    try:
        ret = mf_acc_offload.sparse_copy_urma(src_ptrs, dst_ptrs, len_ptrs, 1, device)
    except Exception as exc:
        _log_info(args, f"negative={args.negative} failed as expected in sparse_copy_urma: {exc}")
        return {"negative": args.negative, "expected_failure": True, "hcomm_ret": "exception"}
    if ret == 0:
        _fail(args, "negative_test", f"negative={args.negative} unexpectedly succeeded", ret)
    _log_info(args, f"negative={args.negative} failed as expected, ret={ret}")
    return {"negative": args.negative, "expected_failure": True, "hcomm_ret": int(ret)}


def _validate_host_source(handle, bm, args):
    try:
        host_gva = handle.peer_rank_ptr(HOST_RANK, bm.BmMemType.HOST)
    except Exception as exc:
        _fail(args, "host_gva", f"Host GVA lookup raised, rank={HOST_RANK}: {exc}")
    if host_gva == 0:
        _fail(args, "host_gva", "fixed Host GVA allocation failed")
    _log_debug(args, "host_gva", f"peer_rank={HOST_RANK} host_gva=0x{host_gva:x}")
    try:
        host_view = handle.gva_to_va(host_gva, bm.BmMemType.LOCAL_HOST)
    except Exception as exc:
        _fail(args, "host_gva", f"Host GVA to VA raised, gva=0x{host_gva:x}: {exc}")
    if host_view != host_gva:
        _fail(args, "host_gva", f"Host GVA/view mismatch: gva=0x{host_gva:x} view=0x{host_view:x}")
    _log_debug(args, "host_gva", f"local_host_view=0x{host_view:x} equality={host_view == host_gva}")
    try:
        pattern = _host_pattern(DEFAULT_SEED)
        ret = handle.copy_data(ctypes.addressof(pattern), host_gva, POOL_BYTES, bm.BmCopyType.H2G, 0)
    except Exception as exc:
        _fail(args, "host_source", f"source initialization raised, gva=0x{host_gva:x} "
              f"bytes={POOL_BYTES}: {exc}")
    if ret != 0:
        _fail(args, "host_source", f"source initialization failed, gva=0x{host_gva:x}", ret)
    _log_debug(
        args,
        "host_source",
        f"H2G initialized ret={ret} gva=0x{host_gva:x} bytes={POOL_BYTES} checksum={_fnv1a(pattern)}",
    )
    return host_gva, pattern, _fnv1a(pattern)


def _validate_hello(args, message):
    if not isinstance(message, dict) or message.get("type") != "HELLO" or message.get("version") != 1:
        _fail(args, "control_hello", f"unexpected HELLO message: {message}")
    if message.get("role") != "npu" or message.get("round") != 0:
        _fail(args, "control_hello", f"HELLO role/round mismatch: {message}")
    if not isinstance(message.get("pid"), int) or message["pid"] <= 0:
        _fail(args, "control_hello", f"HELLO pid is invalid: {message.get('pid')}")
    fields = {
        "rank": NPU_RANK,
        "physical_device_id": args.physical_device_id,
        "logical_device_id": args.device_id,
        "runtime_device_id": _runtime_device_id(args),
        "host_eid": args.host_eid,
        "device_eid": args.device_eid,
    }
    for name, expected in fields.items():
        if message.get(name) != expected:
            _fail(args, "control_hello", f"HELLO field mismatch: {name} expected={expected} "
                  f"got={message.get(name)}")


def _validate_source_ready(args, message, host_gva):
    if (not isinstance(message, dict) or message.get("type") != "SOURCE_READY" or
            message.get("version") != 1 or message.get("round") != 0):
        _fail(args, "control_source", f"unexpected SOURCE_READY message: {message}")
    expected = {
        "host_gva": host_gva,
        "pool_bytes": POOL_BYTES,
        "item_bytes": ITEM_BYTES,
        "seed": DEFAULT_SEED,
        "checksum": _fnv1a(_expected_bytes(0, POOL_BYTES, DEFAULT_SEED)),
    }
    for name, value in expected.items():
        if message.get(name) != value:
            _fail(args, "control_source", f"SOURCE_READY field mismatch: {name} "
                  f"expected={value} got={message.get(name)}")


def _validate_copy_ready(args, message, host_gva):
    if (not isinstance(message, dict) or message.get("type") != "COPY_READY" or
            message.get("version") != 1 or message.get("round") != 0):
        _fail(args, "control_route", f"unexpected COPY_READY message: {message}")
    if message.get("imported_gva") != host_gva or message.get("import_view") != host_gva:
        _fail(args, "control_route", "Host import equality failed in control metadata")
    if (message.get("route_mode") != "HOST_DRAM" or message.get("hbm_gva", 0) == 0 or
            message.get("hbm_va", 0) == 0):
        _fail(args, "control_route", f"invalid route metadata: {message}")


def _validate_copy_done(args, message):
    if (not isinstance(message, dict) or message.get("type") != "COPY_DONE" or
            message.get("version") != 1 or message.get("round") != args.rounds - 1):
        _fail(args, "control_done", f"unexpected COPY_DONE message: {message}")
    required = ("bytes", "expected_checksum", "actual_checksum", "first_mismatch", "hcomm_ret")
    if any(name not in message for name in required):
        _fail(args, "control_done", f"COPY_DONE metadata is incomplete: {message}")
    if args.negative != "none":
        if (not message.get("expected_failure") or message.get("negative") != args.negative or
                "hcomm_ret" not in message):
            _fail(args, "control_done", f"negative result mismatch: {message}")
        return
    if (message.get("result") != "PASS" or message.get("hcomm_ret") != 0 or
            message.get("first_mismatch") is not None or not isinstance(message.get("cases"), list)):
        _fail(args, "control_done", f"copy result is not PASS: {message}")
    if message.get("expected_checksum") != message.get("actual_checksum"):
        _fail(args, "control_done", f"copy checksum mismatch: expected={message.get('expected_checksum')} "
              f"actual={message.get('actual_checksum')}")


def _run_local_host(args, handle, bm):
    host_gva, pattern, source_checksum = _validate_host_source(handle, bm, args)
    _log_info(args, f"validation_role=host rank=0 gva=0x{host_gva:x} va=0x{host_gva:x} "
                    f"bytes={POOL_BYTES} key_exported=true")
    with _create_control_server(args) as server:
        conn = _accept_control_connection(args, server)
        with conn:
            conn.settimeout(args.ctrl_timeout)
            _validate_hello(args, _recv(conn, args, "control_hello"))
            _send(conn, {
                "type": "SOURCE_READY",
                "version": 1,
                "round": 0,
                "host_gva": host_gva,
                "pool_bytes": POOL_BYTES,
                "item_bytes": ITEM_BYTES,
                "seed": DEFAULT_SEED,
                "checksum": source_checksum,
            }, args, "control_source")
            _validate_copy_ready(args, _recv(conn, args, "control_route"), host_gva)
            done = _recv(conn, args, "control_done")
            _validate_copy_done(args, done)
            _send(conn, {"type": "RELEASE", "version": 1, "round": args.rounds - 1}, args, "control_release")
    del pattern
    _log_info(args, "peer_result=PASS release=sent")


def _lookup_npu_views(args, handle, bm):
    try:
        host_gva = handle.peer_rank_ptr(HOST_RANK, bm.BmMemType.HOST)
        hbm_gva = handle.peer_rank_ptr(NPU_RANK, bm.BmMemType.DEVICE)
    except Exception as exc:
        _fail(args, "pool_lookup", f"peer GVA lookup raised, host_rank={HOST_RANK} "
              f"device_rank={NPU_RANK}: {exc}")
    if host_gva == 0 or hbm_gva == 0:
        _fail(args, "pool_lookup", f"invalid GVA host=0x{host_gva:x} hbm=0x{hbm_gva:x}")
    try:
        import_view = handle.gva_to_va(host_gva, bm.BmMemType.LOCAL_HOST)
        hbm_va = handle.gva_to_va(hbm_gva, bm.BmMemType.LOCAL_DEVICE)
    except Exception as exc:
        _fail(args, "pool_view", f"GVA to VA raised, host=0x{host_gva:x} hbm=0x{hbm_gva:x}: {exc}")
    if import_view != host_gva:
        _fail(args, "import_view", f"Host GVA/import view mismatch: gva=0x{host_gva:x} view=0x{import_view:x}")
    if hbm_va == 0:
        _fail(args, "hbm_view", f"HBM GVA to device VA failed: gva=0x{hbm_gva:x}")
    _log_debug(
        args,
        "pool_view",
        f"host_gva=0x{host_gva:x} import_view=0x{import_view:x} hbm_gva=0x{hbm_gva:x} hbm_va=0x{hbm_va:x}",
    )
    return host_gva, hbm_gva, import_view, hbm_va


def _local_npu_handshake(args, conn, host_gva, hbm_gva, hbm_va, import_view):
    _send(conn, {
        "type": "HELLO",
        "version": 1,
        "role": "npu",
        "rank": NPU_RANK,
        "pid": os.getpid(),
        "round": 0,
        "physical_device_id": args.physical_device_id,
        "logical_device_id": args.device_id,
        "runtime_device_id": _runtime_device_id(args),
        "host_eid": args.host_eid,
        "device_eid": args.device_eid,
    }, args, "control_hello")
    source = _recv(conn, args, "control_source")
    _validate_source_ready(args, source, host_gva)
    _send(conn, {
        "type": "COPY_READY",
        "version": 1,
        "round": 0,
        "imported_gva": host_gva,
        "import_view": import_view,
        "hbm_gva": hbm_gva,
        "hbm_va": hbm_va,
        "route_mode": "HOST_DRAM",
    }, args, "control_route")


def _run_local_npu(args, handle, bm):
    import torch
    import mf_acc_offload

    host_gva, hbm_gva, import_view, hbm_va = _lookup_npu_views(args, handle, bm)
    with _connect_control(args) as conn:
        conn.settimeout(args.ctrl_timeout)
        _local_npu_handshake(args, conn, host_gva, hbm_gva, hbm_va, import_view)
        _log_info(args, f"rank=1 runtime_device={_runtime_device_id(args)} "
                        f"logical_device={args.device_id} physical_device={args.physical_device_id} "
                        f"host_gva=0x{host_gva:x} import_view=0x{import_view:x} equality=pass")
        _log_info(args, f"hbm_gva=0x{hbm_gva:x} hbm_va=0x{hbm_va:x} registered_pool_bytes={POOL_BYTES} "
                        "route=HOST_DRAM")
        if args.negative == "none":
            cases = _run_local_cases(args, handle, bm, host_gva, hbm_gva, hbm_va, torch, mf_acc_offload)
            done = _summarize_cases(cases)
            done.update({"type": "COPY_DONE", "version": 1, "round": args.rounds - 1,
                         "result": "PASS", "cases": cases})
            _send(conn, done, args, "control_done")
        else:
            negative = _run_local_negative(args, handle, host_gva, hbm_gva, hbm_va, torch, mf_acc_offload)
            negative.update({"type": "COPY_DONE", "version": 1, "round": args.rounds - 1,
                             "result": "EXPECTED_FAILURE", "bytes": 0, "expected_checksum": 0,
                             "actual_checksum": 0, "first_mismatch": None})
            _send(conn, negative, args, "control_done")
        release = _recv(conn, args, "control_release")
        if (not isinstance(release, dict) or release.get("type") != "RELEASE" or
                release.get("version") != 1 or release.get("round") != args.rounds - 1):
            _fail(args, "control_release", f"unexpected RELEASE message: {release}")
    _log_info(args, "02_host_device_urma local validation: PASS")
