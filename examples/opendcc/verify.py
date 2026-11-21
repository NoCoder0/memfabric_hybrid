#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import argparse
import hashlib
import json
import re
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(chunk)
    return sha.hexdigest()


def verify_process(spec):
    pid = spec["pid"]
    proc = Path(f"/proc/{pid}")
    maps = proc.joinpath("maps").read_text().splitlines()
    paths = {line.split(maxsplit=5)[5] for line in maps if len(line.split(maxsplit=5)) == 6}
    libraries = spec["libraries"]
    require({"libmf_smem.so", "libmf_hybm_core.so", "libmf_opendcc.so"} <= libraries.keys(), "Missing MF hashes")
    require(any("memcache" in name for name in libraries), "Include the loaded MemCache library hash")
    loaded = {}
    for name, expected in libraries.items():
        matches = [path for path in paths if Path(path).name == name]
        require(len(matches) == 1, f"pid={pid}: expected one loaded {name}, found {matches}")
        path = matches[0]
        actual = digest(proc / "root" / path.lstrip("/"))
        require(actual == expected, f"pid={pid}: hash mismatch for {path}: {actual}")
        loaded[name] = {"path": path, "sha256": actual}
    env = dict(item.split(b"=", 1) for item in proc.joinpath("environ").read_bytes().split(b"\0") if b"=" in item)
    for name, value in {
        "MF_HYBM_RDMA_USE_OPENDCC": "1",
        "MF_HYBM_RDMA_USE_HCOMM": "0",
        "MF_HYBM_RDMA_FORCE_UNREGISTERED": "0",
        "MF_HYBM_RDMA_SWAP_SPACE_SIZE": "0",
    }.items():
        require(env.get(name.encode()) == value.encode(), f"pid={pid}: {name} must be {value}")
    text = Path(spec["log"]).read_text(errors="replace")
    own = "\n".join(line for line in text.splitlines() if f"[{pid}-" in line)
    provider = 3 if spec["role"] == "device" else 1
    require(
        f"openDCC transport rank={spec['rank']} provider={provider} ret=0" in own,
        f"pid={pid}: missing successful manager/provider selection",
    )
    require(
        not any(" ERROR " in line and ("[openDCC " in line or "[HYBM " in line) for line in own.splitlines()),
        f"pid={pid}: transport errors in evidence log",
    )
    result = {"pid": pid, "rank": spec["rank"], "role": spec["role"], "libraries": loaded}
    if spec["role"] == "host":
        return result
    require(spec["role"] == "device", "Role must be device or host")
    kv = [
        (int(a, 16), int(n))
        for a, n in re.findall(rf"openDCC KV registered pid={pid} device=\d+ address=(0x[0-9a-f]+) size=(\d+)", text)
    ]
    require(kv, f"pid={pid}: no successful business KV registration records")
    mrs = [
        (int(a, 16), int(n))
        for a, n in re.findall(rf"openDCC MR rank={spec['rank']} type=2 address=(0x[0-9a-f]+) size=(\d+)", own)
    ]
    require(
        all(any(a >= m and a + n <= m + size for m, size in mrs) for a, n in kv),
        f"pid={pid}: business KV is not covered by HBM MRs",
    )
    completed = {0: 0, 1: 0}
    peers = set()
    for op, peer, address, size in re.findall(
        r"openDCC completed op=([01]) rank=(\d+) local=(0x[0-9a-f]+) remote=0x[0-9a-f]+ size=(\d+)", own
    ):
        address, size = int(address, 16), int(size)
        require(
            any(address >= base and address + size <= base + length for base, length in kv),
            f"pid={pid}: completed DMA outside business KV HBM, address={address:#x} size={size}",
        )
        completed[int(op)] += size
        peers.add(int(peer))
    require(all(completed.values()), f"pid={pid}: require completed business KV READ and WRITE")
    require(set(spec["peers"]) <= peers, f"pid={pid}: missing expected business peers")
    result.update(read_bytes=completed[0], write_bytes=completed[1], peers=sorted(peers), kv=kv)
    return result


def main():
    parser = argparse.ArgumentParser(description="Verify live openDCC workers and recorded KV DMA evidence")
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    require(manifest["processes"], "No processes in manifest")
    report = {"processes": [verify_process(spec) for spec in manifest["processes"]]}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print("Runtime and recorded KV DMA checks passed; model correctness and performance require benchmark results.")


if __name__ == "__main__":
    main()
