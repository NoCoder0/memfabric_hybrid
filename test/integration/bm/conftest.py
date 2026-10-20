#!/usr/bin/env python
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of the Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import argparse
import logging
import os
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

os.environ.setdefault("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:True")

from config import (
    CHIP_PROTOCOL,
    COMMON,
    PROTOCOL_DATA_OP_TYPE,
    skip_reason,
    validate_config,
)
from helpers import detect_chip, select_devices

LOG = logging.getLogger(__name__)
_BM_RUN = pytest.StashKey["BmRun"]()
_BM_ROOT = Path(__file__).parent.resolve()


def _boolean(value):
    if value == "true":
        return True
    if value == "false":
        return False
    raise argparse.ArgumentTypeError("expected true or false")


@dataclass
class BmRun:
    config: pytest.Config
    pool: object | None = None
    failure_nodeid: str | None = None
    failure_reason: str | None = None
    close_started: bool = False

    def failed(self):
        return self.failure_nodeid is not None

    def record_failure(self, nodeid, reason):
        if self.failed():
            return
        self.failure_nodeid = nodeid
        self.failure_reason = " ".join(str(reason).splitlines())[:2000]
        cleanup_error = self.close_pool()
        if cleanup_error is not None:
            self.failure_reason = f"{self.failure_reason}; pool cleanup: {cleanup_error}"

    def close_pool(self):
        if self.pool is None or self.close_started:
            return None
        self.close_started = True
        try:
            self.pool.close()
        except Exception as exc:
            LOG.exception("BM pool cleanup failed")
            return exc
        return None


def pytest_addoption(parser):
    group = parser.getgroup("bm")
    group.addoption("--bm-chip", default=None, choices=["A2", "A3", "A5"])
    group.addoption("--bm-protocol", default=None, choices=tuple(PROTOCOL_DATA_OP_TYPE))
    group.addoption("--bm-store-url", default=COMMON["store_url"])
    group.addoption("--bm-nic", default=COMMON["nic"])
    group.addoption("--bm-devices", nargs=2, type=int, default=None, metavar=("RANK0", "RANK1"))
    group.addoption(
        "--bm-enable-56bit",
        type=_boolean,
        default=COMMON["enable_56bits_gva"],
        metavar="true|false",
    )
    group.addoption("--bm-world-size", type=int, default=COMMON["world_size"])
    group.addoption("--bm-local-dram-size", type=int, default=COMMON["local_dram_size"])
    group.addoption("--bm-max-dram-size", type=int, default=COMMON["max_dram_size"])
    group.addoption("--bm-local-hbm-size", type=int, default=COMMON["local_hbm_size"])
    group.addoption("--bm-max-hbm-size", type=int, default=COMMON["max_hbm_size"])
    group.addoption("--bm-setup-timeout", type=float, default=COMMON["setup_timeout"])
    group.addoption("--bm-case-timeout", type=float, default=COMMON["case_timeout"])
    group.addoption("--bm-cleanup-timeout", type=float, default=COMMON["cleanup_timeout"])


def _config(config, devices, chip=None):
    selected = dict(COMMON)
    selected.update(
        {
            "devices": list(devices),
            "enable_56bits_gva": config.getoption("--bm-enable-56bit"),
            "world_size": config.getoption("--bm-world-size"),
            "local_dram_size": config.getoption("--bm-local-dram-size"),
            "max_dram_size": config.getoption("--bm-max-dram-size"),
            "local_hbm_size": config.getoption("--bm-local-hbm-size"),
            "max_hbm_size": config.getoption("--bm-max-hbm-size"),
            "setup_timeout": config.getoption("--bm-setup-timeout"),
            "case_timeout": config.getoption("--bm-case-timeout"),
            "cleanup_timeout": config.getoption("--bm-cleanup-timeout"),
            "store_url": config.getoption("--bm-store-url"),
            "nic": config.getoption("--bm-nic"),
        }
    )
    selected["protocol"] = config.getoption("--bm-protocol") or CHIP_PROTOCOL.get(chip, "")
    return selected


def pytest_configure(config):
    # Only static command-line parameters are checked here. collect-only must not load ACL or inspect devices.
    workers = config.getoption("numprocesses", default=0)
    if workers not in (None, 0, "0"):
        raise pytest.UsageError("BM tests require one pytest process; do not use pytest-xdist -n")
    try:
        validate_config(_config(config, config.getoption("--bm-devices") or COMMON["devices"]))
    except ValueError as exc:
        raise pytest.UsageError(str(exc)) from exc
    config.stash[_BM_RUN] = BmRun(config)


def _is_bm_item(item):
    try:
        item.path.resolve().relative_to(_BM_ROOT)
    except ValueError:
        return False
    return True


def _skip_after_failure(item):
    state = item.config.stash[_BM_RUN]
    if state.failed():
        pytest.skip(f"BM run stopped after {state.failure_nodeid}: {state.failure_reason}")


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item):
    if _is_bm_item(item):
        _skip_after_failure(item)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    if _is_bm_item(item) and report.failed:
        reason = str(call.excinfo.value) if call.excinfo is not None else report.longreprtext
        item.config.stash[_BM_RUN].record_failure(item.nodeid, f"{report.when}: {reason}")


@pytest.fixture(scope="session")
def shared_pool(request):
    from bm_runtime import Pool

    state = request.config.stash[_BM_RUN]
    _skip_after_failure(request.node)
    if state.pool is None:
        chip = request.config.getoption("--bm-chip")
        if chip is None and sys.platform == "linux":
            chip = detect_chip()
            if chip is None:
                raise pytest.UsageError("Unable to auto-detect chip; pass --bm-chip explicitly")
        devices = request.config.getoption("--bm-devices")
        if devices is None:
            if sys.platform == "linux":
                physical = select_devices(2)
                if physical is None:
                    raise pytest.UsageError("Unable to select idle devices; pass --bm-devices explicitly")
                os.environ["ASCEND_RT_VISIBLE_DEVICES"] = ",".join(map(str, physical))
            devices = COMMON["devices"]
        selected = _config(request.config, devices, chip)
        reason = skip_reason(selected, chip)
        if reason:
            pytest.skip(reason)
        try:
            validate_config(selected)
        except ValueError as exc:
            raise pytest.UsageError(str(exc)) from exc
        state.pool = Pool(selected)
    try:
        yield state.pool
    finally:
        cleanup_error = state.close_pool()
        if cleanup_error is not None and not state.failed():
            raise cleanup_error
