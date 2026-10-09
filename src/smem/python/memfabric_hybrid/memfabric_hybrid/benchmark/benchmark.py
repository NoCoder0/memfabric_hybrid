# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Benchmark entry point for mfcli benchmark command."""

import logging
import os
import re
import shutil
import subprocess
from pathlib import Path

LOGGER = logging.getLogger(__name__)


def _is_localhost(store_url):
    host = store_url.split("://")[-1].split(":")[0]
    return host in ("127.0.0.1", "localhost")


def _count_from_npu_smi(output):
    """Count NPUs from `npu-smi info -l` output."""
    total = 0
    ids = 0
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
    return total or ids


def _parse_npu_devices():
    visible = os.environ.get("ASCEND_RT_VISIBLE_DEVICES", "")
    if visible:
        devices = [int(x.strip()) for x in visible.split(",") if x.strip()]
        if devices:
            return devices

    npu_smi = shutil.which("npu-smi")
    if npu_smi:
        try:
            proc = subprocess.run([npu_smi, "info", "-l"], capture_output=True, text=True, timeout=30)
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


def run(args):
    devices = _parse_npu_devices()
    npu_count = len(devices)

    if args.device is None:
        args.device = 0
    if args.local_rank_size is None:
        args.local_rank_size = npu_count

    if args.world_size == -1:
        if _is_localhost(args.ip):
            args.world_size = args.local_rank_size
        else:
            args.world_size = 256

    LOGGER.info(
        "benchmark: cmd=%s protocol=%s framework=%s model=%s npu_devices=%s world_size=%d local_rank_size=%d",
        args.command,
        args.protocol,
        args.framework,
        args.model,
        devices,
        args.world_size,
        args.local_rank_size,
    )

    if args.protocol == "sdma" and not _is_localhost(args.ip):
        LOGGER.error("sdma protocol is not supported in multi-node mode")
        return 1

    if args.command in ("write", "read"):
        from .bm_band_width_benchmark import run_bm

        return run_bm(args)
    LOGGER.error("unknown benchmark command: %s", args.command)
    return 1
