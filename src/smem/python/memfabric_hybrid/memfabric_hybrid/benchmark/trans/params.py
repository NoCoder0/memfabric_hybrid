# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Trans-specific endpoint and descriptor validation, without native imports."""

import dataclasses
import ipaddress
import socket


def validate_params(params):
    if params.protocol not in ("sdma", "device_rdma", "device_urma", "host_rdma"):
        raise ValueError(f"Trans does not expose protocol {params.protocol}")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            if params.local_ip:
                host = socket.gethostbyname(params.local_ip)
            else:
                probe.connect((params.sync_host, params.sync_port))
                host = probe.getsockname()[0]
            if ipaddress.ip_address(host).is_unspecified:
                raise ValueError("--local-ip must be a reachable local address, not 0.0.0.0")
            if params.local_ip:
                probe.bind((host, 0))
    except OSError as error:
        raise ValueError(f"cannot select Trans local endpoint: {error}; set --local-ip") from error
    return dataclasses.replace(params, local_ip=host)


def peer_buffer(metadata, mem, length):
    """Validate an exchanged buffer before constructing any native copy descriptors."""
    endpoint = metadata["endpoint"]
    host, port = endpoint.rsplit(":", 1)
    if ipaddress.IPv4Address(host).is_unspecified or not 0 < int(port) < 65536:
        raise ValueError(f"invalid Trans peer endpoint {endpoint!r}")
    address, capacity = metadata["buffers"][mem]
    if not isinstance(address, int) or not isinstance(capacity, int) or address <= 0 or not 0 < length <= capacity:
        raise ValueError(f"invalid Trans peer buffer: endpoint={endpoint} mem={mem} size={length} capacity={capacity}")
    return endpoint, address
