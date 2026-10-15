# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""CPU regressions for ACL-only SoC detection and strict model mapping."""

import sys
import types
import unittest
from unittest.mock import Mock, patch

from test_paired_benchmark import PACKAGE, load

env = load("_benchmark_env_under_test", PACKAGE / "env_utils.py")


class BenchmarkProtocolTest(unittest.TestCase):
    def test_model_mapping_rejects_ambiguous_names_and_preserves_kernel_behavior(self):
        for soc, expected in (
            ("Ascend910B3", "A2"),
            ("910B4", "A2"),
            ("Ascend910_9382", "A3"),
            ("Ascend950PR_9579", "A5"),
            ("Ascend910_9589", "A5"),
            ("Ascend910", None),
            ("Ascend910A", None),
            ("9382", None),
            ("Ascend310P3", None),
            ("Ascend910_9999", None),
            (None, None),
        ):
            with self.subTest(soc=soc):
                self.assertEqual(env._benchmark_soc_key(soc), expected)
        self.assertEqual(env._map_soc_key("Ascend910"), "A3")

    def test_detection_uses_only_acl_and_handles_unavailable_or_invalid_results(self):
        query = Mock(return_value="Ascend910_9382")
        with (
            patch.dict(sys.modules, {"acl": types.SimpleNamespace(get_soc_name=query)}),
            patch.object(env, "_detect_soc_from_npu_smi", side_effect=AssertionError("no fallback")),
        ):
            self.assertEqual(env.get_benchmark_soc(), ("Ascend910_9382", "A3"))
            query.assert_called_once_with()
            for soc in ("Ascend910", "Ascend910_9999", None, "", 9382):
                with self.subTest(soc=soc):
                    query.return_value = soc
                    name = soc if isinstance(soc, str) and soc else None
                    self.assertEqual(env.get_benchmark_soc(), (name, None))
            query.side_effect = RuntimeError("ACL unavailable")
            self.assertEqual(env.get_benchmark_soc(), (None, None))
        with patch.dict(sys.modules, {"acl": None}):
            self.assertEqual(env.get_benchmark_soc(), (None, None))
