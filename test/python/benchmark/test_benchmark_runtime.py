# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""CPU regressions for benchmark imports and parent-owned worker cleanup."""

import importlib
import os
import signal
import sys
import time
import types
import unittest
from pathlib import Path
from queue import Empty
from unittest.mock import Mock, patch

PACKAGE = Path(__file__).resolve().parents[3] / "src/smem/python/memfabric_hybrid/memfabric_hybrid"
PREFIX = "_benchmark_runtime_under_test"


def _idle_worker(params, rank_id, local_index, queue):
    # Exercise real process ownership without importing torch or touching an NPU.
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    queue.put({"ready_pid": os.getpid()})
    while True:
        time.sleep(1)


def _crash_or_ignore_terminate(params, rank_id, local_index, queue):
    if rank_id == 0:
        params.ready.wait(timeout=10)
        os._exit(3)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    params.ready.set()
    while True:
        time.sleep(1)


class BenchmarkModuleTest(unittest.TestCase):
    def setUp(self):
        saved = {name: module for name, module in sys.modules.items() if self.is_test_module(name)}
        self.addCleanup(self.restore_modules, saved)
        package = types.ModuleType(PREFIX)
        package.__path__ = [str(PACKAGE / "benchmark")]
        sys.modules[PREFIX] = package
        # The package bootstrap is outside this test; isolate these Python modules
        # from installed torch and native extensions, even on a development host.
        sys.modules["torch"] = None
        sys.modules["torch_npu"] = None
        self.runtime = importlib.import_module(f"{PREFIX}.common.runtime")

    @staticmethod
    def is_test_module(name):
        return name == PREFIX or name.startswith(f"{PREFIX}.") or name in ("torch", "torch_npu")

    def restore_modules(self, saved):
        for name in list(sys.modules):
            if self.is_test_module(name):
                del sys.modules[name]
        sys.modules.update(saved)

    def load_entry(self):
        return importlib.import_module(f"{PREFIX}.benchmark")


class BenchmarkImportTest(BenchmarkModuleTest):
    def test_import_does_not_load_worker_dependencies(self):
        entry = self.load_entry()
        self.assertTrue(callable(entry.run))
        self.assertNotIn(f"{PREFIX}.bm.pairwise", sys.modules)

    def test_invalid_configuration_is_rejected_before_worker_import(self):
        entry = self.load_entry()
        args = types.SimpleNamespace(
            log=False,
            protocol="sdma",
            direction="all",
            block_size="2M",
            batch_size=None,
            device="0,1",
            ip="tcp://127.0.0.1:12050",
            group=None,
            nic="tcp://0.0.0.0:10005",
            copy_count=0,
            warmup_count=1,
        )
        with patch.object(entry, "_parse_npu_devices", return_value=[0, 1]):
            with self.assertLogs(entry.LOGGER, level="ERROR"):
                self.assertEqual(entry.run(args), 1)
        self.assertNotIn(f"{PREFIX}.bm.pairwise", sys.modules)

    def test_valid_configuration_dispatches_to_worker(self):
        entry = self.load_entry()
        args = types.SimpleNamespace(log=False)
        for backend in ("bm", "trans"):
            with self.subTest(backend=backend):
                worker_module = types.ModuleType(f"{PREFIX}.{backend}.pairwise")
                worker_module.run_rank = Mock()
                sys.modules[worker_module.__name__] = worker_module
                params = types.SimpleNamespace(backend=backend)
                with (
                    patch.object(entry, "_parse_npu_devices", return_value=[0, 1]),
                    patch.object(entry, "_resolve_protocol"),
                    patch.object(entry, "build_params", return_value=params),
                    patch.object(entry, "BenchmarkRunner") as runner,
                ):
                    runner.return_value.run.return_value = 0
                    self.assertEqual(entry.run(args), 0)
                runner.assert_called_once_with(params, worker_module.run_rank)


class BenchmarkCleanupTest(BenchmarkModuleTest):
    def make_runner(self):
        runner = object.__new__(self.runtime.BenchmarkRunner)
        phase = importlib.import_module(f"{PREFIX}.common.params").Phase("write", "device", "host")
        runner.params = types.SimpleNamespace(
            group=-1,
            rank_start=0,
            local_rank_size=2,
            world_size=2,
            phases=(phase,),
            block_sizes=(4096,),
            group_of=lambda rank: rank,
        )
        runner.worker = Mock()
        runner.ctx = Mock()
        runner.queue = Mock()
        runner.processes = []
        return runner

    @staticmethod
    def process(exitcode=None, resist_terminate=False):
        process = Mock(exitcode=exitcode)
        if not resist_terminate:
            process.terminate.side_effect = lambda: setattr(process, "exitcode", -15)
        process.kill.side_effect = lambda: setattr(process, "exitcode", -9)
        return process

    def run_failing(self, runner, failure):
        with (
            patch.object(self.runtime.output, "print_start"),
            patch.object(self.runtime.LOGGER, "exception"),
            patch.object(self.runtime.LOGGER, "error"),
        ):
            with self.assertRaises(type(failure)) as caught:
                runner.run()
        self.assertIs(caught.exception, failure)
        runner.queue.close.assert_called_once()
        runner.queue.join_thread.assert_called_once()

    def test_partial_spawn_failure_reaps_started_worker(self):
        runner = self.make_runner()
        started, unstarted = self.process(), self.process()
        failure = OSError("injected process.start failure")
        unstarted.start.side_effect = failure
        runner.ctx.Process.side_effect = [started, unstarted]
        self.run_failing(runner, failure)
        started.terminate.assert_called_once()
        started.join.assert_called()
        unstarted.join.assert_not_called()

    def test_collect_failure_reaps_all_workers_before_queue_close(self):
        runner = self.make_runner()
        runner.processes = [self.process(), self.process()]
        runner._spawn = Mock()
        failure = RuntimeError("injected collection failure")
        runner._collect = Mock(side_effect=failure)
        events = []
        for process in runner.processes:
            process.join.side_effect = lambda **kwargs: events.append("join")
        runner.queue.close.side_effect = lambda: events.append("queue close")
        self.run_failing(runner, failure)
        self.assertEqual(events, ["join", "join", "queue close"])
        for process in runner.processes:
            process.terminate.assert_called_once()

    def test_keyboard_interrupt_reaps_workers_and_propagates(self):
        runner = self.make_runner()
        runner.processes = [self.process()]
        runner._spawn = Mock()
        failure = KeyboardInterrupt()
        runner._collect = Mock(side_effect=failure)
        self.run_failing(runner, failure)
        runner.processes[0].terminate.assert_called_once()
        runner.processes[0].join.assert_called()

    def test_normal_completion_does_not_terminate_finished_workers(self):
        runner = self.make_runner()
        runner.processes = [self.process(exitcode=0)]
        runner._spawn = Mock()
        rows = [{"bandwidth_gbps": 20.0}]
        runner._collect = Mock(return_value=(rows, []))
        with (
            patch.object(self.runtime.output, "print_start"),
            patch.object(self.runtime.output, "print_matrix") as report,
        ):
            self.assertEqual(runner.run(), 0)
        runner.processes[0].terminate.assert_not_called()
        runner.processes[0].kill.assert_not_called()
        report.assert_called_once_with(rows, runner.params)
        runner.queue.close.assert_called_once()

    def test_worker_ignoring_terminate_is_killed(self):
        runner = self.make_runner()
        process = self.process(resist_terminate=True)
        runner.processes = [process]
        runner._spawn = Mock()
        failure = RuntimeError("injected collection failure")
        runner._collect = Mock(side_effect=failure)
        self.run_failing(runner, failure)
        process.terminate.assert_called_once()
        process.kill.assert_called_once()
        self.assertEqual(process.join.call_count, 2)

    def test_collect_kills_resistant_peer_after_local_crash(self):
        runner = self.make_runner()
        failed, survivor = self.process(exitcode=1), self.process(resist_terminate=True)
        runner.processes = [failed, survivor]
        runner.queue.get.side_effect = Empty
        runner.queue.get_nowait.side_effect = Empty
        with (
            patch.object(self.runtime, "_ABORT_GRACE_SEC", 0),
            patch.object(self.runtime.time, "monotonic", side_effect=[0, 1, 2, 3]),
            patch.object(self.runtime.LOGGER, "error"),
        ):
            _, errors = runner._collect()
        survivor.terminate.assert_called_once()
        survivor.kill.assert_called_once()
        self.assertEqual({rank for rank, _ in errors}, {0, 1})

    def test_real_crash_reaps_peer_that_ignores_terminate(self):
        ctx = self.runtime.mp.get_context("spawn")
        params = types.SimpleNamespace(group=-1, rank_start=0, local_rank_size=2, ready=ctx.Event())
        runner = self.runtime.BenchmarkRunner(params, _crash_or_ignore_terminate)
        deadline = time.monotonic() + 10
        drain = runner._drain

        def bounded_drain(*args, **kwargs):
            if time.monotonic() >= deadline:
                raise TimeoutError("collector failed to reap the crashed worker's peer")
            return drain(*args, **kwargs)

        try:
            with (
                patch.object(runner, "_drain", side_effect=bounded_drain),
                patch.object(self.runtime.output, "print_start"),
                patch.object(self.runtime.output, "print_matrix") as report,
                patch.object(self.runtime, "_ABORT_GRACE_SEC", 0),
                patch.object(self.runtime, "_REAP_TIMEOUT_SEC", 0.1),
                patch.object(self.runtime.LOGGER, "error"),
                patch("builtins.print"),
            ):
                self.assertEqual(runner.run(), 1)
            report.assert_not_called()
            self.assertTrue(all(not process.is_alive() for process in runner.processes))
            self.assertEqual(runner.processes[0].exitcode, 3)
            if os.name == "posix":
                self.assertEqual(runner.processes[1].exitcode, -signal.SIGKILL)
        finally:
            for process in runner.processes:
                if process.is_alive():
                    process.kill()
                process.join(timeout=5)

    def test_incomplete_or_duplicate_reports_cannot_print_success(self):
        row = {"direction": "D2rH", "copy_size": 4096, "rank_id": 0}
        complete = [{"rank_id": 0, "results": [row]}, {"rank_id": 1, "results": []}]
        cases = {
            "missing reports": [],
            "missing passive report": complete[:1],
            "missing result": [{"rank_id": 0, "results": []}, complete[1]],
            "duplicate result": [{"rank_id": 0, "results": [row, row]}, complete[1]],
            "unexpected result": [{"rank_id": 0, "results": [dict(row, rank_id=1)]}, complete[1]],
            "duplicate report": complete + [complete[0]],
            "unexpected report": complete + [{"rank_id": 2, "results": []}],
        }
        for name, messages in cases.items():
            with self.subTest(name=name):
                runner = self.make_runner()
                runner.processes = [self.process(exitcode=0), self.process(exitcode=0)]
                runner._spawn = Mock()
                runner.queue.get_nowait.side_effect = messages + [Empty]
                with (
                    patch.object(self.runtime.output, "print_start"),
                    patch.object(self.runtime.output, "print_matrix") as report,
                    patch.object(self.runtime.LOGGER, "error") as error,
                    patch("builtins.print"),
                ):
                    self.assertEqual(runner.run(), 1)
                report.assert_not_called()
                error.assert_called()

    def test_complete_reports_include_passive_group_and_gathered_remote_rows(self):
        phase_type = importlib.import_module(f"{PREFIX}.common.params").Phase
        phases = (phase_type("write", "device", "host"), phase_type("read", "device", "host"))
        for group in (-1, 0, 1):
            for selected in (phases, phases[:1]):
                with self.subTest(group=group, directions=[phase.name for phase in selected]):
                    runner = self.make_runner()
                    params = runner.params
                    params.world_size = 4
                    params.group_of = lambda rank: rank // 2
                    params.group = group
                    params.rank_start = 2 if group == 1 else 0
                    params.local_rank_size = 4 if group == -1 else 2
                    params.phases = selected
                    ranks = range(params.rank_start, params.rank_start + params.local_rank_size)
                    rows = [
                        {"direction": phase.name, "copy_size": size, "rank_id": rank}
                        for phase in selected
                        for size in params.block_sizes
                        for rank in range(4)
                        if params.group_of(rank) == phase.initiator_group and (group != 1 or rank in ranks)
                    ]
                    messages = [
                        {
                            "rank_id": rank,
                            "results": rows
                            if rank == 0
                            else [row for row in rows if row["rank_id"] == rank]
                            if group == 1
                            else [],
                        }
                        for rank in ranks
                    ]
                    runner.processes = [self.process(exitcode=0) for _ in ranks]
                    runner.queue.get_nowait.side_effect = messages + [Empty]
                    results, errors = runner._collect()
                    self.assertEqual(errors, [])
                    self.assertEqual(results, rows)

    def test_real_worker_is_reaped_after_collect_exception(self):
        params = types.SimpleNamespace(group=-1, rank_start=0, local_rank_size=1)
        runner = self.runtime.BenchmarkRunner(params, _idle_worker)
        failure = RuntimeError("injected after real worker started")

        def fail_after_ready():
            message = runner.queue.get(timeout=10)
            self.assertEqual(message["ready_pid"], runner.processes[0].pid)
            raise failure

        try:
            with (
                patch.object(self.runtime.output, "print_start"),
                patch.object(self.runtime, "_REAP_TIMEOUT_SEC", 0.1, create=True),
                patch.object(runner, "_collect", side_effect=fail_after_ready),
                patch.object(self.runtime.LOGGER, "exception"),
                patch.object(self.runtime.LOGGER, "error"),
            ):
                with self.assertRaises(RuntimeError) as caught:
                    runner.run()
            self.assertIs(caught.exception, failure)
            self.assertFalse(runner.processes[0].is_alive())
            self.assertIsNotNone(runner.processes[0].exitcode)
        finally:
            for process in runner.processes:
                if process.is_alive():
                    process.kill()
                process.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
