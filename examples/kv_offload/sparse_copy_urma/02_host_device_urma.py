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

import argparse
import os
import sys

from urma_example_common import (
    CTRL_PORT,
    DEFAULT_SEED,
    HBM_GVA_MAX_BYTES,
    HOST_RANK,
    ITEM_BYTES,
    LOCAL_VALIDATION_ROLES,
    MF_LOG_LEVEL_CHOICES,
    NPU_RANK,
    POOL_BYTES,
    STORE_PORT,
    WORLD_SIZE,
    _accept_control_connection,
    _cleanup,
    _configure_local_environment,
    _connect_control,
    _create_control_server,
    _create_handle,
    _fail,
    _load_configured_runtime,
    _log_debug,
    _log_error,
    _log_info,
    _normalize_eid,
    _parse_common_options,
    _parse_local_options,
    _recv,
    _runtime_device_id,
    _send,
    _set_local_device,
    ValidationError,
)
from urma_local_validation import (
    _copy_batch,
    _lookup_npu_views,
    _run_local_host,
    _run_local_npu,
    _validate_host_source,
)


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
                        f"runtime_device={_runtime_device_id(args)} logical_device={args.device_id}")
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
    _log_debug(args, "process_start", "enter Host rank execution")
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
        _log_debug(args, "mf_initialize", "calling mf.initialize for Host rank")
        ret = mf.initialize()
        _log_debug(args, "mf_initialize", f"Host rank mf.initialize returned ret={ret}")
        if ret != 0:
            _fail(args, "mf_initialize", "host mf.initialize failed", ret)
        mf_initialized = True
        _log_debug(
            args,
            "bm_lifecycle",
            f"creating Host handle store=tcp://{args.head_ip}:{args.store_port} world_size={WORLD_SIZE} "
            f"local_dram_size={POOL_BYTES} local_hbm_size=0 "
            f"max_dram_size={POOL_BYTES} max_hbm_size={HBM_GVA_MAX_BYTES}",
        )
        handle = _create_handle(args, bm, HOST_RANK)
        bm_initialized = True
        joined = True
        _log_debug(args, "bm_lifecycle", "Host handle created and joined")
        if args.local_dram_validation:
            _run_local_host(args, handle, bm)
        else:
            _run_host_production(args, handle, bm)
    finally:
        _cleanup(args, mf, bm, handle, joined, bm_initialized, mf_initialized)


def _run_npu(args):
    _log_debug(args, "process_start", "enter NPU rank execution")
    if args.local_dram_validation:
        _configure_local_environment(args)
    _set_local_device(args)
    mf, bm = _load_configured_runtime(args)
    mf_initialized = False
    bm_initialized = False
    handle = None
    joined = False
    try:
        _log_debug(args, "mf_initialize", "calling mf.initialize for NPU rank")
        ret = mf.initialize()
        _log_debug(args, "mf_initialize", f"NPU rank mf.initialize returned ret={ret}")
        if ret != 0:
            _fail(args, "mf_initialize", "NPU mf.initialize failed", ret)
        mf_initialized = True
        _log_debug(
            args,
            "bm_lifecycle",
            f"creating NPU handle store=tcp://{args.head_ip}:{args.store_port} world_size={WORLD_SIZE} "
            f"local_dram_size=0 local_hbm_size={POOL_BYTES} "
            f"max_dram_size={POOL_BYTES} max_hbm_size={HBM_GVA_MAX_BYTES}",
        )
        handle = _create_handle(args, bm, NPU_RANK)
        bm_initialized = True
        joined = True
        _log_debug(args, "bm_lifecycle", "NPU handle created and joined")
        if args.local_dram_validation:
            _run_local_npu(args, handle, bm)
        else:
            _run_npu_production(args, handle, bm)
    finally:
        _cleanup(args, mf, bm, handle, joined, bm_initialized, mf_initialized)


def _build_parser():
    parser = argparse.ArgumentParser(description="Host-DDR to NPU HBM sparse_copy_urma example")
    parser.add_argument("--rank", type=int, required=True, choices=(HOST_RANK, NPU_RANK))
    parser.add_argument("--head-ip", required=True, help="Config-store and control-plane peer address")
    parser.add_argument("--eid", help="Legacy Host URMA EID, 32 hex characters; required for --rank 0")
    parser.add_argument("--device-id", type=int, default=None,
                        help="Legacy EID/DCMI logical id override; local mode defaults from EID metadata")
    parser.add_argument("--runtime-device-id", type=int,
                        help="Optional ACL/Torch visible index; local mode derives it from ASCEND_RT_VISIBLE_DEVICES")
    parser.add_argument("--store-port", type=int, default=STORE_PORT)
    parser.add_argument("--ctrl-port", type=int, default=CTRL_PORT)
    parser.add_argument("--bind-ip", default="0.0.0.0", help="Host-side control listener bind address")
    parser.add_argument("--local-dram-validation", action="store_true", help="Enable validation-only local DRAM mode")
    parser.add_argument(
        "--role",
        choices=LOCAL_VALIDATION_ROLES,
        default=None,
        help="Local validation role; required with --local-dram-validation",
    )
    parser.add_argument("--physical-device-id", type=int,
                        help="Optional physical device id override; local mode defaults from EID metadata")
    parser.add_argument("--host-eid", help="Local validation Host/DRAM EID; defaults to MF_HOST_URMA_EID")
    parser.add_argument("--device-eid", help="Local validation Device/HBM EID; defaults to USE_LOCAL_EID")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--sizes", default="1,4096,1048576", help="Single-copy byte sizes")
    parser.add_argument("--batch-counts", default="1,999,1000,1001", help="Batch item counts")
    parser.add_argument("--ctrl-timeout", type=float, default=120.0)
    parser.add_argument(
        "--env-file",
        default=None,
        help="Local validation env file; defaults to sparse_copy_urma/env in local mode",
    )
    parser.add_argument(
        "--log-level",
        type=int,
        choices=MF_LOG_LEVEL_CHOICES,
        default=None,
        help="Shared MemFabric/Python log level: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR, 4=OFF",
    )
    return parser


def main():
    args = _build_parser().parse_args()
    try:
        _parse_common_options(args)
        if args.local_dram_validation:
            if args.role is None:
                _fail(args, "argument_validation", "--role is required with --local-dram-validation")
            _parse_local_options(args)
        elif args.role is not None:
            _fail(args, "argument_validation", "--role requires --local-dram-validation")
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
