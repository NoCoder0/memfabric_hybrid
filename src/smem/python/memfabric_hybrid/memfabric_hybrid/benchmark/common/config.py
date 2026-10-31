# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Discover devices and resolve the benchmark protocol."""

import logging
import os
import re
import shutil
import subprocess
from pathlib import Path

LOGGER = logging.getLogger(__name__)


def _count_from_npu_smi(output):
    """Count compute chips, rather than boards, from `npu-smi info -l`."""
    total = 0
    ids = 0
    chips = 0
    for line in (output or "").splitlines():
        text = line.strip()
        if ":" not in text:
            continue
        key, value = text.split(":", 1)
        key = key.strip().upper()
        if key.startswith("TOTAL COUNT"):
            found = re.search(r"\d+", value)
            if found:
                total = int(found.group(0))
        elif key == "NPU ID":
            ids += 1
        elif key == "CHIP COUNT":
            found = re.search(r"\d+", value)
            if found:
                chips += int(found.group(0))
    return chips or total or ids


def _parse_npu_devices():
    visible = os.environ.get("ASCEND_RT_VISIBLE_DEVICES", "")
    if visible:
        devices = [int(x.strip()) for x in visible.split(",") if x.strip()]
        if devices:
            return devices

    npu_smi = shutil.which("npu-smi")
    if npu_smi:
        try:
            proc = subprocess.run([npu_smi, "info", "-l"], check=False, capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            proc = None
        if proc is not None:
            count = _count_from_npu_smi(proc.stdout)
            if count > 0:
                return list(range(count))

    # /dev/davinci0..N-1. The numeric suffix filter matters: a plain "davinci*"
    # glob also matches /dev/davinci_manager and would report one NPU too many.
    davinci_devs = [dev for dev in Path("/dev").glob("davinci*") if dev.name[len("davinci") :].isdigit()]
    if davinci_devs:
        return list(range(len(davinci_devs)))

    return [0]


def _resolve_protocol(args):
    if args.protocol != "auto":
        return
    from memfabric_hybrid.env_utils import get_benchmark_soc

    soc, generation = get_benchmark_soc()
    protocol = {"A2": "device_rdma", "A3": "sdma", "A5": "device_urma"}.get(generation)
    if protocol is None:
        raise ValueError(
            f"cannot select protocol automatically: soc={soc!r}; "
            "acl.get_soc_name() did not identify A2/A3/A5; specify --protocol explicitly"
        )
    args.protocol = protocol
    LOGGER.info("benchmark protocol: requested=auto soc=%r generation=%s protocol=%s", soc, generation, protocol)
