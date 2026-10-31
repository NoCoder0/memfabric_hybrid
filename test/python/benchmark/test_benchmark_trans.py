# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Small CPU regressions for Trans contracts and the shared measurement stages."""

import dataclasses
import importlib
import sys
import types
from unittest.mock import MagicMock, Mock, patch

from test_benchmark_runtime import PREFIX, BenchmarkModuleTest


class TransContractTest(BenchmarkModuleTest):
    def test_bm_pool_covers_large_payload(self):
        mf = types.ModuleType("memfabric_hybrid")
        mf.bm = Mock()
        with patch.dict(sys.modules, {"memfabric_hybrid": mf, "torch": Mock()}):
            module = importlib.import_module(f"{PREFIX}.bm.rank")
        for payload, capacity in ((128 * 1024**2, 4), (4 * 1024**3 + 4096, 5), (20 * 1024**3, 20)):
            with self.subTest(payload=payload):
                params = types.SimpleNamespace(
                    device_indices=[0], devices=[0], payload_size=payload, local_hbm_size=0, protocol="sdma"
                )
                rank = module.BmRank(params, 0, 0)
                rank._create_pool()
                kwargs = mf.bm.create2.call_args.kwargs
                self.assertEqual(kwargs["local_dram_size"], capacity * 1024**3)
                self.assertEqual(kwargs["max_dram_size"], capacity * 1024**3)

    def test_sdma_host_uses_exported_pool_and_other_paths_do_not(self):
        native = types.ModuleType(f"{PREFIX}.common.native")
        native.configure_native_logging = Mock()

        def require_ok(result, operation):
            if result != 0:
                raise RuntimeError(operation)

        native.require_ok = require_ok
        mf = types.ModuleType("memfabric_hybrid")
        mf.TransferEngine = Mock()
        mf.offload = Mock()
        tensor = MagicMock()
        tensor.data_ptr.return_value = 2 * 1024**2
        tensor.__getitem__.return_value = tensor
        torch = Mock(int32="int32")
        torch.zeros.return_value = torch.frombuffer.return_value = tensor
        mf.offload.empty.return_value = tensor
        mf.offload.initialize.return_value = 0
        engine = mf.TransferEngine.return_value
        engine.initialize.return_value = engine.register_memory.return_value = 0
        with patch.dict(sys.modules, {native.__name__: native, "memfabric_hybrid": mf, "torch": torch}):
            module = importlib.import_module(f"{PREFIX}.trans.rank")
        phase = types.SimpleNamespace(local_mem="device", peer_mem="host")
        params = types.SimpleNamespace(
            device_indices=[0, 1],
            devices=[8, 9],
            log=False,
            protocol="sdma",
            local_ip="127.0.0.1",
            store_url="tcp://127.0.0.1:12050",
            nic="",
            phases=[phase],
            payload_size=4096,
            world_size=2,
            local_rank_size=2,
            group_of=lambda rank: rank,
            peer_of=lambda rank: 1 - rank,
        )
        for protocol, host, pool in (("sdma", True, True), ("device_rdma", True, False), ("sdma", False, False)):
            with self.subTest(protocol=protocol, host=host), patch.object(module.mmap, "mmap"):
                params.protocol = protocol
                phase.peer_mem = "host" if host else "device"
                mf.offload.reset_mock()
                rank = module.TransRank(params, 1, 1)
                rank.initialize(MagicMock())
                self.assertEqual(mf.offload.initialize.called, pool)
                self.assertEqual(mf.offload.empty.called, pool)
                if pool:
                    config = mf.offload.initialize.call_args.args[0]
                    self.assertEqual((config.device_id, config.rank_id, config.world_size), (1, 1, 2))
                    self.assertEqual((config.reserve_size, config.alloc_size), (4 * 1024**2,) * 2)
                    self.assertEqual(config.scene, mf.offload.Scene.SHARED)
                    self.assertEqual(config.store_url, params.store_url)
                rank.close()
                self.assertEqual(mf.offload.uninitialize.called, pool)
        params.protocol, phase.peer_mem = "sdma", "host"
        engine.register_memory.return_value = -100
        rank = module.TransRank(params, 1, 1)
        with self.assertRaisesRegex(RuntimeError, "register_memory"):
            rank.initialize(Mock())
        self.assertTrue(rank.offload_initialized)
        rank.close()
        self.assertFalse(rank.offload_initialized)

    def test_chunked_verification_checks_tail(self):
        sys.modules.pop("torch", None)
        sys.modules.pop("torch_npu", None)
        try:
            import torch
        except ImportError:
            self.skipTest("CPU torch is required for tensor verification")
        native = types.ModuleType(f"{PREFIX}.common.native")
        native.require_ok = native.configure_native_logging = native.configure_worker_logging = Mock()
        mf = types.ModuleType("memfabric_hybrid")
        mf.TransferEngine = Mock()
        mf.offload = Mock()
        with patch.dict(sys.modules, {native.__name__: native, "memfabric_hybrid": mf}):
            module = importlib.import_module(f"{PREFIX}.trans.pairwise")
        phase = importlib.import_module(f"{PREFIX}.common.params").Phase("read", "host", "host")
        target = torch.zeros(21, dtype=torch.int32)
        rank = types.SimpleNamespace(
            rank_id=1, params=types.SimpleNamespace(group_of=lambda rank: 1), buffer=lambda _: target
        )
        with patch.object(module, "_VERIFY_CHUNK_BYTES", 32):
            case = module.TransCase(phase, 28, 3, rank, 0)
        self.assertEqual(len(case.expected), 8)
        with patch.object(torch, "npu", types.SimpleNamespace(synchronize=Mock()), create=True):
            case.seed_own_slot()
            case.verify()
            target[-1] = -(2**31)
            with self.assertRaisesRegex(RuntimeError, "wrong_bytes"):
                case.verify()

    def test_parameters_and_remote_ranges(self):
        common = importlib.import_module(f"{PREFIX}.common.params")
        trans = importlib.import_module(f"{PREFIX}.trans.params")
        args = types.SimpleNamespace(
            backend="trans",
            local_ip="127.0.0.1",
            ip="tcp://127.0.0.1:12050",
            device=None,
            group=None,
            protocol="device_rdma",
            nic="",
            direction="all",
            block_size="16K,2M",
            batch_size=1,
            copy_count=2,
            warmup_count=0,
            log=False,
        )
        params = common.build_params(args, [0, 1])
        self.assertEqual(params.local_hbm_size, 0)
        self.assertEqual(params.signature(), dataclasses.replace(params, local_ip="127.0.0.2").signature())
        self.assertNotEqual(params.signature(), dataclasses.replace(params, backend="bm").signature())
        args.direction = args.block_size = args.batch_size = None
        params = common.build_params(args, [0, 1])
        self.assertEqual([p.name for p in params.phases], ["D2rD", "D2rH", "rD2D", "rH2D"])
        self.assertEqual((params.block_sizes[0], params.block_sizes[-1]), (16 * 1024, 8 * 1024**2))
        self.assertEqual(params.batch_for(8 * 1024**2), 16)
        args.transfer_size = "256M"
        params = common.build_params(args, [0, 1])
        self.assertEqual(params.batch_for(8 * 1024**2), 32)
        self.assertNotEqual(params.signature(), dataclasses.replace(params, transfer_size=128 * 1024**2).signature())
        args.batch_size = 2000
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            common.build_params(args, [0, 1])
        args.transfer_size, args.block_size = None, "8M"
        self.assertEqual(common.build_params(args, [0, 1]).payload_size, 8 * 1024**2 * 2000)
        args.batch_size = 2560
        self.assertEqual(common.build_params(args, [0, 1]).payload_size, 20 * 1024**3)
        args.batch_size = 2561
        with self.assertRaisesRegex(ValueError, "exceeds"):
            common.build_params(args, [0, 1])
        args.batch_size = None
        for invalid in ("0", "21G"):
            args.transfer_size = invalid
            with self.assertRaises(ValueError):
                common.build_params(args, [0, 1])
        args.backend, args.local_ip, args.transfer_size, args.block_size = "bm", None, None, None
        params = common.build_params(args, [0, 1])
        self.assertEqual(len(params.phases), 8)
        self.assertEqual(params.block_sizes[-1], 2 * 1024**2)
        self.assertEqual(params.batch_for(16 * 1024), 8192)
        args.block_size, args.batch_size = "8M", 2000
        self.assertEqual(common.build_params(args, [0, 1]).payload_size, 8 * 1024**2 * 2000)
        for backend in ("bm", "trans"):
            args.backend = backend
            args.transfer_size, args.batch_size = None, 2560
            self.assertEqual(common.build_params(args, [0, 1]).payload_size, 20 * 1024**3)
            args.batch_size = 2561
            with self.assertRaisesRegex(ValueError, "exceeds"):
                common.build_params(args, [0, 1])
            args.transfer_size, args.batch_size = "20G", None
            self.assertEqual(common.build_params(args, [0, 1]).payload_size, 20 * 1024**3)
            args.transfer_size = "21G"
            with self.assertRaisesRegex(ValueError, "at most"):
                common.build_params(args, [0, 1])
        args.transfer_size = None
        args.backend, args.batch_size = "trans", 1
        args.protocol = "host_tcp"
        with self.assertRaisesRegex(ValueError, "does not expose"):
            common.build_params(args, [0, 1])
        info = {"endpoint": "127.0.0.1:12345", "buffers": {"host": [4096, 8192]}}
        self.assertEqual(trans.peer_buffer(info, "host", 8192), ("127.0.0.1:12345", 4096))
        with self.assertRaisesRegex(ValueError, "invalid Trans peer buffer"):
            trans.peer_buffer(info, "host", 8193)

    def test_connection_and_validation_stay_outside_measurement(self):
        native = types.ModuleType(f"{PREFIX}.common.native")
        native.require_ok = lambda result, operation: None
        sys.modules[native.__name__] = native
        common = importlib.import_module(f"{PREFIX}.common.pairwise")
        phase = importlib.import_module(f"{PREFIX}.common.params").Phase("write", "device", "host")
        events = []
        rank = types.SimpleNamespace(rank_id=0, device_id=2)
        case = types.SimpleNamespace(
            phase=phase,
            size=4096,
            batch=2,
            length=8192,
            rank=rank,
            peer=1,
            copy_args={},
            prepare_initiator=lambda: events.append("prepare"),
            connect=lambda: events.append("connect"),
            warmup=lambda count: events.append(("warmup/reset", count)),
            copy=lambda: events.append("copy") or 0,
            verify=lambda: events.append("verify"),
        )
        params = types.SimpleNamespace(
            phases=[phase],
            block_sizes=[4096],
            warmup_count=0,
            copy_count=2,
            group_of=lambda rank_id: rank_id,
            peer_of=lambda rank_id: 1,
            batch_for=lambda size: 2,
        )
        sync = types.SimpleNamespace(barrier=lambda name: events.append(name.rsplit("/", 1)[-1]))
        with patch.object(common.time, "monotonic_ns", side_effect=[100, 1100]):
            rows = common.PairwiseBenchmark(rank, sync, params, lambda *args: case).run()
        self.assertEqual(
            events,
            [
                "prepare",
                "prepared",
                "connect",
                ("warmup/reset", 0),
                "start",
                "copy",
                "copy",
                "copied",
                "verify",
                "verified",
            ],
        )
        self.assertEqual(rows[0]["total_bytes"], 16384)
        self.assertEqual(rows[0]["bandwidth_gbps"], 16.384)

    def test_registered_buffers_outlive_engine_destroy(self):
        native = types.ModuleType(f"{PREFIX}.common.native")
        native.configure_native_logging = native.configure_worker_logging = native.require_ok = Mock()
        sys.modules[native.__name__] = native
        mf = types.ModuleType("memfabric_hybrid")
        mf.TransferEngine = Mock()
        mf.offload = Mock()
        with patch.dict(sys.modules, {"memfabric_hybrid": mf, "torch": types.ModuleType("torch")}):
            module = importlib.import_module(f"{PREFIX}.trans.rank")
        rank = object.__new__(module.TransRank)
        rank.buffers = {"host": object()}
        rank.offload_initialized = True
        events = []
        rank.engine = Mock()
        rank.engine.destroy.side_effect = lambda: events.append(("destroy", bool(rank.buffers)))
        rank.engine.unInitialize.side_effect = lambda: events.append(("uninit", bool(rank.buffers)))
        mf.offload.uninitialize.side_effect = lambda: events.append(("offload", bool(rank.buffers)))
        rank.close()
        self.assertEqual(events, [("destroy", True), ("offload", False), ("uninit", False)])
        self.assertFalse(rank.buffers)
        rank.close()
        mf.offload.uninitialize.assert_called_once()
        rank.buffers = {"host": object()}
        rank.offload_initialized = True
        rank.engine = Mock()
        rank.engine.destroy.side_effect = RuntimeError("destroy failed")
        with self.assertRaisesRegex(RuntimeError, "destroy failed"):
            rank.close()
        self.assertTrue(rank.buffers)
        self.assertTrue(rank.offload_initialized)
        mf.offload.uninitialize.assert_called_once()
        with patch.dict(sys.modules, {"memfabric_hybrid": mf, "torch": types.ModuleType("torch")}):
            worker = importlib.import_module(f"{PREFIX}.trans.pairwise")
        params = types.SimpleNamespace(log=False, protocol="device_rdma", signature=dict)
        queue = Mock()
        with patch.object(worker, "TransRank") as factory, patch.object(worker, "BenchmarkSync"):
            factory.return_value.initialize.side_effect = RuntimeError("registration failed")
            with self.assertLogs(worker.LOGGER, level="ERROR"), self.assertRaises(SystemExit) as caught:
                worker.run_rank(params, 0, 0, queue)
        self.assertEqual(caught.exception.code, 1)
        factory.return_value.close.assert_called_once()
        queue.close.assert_called_once()
