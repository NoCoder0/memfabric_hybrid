#!/usr/bin/env python3
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
import json
import math
from pathlib import Path

from verify import require


def main():
    parser = argparse.ArgumentParser(description="Compare paired 128K/256K measurements without a regression allowance")
    parser.add_argument("baseline", type=Path)
    parser.add_argument("opendcc", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    old = json.loads(args.baseline.read_text())
    new = json.loads(args.opendcc.read_text())
    require(old["comparison_config"] == new["comparison_config"], "Hardware/model/MC/MF/configuration differs")
    for run in (old, new):
        require(run["artifacts"] and run["evidence"], "Archive artifact hashes and path/correctness evidence")
        require(
            len({(s["input_tokens"], s["stage"], s["round"]) for s in run["stages"]}) == len(run["stages"]),
            "Duplicate measurement stage/round",
        )
    before = {(s["input_tokens"], s["stage"], s["round"]): s for s in old["stages"]}
    after = {(s["input_tokens"], s["stage"], s["round"]): s for s in new["stages"]}
    require(before.keys() == after.keys(), "Missing paired stages/rounds")
    require({key[0] for key in before} == {131072, 262144}, "Require only 128K and 256K results")
    rows = []
    for key, baseline in before.items():
        candidate = after[key]
        for field in ("dataset_sha256", "requests", "output_tokens", "concurrency", "warmup", "max_model_len"):
            require(baseline[field] == candidate[field], f"{key}: incomparable {field}")
        for run in (baseline, candidate):
            require(run["succeeded"] == run["requests"] and run["data_correct"], f"{key}: functional failure")
        require(candidate["hbm_direct"] and candidate["swap_bytes"] == 0, f"{key}: not HBM direct")
        metrics = {}
        for metric, higher in {
            "requests_per_second": True,
            "output_tokens_per_second": True,
            "ttft_mean_ms": False,
            "ttft_p50_ms": False,
            "ttft_p95_ms": False,
            "tpot_ms": False,
            "duration_seconds": False,
        }.items():
            a, b = baseline[metric], candidate[metric]
            require(math.isfinite(a) and math.isfinite(b) and a > 0 and b > 0, f"{key}: invalid {metric}")
            metrics[metric] = {"baseline": a, "opendcc": b, "passed": b >= a if higher else b <= a}
        rows.append({"input_tokens": key[0], "stage": key[1], "round": key[2], "metrics": metrics})
    passed = all(metric["passed"] for row in rows for metric in row["metrics"].values())
    args.output.write_text(json.dumps({"passed": passed, "stages": rows}, indent=2) + "\n")
    require(passed, "Performance regression: inspect report; repeat paired rounds to assess measurement variation")
    print("All supplied paired 128K/256K stages meet the no-regression criterion.")


if __name__ == "__main__":
    main()
