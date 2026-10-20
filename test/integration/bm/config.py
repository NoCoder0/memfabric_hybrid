#!/usr/bin/env python
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import math
import sys

GIB = 1 << 30
TIB = 1 << 40
CHIP_PROTOCOL = {"A2": "device_rdma", "A3": "device_sdma", "A5": "device_urma"}
PROTOCOL_DATA_OP_TYPE = {
    "device_rdma": "DEVICE_RDMA",
    "device_sdma": "SDMA",
    "device_urma": "DEVICE_URMA",
}
SUPPORTED = [
    {"protocol": "device_rdma", "pool": "dram", "chips": ["A2"], "implemented": True},
    {"protocol": "device_sdma", "pool": "dram", "chips": [], "implemented": False},
    {"protocol": "device_urma", "pool": "dram", "chips": [], "implemented": False},
]
COMMON = {
    "protocol": "",
    "pool": "dram",
    "chips": [],
    "world_size": 256,
    "local_dram_size": GIB,
    "max_dram_size": TIB,
    "local_hbm_size": 0,
    "max_hbm_size": 0,
    "enable_56bits_gva": True,
    "devices": [0, 1],
    "store_url": "tcp://127.0.0.1:29300",
    "nic": "tcp://127.0.0.1:10005",
    "setup_timeout": 180.0,
    "case_timeout": 120.0,
    "cleanup_timeout": 60.0,
}


def validate_config(config):
    if config["world_size"] < 2 or len(config["devices"]) != 2 or len(set(config["devices"])) != 2:
        raise ValueError("Two distinct devices and world_size >= 2 are required")
    if any(device < 0 for device in config["devices"]):
        raise ValueError("Device IDs must be non-negative")
    for kind in ("dram", "hbm"):
        if not 0 <= config[f"local_{kind}_size"] <= config[f"max_{kind}_size"]:
            raise ValueError(f"Invalid {kind} capacity")
    capacity = (config["max_dram_size"] + config["max_hbm_size"]) * config["world_size"]
    if capacity > 32 * TIB and not config["enable_56bits_gva"]:
        raise ValueError("Capacity above 32 TiB requires enable_56bits_gva=True")
    if any(
        not math.isfinite(config[key]) or config[key] <= 0
        for key in ("setup_timeout", "case_timeout", "cleanup_timeout")
    ):
        raise ValueError("Timeouts must be finite positive seconds")


def skip_reason(config, chip):
    row = next((r for r in SUPPORTED if r["protocol"] == config["protocol"] and r["pool"] == config["pool"]), None)
    if row is None or not row["implemented"]:
        return "This protocol/pool test is not implemented yet"
    # chips 为空表示不限制，仅非空列表参与匹配。
    if row["chips"] and chip not in row["chips"]:
        return f"The current test is implemented only for chips {row['chips']}; selected chip={chip or 'unspecified'}"
    if config["chips"] and chip not in config["chips"]:
        return (
            f"The current test is implemented only for chips {config['chips']}; selected chip={chip or 'unspecified'}"
        )
    if sys.platform != "linux":
        return "This device integration test requires a Linux Ascend environment"
    return None
