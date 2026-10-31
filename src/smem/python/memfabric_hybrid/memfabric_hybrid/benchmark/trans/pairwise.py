# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Trans synchronous one-to-one reads/writes using application-owned buffers."""

import logging

import torch

from ..common.data import generate_data, wrong_bytes
from ..common.native import configure_worker_logging, require_ok
from ..common.pairwise import PairwiseBenchmark, rows_to_report
from ..common.sync import BenchmarkSync, PeerFailedError
from .params import peer_buffer
from .rank import TransRank

LOGGER = logging.getLogger(__name__)
_POISON = -(2**31)
_VERIFY_CHUNK_BYTES = 16 * 1024**2


class TransCase:
    def __init__(self, phase, size, batch, rank, peer):
        self.phase, self.size, self.batch, self.rank, self.peer = phase, size, batch, rank, peer
        self.length = size * batch
        source_rank = rank.rank_id if rank.params.group_of(rank.rank_id) == 0 else peer
        # Bound host scratch space even when a registered buffer is tens of GiB.
        self.expected = torch.zeros(min(self.length, _VERIFY_CHUNK_BYTES) // 4, dtype=torch.int32)
        generate_data(self.expected, source_rank)

    @property
    def local(self):
        return self.rank.buffer(self.phase.local_mem)[: self.length // 4]

    @property
    def copy(self):
        return getattr(self.rank.engine, f"batch_transfer_sync_{self.phase.op}")

    def prepare_initiator(self):
        endpoint, address = peer_buffer(self.rank.peer, self.phase.peer_mem, self.length)
        local_address = self.local.data_ptr()
        offsets = range(0, self.length, self.size)
        self.copy_args = {
            "dest_session": endpoint,
            "buffers": [local_address + offset for offset in offsets],
            "peer_buffers": [address + offset for offset in offsets],
            "lengths": [self.size] * self.batch,
        }
        self._fill_local(self.expected if self.phase.op == "write" else _POISON)

    def seed_own_slot(self):
        # This is an application buffer, not a BM pool: the owner can fill it directly.
        self._fill_expected(self.rank.buffer(self.phase.peer_mem)[: self.length // 4])
        torch.npu.synchronize()

    def connect(self):
        # First copy lazily connects. Keep it outside timing even with warmup_count=0.
        require_ok(self.copy(**self.copy_args), "Trans prepare connection")

    def warmup(self, count):
        for _ in range(count):
            require_ok(self.copy(**self.copy_args), "Trans warmup")
        self._fill_local(_POISON)
        if self.phase.op == "write":
            require_ok(self.copy(**self.copy_args), "Trans poison target")
            self._fill_local(self.expected)

    def verify(self):
        if self.phase.op == "write":
            self._fill_local(_POISON)
            require_ok(self.rank.engine.batch_transfer_sync_read(**self.copy_args), "Trans verify read back")
        wrong = 0
        for offset in range(0, len(self.local), len(self.expected)):
            chunk = self.local[offset : offset + len(self.expected)].cpu()
            wrong += wrong_bytes(self.expected[: len(chunk)], chunk)
        if wrong:
            raise RuntimeError(f"Trans verification failed: rank={self.rank.rank_id} wrong_bytes={wrong}")

    def _fill_local(self, value):
        if isinstance(value, torch.Tensor):
            self._fill_expected(self.local)
        else:
            self.local.fill_(value)
        torch.npu.synchronize()

    def _fill_expected(self, target):
        for offset in range(0, len(target), len(self.expected)):
            chunk = target[offset : offset + len(self.expected)]
            chunk.copy_(self.expected[: len(chunk)])


def run_rank(params, rank_id, local_index, queue):
    configure_worker_logging(params.log)
    rank, sync = TransRank(params, rank_id, local_index), None
    failed = False
    try:
        sync = BenchmarkSync(params, rank_id)
        sync.agree(params.signature())
        rank.initialize(sync)
        bench = PairwiseBenchmark(rank, sync, params, TransCase)
        sync.publish(bench.run())
        sync.barrier("results published")
        rows = rows_to_report(params, rank_id, sync)
        sync.barrier("finished")
        rank.close()
        sync.barrier("engines closed")
        queue.put({"rank_id": rank_id, "results": rows})
    except Exception as error:  # noqa: BLE001 - report native/worker failures to the parent
        failed = True
        LOGGER.error("Trans rank failed: rank=%s protocol=%s error=%s", rank_id, params.protocol, error)
        queue.put({"rank_id": rank_id, "error": str(error)})
        if sync is not None:
            if not isinstance(error, PeerFailedError):
                sync.fail(str(error))
            sync.finish_failure()
    finally:
        try:
            rank.close()
        except Exception as error:  # noqa: BLE001 - retain buffers on failed teardown
            failed = True
            LOGGER.error("Trans cleanup failed: rank=%s error=%s", rank_id, error)
            queue.put({"rank_id": rank_id, "error": str(error)})
        queue.close()
        queue.join_thread()
    if failed:
        raise SystemExit(1)
