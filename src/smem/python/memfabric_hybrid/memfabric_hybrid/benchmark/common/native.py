# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Native logging and return-code checks shared by benchmark backends."""

import logging
import sys
import time

import memfabric_hybrid as mf

# Native MF levels: 0 DEBUG, 1 INFO, 2 WARN, 3 ERROR, 4 FATAL, 5 TRACE. TRACE is numerically the highest,
# so set_log_level() alone cannot hide it; without --log the quiet logger shows only ERROR and FATAL.
_NATIVE_SHOWN_LEVELS = {3: "ERROR", 4: "FATAL"}


def _quiet_native_log(level, message):
    name = _NATIVE_SHOWN_LEVELS.get(level)
    if name is not None:
        sys.stdout.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {name} {message}\n")
        sys.stdout.flush()


def configure_native_logging(show_logs):
    """Call before mf.initialize() so that initialization output is filtered too."""
    if show_logs:
        require_ok(mf.set_log_level(0), "mf.set_log_level")
        return
    require_ok(mf.set_log_level(3), "mf.set_log_level")
    mf.set_extern_logger(_quiet_native_log)


def configure_worker_logging(show_logs):
    logging.basicConfig(format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    logging.getLogger().setLevel(logging.DEBUG if show_logs else logging.ERROR)


def require_ok(ret, operation):
    if ret != 0:
        raise RuntimeError(f"{operation} failed, ret={ret}, detail={mf.get_last_err_msg()}")
