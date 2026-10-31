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

from .common.config import _parse_npu_devices, _resolve_protocol
from .common.params import build_params
from .common.runtime import BenchmarkRunner

LOGGER = logging.getLogger(__name__)


def run(args):
    logging.getLogger().setLevel(logging.DEBUG if args.log else logging.ERROR)
    devices = _parse_npu_devices()
    try:
        _resolve_protocol(args)
        params = build_params(args, devices)
    except ValueError as error:
        LOGGER.error(
            "invalid benchmark configuration: %s; direction=%s block_size=%s batch_size=%s ip=%s group=%s "
            "device=%s visible=%s transfer_size=%s",
            error,
            args.direction,
            args.block_size,
            args.batch_size,
            args.ip,
            args.group,
            args.device,
            devices,
            getattr(args, "transfer_size", None),
        )
        return 1
    if params.backend == "trans":
        from .trans.pairwise import run_rank
    else:
        from .bm.pairwise import run_rank

    return BenchmarkRunner(params, run_rank).run()
