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
import ctypes
import multiprocessing as mp
import os
import queue
import sys
import threading
from dataclasses import dataclass

from urma_example_common import (
    HBM_GVA_MAX_BYTES,
    HOST_RANK,
    NPU_RANK,
    ValidationError,
    _accept_control_connection,
    _cleanup,
    _configure_local_environment,
    _configure_python_logging,
    _connect_control,
    _create_control_server,
    _fail,
    _load_configured_runtime,
    _load_env_file,
    _log_error,
    _parse_local_options,
    _recv,
    _runtime_device_id,
    _send,
    _set_local_device,
)


WORLD_SIZE = 2
CARD_COUNT = 4
ONE_GIB = 1 << 30
CONTROL_HBM_BYTES = 8 << 20
BATCH_SIZE = 4
TOKENS_PER_REQUEST = 2048
TOKEN_COUNT = BATCH_SIZE * TOKENS_PER_REQUEST
K_DIM = 512
V_DIM = 64
BF16_BYTES = 2
K_BYTES = K_DIM * BF16_BYTES
V_BYTES = V_DIM * BF16_BYTES
TOKEN_BYTES = K_BYTES + V_BYTES
PAYLOAD_BYTES = TOKEN_COUNT * TOKEN_BYTES
LIST_NUM = TOKEN_COUNT * 2
TOTAL_CALLS = 20
WARMUP_CALLS = 6
PROFILE_STEP_CALLS = TOTAL_CALLS - WARMUP_CALLS
MF_ERROR_LOG_LEVEL = 3
DEFAULT_STORE_PORT = 8574
DEFAULT_CTRL_PORT = 9877
DEFAULT_NIC_PORT = 10005
DEFAULT_CTRL_TIMEOUT_SECONDS = 600.0
DEFAULT_PROFILING_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiling")


@dataclass(frozen=True)
class PairConfig:
    pair_id: int
    physical_device_id: int
    head_ip: str
    store_port: int
    ctrl_port: int
    nic_port: int
    env_file: str
    profiling_path: str
    batch_copy_lanes: int
    ctrl_timeout: float


@dataclass
class DeviceTensors:
    keys: list
    values: list
    src_ptrs: object
    dst_ptrs: object
    len_ptrs: object
    device: object
    torch: object
    torch_npu: object
    mf_acc_offload: object


def _parse_cards(text):
    try:
        cards = tuple(int(value.strip()) for value in text.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"cards must be comma-separated integers: {text}") from exc
    if not cards or len(cards) > CARD_COUNT or len(set(cards)) != len(cards) or min(cards) < 0:
        raise argparse.ArgumentTypeError(f"cards must contain 1..{CARD_COUNT} unique non-negative ids: {text}")
    return cards


def _build_parser():
    parser = argparse.ArgumentParser(description="Four-card Host DRAM to NPU HBM sparse_copy_urma profiling test")
    parser.add_argument("--cards", type=_parse_cards, default=_parse_cards("0,1,2,3"))
    parser.add_argument("--head-ip", default="127.0.0.1")
    parser.add_argument("--env-dir", required=True, help="Directory containing card<physical_id>.env files")
    parser.add_argument("--store-port-base", type=int, default=DEFAULT_STORE_PORT)
    parser.add_argument("--ctrl-port-base", type=int, default=DEFAULT_CTRL_PORT)
    parser.add_argument("--nic-port-base", type=int, default=DEFAULT_NIC_PORT)
    parser.add_argument(
        "--batch-copy-lanes",
        type=int,
        choices=(1, 2, 4),
        default=1,
        help="HCOMM BatchCopy lanes per Host/Device pair",
    )
    parser.add_argument("--ctrl-timeout", type=float, default=DEFAULT_CTRL_TIMEOUT_SECONDS)
    parser.add_argument(
        "--profiling-dir",
        default=DEFAULT_PROFILING_DIR,
        help="Root directory for per-card torch_npu profiler traces",
    )
    return parser


def _make_pair_configs(args):
    configs = []
    for pair_id, card_id in enumerate(args.cards):
        configs.append(PairConfig(
            pair_id=pair_id,
            physical_device_id=card_id,
            head_ip=args.head_ip,
            store_port=args.store_port_base + pair_id,
            ctrl_port=args.ctrl_port_base + pair_id,
            nic_port=args.nic_port_base + pair_id,
            env_file=os.path.join(os.path.abspath(args.env_dir), f"card{card_id}.env"),
            profiling_path=os.path.join(
                os.path.abspath(args.profiling_dir), f"lanes{args.batch_copy_lanes}", f"card{card_id}"
            ),
            batch_copy_lanes=args.batch_copy_lanes,
            ctrl_timeout=args.ctrl_timeout,
        ))
    return tuple(configs)


def _validate_pair_configs(args, configs):
    if args.ctrl_timeout <= 0:
        print(f"[ERROR] stage=argument_validation ctrl_timeout={args.ctrl_timeout} must be positive", file=sys.stderr)
        return False
    for config in configs:
        for name, port in (("store", config.store_port), ("control", config.ctrl_port), ("nic", config.nic_port)):
            if port <= 0 or port > 65535:
                print(
                    f"[ERROR] stage=argument_validation pair={config.pair_id} {name}_port={port} must be in 1..65535",
                    file=sys.stderr,
                )
                return False
    return True


def _worker_args(config, role):
    rank = HOST_RANK if role == "host" else NPU_RANK
    return argparse.Namespace(
        batch_counts="1",
        bind_ip="0.0.0.0",
        ctrl_port=config.ctrl_port,
        ctrl_timeout=config.ctrl_timeout,
        device_eid=None,
        device_id=None,
        env_file=config.env_file,
        head_ip=config.head_ip,
        host_eid=None,
        local_dram_validation=True,
        log_level=MF_ERROR_LOG_LEVEL,
        physical_device_id=config.physical_device_id,
        rank=rank,
        role=role,
        rounds=1,
        runtime_device_id=None,
        batch_copy_lanes=config.batch_copy_lanes,
        sizes="1",
        store_port=config.store_port,
    )


def _configure_worker(args):
    _configure_python_logging(args.log_level)
    _load_env_file(args, args.env_file)
    os.environ["ASCEND_MF_BATCH_COPY_LANES"] = str(args.batch_copy_lanes)
    _parse_local_options(args)
    _configure_local_environment(args)
    _set_local_device(args)


def _bm_config(bm, config, rank_id):
    bm_config = bm.BmConfig()
    bm_config.rank_id = rank_id
    bm_config.start_store = rank_id == HOST_RANK
    bm_config.auto_ranking = False
    bm_config.set_nic(f"tcp://{config.head_ip}:{config.nic_port}")
    return bm_config


def _rollback_handle(args, bm, handle, bm_initialized):
    if handle is not None:
        try:
            handle.destroy()
        except Exception as exc:
            _log_error(args, "bm_create_rollback", f"handle.destroy raised: {exc}")
    if bm_initialized:
        try:
            bm.uninitialize(0)
        except Exception as exc:
            _log_error(args, "bm_create_rollback", f"bm.uninitialize raised: {exc}")


def _create_performance_handle(args, config, bm):
    rank_id = args.rank
    store_url = f"tcp://{config.head_ip}:{config.store_port}"
    runtime_device_id = _runtime_device_id(args)
    handle = None
    bm_initialized = False
    try:
        ret = bm.initialize(store_url, WORLD_SIZE, runtime_device_id, _bm_config(bm, config, rank_id))
        if ret != 0:
            _fail(args, "bm_initialize", f"store={store_url} world_size={WORLD_SIZE} pair={config.pair_id}", ret)
        bm_initialized = True
        is_host = rank_id == HOST_RANK
        handle = bm.create2(
            id=0,
            local_dram_size=ONE_GIB if is_host else 0,
            max_dram_size=ONE_GIB,
            local_hbm_size=0 if is_host else CONTROL_HBM_BYTES,
            max_hbm_size=HBM_GVA_MAX_BYTES,
            data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
            enable_56bits_gva=False,
        )
        if handle is None:
            _fail(args, "bm_create", f"bm.create2 returned no handle, pair={config.pair_id} rank={rank_id}")
        ret = handle.join()
        if ret != 0:
            _fail(args, "bm_join", f"handle.join failed, pair={config.pair_id} rank={rank_id}", ret)
        return handle
    except ValidationError:
        _rollback_handle(args, bm, handle, bm_initialized)
        raise
    except Exception as exc:
        _log_error(args, "bm_create", f"pair={config.pair_id} rank={rank_id} raised: {exc}")
        _rollback_handle(args, bm, handle, bm_initialized)
        raise ValidationError(str(exc)) from exc


def _initialize_worker(args, config):
    _configure_worker(args)
    mf, bm = _load_configured_runtime(args)
    try:
        ret = mf.initialize()
    except Exception as exc:
        _fail(args, "mf_initialize", f"pair={config.pair_id} mf.initialize raised: {exc}")
    if ret != 0:
        _fail(args, "mf_initialize", f"pair={config.pair_id} mf.initialize failed", ret)
    try:
        handle = _create_performance_handle(args, config, bm)
    except Exception:
        try:
            mf.uninitialize()
        except Exception as exc:
            _log_error(args, "mf_initialize_rollback", f"pair={config.pair_id} mf.uninitialize raised: {exc}")
        raise
    return mf, bm, handle


def _prepare_host_source(args, handle, bm, pair_id):
    try:
        host_gva = handle.peer_rank_ptr(HOST_RANK, bm.BmMemType.HOST)
        host_view = handle.gva_to_va(host_gva, bm.BmMemType.LOCAL_HOST)
    except Exception as exc:
        _fail(args, "host_source", f"pair={pair_id} Host GVA lookup raised: {exc}")
    if host_gva == 0 or host_view != host_gva:
        _fail(args, "host_source", f"pair={pair_id} invalid Host GVA/view gva=0x{host_gva:x} view=0x{host_view:x}")
    source = (ctypes.c_uint8 * PAYLOAD_BYTES)()
    try:
        ret = handle.copy_data(ctypes.addressof(source), host_gva, PAYLOAD_BYTES, bm.BmCopyType.H2G, 0)
    except Exception as exc:
        _fail(args, "host_source", f"pair={pair_id} H2G bytes={PAYLOAD_BYTES} raised: {exc}")
    if ret != 0:
        _fail(args, "host_source", f"pair={pair_id} H2G failed bytes={PAYLOAD_BYTES}", ret)
    return host_gva, source


def _lookup_host_gva(args, handle, bm, pair_id):
    try:
        host_gva = handle.peer_rank_ptr(HOST_RANK, bm.BmMemType.HOST)
    except Exception as exc:
        _fail(args, "host_gva", f"pair={pair_id} peer_rank_ptr raised: {exc}")
    if host_gva == 0:
        _fail(args, "host_gva", f"pair={pair_id} peer_rank_ptr returned zero")
    return host_gva


def _prepare_device_tensors(args, host_gva, pair_id):
    try:
        import mf_acc_offload
        import torch
        import torch_npu

        torch_npu.npu.config.allow_internal_format = True
        device = torch.device(f"npu:{_runtime_device_id(args)}")
        keys = []
        values = []
        for _ in range(TOKEN_COUNT):
            keys.append(torch.ones(K_DIM, dtype=torch.bfloat16, device=device))
            values.append(torch.ones(V_DIM, dtype=torch.bfloat16, device=device))
        key_sources = [host_gva + index * TOKEN_BYTES for index in range(TOKEN_COUNT)]
        value_sources = [address + K_BYTES for address in key_sources]
        src_ptrs = torch.tensor(key_sources + value_sources, dtype=torch.int64, device=device)
        dst_ptrs = torch.tensor([tensor.data_ptr() for tensor in keys + values], dtype=torch.int64, device=device)
        lengths = [K_BYTES] * TOKEN_COUNT + [V_BYTES] * TOKEN_COUNT
        len_ptrs = torch.tensor(lengths, dtype=torch.int64, device=device)
        return DeviceTensors(keys, values, src_ptrs, dst_ptrs, len_ptrs, device, torch, torch_npu, mf_acc_offload)
    except Exception as exc:
        _fail(args, "tensor_prepare", f"pair={pair_id} tokens={TOKEN_COUNT} list_num={LIST_NUM} raised: {exc}")


def _submit_copy(args, tensors, pair_id, stage, iteration):
    try:
        ret = tensors.mf_acc_offload.sparse_copy_urma(
            tensors.src_ptrs, tensors.dst_ptrs, tensors.len_ptrs, LIST_NUM, tensors.device)
    except Exception as exc:
        _fail(args, stage, f"pair={pair_id} iteration={iteration} list_num={LIST_NUM} raised: {exc}")
    if ret != 0:
        _fail(args, stage, f"pair={pair_id} iteration={iteration} list_num={LIST_NUM}", ret)


def _warm_up(args, tensors, pair_id):
    for iteration in range(WARMUP_CALLS):
        _submit_copy(args, tensors, pair_id, "warmup_copy", iteration)


def _profile_active_calls(args, tensors, config, profiler):
    for iteration in range(PROFILE_STEP_CALLS):
        try:
            profiler.step()
        except Exception as exc:
            _fail(
                args,
                "profiler_step",
                f"pair={config.pair_id} iteration={iteration} path={config.profiling_path} raised: {exc}",
            )
        _submit_copy(args, tensors, config.pair_id, "profiled_copy", iteration)


def _create_profiler(args, tensors, config):
    torch_npu = tensors.torch_npu
    try:
        experimental_config = torch_npu.profiler._ExperimentalConfig(
            aic_metrics=torch_npu.profiler.AiCMetrics.PipeUtilization,
            profiler_level=torch_npu.profiler.ProfilerLevel.Level2,
            l2_cache=False,
            data_simplification=False,
        )
        return torch_npu.profiler.profile(
            activities=[
                torch_npu.profiler.ProfilerActivity.CPU,
                torch_npu.profiler.ProfilerActivity.NPU,
            ],
            on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(config.profiling_path),
            schedule=torch_npu.profiler.schedule(wait=1, warmup=1, active=10, repeat=1, skip_first=1),
            record_shapes=True,
            profile_memory=True,
            with_stack=False,
            with_flops=False,
            with_modules=False,
            experimental_config=experimental_config,
        )
    except Exception as exc:
        _fail(args, "profiler_create", f"pair={config.pair_id} path={config.profiling_path} raised: {exc}")


def _start_profiler(args, tensors, profiler, config):
    try:
        tensors.torch.npu.synchronize()
        profiler.start()
    except Exception as exc:
        _fail(args, "profiler_start", f"pair={config.pair_id} path={config.profiling_path} raised: {exc}")


def _stop_profiler(args, tensors, profiler, config):
    try:
        tensors.torch.npu.synchronize()
        profiler.stop()
    except Exception as exc:
        _fail(args, "profiler_stop", f"pair={config.pair_id} path={config.profiling_path} raised: {exc}")


def _verify_destination(args, tensors, pair_id):
    try:
        import torch

        key_nonzero = torch.count_nonzero(torch.cat(tensors.keys)).item()
        value_nonzero = torch.count_nonzero(torch.cat(tensors.values)).item()
    except Exception as exc:
        _fail(args, "data_verify", f"pair={pair_id} destination verification raised: {exc}")
    if key_nonzero != 0 or value_nonzero != 0:
        _fail(args, "data_verify", f"pair={pair_id} key_nonzero={key_nonzero} value_nonzero={value_nonzero}")


def _run_host(args, config, mf, bm, handle):
    host_gva, source = _prepare_host_source(args, handle, bm, config.pair_id)
    with _create_control_server(args) as server:
        with _accept_control_connection(args, server) as conn:
            conn.settimeout(args.ctrl_timeout)
            _send(conn, {"host_gva": host_gva, "bytes": PAYLOAD_BYTES, "list_num": LIST_NUM}, args, "source")
            if _recv(conn, args, "ready") != "READY":
                _fail(args, "ready", f"pair={config.pair_id} unexpected Device ready message")
            done = _recv(conn, args, "done")
            if not isinstance(done, dict) or done.get("result") != "PASS":
                _fail(args, "done", f"pair={config.pair_id} unexpected Device result={done}")
            _send(conn, "RELEASE", args, "release")
    del source


def _run_profiled_copies(args, tensors, config, barrier, conn):
    profiler = _create_profiler(args, tensors, config)
    profiler_started = False
    try:
        _start_profiler(args, tensors, profiler, config)
        profiler_started = True
        _warm_up(args, tensors, config.pair_id)
        _send(conn, "READY", args, "ready")
        try:
            barrier.wait(timeout=args.ctrl_timeout)
        except threading.BrokenBarrierError as exc:
            _fail(args, "measure_barrier", f"pair={config.pair_id} barrier failed: {exc}")
        _profile_active_calls(args, tensors, config, profiler)
    finally:
        if profiler_started:
            _stop_profiler(args, tensors, profiler, config)


def _run_device(args, config, mf, bm, handle, barrier, result_queue):
    host_gva = _lookup_host_gva(args, handle, bm, config.pair_id)
    tensors = _prepare_device_tensors(args, host_gva, config.pair_id)
    with _connect_control(args) as conn:
        conn.settimeout(args.ctrl_timeout)
        source = _recv(conn, args, "source")
        expected = {"host_gva": host_gva, "bytes": PAYLOAD_BYTES, "list_num": LIST_NUM}
        if not isinstance(source, dict) or any(source.get(key) != value for key, value in expected.items()):
            _fail(args, "source", f"pair={config.pair_id} source metadata mismatch={source}")
        _run_profiled_copies(args, tensors, config, barrier, conn)
        _verify_destination(args, tensors, config.pair_id)
        result = _build_result(args, config)
        _send(conn, {"result": "PASS"}, args, "done")
        if _recv(conn, args, "release") != "RELEASE":
            _fail(args, "release", f"pair={config.pair_id} unexpected Host release message")
    result_queue.put(result)


def _build_result(args, config):
    return {
        "pair_id": config.pair_id,
        "physical_device_id": config.physical_device_id,
        "logical_device_id": args.device_id,
        "runtime_device_id": _runtime_device_id(args),
        "pid": os.getpid(),
        "batch_copy_lanes": config.batch_copy_lanes,
        "profiling_path": config.profiling_path,
    }


def _worker_entry(config, role, barrier, result_queue):
    args = _worker_args(config, role)
    mf = bm = handle = None
    initialized = False
    try:
        mf, bm, handle = _initialize_worker(args, config)
        initialized = True
        if role == "host":
            _run_host(args, config, mf, bm, handle)
        else:
            _run_device(args, config, mf, bm, handle, barrier, result_queue)
    except ValidationError:
        raise
    except Exception as exc:
        _log_error(args, "worker", f"pair={config.pair_id} role={role} unhandled failure: {exc}")
        raise
    finally:
        if initialized:
            _cleanup(args, mf, bm, handle, True, True, True)


def _start_workers(configs, barrier, result_queue):
    workers = []
    for config in configs:
        process = mp.Process(
            target=_worker_entry, args=(config, "host", barrier, result_queue), name=f"host-pair{config.pair_id}")
        process.start()
        workers.append((config, "host", process))
    for config in configs:
        process = mp.Process(
            target=_worker_entry, args=(config, "device", barrier, result_queue), name=f"device-pair{config.pair_id}")
        process.start()
        workers.append((config, "device", process))
    return workers


def _join_workers(workers):
    failed = False
    for config, role, process in workers:
        process.join()
        if process.exitcode != 0:
            print(
                f"[ERROR] stage=worker_exit pair={config.pair_id} role={role} pid={process.pid} "
                f"physical_device={config.physical_device_id} exitcode={process.exitcode}",
                file=sys.stderr,
            )
            failed = True
    return not failed


def _collect_results(result_queue, expected_count):
    results = []
    for _ in range(expected_count):
        try:
            results.append(result_queue.get(timeout=5.0))
        except queue.Empty:
            print(f"[ERROR] stage=result_collect expected={expected_count} received={len(results)}", file=sys.stderr)
            break
    return sorted(results, key=lambda result: result["pair_id"])


def _print_results(results):
    for result in results:
        print(
            f"card={result['physical_device_id']} pair={result['pair_id']} "
            f"batch_copy_lanes={result['batch_copy_lanes']} profile_step_calls={PROFILE_STEP_CALLS} "
            f"profiling_path={result['profiling_path']}"
        )
    print(f"cards={len(results)} profiling_complete=true total_copy_calls={len(results) * TOTAL_CALLS}")


def main():
    args = _build_parser().parse_args()
    try:
        return _run(args)
    except Exception as exc:
        print(f"[ERROR] stage=main cards={args.cards} unhandled failure: {exc}", file=sys.stderr)
        return 1


def _prepare_profiling_dirs(configs):
    for config in configs:
        try:
            os.makedirs(config.profiling_path, exist_ok=True)
        except OSError as exc:
            print(
                f"[ERROR] stage=profiling_dir pair={config.pair_id} path={config.profiling_path} raised: {exc}",
                file=sys.stderr,
            )
            return False
    return True


def _run(args):
    _configure_python_logging(MF_ERROR_LOG_LEVEL)
    configs = _make_pair_configs(args)
    if not _validate_pair_configs(args, configs):
        return 1
    missing = [config.env_file for config in configs if not os.path.isfile(config.env_file)]
    if missing:
        print(f"[ERROR] stage=env_validation missing_files={','.join(missing)}", file=sys.stderr)
        return 1
    if not _prepare_profiling_dirs(configs):
        return 1
    print(
        f"cards={','.join(str(card) for card in args.cards)} payload_bytes={PAYLOAD_BYTES} list_num={LIST_NUM} "
        f"batch_copy_lanes={args.batch_copy_lanes} warmup_calls={WARMUP_CALLS} "
        f"profile_step_calls={PROFILE_STEP_CALLS} profiling_dir={args.profiling_dir}"
    )
    mp.set_start_method("spawn", force=True)
    barrier = mp.Barrier(len(configs))
    result_queue = mp.Queue()
    workers = _start_workers(configs, barrier, result_queue)
    if not _join_workers(workers):
        return 1
    results = _collect_results(result_queue, len(configs))
    if len(results) != len(configs):
        return 1
    _print_results(results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
