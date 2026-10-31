# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Immutable benchmark parameters and the phase plan derived from CLI arguments."""

import dataclasses
import errno
import ipaddress
import re
import socket
from dataclasses import dataclass
from urllib.parse import urlsplit

_KIB = 1024
_GIB = 1024**3
_ALIGNMENT = 4096
_MAX_BLOCK_SIZE = _GIB
_DEFAULT_TRANSFER_SIZE = 128 * 1024**2
_MAX_PAYLOAD_SIZE = 20 * _GIB
_SYNC_TIMEOUT_SEC = 3600.0
_UNITS = {"": 1, "B": 1, "K": _KIB, "M": _KIB**2, "G": _GIB}
_MEM_CODE = {"D": "device", "H": "host"}
_WRITE_PAIRS = (("device", "device"), ("device", "host"), ("host", "device"), ("host", "host"))


@dataclass(frozen=True)
class Phase:
    """One timed direction: every initiator rank copies between its buffer and the peer rank's pool slot."""

    op: str  # "write" or "read"
    local_mem: str  # initiator buffer: "device" or "host"
    peer_mem: str  # target pool slot: "device" (HBM) or "host" (DRAM)

    @property
    def initiator_group(self):
        # Group 0 writes into group 1; group 1 reads back from group 0.
        return 0 if self.op == "write" else 1

    @property
    def name(self):
        local, peer = self.local_mem[0].upper(), self.peer_mem[0].upper()
        return f"{local}2r{peer}" if self.op == "write" else f"r{peer}2{local}"


ALL_PHASES = tuple(Phase(op, local, peer) for op in ("write", "read") for local, peer in _WRITE_PAIRS)


@dataclass(frozen=True)
class BenchmarkParams:
    protocol: str
    store_url: str
    nic: str
    world_size: int
    local_rank_size: int
    rank_start: int
    group: int  # -1: one command holds both groups
    device_indices: tuple  # logical ids (under ASCEND_RT_VISIBLE_DEVICES) of the cards this command uses, rank order
    devices: tuple  # physical ids of the same cards, for reports
    idle_devices: tuple  # visible cards left unused (odd count on one machine)
    block_sizes: tuple
    batch_size: int | None  # None: derive the batch from the default transfer size
    copy_count: int
    warmup_count: int
    local_hbm_size: int
    sync_host: str
    sync_port: int
    sync_timeout: float
    phases: tuple
    log: bool
    backend: str = "bm"
    local_ip: str = ""
    transfer_size: int = _DEFAULT_TRANSFER_SIZE

    @property
    def group_size(self):
        return self.world_size // 2

    @property
    def payload_size(self):
        return max(size * self.batch_for(size) for size in self.block_sizes)

    def group_of(self, rank):
        return rank // self.group_size

    def peer_of(self, rank):
        return (rank + self.group_size) % self.world_size

    def batch_for(self, block_size):
        if self.batch_size is not None:
            return self.batch_size
        return max(1, self.transfer_size // block_size)

    def signature(self):
        """Fields both commands must agree on before joining BM."""
        fields = dataclasses.asdict(self)
        for key in ("rank_start", "group", "device_indices", "devices", "idle_devices", "log", "nic", "local_ip"):
            fields.pop(key)
        fields["phases"] = [phase.name for phase in self.phases]
        return fields


def parse_size(text):
    found = re.fullmatch(r"\s*(\d+)\s*([KMG]?)B?\s*", text.upper())
    if not found:
        raise ValueError(f"invalid size {text!r}; use bytes or a K/M/G suffix, for example 64K or 2M")
    return int(found.group(1)) * _UNITS[found.group(2)]


def parse_block_sizes(text):
    """Parse `2M`, `4K,64K,1M` or the doubling range `16K:2M`."""
    if ":" in text:
        first, last = (parse_size(part) for part in text.split(":", 1))
        if first <= 0 or first > last:
            raise ValueError(f"invalid block size range {text!r}; the start must be positive and not above the end")
        sizes = []
        while first <= last:
            sizes.append(first)
            first *= 2
    else:
        sizes = [parse_size(part) for part in text.split(",") if part.strip()]
    if not sizes:
        raise ValueError("--block-size is empty")
    for size in sizes:
        if size <= 0 or size % _ALIGNMENT or size > _MAX_BLOCK_SIZE:
            raise ValueError(f"block size {size} must be a positive multiple of 4096 bytes and at most 1G")
    return tuple(sorted(set(sizes)))


def parse_device_indices(text, visible_count):
    """Parse `2,3` or `0-3,6` into logical device indices in the given order; None selects every visible card."""
    if text is None:
        return tuple(range(visible_count))
    indices = []
    for token in (part.strip() for part in text.split(",") if part.strip()):
        found = re.fullmatch(r"(\d+)(?:-(\d+))?", token)
        if not found:
            raise ValueError(f"invalid --device item {token!r}; use ids such as 0,1 or ranges such as 0-3")
        first = int(found.group(1))
        last = int(found.group(2)) if found.group(2) is not None else first
        if first > last:
            raise ValueError(f"invalid --device range {token!r}; the start must not be above the end")
        indices.extend(range(first, last + 1))
    if not indices:
        raise ValueError("--device is empty")
    duplicates = sorted({index for index in indices if indices.count(index) > 1})
    if duplicates:
        raise ValueError(f"--device repeats {duplicates}")
    outside = [index for index in indices if index >= visible_count]
    if outside:
        raise ValueError(
            f"--device {outside} out of range; {visible_count} visible NPU(s) have logical ids 0-{visible_count - 1}"
        )
    return tuple(indices)


def parse_directions(text):
    """Parse `all`, `write`, `read` or a comma list such as `D2rD,rD2D` (case-insensitive)."""
    selected = []
    by_name = {phase.name.upper(): phase for phase in ALL_PHASES}
    for token in (part.strip().upper() for part in text.split(",") if part.strip()):
        if token == "ALL":
            matched = ALL_PHASES
        elif token in ("WRITE", "READ"):
            matched = tuple(phase for phase in ALL_PHASES if phase.op == token.lower())
        elif token in by_name:
            matched = (by_name[token],)
        else:
            names = ", ".join(phase.name for phase in ALL_PHASES)
            raise ValueError(f"unknown direction {token!r}; use all, write, read or a list of: {names}")
        selected.extend(phase for phase in matched if phase not in selected)
    if not selected:
        raise ValueError("--direction is empty")
    # Plan order stays fixed: all writes first, then all reads.
    return tuple(phase for phase in ALL_PHASES if phase in selected)


def resolve_group(explicit, host):
    """Return the cross-machine group of this command, or None when one machine runs both groups.

    Without --group, a loopback --ip keeps the single-machine layout; any other --ip is a cross-machine run
    where the machine that owns the --ip address is group 0 (it hosts the config store) and the other is group 1.
    """
    if explicit is not None:
        return explicit
    address = _resolve_ipv4(host)
    if ipaddress.ip_address(address).is_loopback:
        return None
    return 0 if _is_local_address(address) else 1


def _resolve_ipv4(host):
    try:
        return socket.getaddrinfo(host, None, socket.AF_INET)[0][4][0]
    except (OSError, IndexError) as error:
        raise ValueError(f"cannot resolve --ip host {host!r} ({error}); pass --group 0 or 1 explicitly") from error


def _is_local_address(address):
    # Binding succeeds only for an address that one of this machine's interfaces owns.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.bind((address, 0))
        except OSError as error:
            if error.errno == errno.EADDRNOTAVAIL:
                return False
            raise ValueError(
                f"cannot tell whether --ip {address} is local ({error}); pass --group 0 or 1 explicitly"
            ) from error
    return True


def build_params(args, visible):
    """Validate the layout and return immutable worker parameters; raises ValueError.

    `visible` lists the physical ids of the visible NPUs; logical id i is visible[i].
    """
    endpoint = urlsplit(args.ip)
    if endpoint.scheme != "tcp" or endpoint.hostname in (None, "0.0.0.0") or endpoint.port is None:
        raise ValueError("--ip must be tcp://<group 0 host>:<port> with a reachable host")
    if not 0 < endpoint.port < 65535:
        raise ValueError("--ip port must leave port + 1 free for benchmark synchronization")
    indices = parse_device_indices(args.device, len(visible))
    group = resolve_group(args.group, endpoint.hostname)
    cross = group is not None
    local = len(indices) if cross else len(indices) // 2 * 2
    if local < (1 if cross else 2):
        raise ValueError(f"not enough NPUs: selected={list(indices)}; one machine needs at least 2")
    devices = [visible[index] for index in indices]
    backend = getattr(args, "backend", "bm")
    phases = parse_directions(args.direction or ("D2rD,D2rH,rD2D,rH2D" if backend == "trans" else "all"))
    block_sizes = parse_block_sizes(args.block_size or ("16K:8M" if backend == "trans" else "16K:2M"))
    transfer_text = getattr(args, "transfer_size", None)
    if transfer_text is not None and args.batch_size is not None:
        raise ValueError("--transfer-size and --batch-size are mutually exclusive")
    transfer_size = parse_size(transfer_text) if transfer_text is not None else _DEFAULT_TRANSFER_SIZE
    limit = _MAX_PAYLOAD_SIZE
    if not 0 < transfer_size <= limit:
        raise ValueError(f"--transfer-size must be positive and at most {limit} bytes for {backend}")
    if args.batch_size is not None and args.batch_size <= 0:
        raise ValueError("--batch-size must be positive")
    params = BenchmarkParams(
        protocol=args.protocol,
        store_url=args.ip,
        nic=args.nic,
        world_size=2 * local if cross else local,
        local_rank_size=local,
        rank_start=group * local if cross else 0,
        group=group if cross else -1,
        device_indices=indices[:local],
        devices=tuple(devices[:local]),
        idle_devices=tuple(devices[local:]),
        block_sizes=block_sizes,
        batch_size=args.batch_size,
        copy_count=args.copy_count,
        warmup_count=args.warmup_count,
        local_hbm_size=0,
        sync_host=endpoint.hostname,
        sync_port=endpoint.port + 1,
        sync_timeout=_SYNC_TIMEOUT_SEC,
        phases=phases,
        log=args.log,
        backend=backend,
        local_ip=getattr(args, "local_ip", None) or "",
        transfer_size=transfer_size,
    )
    if params.backend == "trans":
        from ..trans.params import validate_params

        params = validate_params(params)
    else:
        if params.local_ip:
            raise ValueError("--local-ip is only supported with --backend trans")
        params = dataclasses.replace(params, local_hbm_size=_hbm_size(params))
    _validate(params)
    return params


def _hbm_size(params):
    if not any(phase.peer_mem == "device" for phase in params.phases):
        return 0
    # HBM pool slots must hold the largest case; round up to whole GiB.
    return max(_GIB, -(-params.payload_size // _GIB) * _GIB)


def _validate(params):
    if params.copy_count <= 0 or params.warmup_count < 0:
        raise ValueError("--copy-count must be positive and --warmup-count must be nonnegative")
    limit = _MAX_PAYLOAD_SIZE
    if params.payload_size > limit:
        raise ValueError(
            f"block size x batch size {params.payload_size} exceeds the {limit} byte limit for {params.backend}"
        )
    if params.protocol.startswith("host_") and any(phase.peer_mem == "device" for phase in params.phases):
        raise ValueError(f"protocol {params.protocol} cannot target HBM pool slots; select only *2rH/rH2* directions")
