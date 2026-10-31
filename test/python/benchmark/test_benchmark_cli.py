# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import contextlib
import importlib
import io
import sys
import types
import unittest
from unittest.mock import Mock, patch

from test_benchmark_runtime import PACKAGE


class BenchmarkCliTest(unittest.TestCase):
    def setUp(self):
        package = types.ModuleType("_benchmark_cli_test")
        package.__path__ = [str(PACKAGE)]
        modules = {
            package.__name__: package,
            f"{package.__name__}.mem_scan": Mock(),
            f"{package.__name__}._acc_offload_build": Mock(),
        }
        module_patch = patch.dict(sys.modules, modules)
        module_patch.start()
        self.addCleanup(module_patch.stop)
        self.cli = importlib.import_module(f"{package.__name__}.cli")
        trace_patch = patch.object(self.cli, "_register_trace_command")
        trace_patch.start()
        self.addCleanup(trace_patch.stop)

    def test_backend_and_operation_aliases(self):
        for backend in ("bm", "trans"):
            for operation in (None, "write", "read"):
                with self.subTest(backend=backend, operation=operation):
                    positional = [backend] + ([operation] if operation else [])
                    options = ["--backend", backend] + (["--direction", operation] if operation else [])
                    new = self.cli._parse_args(["benchmark", *positional, "--transfer-size", "256M"])
                    old = self.cli._parse_args(["benchmark", *options, "--transfer-size", "256M"])
                    for args in (new, old):
                        del args.target, args.operation
                    self.assertEqual(vars(new), vars(old))
        self.assertEqual(self.cli._parse_args(["benchmark"]).backend, "bm")
        for operation in ("write", "read"):
            args = self.cli._parse_args(["benchmark", operation])
            self.assertEqual((args.backend, args.direction), ("bm", operation))
            args = self.cli._parse_args(["benchmark", operation, "--backend", "trans"])
            self.assertEqual((args.backend, args.direction), ("trans", operation))

    def test_conflicting_selectors_fail_before_run(self):
        for command in (
            "trans --backend bm",
            "bm --backend trans",
            "trans read --direction write",
            "bm write --direction D2rD",
            "write read",
            "trans invalid",
        ):
            with self.subTest(command=command), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    self.cli._parse_args(["benchmark", *command.split()])
                self.assertEqual(error.exception.code, 2)
        args = self.cli._parse_args(["benchmark", "trans", "read", "--direction", "READ", "--backend", "trans"])
        self.assertEqual((args.backend, args.direction), ("trans", "read"))

    def test_help_and_other_commands(self):
        for target in ("bm", "trans"):
            output = io.StringIO()
            with contextlib.redirect_stdout(output), self.assertRaises(SystemExit) as error:
                self.cli._parse_args(["benchmark", target, "--help"])
            self.assertEqual(error.exception.code, 0)
            self.assertIn("--transfer-size", output.getvalue())
        self.assertEqual(self.cli._parse_args(["version"]).handler, self.cli._show_version)
        self.assertEqual(self.cli._parse_args(["logs", "--help"]).collector_args, ["--help"])
