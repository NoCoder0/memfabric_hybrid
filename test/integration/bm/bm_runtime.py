#!/usr/bin/env python
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import importlib
import logging
import multiprocessing as mp
import os
import time
from dataclasses import dataclass
from multiprocessing.connection import Connection, wait

from config import PROTOCOL_DATA_OP_TYPE, validate_config

LOG = logging.getLogger(__name__)


def _checked(name, result, allow_none=False):
    if result != 0 and not (allow_none and result is None):
        raise RuntimeError(f"{name} failed, ret={result}")


class NativeRank:
    def __init__(self, rank, config, barrier, step):
        self.rank, self.config, self.barrier, self.step = rank, config, barrier, step
        self.handle = None
        self.mf_ready = self.bm_ready = self.joined = False

    def start(self):
        import torch
        import torch_npu

        import memfabric_hybrid as mf
        from memfabric_hybrid import bm

        protocol = self.config["protocol"]
        enum_name = PROTOCOL_DATA_OP_TYPE[protocol]
        data_op_type = getattr(bm.BmDataOpType, enum_name)

        self.mf = mf
        self.bm = bm
        self.torch = torch
        self.npu = torch_npu.npu

        self.step("mf.set_log_level info")
        mf.set_log_level(1)
        _checked("mf.initialize", mf.initialize())
        self.mf_ready = True
        cfg = bm.BmConfig()
        cfg.auto_ranking = False
        cfg.rank_id = self.rank
        cfg.start_store = self.rank == 0
        cfg.set_nic(self.config["nic"])
        user_device_id = self.config["devices"][self.rank]
        self.step(
            f"bm.initialize for device: {user_device_id}, "
            f"ASCEND_RT_VISIBLE_DEVICES: {os.environ.get('ASCEND_RT_VISIBLE_DEVICES')}"
        )
        _checked(
            "bm.initialize",
            bm.initialize(self.config["store_url"], self.config["world_size"], user_device_id, cfg),
        )
        self.bm_ready = True
        self.step(f"bm.create2 protocol={protocol} enum={enum_name}")
        sizes = {
            key: self.config[key]
            for key in ("local_dram_size", "max_dram_size", "local_hbm_size", "max_hbm_size", "enable_56bits_gva")
        }
        self.handle = bm.create2(id=0, data_op_type=data_op_type, flags=0, **sizes)
        if self.handle is None:
            raise RuntimeError("bm.create2 returned None")
        self.step("handle.join")
        _checked("handle.join", self.handle.join())
        self.joined = True
        self.step(f"set NPU device id: {user_device_id}")
        self.npu.set_device(user_device_id)
        self.barrier.wait(timeout=self.config["setup_timeout"])

    def sync(self):
        self.barrier.wait(timeout=self.config["case_timeout"])

    def close(self):
        errors = []
        actions = []
        if self.handle is not None:
            if self.joined:
                actions.append(("handle.leave", self.handle.leave))
            actions.append(("handle.destroy", self.handle.destroy))
        if self.bm_ready:
            actions.append(("bm.uninitialize", self.bm.uninitialize))
        if self.mf_ready:
            actions.append(("mf.uninitialize", self.mf.uninitialize))
        for name, action in actions:
            try:
                self.step(name)
                _checked(name, action(), allow_none=name != "handle.leave")
            except Exception as exc:
                LOG.exception("rank=%d %s failed", self.rank, name)
                errors.append(f"{name}: {exc}")
        if errors:
            raise RuntimeError("; ".join(errors))


@dataclass
class Worker:
    rank: int
    process: mp.Process
    connection: Connection
    sequence: int = 0
    state: str = "starting"
    step: str = "bootstrap"
    error: str | None = None
    cleanup_ok: bool | None = None
    cleanup_error: str | None = None
    stop_sent: bool = False
    eof: bool = False


def _worker(rank, config, barrier, connection, bootstrap: type[NativeRank]):
    sequence, current_step = 0, "bootstrap"
    context = None

    def send(kind, **fields):
        try:
            connection.send({"kind": kind, "seq": sequence, "step": current_step, **fields})
            return True
        except (BrokenPipeError, EOFError, OSError):
            return False

    def step(name):
        nonlocal current_step
        current_step = name
        send("step")

    try:
        context = bootstrap(rank, config, barrier, step)
        context.start()
        send("ready")
        while True:
            command = connection.recv()
            if command.get("kind") == "STOP":
                break
            if command.get("kind") != "RUN":
                raise RuntimeError(f"unexpected worker command {command.get('kind')!r}")
            if command["seq"] <= sequence:
                raise RuntimeError(f"out-of-order sequence {command['seq']}")
            sequence = command["seq"]
            step(f"run {command['module']}.{command['class']}")
            error = None
            case = None
            case_error = None
            teardown_error = None
            try:
                case_type = getattr(importlib.import_module(command["module"]), command["class"])
                case = case_type()
            except Exception as exc:
                LOG.exception("rank=%d step=%s case construction failed", rank, current_step)
                error = f"{current_step}: {type(exc).__name__}: {exc}"
            if case is not None:
                try:
                    case.setup(context)
                    case.run(context)
                except BaseException as exc:
                    LOG.exception("rank=%d step=%s case failed", rank, current_step)
                    error = f"{current_step}: {type(exc).__name__}: {exc}"
                    case_error = exc
                try:
                    case.teardown(context)
                except BaseException as exc:
                    LOG.exception("rank=%d case teardown failed", rank)
                    detail = f"case teardown: {type(exc).__name__}: {exc}"
                    error = f"{error}; {detail}" if error else detail
                    teardown_error = exc
            fatal = next(
                (exc for exc in (case_error, teardown_error) if exc is not None and not isinstance(exc, Exception)),
                None,
            )
            if fatal is not None:
                if case_error is not None and teardown_error is not None:
                    raise RuntimeError(error) from fatal
                raise fatal
            if error:
                send("error", error=error[:2000])
                continue
            send("done")
    except BaseException as exc:
        error = f"rank={rank} step={current_step}: {type(exc).__name__}: {exc}"
        LOG.exception("%s", error)
        send("error", error=error[:2000])
    finally:
        try:
            if context is None:
                raise RuntimeError("worker context was not created")
            context.close()
        except BaseException as exc:
            error = f"rank={rank} cleanup: {type(exc).__name__}: {exc}"
            LOG.exception("%s", error)
            send("cleanup_done", ok=False, error=error[:2000])
        else:
            send("cleanup_done", ok=True)
        connection.close()


class Pool:
    def __init__(self, config, bootstrap=NativeRank):
        validate_config(config)
        self.config, self.bootstrap = dict(config), bootstrap
        self.workers = []
        self.sequence = 0
        self.state = "empty"
        self.start_attempted = False
        self.close_attempted = False

    def _start(self):
        if self.start_attempted:
            raise RuntimeError("BM pool startup was already attempted")
        self.start_attempted = True
        if self.workers:
            raise RuntimeError("Old workers have not been reaped")
        deadline = time.monotonic() + self.config["setup_timeout"]
        context = mp.get_context("spawn")
        barrier = context.Barrier(2)
        try:
            for rank in (0, 1):
                parent, child = context.Pipe(duplex=True)
                worker = context.Process(
                    target=_worker,
                    args=(rank, self.config, barrier, child, self.bootstrap),
                    name=f"BM-rank-{rank}",
                )
                try:
                    worker.start()
                except BaseException:
                    parent.close()
                    if worker.pid is None:
                        worker.close()
                    else:
                        self.workers.append(Worker(rank, worker, parent, eof=True))
                    raise
                finally:
                    child.close()
                self.workers.append(Worker(rank, worker, parent))
            self.sequence = 0
            self._wait_for("ready", self.sequence, deadline)
        except BaseException:
            self.state = "broken"
            raise
        self.state = "ready"
        LOG.info(
            "BM pool ready, pids=%s protocol=%s world_size=%d",
            [worker.process.pid for worker in self.workers],
            self.config["protocol"],
            self.config["world_size"],
        )

    def _record_event(self, worker, event):
        if not isinstance(event, dict):
            worker.error = f"rank={worker.rank} invalid worker event {event!r}"
            worker.state = "error"
            return
        kind = event.get("kind")
        sequence = event.get("seq")
        step = event.get("step")
        if not isinstance(sequence, int) or not isinstance(step, str):
            worker.error = f"rank={worker.rank} invalid worker event {event!r}"
            worker.state = "error"
            return
        worker.sequence = sequence
        worker.step = step
        if kind == "step":
            if worker.error is None:
                worker.state = "busy"
            LOG.info("rank=%d seq=%d step=%s", worker.rank, sequence, step)
        elif kind == "ready":
            worker.state = "ready"
            LOG.info("rank=%d seq=%d step=ready", worker.rank, sequence)
        elif kind == "done":
            if worker.error is None:
                worker.state = "done"
            LOG.info("rank=%d seq=%d step=done", worker.rank, sequence)
        elif kind == "error":
            worker.error = event.get("error", "worker reported an unspecified error")
            worker.state = "error"
            LOG.error("rank=%d seq=%d step=%s error=%s", worker.rank, sequence, step, worker.error)
        elif kind == "cleanup_done":
            worker.cleanup_ok = event.get("ok") is True
            worker.cleanup_error = event.get("error")
            LOG.info("rank=%d seq=%d step=cleanup_done", worker.rank, sequence)
        else:
            worker.error = f"rank={worker.rank} unexpected worker event {kind!r}"
            worker.state = "error"

    def _drain_worker(self, worker, deadline):
        if worker.eof:
            return
        while True:
            try:
                if not worker.connection.poll():
                    return
                self._record_event(worker, worker.connection.recv())
            except (EOFError, OSError):
                worker.eof = True
                return
            if worker.process.exitcode is None and time.monotonic() >= deadline:
                return

    def _drain_events(self, deadline):
        pending = [worker.connection for worker in self.workers if not worker.eof]
        pending.extend(worker.process.sentinel for worker in self.workers if worker.process.exitcode is None)
        if not pending:
            return
        ready = wait(pending, timeout=max(0, deadline - time.monotonic()))
        sentinels = {worker.process.sentinel: worker for worker in self.workers}
        for worker in self.workers:
            if worker.connection in ready:
                self._drain_worker(worker, deadline)
        for sentinel in ready:
            worker = sentinels.get(sentinel)
            if worker is not None:
                self._drain_worker(worker, deadline)
                worker.process.join(timeout=0)

    def _raise_worker_failure(self, deadline):
        for worker in self.workers:
            if worker.process.exitcode is not None:
                self._drain_worker(worker, deadline)
            if worker.error is not None:
                raise RuntimeError(worker.error)
            if worker.eof:
                raise RuntimeError(f"rank={worker.rank} control connection closed")
            if worker.process.exitcode is not None:
                raise RuntimeError(
                    f"rank={worker.rank} exited, exitcode={worker.process.exitcode}, state={worker.state}"
                )

    def _wait_for(self, state, sequence, deadline):
        while time.monotonic() < deadline:
            self._raise_worker_failure(deadline)
            if all(worker.state == state and worker.sequence == sequence for worker in self.workers):
                return
            self._drain_events(deadline)
        self._raise_worker_failure(deadline)
        statuses = [
            f"rank={worker.rank} seq={worker.sequence} state={worker.state} step={worker.step}"
            for worker in self.workers
        ]
        raise TimeoutError(f"BM {state} deadline exceeded, statuses={statuses}")

    def _send_run(self, worker, case_type):
        if worker.state == "running":
            raise RuntimeError(f"rank={worker.rank} already has a running case")
        try:
            worker.connection.send(
                {"kind": "RUN", "seq": self.sequence, "module": case_type.__module__, "class": case_type.__name__}
            )
        except (BrokenPipeError, EOFError, OSError) as exc:
            worker.error = f"rank={worker.rank} RUN send failed: {exc}"
            worker.state = "error"
            raise RuntimeError(worker.error) from exc
        worker.sequence = self.sequence
        worker.state = "running"

    def run(self, case_type):
        try:
            if self.state == "empty":
                self._start()
            if self.state != "ready":
                raise RuntimeError(f"BM pool is not usable, state={self.state}")
            deadline = time.monotonic() + self.config["case_timeout"]
            for worker in self.workers:
                self._drain_worker(worker, deadline)
                worker.error = None
                worker.state = "ready"
            self.sequence += 1
            for worker in self.workers:
                self._send_run(worker, case_type)
            self._wait_for("done", self.sequence, deadline)
        except BaseException as exc:
            self.state = "broken"
            try:
                self.close()
            except Exception as cleanup:
                LOG.exception("Pool cleanup failed after case failure")
                raise RuntimeError(f"{exc}; pool cleanup: {cleanup}") from exc
            raise

    def _send_stop(self, errors):
        for worker in self.workers:
            if worker.stop_sent or worker.process.exitcode is not None:
                continue
            worker.stop_sent = True
            try:
                worker.connection.send({"kind": "STOP"})
            except (BrokenPipeError, EOFError, OSError) as exc:
                errors.append(f"rank={worker.rank} STOP send: {exc}")

    def _wait_for_exit(self, deadline):
        while any(worker.process.is_alive() for worker in self.workers) and time.monotonic() < deadline:
            self._drain_events(deadline)
        for worker in self.workers:
            worker.process.join(timeout=0)
            self._drain_worker(worker, deadline)

    def _stop_workers(self, deadline, errors):
        self._send_stop(errors)
        for fraction, action in ((0.5, None), (0.8, "terminate"), (1.0, "kill")):
            if action is not None:
                for worker in self.workers:
                    if worker.process.is_alive():
                        errors.append(f"rank={worker.rank} required {action}")
                        try:
                            getattr(worker.process, action)()
                        except Exception as exc:
                            LOG.exception("rank=%d %s failed", worker.rank, action)
                            errors.append(f"rank={worker.rank} {action}: {exc}")
            stage_deadline = deadline - (1 - fraction) * self.config["cleanup_timeout"]
            try:
                self._wait_for_exit(stage_deadline)
            except Exception as exc:
                LOG.exception("BM worker exit wait failed")
                errors.append(f"worker exit wait: {exc}")

    def close(self):
        if self.close_attempted:
            return
        self.close_attempted = True
        if not self.workers:
            self.state = "closed"
            return
        deadline = time.monotonic() + self.config["cleanup_timeout"]
        errors = []
        pids = [worker.process.pid for worker in self.workers]
        try:
            self._stop_workers(deadline, errors)
        except Exception as exc:
            LOG.exception("BM worker shutdown failed; forcing worker termination")
            errors.append(f"worker shutdown: {exc}")
            for worker in self.workers:
                try:
                    if worker.process.is_alive():
                        worker.process.kill()
                except Exception as kill_error:
                    LOG.exception("rank=%d emergency kill failed", worker.rank)
                    errors.append(f"rank={worker.rank} emergency kill: {kill_error}")
            for worker in self.workers:
                try:
                    worker.process.join(timeout=max(0, deadline - time.monotonic()))
                except Exception as join_error:
                    LOG.exception("rank=%d emergency join failed", worker.rank)
                    errors.append(f"rank={worker.rank} emergency join: {join_error}")
        retained = []
        for worker in self.workers:
            try:
                worker.process.join(timeout=max(0, deadline - time.monotonic()))
            except Exception as exc:
                LOG.exception("rank=%d final worker join failed", worker.rank)
                errors.append(f"rank={worker.rank} final worker join: {exc}")
            if worker.process.is_alive():
                errors.append(f"rank={worker.rank} pid={worker.process.pid} still alive after KILL")
                try:
                    worker.connection.close()
                except Exception as exc:
                    LOG.exception("rank=%d control connection close failed", worker.rank)
                    errors.append(f"rank={worker.rank} control connection close: {exc}")
                worker.eof = True
                retained.append(worker)
                continue
            self._drain_worker(worker, deadline)
            if worker.cleanup_ok is not True:
                detail = worker.cleanup_error or "cleanup_done was not received"
                errors.append(f"rank={worker.rank} cleanup: {detail}")
            if worker.process.exitcode != 0:
                errors.append(f"rank={worker.rank} exitcode={worker.process.exitcode}")
            try:
                worker.process.close()
            except Exception as exc:
                LOG.exception("rank=%d process handle close failed", worker.rank)
                errors.append(f"rank={worker.rank} process handle close: {exc}")
                retained.append(worker)
            try:
                worker.connection.close()
            except Exception as exc:
                LOG.exception("rank=%d control connection close failed", worker.rank)
                errors.append(f"rank={worker.rank} control connection close: {exc}")
        self.workers = retained
        self.state = "broken" if retained else "closed"
        if not retained:
            LOG.info("BM pool workers exited, pids=%s", pids)
        if errors:
            raise RuntimeError("; ".join(errors))

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except Exception as cleanup:
            LOG.exception("Pool context cleanup failed")
            if exc is not None:
                raise RuntimeError(f"{exc}; pool cleanup: {cleanup}") from exc
            raise
