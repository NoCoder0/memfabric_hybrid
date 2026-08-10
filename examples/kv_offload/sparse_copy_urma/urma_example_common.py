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

import json
import os
import socket
import struct
import sys
import time


STORE_PORT = 8574
CTRL_PORT = 9877
WORLD_SIZE = 2
HOST_RANK, NPU_RANK = 0, 1
POOL_BYTES = 8 << 20
# Ascend 950 VMM requires the per-rank HBM GVA limit to be 1 GiB aligned.
HBM_GVA_MAX_BYTES = 1 << 30
ITEM_BYTES = 4 << 10
DEFAULT_SEED = 17
MAX_CONTROL_BYTES = 1 << 20
CONTROL_CONNECT_RETRY_SECONDS = 0.1
# MemFabric uses 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR.  Keep the example at
# the most verbose level while diagnosing the temporary local validation path.
MF_DEBUG_LOG_LEVEL = 0
MF_PRODUCTION_LOG_LEVEL = 1


class ValidationError(RuntimeError):
    """An expected validation or runtime failure already reported with context."""


def _role(args):
    return "host" if args.rank == HOST_RANK else "npu"


def _runtime_device_id(args):
    runtime_device_id = getattr(args, "runtime_device_id", None)
    return args.device_id if runtime_device_id is None else runtime_device_id


def _log_error(args, stage, detail, ret=None):
    ret_text = "" if ret is None else f" ret={ret}"
    physical = "unset" if args.physical_device_id is None else args.physical_device_id
    print(
        f"[ERROR] stage={stage} role={_role(args)} rank={args.rank} "
        f"logical_device={args.device_id} runtime_device={_runtime_device_id(args)} "
        f"physical_device={physical}{ret_text} detail={detail}",
        file=sys.stderr,
        flush=True,
    )


def _fail(args, stage, detail, ret=None):
    _log_error(args, stage, detail, ret)
    raise ValidationError(detail)


def _log_info(args, message):
    print(f"[{_role(args)}] {message}", flush=True)


def _log_debug(args, stage, detail):
    physical = getattr(args, "physical_device_id", None)
    physical_text = "unset" if physical is None else physical
    print(
        f"[DEBUG] stage={stage} role={_role(args)} pid={os.getpid()} rank={args.rank} "
        f"logical_device={args.device_id} runtime_device={_runtime_device_id(args)} "
        f"physical_device={physical_text} detail={detail}",
        flush=True,
    )


def _control_message_summary(message):
    if not isinstance(message, dict):
        return f"value={message!r}"
    fields = (
        "type", "version", "role", "rank", "pid", "round", "physical_device_id", "logical_device_id",
        "runtime_device_id", "host_gva", "imported_gva", "import_view", "hbm_gva", "hbm_va",
        "route_mode", "bytes", "result", "hcomm_ret", "negative", "expected_failure",
    )
    summary = [f"{name}={message[name]}" for name in fields if name in message]
    if isinstance(message.get("cases"), list):
        summary.append(f"cases={len(message['cases'])}")
    return " ".join(summary)


def _visible_device_summary(physical_device_id=None):
    visible = os.environ.get("ASCEND_RT_VISIBLE_DEVICES")
    if not visible:
        return "unset"
    entries = tuple(item.strip() for item in visible.split(",") if item.strip())
    mapping = ""
    if physical_device_id is not None:
        try:
            visible_index = entries.index(str(physical_device_id))
            mapping = f" physical_device_visible_index={visible_index}"
        except ValueError:
            mapping = " physical_device_visible_index=not_found"
    return f"{visible} logical_count={len(entries)}{mapping}"


def _load_runtime():
    import memfabric_hybrid as mf
    from memfabric_hybrid import bm

    return mf, bm


def _load_configured_runtime(args):
    local_validation = getattr(args, "local_dram_validation", False)
    log_level = MF_DEBUG_LOG_LEVEL if local_validation else MF_PRODUCTION_LOG_LEVEL
    try:
        if local_validation:
            os.environ["MF_LOG_LEVEL"] = str(log_level)
        mf, bm = _load_runtime()
        ret = mf.set_log_level(log_level)
    except Exception as exc:
        _fail(args, "runtime_import", f"MemFabric runtime import/configuration failed: {exc}")
    if ret != 0:
        _fail(args, "runtime_log_level", "mf.set_log_level failed", ret)
    _log_debug(
        args,
        "runtime_log_level",
        f"mf_log_level={log_level} local_validation={local_validation} "
        f"MF_LOG_LEVEL={os.environ.get('MF_LOG_LEVEL', 'unset')} "
        f"ASCEND_MF_LOG_LEVEL={os.environ.get('ASCEND_MF_LOG_LEVEL', 'unset')}",
    )
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
    _log_debug(args, stage, f"send payload_bytes={len(payload)} {_control_message_summary(message)}")
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
    _log_debug(args, stage, f"recv payload_bytes={payload_size} {_control_message_summary(message)}")
    return message


def _create_control_server(args):
    try:
        server = socket.create_server((args.bind_ip, args.ctrl_port))
        server.settimeout(args.ctrl_timeout)
        return server
    except OSError as exc:
        _fail(args, "control_listener", f"bind/setup failed, ip={args.bind_ip} port={args.ctrl_port} "
              f"timeout={args.ctrl_timeout}: {exc}")


def _accept_control_connection(args, server):
    try:
        return server.accept()[0]
    except OSError as exc:
        _fail(args, "control_accept", f"accept failed, ip={args.bind_ip} port={args.ctrl_port} "
              f"timeout={args.ctrl_timeout}: {exc}")


def _connect_control(args):
    deadline = time.monotonic() + args.ctrl_timeout
    attempts = 0
    last_error = None
    while time.monotonic() < deadline:
        attempts += 1
        try:
            remaining = max(deadline - time.monotonic(), CONTROL_CONNECT_RETRY_SECONDS)
            return socket.create_connection((args.head_ip, args.ctrl_port), timeout=remaining)
        except OSError as exc:
            last_error = exc
            time.sleep(min(CONTROL_CONNECT_RETRY_SECONDS, max(0.0, deadline - time.monotonic())))
    _fail(args, "control_connect", f"connect failed, ip={args.head_ip} port={args.ctrl_port} "
          f"timeout={args.ctrl_timeout} attempts={attempts}: {last_error}")


def _config(bm, rank_id, head_ip):
    cfg = bm.BmConfig()
    cfg.rank_id = rank_id
    cfg.start_store = rank_id == HOST_RANK
    cfg.auto_ranking = False
    cfg.set_nic(f"tcp://{head_ip}:10005")
    return cfg


def _create_handle(args, bm, rank_id):
    store_url = f"tcp://{args.head_ip}:{args.store_port}"
    runtime_device_id = _runtime_device_id(args)
    try:
        config = _config(bm, rank_id, args.head_ip)
        ret = bm.initialize(store_url, WORLD_SIZE, runtime_device_id, config)
    except Exception as exc:
        _fail(args, "bm_initialize", f"bm.initialize raised, store={store_url} rank={rank_id} "
              f"runtime_device={runtime_device_id}: {exc}")
    if ret != 0:
        _fail(args, "bm_initialize", f"bm.initialize failed, store={store_url} rank={rank_id} "
              f"runtime_device={runtime_device_id}", ret)

    is_host = rank_id == HOST_RANK
    handle = None
    try:
        try:
            handle = bm.create2(
                id=0,
                local_dram_size=POOL_BYTES if is_host else 0,
                # max_* defines a rank-independent GVA layout; only local_* is role-specific.
                max_dram_size=POOL_BYTES,
                local_hbm_size=0 if is_host else POOL_BYTES,
                max_hbm_size=HBM_GVA_MAX_BYTES,
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
            bm.uninitialize(0)
        except Exception as exc:
            _log_error(args, "bm_create_rollback", f"bm.uninitialize raised: {exc}")
        raise


def _cleanup(args, mf, bm, handle, joined, bm_initialized, mf_initialized):
    _log_debug(
        args,
        "cleanup",
        f"handle={handle is not None} joined={joined} bm_initialized={bm_initialized} "
        f"mf_initialized={mf_initialized}",
    )
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
            bm.uninitialize(0)
        except Exception as exc:
            _log_error(args, "cleanup_bm", f"bm.uninitialize raised: {exc}")
    if mf_initialized:
        try:
            mf.uninitialize()
        except Exception as exc:
            _log_error(args, "cleanup_mf", f"mf.uninitialize raised: {exc}")
    _log_debug(args, "cleanup", "completed")


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


def _parse_env_uint(args, env_name):
    text = os.environ.get(env_name)
    if text is None:
        _fail(args, "argument_validation", f"{env_name} is required when its CLI override is omitted")
    try:
        value = int(text.strip(), 10)
    except ValueError:
        _fail(args, "argument_validation", f"{env_name} must be a decimal integer: {text}")
    return _parse_uint_option(args, value, env_name)


def _resolve_local_device_id(args, cli_value, env_name, option_name):
    env_value = os.environ.get(env_name)
    if env_value is None:
        if cli_value is None:
            _fail(args, "device_mapping", f"{env_name} is required when {option_name} is omitted")
        return _parse_uint_option(args, cli_value, option_name)
    expected = _parse_env_uint(args, env_name)
    if cli_value is not None:
        value = _parse_uint_option(args, cli_value, option_name)
        if value != expected:
            _fail(args, "device_mapping", f"{option_name} differs from {env_name}")
        return value
    return expected


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
    if args.device_id is not None:
        args.device_id = _parse_uint_option(args, args.device_id, "--device-id")
    elif not args.local_dram_validation:
        args.device_id = 0
    if args.runtime_device_id is not None:
        args.runtime_device_id = _parse_uint_option(args, args.runtime_device_id, "--runtime-device-id")
    for value, name in ((args.store_port, "--store-port"), (args.ctrl_port, "--ctrl-port")):
        if value <= 0 or value > 65535:
            _fail(args, "argument_validation", f"{name} must be in 1..65535: {value}")
    if args.ctrl_timeout <= 0:
        _fail(args, "argument_validation", f"--ctrl-timeout must be positive: {args.ctrl_timeout}")


def _validate_runtime_device_mapping(args):
    visible = os.environ.get("ASCEND_RT_VISIBLE_DEVICES")
    if not visible:
        _fail(args, "device_mapping", "ASCEND_RT_VISIBLE_DEVICES is required in local validation mode")
    entries = tuple(item.strip() for item in visible.split(",") if item.strip())
    runtime_device_id = _runtime_device_id(args)
    if runtime_device_id >= len(entries):
        _fail(args, "device_mapping", f"runtime device index is outside visible devices: "
              f"runtime_device_id={runtime_device_id} visible_devices={visible}")
    if entries[runtime_device_id] != str(args.physical_device_id):
        _fail(args, "device_mapping", f"runtime device maps to a different physical device: "
              f"runtime_device_id={runtime_device_id} mapped_physical={entries[runtime_device_id]} "
              f"expected_physical={args.physical_device_id}")


def _resolve_runtime_device_id(args):
    visible = os.environ.get("ASCEND_RT_VISIBLE_DEVICES")
    if args.runtime_device_id is None:
        if not visible:
            _fail(args, "device_mapping", "ASCEND_RT_VISIBLE_DEVICES is required in local validation mode")
        entries = tuple(item.strip() for item in visible.split(",") if item.strip())
        try:
            args.runtime_device_id = entries.index(str(args.physical_device_id))
        except ValueError:
            _fail(args, "device_mapping", f"physical device is not visible: "
                  f"physical_device_id={args.physical_device_id} visible_devices={visible}")
    else:
        args.runtime_device_id = _parse_uint_option(args, args.runtime_device_id, "--runtime-device-id")
    _validate_runtime_device_mapping(args)


def _parse_local_options(args):
    args.physical_device_id = _resolve_local_device_id(
        args, args.physical_device_id, "MF_LOCAL_DRAM_PHYSICAL_DEVICE_ID", "--physical-device-id")
    args.device_id = _resolve_local_device_id(
        args, args.device_id, "MF_LOCAL_DRAM_LOGICAL_DEVICE_ID", "--device-id")
    _resolve_runtime_device_id(args)
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
    _log_debug(
        args,
        "local_options",
        f"physical_device_id={args.physical_device_id} logical_device_id={args.device_id} "
        f"runtime_device_id={args.runtime_device_id} "
        f"visible_devices={_visible_device_summary(args.physical_device_id)} "
        f"host_eid={args.host_eid} device_eid={args.device_eid} "
        f"topology={os.environ.get('MF_LOCAL_DRAM_TOPOLOGY', 'unset')} "
        f"udma={os.environ.get('MF_LOCAL_DRAM_UDMA', 'unset')}",
    )


def _configure_local_environment(args):
    role = os.environ.get("MF_LOCAL_DRAM_VALIDATION_ROLE")
    _log_debug(
        args,
        "local_environment",
        f"role_before={role or 'unset'} visible_devices={_visible_device_summary(args.physical_device_id)} "
        f"host_eid={args.host_eid} device_eid={args.device_eid}",
    )
    if args.rank == HOST_RANK:
        if role not in (None, "host"):
            _fail(args, "validation_role", f"host rank requires role=host, got={role}")
        swap_size = os.environ.get("MF_HYBM_RDMA_SWAP_SPACE_SIZE", "unset")
        os.environ["MF_LOCAL_DRAM_VALIDATION_ROLE"] = "host"
        os.environ["MF_HOST_URMA_EID"] = args.host_eid
        os.environ["MF_HYBM_RDMA_SWAP_SPACE_SIZE"] = "0"
        _log_debug(args, "local_environment", "configured Host role/EID and disabled unused Host RDMA swap, "
                                               f"swap_size_before={swap_size}")
    elif role is not None:
        _fail(args, "validation_role", "device rank must not set MF_LOCAL_DRAM_VALIDATION_ROLE")
    else:
        os.environ["USE_LOCAL_EID"] = args.device_eid
        _log_debug(args, "local_environment", "configured USE_LOCAL_EID for device rank")


def _set_local_device(args):
    runtime_device_id = _runtime_device_id(args)
    _log_debug(
        args,
        "acl_context",
        f"before aclrtSetDevice runtime_device_id={runtime_device_id} "
        f"discovered_logical_device={args.device_id} physical_device={args.physical_device_id} "
        f"visible_devices={_visible_device_summary(args.physical_device_id)}",
    )
    try:
        import torch
        import torch_npu  # noqa: F401

        try:
            device_count = torch.npu.device_count()
        except Exception as exc:
            device_count = "unknown"
            _log_debug(args, "acl_context", f"torch.npu.device_count failed: {exc}")
        _log_debug(args, "acl_context", f"torch_npu_device_count={device_count}")
        if isinstance(device_count, int) and runtime_device_id >= device_count:
            _fail(args, "acl_context", f"runtime device id is out of range: "
                  f"runtime_device_id={runtime_device_id} available_count={device_count}")
        torch.npu.set_device(runtime_device_id)
        try:
            current_device = torch.npu.current_device()
        except Exception as exc:
            current_device = "unknown"
            _log_debug(args, "acl_context", f"torch.npu.current_device failed after set: {exc}")
        _log_debug(args, "acl_context", f"aclrtSetDevice completed current_device={current_device}")
    except ValidationError:
        raise
    except Exception as exc:
        _fail(args, "acl_context", f"failed to set local NPU device: {exc}")
