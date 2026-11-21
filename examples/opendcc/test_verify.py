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

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import verify


class VerifyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        proc = self.root / "proc/123"
        proc.mkdir(parents=True)
        libraries = [
            "libmf_smem.so",
            "libmf_hybm_core.so",
            "libmf_opendcc.so",
            "libmemcache.so",
        ]
        (proc / "maps").write_text("\n".join(f"0 0 0 0 0 /lib/{name}" for name in libraries))
        (proc / "environ").write_bytes(
            b"MF_HYBM_RDMA_USE_OPENDCC=1\0MF_HYBM_RDMA_USE_HCOMM=0\0"
            b"MF_HYBM_RDMA_FORCE_UNREGISTERED=0\0MF_HYBM_RDMA_SWAP_SPACE_SIZE=0\0"
        )
        self.spec = {
            "pid": 123,
            "rank": 0,
            "role": "device",
            "peers": [16],
            "log": str(self.root / "device.log"),
            "libraries": dict.fromkeys(libraries, "hash"),
        }
        self.hosts = []
        for rank in (16, 17):
            log = self.root / f"host-{rank}.log"
            log.write_text(
                f"[456-1] openDCC transport rank={rank} provider=1 ret=0\n"
                f"[456-1] openDCC MR rank={rank} type=1 address=0x2000 size=256\n"
            )
            self.hosts.append({"pid": 456, "rank": rank, "log": str(log)})
        self.addCleanup(patch.stopall)
        patch.object(verify, "digest", return_value="hash").start()
        patch.object(
            verify,
            "Path",
            side_effect=lambda value: self.root / str(value).lstrip("/")
            if str(value).startswith("/proc/")
            else Path(value),
        ).start()

    def evidence(self, operations):
        text = (
            "[123-1] openDCC transport rank=0 provider=3 ret=0\n"
            "[123-1] openDCC KV registered pid=123 device=0 address=0x1000 size=256\n"
            "[123-1] openDCC MR rank=0 type=2 address=0x1000 size=256\n"
        )
        for op, rank, remote in operations:
            text += f"[123-1] openDCC completed op={op} rank={rank} local=0x1000 remote={remote:#x} size=16\n"
        Path(self.spec["log"]).write_text(text)

    def test_each_host_has_read_and_write(self):
        self.spec["peers"] = [16, 17]
        self.evidence([(op, rank, 0x2000) for rank in (16, 17) for op in (0, 1)])
        result = verify.verify_process(self.spec, self.hosts)
        self.assertEqual(result["peer_bytes"], {16: {0: 16, 1: 16}, 17: {0: 16, 1: 16}})

    def test_npu_read_does_not_cover_host_read(self):
        self.evidence([(0, 1, 0x3000), (1, 16, 0x2000)])
        with self.assertRaisesRegex(SystemExit, "READ and WRITE with Host rank=16"):
            verify.verify_process(self.spec, self.hosts)

    def test_other_host_read_does_not_cover_target(self):
        self.spec["peers"] = [16, 17]
        self.evidence([(0, 16, 0x2000), (1, 16, 0x2000), (1, 17, 0x2000)])
        with self.assertRaisesRegex(SystemExit, "READ and WRITE with Host rank=17"):
            verify.verify_process(self.spec, self.hosts)

    def test_remote_address_requires_host_mr(self):
        self.evidence([(0, 16, 0x3000), (1, 16, 0x2000)])
        with self.assertRaisesRegex(SystemExit, "outside Host DRAM MR"):
            verify.verify_process(self.spec, self.hosts)

    def test_host_evidence_is_required(self):
        self.evidence([(0, 16, 0x2000), (1, 16, 0x2000)])
        with self.assertRaisesRegex(SystemExit, "missing target Host DRAM MR logs"):
            verify.verify_process(self.spec)


if __name__ == "__main__":
    unittest.main()
