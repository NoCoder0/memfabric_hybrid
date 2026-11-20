# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

"""Static configuration for TRANS integration tests."""

import math

PROTOCOLS = ("device_rdma", "sdma", "device_urma")
CHIP_PROTOCOL = {"A2": "device_rdma", "A3": "sdma", "A5": "device_urma"}

DEFAULTS = {
    "protocol": None,
    "devices": None,
    "setup_timeout": 180.0,
    "case_timeout": 120.0,
    "cleanup_timeout": 60.0,
}


def validate_config(config):
    devices = config["devices"]
    if devices is not None and (len(devices) != 2 or len(set(devices)) != 2 or min(devices) < 0):
        raise ValueError("TRANS requires two distinct non-negative physical device IDs")
    if config["protocol"] is not None and config["protocol"] not in PROTOCOLS:
        raise ValueError(f"Unsupported TRANS protocol: {config['protocol']}")
    for name in ("setup_timeout", "case_timeout", "cleanup_timeout"):
        value = config[name]
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"{name} must be finite and positive")
