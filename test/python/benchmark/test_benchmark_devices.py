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
import os
import types
from unittest.mock import patch

from test_benchmark_runtime import PREFIX, BenchmarkModuleTest


class BenchmarkDeviceDiscoveryTest(BenchmarkModuleTest):
    def test_chip_count_and_legacy_board_output(self):
        config = importlib.import_module(f"{PREFIX}.common.config")
        a3 = "Total Count : 8\n" + "".join(f"NPU ID : {i}\nChip Count : 2\n" for i in range(8))
        a5 = "Total Count : 8\n" + "".join(f"NPU ID : {i}\n" for i in range(8))
        for output, expected in (
            (a3, 16),
            (a5, 8),
            (a3.replace("Chip Count : 2", "Chip Count : 1"), 8),
            ("NPU ID : 0\nNPU ID : 1\n", 2),
            ("Total Count : 16\n", 16),
            ("", 0),
        ):
            with self.subTest(output=output):
                self.assertEqual(config._count_from_npu_smi(output), expected)
        with (
            patch.dict(os.environ, {"ASCEND_RT_VISIBLE_DEVICES": ""}),
            patch.object(config.shutil, "which", return_value="/usr/bin/npu-smi"),
            patch.object(config.subprocess, "run", return_value=types.SimpleNamespace(stdout=a3)),
        ):
            visible = config._parse_npu_devices()
        self.assertEqual(visible, list(range(16)))
        params_module = importlib.import_module(f"{PREFIX}.common.params")
        args = types.SimpleNamespace(
            ip="tcp://127.0.0.1:12050",
            device=None,
            group=None,
            protocol="sdma",
            nic="",
            direction="all",
            block_size="4K",
            batch_size=1,
            copy_count=1,
            warmup_count=0,
            log=False,
        )
        params = params_module.build_params(args, visible)
        self.assertEqual(params.world_size, 16)
        self.assertEqual(params.devices, tuple(range(16)))
        args.device = "8-15"
        params = params_module.build_params(args, visible)
        self.assertEqual(params.devices, tuple(range(8, 16)))
        self.assertEqual(params.world_size, 8)

    def test_visible_device_order_takes_precedence(self):
        config = importlib.import_module(f"{PREFIX}.common.config")
        with (
            patch.dict(os.environ, {"ASCEND_RT_VISIBLE_DEVICES": "15,8,3,0"}),
            patch.object(config.subprocess, "run") as probe,
        ):
            self.assertEqual(config._parse_npu_devices(), [15, 8, 3, 0])
            probe.assert_not_called()
