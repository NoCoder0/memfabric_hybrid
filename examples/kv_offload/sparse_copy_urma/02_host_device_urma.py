#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import argparse
import ctypes
import json
import os
import socket
import struct
import sys


STORE_PORT = 8574
CTRL_PORT = 9877
WORLD_SIZE = 2
HOST_RANK, NPU_RANK = 0, 1
POOL_BYTES = 8 << 20
ITEM_BYTES = 4 << 10
DEFAULT_SEED = 17
MAX_CONTROL_BYTES = 1 << 20
BYTE_ARRAY = ctypes.c_uint8 * POOL_BYTES


class ValidationError(RuntimeError):
    """An expected validation or runtime failure already reported with context."""


def _role(args):
    return "host" if args.rank == HOST_RANK else "npu"


def _log_error(args, stage, detail, ret=None):
    ret_text = "" if ret is None else f" ret={ret}"
    physical = "unset" if args.physical_device_id is None else args.physical_device_id
    print(
        f"[ERROR] stage={stage} role={_role(args)} rank={args.rank} device={args.device_id} "
        f"physical_device={physical}{ret_text} detail={detail}",
        file=sys.stderr,
        flush=True,
    )


def _fail(args, stage, detail, ret=None):
    _log_error(args, stage, detail, ret)
    raise ValidationError(detail)


def _log_info(args, message):
    print(f"[{_role(args)}] {message}", flush=True)


def _load_runtime():
    import memfabric_hybrid as mf
    from memfabric_hybrid import bm

    return mf, bm


def _load_configured_runtime(args):
    try:
        mf, bm = _load_runtime()
        mf.set_log_level(1)
    except Exception as exc:
        _fail(args, "runtime_import", f"MemFabric runtime import/configuration failed: {exc}")
    return mf, bm


def _read_exact(conn, size, args, stage):
    chunks = []
    remaining = size
    while remaining:
        try:
            chunk = conn.recv(remaining)
        except OSError as exc:
            _fail(args, stage, f"control recv failed: {exc}")
        if not chunk:
            _fail(args, stage, "control connection closed before the message was complete")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _send(conn, message, args, stage):
    try:
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    except (TypeError, ValueError) as exc:
        _fail(args, stage, f"control message serialization failed: {exc}")
    if len(payload) > MAX_CONTROL_BYTES:
        _fail(args, stage, f"control message is too large: bytes={len(payload)}")
    try:
        conn.sendall(struct.pack("!I", len(payload)) + payload)
    except OSError as exc:
        _fail(args, stage, f"control send failed: {exc}")


def _recv(conn, args, stage):
    header = _read_exact(conn, 4, args, stage)
    payload_size = struct.unpack("!I", header)[0]
    if payload_size == 0 or payload_size > MAX_CONTROL_BYTES:
        _fail(args, stage, f"invalid control payload size: {payload_size}")
    payload = _read_exact(conn, payload_size, args, stage)
    try:
        message = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        _fail(args, stage, f"invalid control JSON: {exc}")
    if not isinstance(message, (dict, str)):
        _fail(args, stage, f"unsupported control message type: {type(message).__name__}")
    return message


def _create_control_server(args):
    try:
        return socket.create_server((args.bind_ip, args.ctrl_port))
    except OSError as exc:
        _fail(args, "control_listener", f"bind failed, ip={args.bind_ip} port={args.ctrl_port}: {exc}")


def _accept_control_connection(args, server):
    try:
        return server.accept()[0]
    except OSError as exc:
        _fail(args, "control_accept", f"accept failed, ip={args.bind_ip} port={args.ctrl_port}: {exc}")


def _connect_control(args):
    try:
        return socket.create_connection((args.head_ip, args.ctrl_port), timeout=args.ctrl_timeout)
    except OSError as exc:
        _fail(args, "control_connect", f"connect failed, ip={args.head_ip} port={args.ctrl_port} "
              f"timeout={args.ctrl_timeout}: {exc}")


def _config(bm, rank_id, head_ip):
    cfg = bm.BmConfig()
    cfg.rank_id = rank_id
    cfg.start_store = rank_id == HOST_RANK
    cfg.auto_ranking = False
    cfg.set_nic(f"tcp://{head_ip}:10005")
    return cfg


def _create_handle(args, bm, rank_id):
    store_url = f"tcp://{args.head_ip}:{args.store_port}"
    try:
        config = _config(bm, rank_id, args.head_ip)
        ret = bm.initialize(store_url, WORLD_SIZE, args.device_id, config)
    except Exception as exc:
        _fail(args, "bm_initialize", f"bm.initialize raised, store={store_url} rank={rank_id}: {exc}")
    if ret != 0:
        _fail(args, "bm_initialize", f"bm.initialize failed, store={store_url}", ret)

    is_host = rank_id == HOST_RANK
    handle = None
    try:
        try:
            handle = bm.create2(
                id=0,
                local_dram_size=POOL_BYTES if is_host else 0,
                max_dram_size=POOL_BYTES if is_host else 0,
                local_hbm_size=0 if is_host else POOL_BYTES,
                max_hbm_size=0 if is_host else POOL_BYTES,
                data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
                enable_56bits_gva=False,
            )
        except Exception as exc:
            _fail(args, "bm_create", f"bm.create2 failed: {exc}")
        if handle is None:
            _fail(args, "bm_create", "bm.create2 returned no handle")
        try:
            ret = handle.join()
        except Exception as exc:
            _fail(args, "bm_join", f"handle.join raised, rank={rank_id}: {exc}")
        if ret != 0:
            _fail(args, "bm_join", f"handle.join failed, rank={rank_id}", ret)
        return handle
    except ValidationError:
        if handle is not None:
            try:
                handle.destroy()
            except Exception as exc:
                _log_error(args, "bm_create_rollback", f"handle.destroy raised: {exc}")
        try:
            ret = bm.uninitialize(0)
            if ret != 0:
                _log_error(args, "bm_create_rollback", "bm.uninitialize failed", ret)
        except Exception as exc:
            _log_error(args, "bm_create_rollback", f"bm.uninitialize raised: {exc}")
        raise


def _cleanup(args, mf, bm, handle, joined, bm_initialized, mf_initialized):
    if handle is not None:
        if joined:
            try:
                ret = handle.leave()
                if ret != 0:
                    _log_error(args, "cleanup_leave", "handle.leave failed", ret)
            except Exception as exc:
                _log_error(args, "cleanup_leave", f"handle.leave raised: {exc}")
        try:
            handle.destroy()
        except Exception as exc:
            _log_error(args, "cleanup_destroy", f"handle.destroy raised: {exc}")
    if bm_initialized:
        try:
            ret = bm.uninitialize(0)
            if ret != 0:
                _log_error(args, "cleanup_bm", "bm.uninitialize failed", ret)
        except Exception as exc:
            _log_error(args, "cleanup_bm", f"bm.uninitialize raised: {exc}")
    if mf_initialized:
        try:
            mf.uninitialize()
        except Exception as exc:
            _log_error(args, "cleanup_mf", f"mf.uninitialize raised: {exc}")


def _normalize_eid(args, value, name):
    if value is None or len(value) != 32 or any(character not in "0123456789abcdefABCDEF" for character in value):
        _fail(args, "eid_validation", f"{name} must be exactly 32 hexadecimal characters")
    normalized = value.lower()
    if int(normalized, 16) == 0:
        _fail(args, "eid_validation", f"{name} must not be all zero")
    return normalized


def _select_eid(args, cli_value, env_name, option_name):
    env_value = os.environ.get(env_name)
    if cli_value is not None and env_value is not None:
        cli_normalized = _normalize_eid(args, cli_value, option_name)
        env_normalized = _normalize_eid(args, env_value, env_name)
        if cli_normalized != env_normalized:
            _fail(args, "eid_validation", f"{option_name} differs from {env_name}")
        return cli_normalized
    return _normalize_eid(args, cli_value or env_value, option_name if cli_value is not None else env_name)


def _parse_uint_option(args, value, name):
    if value is None or value < 0:
        _fail(args, "argument_validation", f"{name} must be a non-negative integer")
    return value


def _parse_positive_csv(args, text, name):
    values = []
    for item in text.split(","):
        try:
            value = int(item, 10)
        except ValueError:
            _fail(args, "argument_validation", f"{name} contains a non-integer: {item}")
        if value <= 0:
            _fail(args, "argument_validation", f"{name} contains a non-positive value: {value}")
        values.append(value)
    if not values:
        _fail(args, "argument_validation", f"{name} must not be empty")
    return tuple(values)


def _parse_common_options(args):
    args.device_id = _parse_uint_option(args, args.device_id, "--device-id")
    for value, name in ((args.store_port, "--store-port"), (args.ctrl_port, "--ctrl-port")):
        if value <= 0 or value > 65535:
            _fail(args, "argument_validation", f"{name} must be in 1..65535: {value}")
    if args.ctrl_timeout <= 0:
        _fail(args, "argument_validation", f"--ctrl-timeout must be positive: {args.ctrl_timeout}")


def _parse_local_options(args):
    args.device_id = _parse_uint_option(args, args.device_id, "--device-id")
    if args.physical_device_id is None:
        _fail(args, "argument_validation", "--physical-device-id is required in local validation mode")
    args.physical_device_id = _parse_uint_option(args, args.physical_device_id, "--physical-device-id")
    if args.rounds <= 0:
        _fail(args, "argument_validation", "--rounds must be positive")
    args.sizes = _parse_positive_csv(args, args.sizes, "--sizes")
    args.batch_counts = _parse_positive_csv(args, args.batch_counts, "--batch-counts")
    if max(args.sizes) > POOL_BYTES:
        _fail(args, "argument_validation", f"--sizes exceeds pool bytes: max={max(args.sizes)} pool={POOL_BYTES}")
    if max(args.batch_counts) * ITEM_BYTES > POOL_BYTES:
        _fail(args, "argument_validation", "--batch-counts exceeds the fixed source pool")

    args.host_eid = _select_eid(args, args.host_eid, "MF_HOST_URMA_EID", "--host-eid")
    args.device_eid = _select_eid(args, args.device_eid, "USE_LOCAL_EID", "--device-eid")
    expected_physical = os.environ.get("MF_LOCAL_DRAM_PHYSICAL_DEVICE_ID")
    expected_logical = os.environ.get("MF_LOCAL_DRAM_LOGICAL_DEVICE_ID")
    if expected_physical is not None and str(args.physical_device_id) != expected_physical:
        _fail(args, "device_mapping", "physical device id differs from EID tool metadata")
    if expected_logical is not None and str(args.device_id) != expected_logical:
        _fail(args, "device_mapping", "logical device id differs from EID tool metadata")


def _configure_local_environment(args):
    role = os.environ.get("MF_LOCAL_DRAM_VALIDATION_ROLE")
    if args.rank == HOST_RANK:
        if role not in (None, "host"):
            _fail(args, "validation_role", f"host rank requires role=host, got={role}")
        os.environ["MF_LOCAL_DRAM_VALIDATION_ROLE"] = "host"
        os.environ["MF_HOST_URMA_EID"] = args.host_eid
    elif role is not None:
        _fail(args, "validation_role", "device rank must not set MF_LOCAL_DRAM_VALIDATION_ROLE")
    else:
        os.environ["USE_LOCAL_EID"] = args.device_eid


def _set_local_device(args):
    try:
        import torch
        import torch_npu  # noqa: F401

        torch.npu.set_device(args.device_id)
    except Exception as exc:
        _fail(args, "acl_context", f"failed to set local NPU device: {exc}")


def _host_pattern(seed):
    return BYTE_ARRAY.from_buffer_copy(bytes((index * 131 + seed) & 0xFF for index in range(POOL_BYTES)))


def _fnv1a(data):
    checksum = 1469598103934665603
    for value in data:
        checksum ^= int(value)
        checksum = (checksum * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return checksum


def _checked_add(args, address, length, stage):
    if address < 0 or length < 0 or address + length > (1 << 64) - 1:
        _fail(args, stage, f"address addition overflow, address=0x{address:x} length={length}")
    return address + length


def _first_mismatch(expected, actual):
    for index, (expected_value, actual_value) in enumerate(zip(expected, actual)):
        if expected_value != actual_value:
            return index
    return None if len(expected) == len(actual) else min(len(expected), len(actual))


def _expected_bytes(offset, size, seed):
    return bytes(((offset + index) * 131 + seed) & 0xFF for index in range(size))


def _build_copy_inputs(args, host_gva, hbm_va, torch, count, item_bytes, offset):
    try:
        src_addresses = [host_gva + offset + index * item_bytes for index in range(count)]
        dst_addresses = [hbm_va + index * item_bytes for index in range(count)]
        src_ptrs = torch.tensor(src_addresses, dtype=torch.int64).npu()
        dst_ptrs = torch.tensor(dst_addresses, dtype=torch.int64).npu()
        len_ptrs = torch.full((count,), item_bytes, dtype=torch.int64).npu()
        device = torch.device(f"npu:{args.device_id}")
    except Exception as exc:
        _fail(args, "copy_prepare", f"address tensor construction failed, count={count} "
              f"item_bytes={item_bytes} offset={offset}: {exc}")
    return src_ptrs, dst_ptrs, len_ptrs, device


def _submit_sparse_copy(args, mf_acc_offload, torch, src_ptrs, dst_ptrs, len_ptrs, count, device):
    try:
        ret = mf_acc_offload.sparse_copy_urma(src_ptrs, dst_ptrs, len_ptrs, count, device)
    except Exception as exc:
        _fail(args, "sparse_copy_urma", f"copy raised, count={count} device={args.device_id}: {exc}")
    if ret != 0:
        _fail(args, "sparse_copy_urma", f"copy failed, count={count} device={args.device_id}", ret)
    try:
        torch.npu.synchronize()
    except Exception as exc:
        _fail(args, "copy_fence", f"NPU synchronize failed, count={count}: {exc}")


def _read_hbm_staging(args, handle, bm, hbm_gva, total_bytes):
    staging = (ctypes.c_uint8 * total_bytes)()
    try:
        ret = handle.copy_data(hbm_gva, ctypes.addressof(staging), total_bytes, bm.BmCopyType.G2H, 0)
    except Exception as exc:
        _fail(args, "g2h_verify", f"HBM staging read raised, gva=0x{hbm_gva:x} bytes={total_bytes}: {exc}")
    if ret != 0:
        _fail(args, "g2h_verify", f"HBM staging read failed, gva=0x{hbm_gva:x} bytes={total_bytes}", ret)
    return bytes(staging)


def _copy_batch(args, handle, bm, host_gva, hbm_gva, hbm_va, torch, mf_acc_offload,
                count, item_bytes, offset, seed, round_id=None):
    total_bytes = count * item_bytes
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
    target_device = args.device_id + 1 if args.negative == "wrong-device" else args.device_id
    try:
        src_ptrs = torch.tensor([source], dtype=torch.int64).npu()
        dst_ptrs = torch.tensor([hbm_va], dtype=torch.int64).npu()
        len_ptrs = torch.tensor([length], dtype=torch.int64).npu()
        device = torch.device(f"npu:{target_device}")
        ret = mf_acc_offload.sparse_copy_urma(src_ptrs, dst_ptrs, len_ptrs, 1, device)
    except Exception as exc:
        _log_info(args, f"negative={args.negative} failed as expected before completion: {exc}")
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
    try:
        host_view = handle.gva_to_va(host_gva, bm.BmMemType.LOCAL_HOST)
    except Exception as exc:
        _fail(args, "host_gva", f"Host GVA to VA raised, gva=0x{host_gva:x}: {exc}")
    if host_view != host_gva:
        _fail(args, "host_gva", f"Host GVA/view mismatch: gva=0x{host_gva:x} view=0x{host_view:x}")
    try:
        pattern = _host_pattern(DEFAULT_SEED)
        ret = handle.copy_data(ctypes.addressof(pattern), host_gva, POOL_BYTES, bm.BmCopyType.H2G, 0)
    except Exception as exc:
        _fail(args, "host_source", f"source initialization raised, gva=0x{host_gva:x} bytes={POOL_BYTES}: {exc}")
    if ret != 0:
        _fail(args, "host_source", f"source initialization failed, gva=0x{host_gva:x}", ret)
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
        "host_eid": args.host_eid,
        "device_eid": args.device_eid,
    }
    for name, expected in fields.items():
        if message.get(name) != expected:
            _fail(args, "control_hello", f"HELLO field mismatch: {name} expected={expected} got={message.get(name)}")


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
        _fail(args, "pool_lookup", f"peer GVA lookup raised, host_rank={HOST_RANK} device_rank={NPU_RANK}: {exc}")
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
        _log_info(args, f"rank=1 user_device={args.device_id} phy_device={args.physical_device_id} "
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


def _run_host_production(args, handle, bm):
    host_gva, pattern, _ = _validate_host_source(handle, bm, args)
    with _create_control_server(args) as server:
        conn = _accept_control_connection(args, server)
        with conn:
            _send(conn, {
                "host_gva": host_gva,
                "pool_bytes": POOL_BYTES,
                "item_bytes": ITEM_BYTES,
            }, args, "legacy_source")
            if _recv(conn, args, "legacy_ready") != "READY":
                _fail(args, "legacy_ready", "unexpected NPU handshake")
            if _recv(conn, args, "legacy_done") != "COPY_DONE":
                _fail(args, "legacy_done", "unexpected NPU completion")
            _send(conn, "HOST_RELEASE", args, "legacy_release")
    del pattern
    _log_info(args, "fixed-GVA route and source lifetime checks passed")


def _run_npu_production(args, handle, bm):
    import torch
    import mf_acc_offload

    peer_gva, hbm_gva, import_view, hbm_va = _lookup_npu_views(args, handle, bm)
    with _connect_control(args) as conn:
        conn.settimeout(args.ctrl_timeout)
        route_info = _recv(conn, args, "legacy_source")
        if not isinstance(route_info, dict) or route_info.get("host_gva") != peer_gva:
            _fail(args, "legacy_source", f"peer GVA differs from host metadata: {route_info}")
        if route_info.get("item_bytes") != ITEM_BYTES or route_info.get("pool_bytes") != POOL_BYTES:
            _fail(args, "legacy_source", f"source dimensions mismatch: {route_info}")
        _log_info(args, f"route peer={HOST_RANK} exported_gva=0x{peer_gva:x} import_view=0x{import_view:x} "
                        f"device={args.device_id}")
        _send(conn, "READY", args, "legacy_ready")
        for count in (1, 999, 1000, 1001):
            if count == 1:
                offset = 2 * ITEM_BYTES
            elif count == 999:
                offset = ITEM_BYTES
            else:
                offset = POOL_BYTES - count * ITEM_BYTES
            _copy_batch(args, handle, bm, peer_gva, hbm_gva, hbm_va, torch, mf_acc_offload,
                        count, ITEM_BYTES, offset, DEFAULT_SEED)
        _send(conn, "COPY_DONE", args, "legacy_done")
        if _recv(conn, args, "legacy_release") != "HOST_RELEASE":
            _fail(args, "legacy_release", "unexpected host completion")
    _log_info(args, "Host-DDR sparse_copy_urma checks passed")


def _run_host(args):
    # Temporary local DRAM validation branch; remove it with the validation build option after hardware sign-off.
    if args.local_dram_validation:
        _configure_local_environment(args)
        _set_local_device(args)
    elif args.eid is None:
        _fail(args, "argument_validation", "--eid is required for --rank 0")
    else:
        args.eid = _normalize_eid(args, args.eid, "--eid")
        os.environ["MF_HOST_URMA_EID"] = args.eid
    mf, bm = _load_configured_runtime(args)
    mf_initialized = False
    bm_initialized = False
    handle = None
    joined = False
    try:
        ret = mf.initialize()
        if ret != 0:
            _fail(args, "mf_initialize", "host mf.initialize failed", ret)
        mf_initialized = True
        handle = _create_handle(args, bm, HOST_RANK)
        bm_initialized = True
        joined = True
        if args.local_dram_validation:
            _run_local_host(args, handle, bm)
        else:
            _run_host_production(args, handle, bm)
    finally:
        _cleanup(args, mf, bm, handle, joined, bm_initialized, mf_initialized)


def _run_npu(args):
    _configure_local_environment(args) if args.local_dram_validation else None
    _set_local_device(args)
    mf, bm = _load_configured_runtime(args)
    mf_initialized = False
    bm_initialized = False
    handle = None
    joined = False
    try:
        ret = mf.initialize()
        if ret != 0:
            _fail(args, "mf_initialize", "NPU mf.initialize failed", ret)
        mf_initialized = True
        handle = _create_handle(args, bm, NPU_RANK)
        bm_initialized = True
        joined = True
        if args.local_dram_validation:
            _run_local_npu(args, handle, bm)
        else:
            _run_npu_production(args, handle, bm)
    finally:
        _cleanup(args, mf, bm, handle, joined, bm_initialized, mf_initialized)


def main():
    parser = argparse.ArgumentParser(description="Host-DDR to NPU HBM sparse_copy_urma example")
    parser.add_argument("--rank", type=int, required=True, choices=(HOST_RANK, NPU_RANK))
    parser.add_argument("--head-ip", required=True, help="Config-store and control-plane peer address")
    parser.add_argument("--eid", help="Legacy Host URMA EID, 32 hex characters; required for --rank 0")
    parser.add_argument("--device-id", type=int, default=0, help="Logical NPU device id used by the process")
    parser.add_argument("--store-port", type=int, default=STORE_PORT)
    parser.add_argument("--ctrl-port", type=int, default=CTRL_PORT)
    parser.add_argument("--bind-ip", default="0.0.0.0", help="Host-side control listener bind address")
    parser.add_argument("--local-dram-validation", action="store_true", help="Enable validation-only local DRAM mode")
    parser.add_argument("--physical-device-id", type=int, help="Physical device id reported by the EID tool")
    parser.add_argument("--host-eid", help="Local validation Host/DRAM EID; defaults to MF_HOST_URMA_EID")
    parser.add_argument("--device-eid", help="Local validation Device/HBM EID; defaults to USE_LOCAL_EID")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--sizes", default="1,4096,1048576", help="Single-copy byte sizes")
    parser.add_argument("--batch-counts", default="1,999,1000,1001", help="Batch item counts")
    parser.add_argument("--negative", choices=("none", "bad-gva", "cross-range", "overflow-len", "wrong-device"),
                        default="none")
    parser.add_argument("--ctrl-timeout", type=float, default=120.0)
    args = parser.parse_args()
    try:
        _parse_common_options(args)
        if args.local_dram_validation:
            _parse_local_options(args)
        elif args.negative != "none":
            _fail(args, "argument_validation", "--negative requires --local-dram-validation")
        if args.rank == HOST_RANK:
            _run_host(args)
        else:
            _run_npu(args)
    except ValidationError:
        return 1
    except Exception as exc:
        _log_error(args, "main", f"unhandled validation failure: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
