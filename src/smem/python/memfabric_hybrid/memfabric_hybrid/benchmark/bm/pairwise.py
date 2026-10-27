# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Pairwise phases: initiator ranks copy to the matching rank of the other group, targets only serve slots."""

import logging
import os
import sys
import time

import torch

from memfabric_hybrid import bm

from .data import generate_data, wrong_bytes
from .rank import BmRank, require_ok
from .sync import BenchmarkSync, PeerFailedError

LOGGER = logging.getLogger(__name__)
_POISON = -(2**31)
_COPY_TYPE = {
    ("write", "device"): bm.BmCopyType.L2G,
    ("write", "host"): bm.BmCopyType.H2G,
    ("read", "device"): bm.BmCopyType.G2L,
    ("read", "host"): bm.BmCopyType.G2H,
}


class PairwiseBenchmark:
    """All phases of one rank. Every rank walks the same barriers; only initiators submit copies."""

    def __init__(self, rank, sync, params):
        self.rank, self.sync, self.params = rank, sync, params
        self.rank_id = rank.rank_id
        self.peer = params.peer_of(self.rank_id)

    def run(self):
        results = []
        for phase in self.params.phases:
            for size in self.params.block_sizes:
                try:
                    row = self._run_case(phase, size)
                except PeerFailedError:
                    raise
                except Exception as error:
                    raise RuntimeError(f"direction={phase.name} block_size={size} peer={self.peer}: {error}") from error
                if row is not None:
                    results.append(row)
        return results

    def finish(self):
        """Finish local work before any rank exits; native handles live until process termination."""
        self.rank.unregister_buffers()
        self.sync.barrier("finished")

    def _run_case(self, phase, size):
        initiator = self.params.group_of(self.rank_id) == phase.initiator_group
        stage = f"{phase.name}/{size}"
        case = _Case(phase, size, self.params.batch_for(size), self.rank, self.peer)
        if initiator:
            case.prepare_initiator()
        elif phase.op == "read":
            case.seed_own_slot()
        self.sync.barrier(f"{stage}/prepared")
        if initiator:
            case.warmup(self.params.warmup_count)
        self.sync.barrier(f"{stage}/start")
        row = case.measure(self.params.copy_count) if initiator else None
        self.sync.barrier(f"{stage}/copied")
        if initiator:
            case.verify()
        # No rank may overwrite a slot for the next case before verification finished.
        self.sync.barrier(f"{stage}/verified")
        return row


class _Case:
    """Addresses and data of one (direction, block size) on one rank; holds no native resource."""

    def __init__(self, phase, size, batch, rank, peer):
        self.phase, self.size, self.batch, self.rank, self.peer = phase, size, batch, rank, peer
        self.length = size * batch
        self.remote = None
        self.copy_args = None
        self._expected = None

    @property
    def expected(self):
        """Pattern of the group 0 rank of this pair: it is written by group 0 and read back by group 1."""
        if self._expected is None:
            rank_id = self.rank.rank_id
            source_rank = rank_id if self.rank.params.group_of(rank_id) == 0 else self.peer
            self._expected = torch.zeros(self.length // 4, dtype=torch.int32)
            generate_data(self._expected, source_rank)
        return self._expected

    @property
    def local(self):
        return self.rank.buffer(self.phase.local_mem)[: self.length // 4]

    def prepare_initiator(self):
        self.remote = self.rank.slot(self.peer, self.phase.peer_mem)
        writer = self.phase.op == "write"
        local_ptr = self.local.data_ptr()
        source, destination = (local_ptr, self.remote) if writer else (self.remote, local_ptr)
        offsets = [index * self.size for index in range(self.batch)]
        self.copy_args = {
            "src_addrs": [source + offset for offset in offsets],
            "dst_addrs": [destination + offset for offset in offsets],
            "sizes": [self.size] * self.batch,
            "count": self.batch,
            "type": _COPY_TYPE[(self.phase.op, self.phase.local_mem)],
            "flags": 0,
        }
        if writer:
            self._fill_local(self.expected)
        else:
            self._fill_local(_POISON)

    def seed_own_slot(self):
        """Read-phase target: place the known pattern in its own slot with an untimed local copy."""
        staging = self.rank.buffer("device")[: self.length // 4]
        staging.copy_(self.expected)
        torch.npu.synchronize()
        own = self.rank.slot(self.rank.rank_id, self.phase.peer_mem)
        ret = self.rank.handle.copy_data(staging.data_ptr(), own, self.length, bm.BmCopyType.L2G, 0)
        require_ok(ret, f"seed own slot rank={self.rank.rank_id} length={self.length}")

    def warmup(self, count):
        for _ in range(count):
            require_ok(self.rank.handle.copy_data_batch(**self.copy_args), "warmup copy_data_batch")
        # A measured no-op copy must not pass verification with warmup data.
        if self.phase.op == "write":
            self._fill_local(_POISON)
            copy_type = _COPY_TYPE[("write", self.phase.local_mem)]
            ret = self.rank.handle.copy_data(self.local.data_ptr(), self.remote, self.length, copy_type, 0)
            require_ok(ret, "poison target slot")
            self._fill_local(self.expected)
        else:
            self._fill_local(_POISON)

    def measure(self, count):
        copy, args = self.rank.handle.copy_data_batch, self.copy_args
        # flags=0 and no external stream use BM's synchronous native copy path.
        # Native stream/transport completion is included in each call below.
        start_ns = time.monotonic_ns()
        for _ in range(count):
            ret = copy(**args)
            if ret != 0:
                require_ok(ret, f"copy_data_batch block_size={self.size} batch={self.batch}")
        end_ns = time.monotonic_ns()
        elapsed_ns = max(1, end_ns - start_ns)
        total_bytes = self.length * count
        return {
            "direction": self.phase.name,
            "rank_id": self.rank.rank_id,
            "device_id": self.rank.device_id,
            "peer_rank_id": self.peer,
            "copy_size": self.size,
            "batch_size": self.batch,
            "copy_count": count,
            "total_bytes": total_bytes,
            "total_time_us": elapsed_ns / 1000,
            "bandwidth_gbps": total_bytes / elapsed_ns,
            "start_ns": start_ns,
            "end_ns": end_ns,
        }

    def verify(self):
        if self.phase.op == "write":
            self._fill_local(_POISON)
            read_back = _COPY_TYPE[("read", self.phase.local_mem)]
            ret = self.rank.handle.copy_data(self.remote, self.local.data_ptr(), self.length, read_back, 0)
            require_ok(ret, "verify read back")
        wrong = wrong_bytes(self.expected, self.local.cpu())
        if wrong:
            raise RuntimeError(f"data verification failed: rank={self.rank.rank_id} wrong_bytes={wrong}")

    def _fill_local(self, value):
        local = self.local
        if isinstance(value, torch.Tensor):
            local.copy_(value)
        else:
            local.fill_(value)
        torch.npu.synchronize()


def _configure_worker_logging(show_logs):
    logging.basicConfig(format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    logging.getLogger().setLevel(logging.DEBUG if show_logs else logging.ERROR)


def run_rank(params, rank_id, local_index, queue):
    """Own native resources until the child exits, after flushing its report to the parent."""
    _configure_worker_logging(params.log)
    sync = None
    exit_code = 1
    try:
        sync = BenchmarkSync(params, rank_id)
        sync.agree(params.signature())
        rank = BmRank(params, rank_id, local_index)
        rank.initialize()
        bench = PairwiseBenchmark(rank, sync, params)
        sync.publish(bench.run())
        sync.barrier("results published")
        rows = _rows_to_report(params, rank_id, sync)
        bench.finish()
        queue.put({"rank_id": rank_id, "results": rows})
        exit_code = 0
    except Exception as error:  # noqa: BLE001 - report every worker failure to the parent
        LOGGER.error(
            "benchmark rank failed: rank=%s device=%s error=%s", rank_id, params.device_indices[local_index], error
        )
        queue.put({"rank_id": rank_id, "error": str(error)})
        if sync is not None:
            if not isinstance(error, PeerFailedError):
                sync.fail(str(error))
            sync.finish_failure()
    finally:
        # Leave ACKs and Python barriers cannot drain native REMOVE callbacks.
        # Returning would destroy pybind handles while those callbacks still use
        # them. Flush IPC first, then let process exit reclaim native resources
        # without running handle/interpreter destructors, including on failure.
        try:
            queue.close()
            queue.join_thread()
            sys.stdout.flush()
            sys.stderr.flush()
        except BaseException as error:  # noqa: BLE001 - the worker must exit even if flushing fails
            exit_code = 1
            LOGGER.error("failed to flush rank report: rank=%s error=%s", rank_id, error)
        finally:
            os._exit(exit_code)


def _rows_to_report(params, rank_id, sync):
    # Rank 0 reports both groups; in cross-machine mode the group 1 command reports only its own rows.
    if rank_id == 0:
        return sync.gather()
    if params.group == 1:
        return [row for row in sync.gather() if row["rank_id"] == rank_id]
    return []
