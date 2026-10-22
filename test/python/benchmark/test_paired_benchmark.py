# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""CPU regressions for pairing, writer lifetime, verification and failure handling."""

import importlib.util
import io
import queue
import sys
import threading
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

PACKAGE = Path(__file__).resolve().parents[3] / "src/smem/python/memfabric_hybrid/memfabric_hybrid"
BLOCK_BYTES = 4096
ELEMENT_BYTES = 4
LOCAL_RANKS = 2
PEER_GROUPS = 2
BYTES_PER_KIB = 1024
WRITER_RANK = 0
COPIES = 3
WARMUP_COPIES = 2
SYNC_SECONDS = 2
SHORT_TIMEOUT = 0.1
HOUR_SECONDS = 3600
THREAD_TIMEOUT = 5
FAILED_EXIT = 1
LOOPBACK = "tcp://127.0.0.1:12050"
WRITE_STORE = "tcp://10.0.0.1:12050"


class Buffer(list):
    def flatten(self):
        return self

    def data_ptr(self):
        return id(self)

    def copy_(self, other):
        self[:] = other

    def zero_(self):
        self[:] = [0] * len(self)

    def cpu(self):
        return self

    def __ne__(self, other):
        wrong = sum(a != b for a, b in zip(self, other))
        return types.SimpleNamespace(sum=lambda: types.SimpleNamespace(item=lambda: wrong))


class Handle:
    def __init__(self, test):
        self.test, self.rank = test, test.local.rank
        self.pool = Buffer([0] * (BLOCK_BYTES // ELEMENT_BYTES))
        test.pools[self.rank] = self.pool
        test.memory[self.pool.data_ptr()] = self.pool

    def peer_rank_ptr(self, rank, mem_type):
        return self.test.pools[rank].data_ptr()

    def copy_data(self, source, destination, size, copy_type, flags):
        self.test.memory[destination][:] = self.test.memory[source][: size // ELEMENT_BYTES]
        return 0

    def copy_data_batch(self, src_addrs, dst_addrs, sizes, count, type, flags):
        key = (self.rank, type)
        self.test.calls[key] = self.test.calls.get(key, 0) + 1
        if self.test.noop == type and self.test.calls[key] > WARMUP_COPIES:
            return 0
        for source, destination, size in zip(src_addrs, dst_addrs, sizes):
            self.copy_data(source, destination, size, type, flags)
        return 0

    def leave(self):
        self.test.early_leave |= not self.test.store.check(
            [f"verified/{rank}" for rank in range(LOCAL_RANKS * PEER_GROUPS)]
        )
        return 0


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class PairedBenchmarkTest(unittest.TestCase):
    def setUp(self):
        self.memory, self.pools, self.calls, self.values = {}, {}, {}, {}
        self.local, self.noop, self.early_leave = threading.local(), None, False
        self.output = queue.Queue()
        self.store = types.SimpleNamespace(
            set=lambda key, value: self.values.__setitem__(key, value.encode()),
            get=lambda key: self.values[key],
            check=lambda keys: all(key in self.values for key in keys),
        )
        enum = types.SimpleNamespace(
            **{name.upper(): name for name in "sdma device_rdma device_urma host_rdma host_urma host_tcp".split()}
        )
        bm = types.SimpleNamespace(
            BmDataOpType=enum,
            BmMemType=types.SimpleNamespace(DEVICE="device", HOST="host"),
            BmCopyType=types.SimpleNamespace(L2G="write", G2L="read"),
            BmConfig=lambda: types.SimpleNamespace(set_nic=Mock()),
            initialize=lambda url, world, device, cfg: setattr(self.local, "rank", cfg.rank_id) or 0,
            bm_rank_id=lambda: self.local.rank,
            uninitialize=Mock(),
        )
        torch = types.SimpleNamespace(
            equal=lambda a, b: a == b, npu=types.SimpleNamespace(set_device=Mock(), synchronize=Mock())
        )
        package = types.ModuleType("_benchmark_under_test")
        package.__path__ = [str(PACKAGE / "benchmark")]
        modules = {
            "torch": torch,
            "torch.distributed": types.SimpleNamespace(TCPStore=lambda *args, **kwargs: self.store),
            "memfabric_hybrid": types.SimpleNamespace(
                bm=bm, initialize=lambda: 0, uninitialize=Mock(), set_log_level=Mock()
            ),
            "_benchmark_under_test": package,
            "_benchmark_under_test.mem_scan": Mock(),
            "_benchmark_under_test._acc_offload_build": Mock(),
            "_benchmark_under_test.env_utils": Mock(),
        }
        self._load_runtime(modules)

    def _load_runtime(self, modules):
        module_patch = patch.dict(sys.modules, modules)
        module_patch.start()
        self.addCleanup(module_patch.stop)
        self.entry = load("_benchmark_under_test.benchmark", PACKAGE / "benchmark/benchmark.py")
        self.runner = load(
            "_benchmark_under_test.bm_band_width_benchmark", PACKAGE / "benchmark/bm_band_width_benchmark.py"
        )
        self.paired = load("_benchmark_under_test.paired_benchmark", PACKAGE / "benchmark/paired_benchmark.py")
        self.parser = load("_benchmark_under_test.cli", PACKAGE / "cli.py")._parser()
        self.create_pool = self.paired._create_pool
        for name, replacement in (("_create_pool", self._pool), ("_create_buffer", self._buffers)):
            boundary = patch.object(self.paired, name, replacement)
            boundary.start()
            self.addCleanup(boundary.stop)

    def _pool(self, params):
        handle = Handle(self)
        handle.join, handle.destroy = Mock(return_value=0), Mock()
        return handle

    def _buffers(self, handle, params, rank):
        expected = Buffer([0] * (BLOCK_BYTES // ELEMENT_BYTES))
        self.runner._generate_data(expected, rank if params["direction"] == "write" else rank - LOCAL_RANKS)
        tensor = Buffer(expected if params["direction"] == "write" else [0] * len(expected))
        self.memory[tensor.data_ptr()] = tensor
        return tensor, tensor, expected

    def _args(self, command, options=()):
        args = self.parser.parse_args(
            [
                "benchmark",
                command,
                "--protocol",
                "device_urma",
                "--ip",
                WRITE_STORE,
                "--local-rank-size",
                str(LOCAL_RANKS),
                "--copysize",
                str(BLOCK_BYTES),
                "--copy-count",
                str(COPIES),
                "--warmup-count",
                str(WARMUP_COPIES),
                "--sync-timeout",
                str(SYNC_SECONDS),
                *options,
            ]
        )
        args.device = WRITER_RANK
        self.entry._configure_benchmark(args)
        self.entry._validate_layout(args, LOCAL_RANKS)
        return args

    def _start(self, command, options=()):
        args = self._args(command, options)
        threads = []
        for local_rank in range(LOCAL_RANKS):
            thread = threading.Thread(
                target=self.paired.run_paired_rank,
                args=(
                    self.runner._get_bm_params(args),
                    args.rank_start + local_rank,
                    local_rank,
                    self.output,
                    Mock(),
                ),
            )
            thread.start()
            threads.append(thread)
        return threads

    def _finish(self, threads):
        for thread in threads:
            thread.join(timeout=THREAD_TIMEOUT)
            self.assertFalse(thread.is_alive(), "rank did not terminate")
        return [self.output.get_nowait() for _ in range(self.output.qsize())]

    def test_default_and_explicit_layouts(self):
        self.assertEqual(self.parser.parse_args(["benchmark"]).sync_timeout, HOUR_SECONDS)
        for option in ("--nic", "--rdma"):
            args = self._args("write", [option, WRITE_STORE])
            self.assertEqual(self.runner._get_bm_params(args)["nic"], WRITE_STORE)
        for command in ("write", "read"):
            for endpoint in (LOOPBACK, WRITE_STORE):
                with self.subTest(command=command, endpoint=endpoint):
                    args = self._args(command, ["--ip", endpoint, "--protocol", "sdma"])
                    with (
                        patch.object(self.entry, "_parse_npu_devices", return_value=list(range(LOCAL_RANKS))),
                        patch.object(self.runner, "run_bm", return_value=0),
                    ):
                        self.assertEqual(self.entry.run(args), 0)
                    self.assertEqual(args.paired, endpoint == WRITE_STORE)
                    self.assertEqual(args.world_size, LOCAL_RANKS * PEER_GROUPS if args.paired else LOCAL_RANKS)
                    self.assertEqual(args.rank_start, LOCAL_RANKS if args.paired and command == "read" else WRITER_RANK)
                    explicit = self._args(command, ["--ip", endpoint, "--world-size", str(LOCAL_RANKS * PEER_GROUPS)])
                    self.assertTrue(explicit.paired)

    def test_auto_protocol_is_resolved_before_validation_and_worker_configuration(self):
        env = types.SimpleNamespace(get_benchmark_soc=Mock())
        with (
            patch.dict(sys.modules, {"memfabric_hybrid.env_utils": env}),
            patch.object(self.entry, "_parse_npu_devices", return_value=[0]),
            patch.object(self.runner, "run_bm", return_value=0) as run_bm,
        ):
            for generation, protocol in (("A2", "device_rdma"), ("A3", "sdma"), ("A5", "device_urma")):
                for command in ("write", "read"):
                    with self.subTest(generation=generation, command=command):
                        env.get_benchmark_soc.return_value = ("Ascend-test", generation)
                        args = self.parser.parse_args(["benchmark", command, "--ip", WRITE_STORE])
                        self.assertEqual(args.protocol, "auto")
                        self.assertEqual(self.entry.run(args), 0)
                        self.assertEqual(args.protocol, protocol)
                        self.assertEqual(self.runner._get_bm_params(args)["protocol"], protocol)
                        self.assertIs(self.runner._get_bm_params(args)["enable_56bits_gva"], True)
                        self.assertTrue(args.paired)
            env.get_benchmark_soc.side_effect = AssertionError("explicit protocol must bypass detection")
            for protocol in ("sdma", "device_rdma", "device_urma", "host_tcp"):
                args = self.parser.parse_args(["benchmark", "write", "--protocol", protocol])
                self.assertEqual(self.entry.run(args), 0)
                self.assertEqual(args.protocol, protocol)
            env.get_benchmark_soc.side_effect = None
            for soc in ("Ascend910", None):
                env.get_benchmark_soc.return_value = (soc, None)
                run_bm.reset_mock()
                with self.assertLogs(self.entry.LOGGER, level="ERROR") as logs:
                    self.assertEqual(self.entry.run(self.parser.parse_args(["benchmark", "--protocol", "auto"])), 1)
                self.assertIn("acl.get_soc_name()", logs.output[0])
                self.assertIn("specify --protocol explicitly", logs.output[0])
                run_bm.assert_not_called()

    def test_56bits_is_enabled_for_both_pool_creation_paths(self):
        # Stop at join so the worker regression needs no NPU buffers.
        handle = types.SimpleNamespace(join=Mock(return_value=1))
        with (
            patch.object(self.runner.bm, "create2", return_value=handle, create=True) as create,
            patch.object(self.runner.torch.npu, "Stream", return_value=Mock(), create=True),
        ):
            for protocol in ("device_urma", "sdma"):
                with self.subTest(protocol=protocol):
                    args = self._args("write", ["--ip", LOOPBACK, "--protocol", protocol])
                    params = self.runner._get_bm_params(args)
                    self.assertIs(self.create_pool(params), handle)
                    self.assertIs(create.call_args.kwargs["enable_56bits_gva"], True)
                    create.reset_mock()
                    output = queue.Queue()
                    self.runner._run_bm_rank(params, WRITER_RANK, 0, output, Mock())
                    create.assert_called_once()
                    self.assertIs(create.call_args.kwargs["enable_56bits_gva"], True)
                    self.assertIn("handle.join failed", output.get_nowait()["error"])

    def test_writer_reports_before_reader_and_keeps_paired_pools_alive(self):
        writers = self._start("write")
        try:
            events = [self.output.get(timeout=THREAD_TIMEOUT) for _ in range(LOCAL_RANKS)]
            self.assertTrue(all(event.get("event") == "measurement" for event in events))
            self.assertTrue(all(thread.is_alive() for thread in writers))
            self.assertEqual(set(self.pools), set(range(LOCAL_RANKS)))
            with patch.object(sys, "stdout", io.StringIO()) as captured:
                report = self.runner._writer_reporter(self._args("write"))
                for event in events:
                    report(event["result"])
            self.assertIn("protocol:device_urma", captured.getvalue())
            self.assertIn("Write benchmark complete", captured.getvalue())
            results = self._finish(writers + self._start("read"))
        finally:
            self._finish(writers)
        self.assertEqual(len(results), LOCAL_RANKS * PEER_GROUPS)
        self.assertFalse(any("error" in result for result in results), results)
        for result in results:
            rank = result["rank_id"]
            self.assertEqual(result["peer_rank_id"], rank if rank < LOCAL_RANKS else rank - LOCAL_RANKS)
            self.assertEqual(result["wrong_bytes"], 0)
            self.assertEqual(result["total_kb"], BLOCK_BYTES * COPIES // BYTES_PER_KIB)
            self.assertEqual(self.calls[(rank, "write" if rank < LOCAL_RANKS else "read")], COPIES + WARMUP_COPIES)
        self.assertFalse(self.early_leave)

    def test_noop_measured_copy_cannot_reuse_warmup_data(self):
        for direction in ("write", "read"):
            with self.subTest(direction=direction):
                self.calls.clear()
                self.noop = direction
                messages = self._finish(self._start("write") + self._start("read"))
                results = [message for message in messages if "event" not in message]
                self.assertEqual(len(results), LOCAL_RANKS * PEER_GROUPS)
                self.assertTrue(all("error" in result for result in results), results)
                self.assertTrue(any("verification failed" in result["error"] for result in results))
                # Each direction is a separate session with fresh configuration/failure keys.
                self.values.clear()
                self.memory.clear()
                self.pools.clear()

    def test_configuration_mismatch_fails_both_groups(self):
        for options in (["--copy-count", str(COPIES + 1)], ["--protocol", "sdma"]):
            with self.subTest(options=options):
                results = self._finish(self._start("write") + self._start("read", options))
                errors = [result["error"] for result in results if "error" in result]
                self.assertEqual(len(errors), LOCAL_RANKS * PEER_GROUPS)
                self.assertTrue(any("configuration mismatch" in error for error in errors))
                self.values.clear()
                self.memory.clear()
                self.pools.clear()

    def test_missing_reader_times_out_after_valid_write_measurement(self):
        messages = self._finish(self._start("write", ["--sync-timeout", str(SHORT_TIMEOUT)]))
        measurements = [message["result"] for message in messages if message.get("event") == "measurement"]
        errors = [message["error"] for message in messages if "error" in message]
        self.assertEqual(len(measurements), LOCAL_RANKS)
        self.assertTrue(all(result["wrong_bytes"] == 0 for result in measurements))
        self.assertEqual(len(errors), LOCAL_RANKS)
        self.assertTrue(all("timed out" in error for error in errors))

    def test_parent_returns_failure_for_crashed_or_missing_worker_results(self):
        for exitcode in (FAILED_EXIT, 0):
            with self.subTest(exitcode=exitcode):
                process = types.SimpleNamespace(exitcode=exitcode, join=Mock())
                with (
                    patch.object(self.runner, "_spawn_ranks", return_value=[process]),
                    self.assertLogs(self.runner.LOGGER, level="ERROR"),
                ):
                    self.assertEqual(self.runner.run_bm(self._args("read")), FAILED_EXIT)


if __name__ == "__main__":
    unittest.main()
