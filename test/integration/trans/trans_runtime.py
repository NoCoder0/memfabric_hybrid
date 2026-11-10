# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

"""Spawned, two-rank runtime for TransferEngine integration case classes."""

import importlib
import logging
import multiprocessing as mp
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from multiprocessing.connection import wait
from pathlib import Path

try:
    from .trans_config import validate_config
except ImportError:  # pytest also imports this directory as a top-level path.
    from trans_config import validate_config

LOG = logging.getLogger(__name__)
_CLEANUP_PATTERNS = (
    re.compile(r"free sq_cq failed", re.IGNORECASE),
    re.compile(r"stream id.*(?:fail|error)", re.IGNORECASE),
    re.compile(r"HalHostUnregisterEx.*(?:not successful|ret\s*[:=]?[1-9])", re.IGNORECASE),
)
_KNOWN_CLEANUP_WARNING = re.compile(r"free sq_cq failed:\s*-8\s*$", re.IGNORECASE)
_HCCN_TOOL = Path("/usr/local/Ascend/driver/tools/hccn_tool")


def _checked(method, result, allow_none=False):
    if result == 0 or (allow_none and result is None):
        return result
    raise RuntimeError(f"{method} failed, ret={result}")


def _log_call(step, method, call, allow_none=False):
    try:
        result = call()
        _checked(method, result, allow_none)
    except BaseException as exc:
        step(f"API_CALL method={method} result=error:{type(exc).__name__}")
        raise
    step(f"API_CALL method={method} result={result}")
    return result


class NativeCtx:
    """The only native state exposed to a TRANS integration case."""

    def __init__(self, rank, config, barrier, peer, step):
        self.rank, self.config, self._barrier, self._peer, self.step = rank, config, barrier, peer, step
        self.engine = None
        self.retained = []
        self.streams = []
        self.torch = self.npu = self.acl = None
        self._engine_type = self._data_op = None

    def start(self):
        import acl
        import torch
        import torch_npu

        from memfabric_hybrid import TransDataOpType, TransferEngine, set_log_level

        self.torch, self.npu, self.acl = torch, torch_npu.npu, acl
        self._engine_type = TransferEngine
        self._data_op = getattr(TransDataOpType, self.config["protocol"].upper())
        self.npu.set_device(self.rank)
        _log_call(self.step, "set_log_level", lambda: set_log_level(1), allow_none=True)
        self.start_engine(use_defaults=False)
        self._barrier.wait(timeout=self.config["setup_timeout"])

    def start_engine(self, use_defaults=False):
        if self.engine is not None:
            raise RuntimeError("TransferEngine is already initialized")
        if use_defaults and self.config["protocol"] != "sdma":
            raise ValueError("TransferEngine default parameters are only valid for sdma")
        self.engine = self._engine_type()
        if use_defaults:

            def call():
                return self.engine.initialize("tcp://127.0.0.1", "127.0.0.1:0", self._role(), self.rank)
        else:

            def call():
                return self.engine.initialize("tcp://127.0.0.1", "127.0.0.1:0", self._role(), self.rank, self._data_op)

        _log_call(self.step, "TransferEngine.initialize", call)

    def _role(self):
        return "Prefill" if self.rank == 0 else "Decode"

    def session(self, host="127.0.0.1"):
        if self.engine is None:
            raise RuntimeError("TransferEngine is not initialized")
        port = self.engine.get_rpc_port()
        if not isinstance(port, int) or port <= 0:
            raise RuntimeError(f"TransferEngine.get_rpc_port returned {port!r}")
        self.step(f"API_CALL method=TransferEngine.get_rpc_port result={port}")
        return f"{host}:{port}"

    def retain(self, obj):
        self.retained.append(obj)
        return obj

    def exchange(self, value):
        self._peer.send(value)
        return self._peer.recv()

    def sync(self):
        self._barrier.wait(timeout=self.config["case_timeout"])

    def close_engine(self):
        if self.streams:
            raise RuntimeError("ACL streams remain incomplete; skipping explicit engine and buffer cleanup")
        errors = []
        if self.engine is not None:
            for name, method in (
                ("TransferEngine.destroy", self.engine.destroy),
                ("TransferEngine.unInitialize", self.engine.unInitialize),
            ):
                try:
                    _log_call(self.step, name, method, allow_none=True)
                except Exception as exc:  # noqa: BLE001 - attempt both native cleanup calls
                    errors.append(f"{name}: {exc}")
        if not errors:
            self.engine = None
            self.retained.clear()
        if errors:
            raise RuntimeError("; ".join(errors))

    def close(self):
        self.close_engine()


@dataclass
class Worker:
    rank: int
    process: mp.Process
    connection: object
    sequence: int = 0
    state: str = "starting"
    step: str = "bootstrap"
    error: str | None = None
    cleanup_ok: bool | None = None
    cleanup_error: str | None = None
    eof: bool = False
    stop_sent: bool = False


def _configure_rank_logging(path):
    stream = open(path, "w", encoding="utf-8", buffering=1)  # noqa: SIM115
    # The descriptor must remain open for native stdout/stderr until worker cleanup completes.
    os.dup2(stream.fileno(), 1)
    os.dup2(stream.fileno(), 2)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=[logging.StreamHandler(sys.stderr)],
        force=True,
    )
    return stream


def _worker_main(rank, config, barrier, control, peer, log_path):
    sequence, current_step, ctx = 0, "bootstrap", None
    stream = _configure_rank_logging(log_path)

    def send(kind, **fields):
        try:
            control.send({"kind": kind, "seq": sequence, "step": current_step, **fields})
        except (BrokenPipeError, EOFError, OSError):
            return False
        return True

    def step(name):
        nonlocal current_step
        current_step = name
        LOG.info("rank=%d seq=%d step=%s", rank, sequence, name)
        send("step")

    try:
        ctx = NativeCtx(rank, config, barrier, peer, step)
        ctx.start()
        send("ready")
        while True:
            command = control.recv()
            if command.get("kind") == "STOP":
                break
            if command.get("kind") != "RUN" or command.get("seq", 0) <= sequence:
                raise RuntimeError(f"invalid worker command: {command!r}")
            sequence = command["seq"]
            step(f"run {command['module']}.{command['class']}")
            _run_case(ctx, command, send)
    except BaseException as exc:
        LOG.exception("rank=%d failed at %s", rank, current_step)
        send("error", error=f"{type(exc).__name__}: {exc}"[:2000])
    finally:
        try:
            if ctx is None:
                raise RuntimeError("NativeCtx was not created")
            ctx.close()
        except BaseException as exc:
            LOG.exception("rank=%d cleanup failed", rank)
            send("cleanup_done", ok=False, error=f"{type(exc).__name__}: {exc}"[:2000])
        else:
            send("cleanup_done", ok=True)
        control.close()
        peer.close()
        stream.close()


def _run_case(ctx, command, send):
    case = None
    error = None
    try:
        case_type = getattr(importlib.import_module(command["module"]), command["class"])
        case = case_type()
        case.setup(ctx)
        case.run(ctx)
    except BaseException as exc:
        LOG.exception("rank=%d case failed", ctx.rank)
        error = f"{type(exc).__name__}: {exc}"
    finally:
        if case is not None:
            try:
                case.teardown(ctx)
            except BaseException as exc:
                LOG.exception("rank=%d case teardown failed", ctx.rank)
                error = f"{error}; teardown: {type(exc).__name__}: {exc}" if error else f"teardown: {exc}"
    send("error", error=error[:2000]) if error else send("done")


class TransPool:
    def __init__(self, config, log_dir):
        validate_config(config)
        self.config, self.log_dir = dict(config), Path(log_dir)
        self.workers, self.sequence, self.state, self._closed = [], 0, "empty", False
        self._barrier = None

    def run(self, case_type):
        protocols = getattr(case_type, "PROTOCOLS", ("device_rdma", "sdma"))
        if self.config["protocol"] not in protocols:
            import pytest

            pytest.skip(f"{case_type.__name__} does not support protocol {self.config['protocol']}")
        try:
            if self.state == "empty":
                self._start()
            if self.state != "ready":
                raise RuntimeError(f"TRANS pool is not usable, state={self.state}")
            self.sequence += 1
            deadline = time.monotonic() + self.config["case_timeout"]
            for worker in self.workers:
                worker.error, worker.state = None, "running"
                worker.connection.send(
                    {"kind": "RUN", "seq": self.sequence, "module": case_type.__module__, "class": case_type.__name__}
                )
            self._wait_for("done", self.sequence, deadline)
        except BaseException as exc:
            self.state = "broken"
            try:
                self.close()
            except Exception as cleanup:  # noqa: BLE001 - preserve the original failure alongside cleanup
                raise RuntimeError(f"{exc}; pool cleanup: {cleanup}") from exc
            raise

    def _start(self):
        self._resolve_devices()
        self.log_dir.mkdir(parents=True, exist_ok=False)
        context = mp.get_context("spawn")
        barrier = context.Barrier(2)
        self._barrier = barrier
        peer0, peer1 = context.Pipe(duplex=True)
        try:
            for rank, peer in ((0, peer0), (1, peer1)):
                parent, child = context.Pipe(duplex=True)
                process = context.Process(
                    target=_worker_main,
                    args=(rank, self.config, barrier, child, peer, str(self.log_dir / f"rank{rank}.log")),
                    name=f"TRANS-rank-{rank}",
                )
                try:
                    self._start_with_visible_devices(process)
                except BaseException:
                    parent.close()
                    child.close()
                    process.close()
                    raise
                child.close()
                self.workers.append(Worker(rank, process, parent))
            self._wait_for("ready", 0, time.monotonic() + self.config["setup_timeout"])
        except BaseException:
            self.state = "broken"
            raise
        finally:
            peer0.close()
            peer1.close()
        self.state = "ready"
        LOG.info(
            "TRANS pool ready, pids=%s protocol=%s", [w.process.pid for w in self.workers], self.config["protocol"]
        )

    def _resolve_devices(self):
        if sys.platform != "linux":
            raise RuntimeError("TRANS device integration requires Linux Ascend")
        devices = self.config["devices"] or select_idle_devices()
        if devices is None:
            raise RuntimeError("No two idle Ascend devices found; pass --trans-devices CARD0 CARD1")
        validate_devices(devices)
        self.config["devices"] = list(devices)

    def _start_with_visible_devices(self, process):
        name, before = "ASCEND_RT_VISIBLE_DEVICES", os.environ.get("ASCEND_RT_VISIBLE_DEVICES")
        os.environ[name] = ",".join(map(str, self.config["devices"]))
        try:
            process.start()
        finally:
            if before is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = before

    def _record(self, worker, event):
        if not isinstance(event, dict) or not isinstance(event.get("step"), str):
            worker.error, worker.state = f"rank={worker.rank} invalid event {event!r}", "error"
            return
        worker.sequence, worker.step = event.get("seq", -1), event["step"]
        kind = event.get("kind")
        if kind == "step":
            LOG.info("rank=%d seq=%d %s", worker.rank, worker.sequence, worker.step)
        elif kind in ("ready", "done"):
            worker.state = kind
        elif kind == "error":
            worker.error, worker.state = event.get("error", "worker error"), "error"
        elif kind == "cleanup_done":
            worker.cleanup_ok, worker.cleanup_error = event.get("ok") is True, event.get("error")
        else:
            worker.error, worker.state = f"rank={worker.rank} unexpected event {kind!r}", "error"

    def _drain(self, deadline):
        pending = [w.connection for w in self.workers if not w.eof]
        pending += [w.process.sentinel for w in self.workers if w.process.exitcode is None]
        for ready in wait(pending, timeout=max(0, deadline - time.monotonic())) if pending else ():
            for worker in self.workers:
                if ready == worker.connection:
                    while True:
                        try:
                            if not worker.connection.poll():
                                break
                            self._record(worker, worker.connection.recv())
                        except (EOFError, OSError):
                            worker.eof = True
                            break
                elif ready == worker.process.sentinel:
                    worker.process.join(timeout=0)

    def _failure(self):
        for worker in self.workers:
            if worker.error:
                raise RuntimeError(f"rank={worker.rank}: {worker.error}")
            if worker.eof:
                raise RuntimeError(f"rank={worker.rank} control connection closed")
            if worker.process.exitcode is not None:
                raise RuntimeError(f"rank={worker.rank} exitcode={worker.process.exitcode}")

    def _wait_for(self, state, sequence, deadline):
        while time.monotonic() < deadline:
            self._failure()
            if all(w.state == state and w.sequence == sequence for w in self.workers):
                return
            self._drain(deadline)
        self._failure()
        details = [f"rank={w.rank} state={w.state} step={w.step}" for w in self.workers]
        raise TimeoutError(f"TRANS waiting for {state} timed out: {details}")

    def close(self):
        if self._closed:
            return
        self._closed = True
        errors, deadline = [], time.monotonic() + self.config["cleanup_timeout"]
        if self._barrier is not None:
            try:
                self._barrier.abort()
            except Exception as exc:  # noqa: BLE001 - continue worker cleanup after a barrier failure
                errors.append(f"barrier abort: {exc}")
        for worker in self.workers:
            if worker.process.is_alive():
                try:
                    worker.connection.send({"kind": "STOP"})
                    worker.stop_sent = True
                except (BrokenPipeError, EOFError, OSError) as exc:
                    errors.append(f"rank={worker.rank} STOP: {exc}")
        self._reap(deadline, errors)
        self._scan_cleanup_logs(errors)
        self.state = "closed"
        if errors:
            raise RuntimeError("; ".join(errors))

    def _reap(self, deadline, errors):
        for stage, action in ((0.65, "terminate"), (0.85, "kill")):
            stage_deadline = deadline - (1 - stage) * self.config["cleanup_timeout"]
            while any(w.process.is_alive() for w in self.workers) and time.monotonic() < stage_deadline:
                self._drain(stage_deadline)
            if action:
                for worker in self.workers:
                    if worker.process.is_alive():
                        errors.append(f"rank={worker.rank} required {action}")
                        getattr(worker.process, action)()
        for worker in self.workers:
            worker.process.join(timeout=max(0, deadline - time.monotonic()))
            self._drain(deadline)
            if worker.process.is_alive() or worker.process.exitcode is None:
                errors.append(f"rank={worker.rank} did not exit")
            elif worker.process.exitcode != 0:
                errors.append(f"rank={worker.rank} exitcode={worker.process.exitcode}")
            if worker.cleanup_ok is not True:
                errors.append(f"rank={worker.rank} cleanup: {worker.cleanup_error or 'cleanup_done not received'}")
        for worker in self.workers:
            try:
                worker.connection.close()
            except OSError as exc:
                errors.append(f"rank={worker.rank} control close: {exc}")
            if worker.process.exitcode is not None:
                try:
                    worker.process.close()
                except ValueError as exc:
                    errors.append(f"rank={worker.rank} process close: {exc}")

    def _scan_cleanup_logs(self, errors):
        for path in self.log_dir.glob("rank*.log"):
            for line in path.read_text(errors="replace").splitlines():
                if _KNOWN_CLEANUP_WARNING.search(line):
                    LOG.warning("Known HAL unload diagnostic in %s: %s", path.name, line)
                    continue
                errors.extend(
                    f"{path.name}: {pattern.pattern}" for pattern in _CLEANUP_PATTERNS if pattern.search(line)
                )


def _command_ok(command):
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as exc:
        LOG.error("Device probe failed, command=%s error=%s", command, exc)
        return None
    if result.returncode:
        LOG.error(
            "Device probe failed, command=%s returncode=%d stderr=%s",
            command,
            result.returncode,
            result.stderr.strip()[:400],
        )
    return result


def _idle(card):
    proc = _command_ok(["npu-smi", "info", "-t", "proc-mem", "-i", str(card)])
    health = _command_ok(["npu-smi", "info", "-t", "health", "-i", str(card)])
    if not proc or not health or proc.returncode or health.returncode:
        return False
    if "No process in device." not in proc.stdout or "OK" not in health.stdout:
        LOG.warning(
            "Device unavailable, card=%d idle=%s healthy=%s",
            card,
            "No process in device." in proc.stdout,
            "OK" in health.stdout,
        )
        return False
    return _hccp_idle(card)


def _hccp_idle(card):
    if not _HCCN_TOOL.is_file():
        LOG.warning("hccn_tool is unavailable in this environment; check HCCP occupancy on the host")
        return True
    result = _command_ok([str(_HCCN_TOOL), "-i", str(card), "-process", "-g"])
    if result is None or result.returncode != 0:
        return False
    idle = re.search(r"device\s+hccp\s+process\s*:\s*not\s+exist", result.stdout, re.IGNORECASE) is not None
    if not idle:
        LOG.warning("HCCP not idle, card=%d response=%s", card, result.stdout.strip()[:400])
    return idle


def select_idle_devices():
    count, _ = _ascend_info()
    devices = [card for card in range(count) if _idle(card)]
    return devices[:2] if len(devices) >= 2 else None


def validate_devices(devices):
    count, soc = _ascend_info()
    if "Ascend910B" not in soc:
        raise RuntimeError(f"A2 (Ascend910B) is required, detected {soc!r}")
    if any(card >= count or not _idle(card) for card in devices):
        raise RuntimeError(f"TRANS devices unavailable, unhealthy, or busy: {devices}")


def _ascend_info():
    import acl

    initialized = False
    try:
        _checked("acl.init", acl.init())
        initialized = True
        count, ret = acl.rt.get_device_count()
        _checked("acl.rt.get_device_count", ret)
        return count, acl.get_soc_name()
    finally:
        if initialized:
            _checked("acl.finalize", acl.finalize(), allow_none=True)
