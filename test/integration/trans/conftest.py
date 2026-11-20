# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

import json
import logging
import os
import time
from pathlib import Path
import sys
import pytest

try:
    from .trans_config import DEFAULTS, PROTOCOLS, validate_config, CHIP_PROTOCOL
    from .trans_runtime import TransPool, _ascend_info, _chip_from_soc
except ImportError:
    from trans_config import DEFAULTS, PROTOCOLS, validate_config, CHIP_PROTOCOL
    from trans_runtime import TransPool, _ascend_info, _chip_from_soc

LOG = logging.getLogger(__name__)
_STATE = pytest.StashKey[dict]()


def pytest_addoption(parser):
    group = parser.getgroup("trans")
    group.addoption("--trans-protocol", choices=PROTOCOLS, default=DEFAULTS["protocol"])
    group.addoption("--trans-devices", nargs=2, type=int, default=None, metavar=("PREFILL", "DECODE"))
    group.addoption("--trans-log-dir", type=Path, default=Path("/tmp/memfabric-trans"))
    group.addoption("--trans-timeout", type=float, default=DEFAULTS["case_timeout"])


def pytest_configure(config):
    if config.getoption("numprocesses", default=0) not in (None, 0, "0"):
        raise pytest.UsageError("TRANS tests require one pytest process; do not use pytest-xdist -n")
    selected = dict(DEFAULTS)
    selected.update(
        {
            "protocol": config.getoption("--trans-protocol"),
            "devices": config.getoption("--trans-devices"),
            "case_timeout": config.getoption("--trans-timeout"),
        }
    )
    selected["setup_timeout"] = selected["cleanup_timeout"] = selected["case_timeout"]
    try:
        validate_config(selected)
    except ValueError as error:
        raise pytest.UsageError(str(error)) from error
    config.stash[_STATE] = {"pool": None, "failed": None, "closed": False}


def _is_trans_item(item):
    root = Path(__file__).parent.resolve()
    path = Path(str(item.path)).resolve()
    return path.parent == root or root in path.parents


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item):
    if _is_trans_item(item) and item.config.stash[_STATE]["failed"]:
        pytest.skip(f"TRANS run stopped after {item.config.stash[_STATE]['failed']}")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    if _is_trans_item(item):
        LOG.info(
            "TRANS_RESULT %s",
            json.dumps(
                {
                    "nodeid": item.nodeid,
                    "phase": report.when,
                    "outcome": report.outcome,
                    "class": item.cls.__name__ if item.cls else None,
                    "apis": list(getattr(item.cls, "APIS", ())),
                    "protocol": item.config.getoption("--trans-protocol"),
                }
            ),
        )
    if _is_trans_item(item) and report.failed:
        item.config.stash[_STATE]["failed"] = item.nodeid


@pytest.fixture(scope="session")
def trans_pool(request):
    """Pool startup selects cards after the case protocol gate has passed."""
    base = request.config.getoption("--trans-log-dir")
    log_dir = base / f"run-{os.getpid()}-{time.time_ns()}"
    config = dict(DEFAULTS)
    chip = None

    if sys.platform == "linux":
        _, soc = _ascend_info()
        chip = _chip_from_soc(soc)
        if chip is None:
            raise pytest.UsageError("Unable to auto-detect chip")
    config.update(
        {
            "protocol": request.config.getoption("--trans-protocol") or CHIP_PROTOCOL.get(chip, ""),
            "devices": request.config.getoption("--trans-devices"),
            "case_timeout": request.config.getoption("--trans-timeout"),
        }
    )
    config["setup_timeout"] = config["cleanup_timeout"] = config["case_timeout"]
    pool = TransPool(config, log_dir)
    state = request.config.stash[_STATE]
    state["pool"] = pool
    try:
        yield pool
    finally:
        if not state["closed"]:
            state["closed"] = True
            pool.close()
