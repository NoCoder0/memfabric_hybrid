# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Shared pairwise stages and synchronous measurement; resources belong to each backend."""

import time

from .native import require_ok
from .sync import PeerFailedError


class PairwiseBenchmark:
    """All phases of one rank. Every rank walks the same barriers; only initiators submit copies."""

    def __init__(self, rank, sync, params, case_type):
        self.case_type = case_type
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

    def _run_case(self, phase, size):
        initiator = self.params.group_of(self.rank_id) == phase.initiator_group
        stage = f"{phase.name}/{size}"
        case = self.case_type(phase, size, self.params.batch_for(size), self.rank, self.peer)
        if initiator:
            case.prepare_initiator()
        elif phase.op == "read":
            case.seed_own_slot()
        self.sync.barrier(f"{stage}/prepared")
        if initiator:
            case.connect()
            case.warmup(self.params.warmup_count)
        self.sync.barrier(f"{stage}/start")
        row = measure(case, self.params.copy_count) if initiator else None
        self.sync.barrier(f"{stage}/copied")
        if initiator:
            case.verify()
        # No rank may overwrite a slot for the next case before verification finished.
        self.sync.barrier(f"{stage}/verified")
        return row


def measure(case, count):
    copy, args = case.copy, case.copy_args
    # Both backends provide a synchronous copy with completion included.
    # Native stream/transport completion is included in each call below.
    start_ns = time.monotonic_ns()
    for _ in range(count):
        ret = copy(**args)
        if ret != 0:
            require_ok(ret, f"batch copy block_size={case.size} batch={case.batch}")
    end_ns = time.monotonic_ns()
    elapsed_ns = max(1, end_ns - start_ns)
    total_bytes = case.length * count
    return {
        "direction": case.phase.name,
        "rank_id": case.rank.rank_id,
        "device_id": case.rank.device_id,
        "peer_rank_id": case.peer,
        "copy_size": case.size,
        "batch_size": case.batch,
        "copy_count": count,
        "total_bytes": total_bytes,
        "total_time_us": elapsed_ns / 1000,
        "bandwidth_gbps": total_bytes / elapsed_ns,
        "start_ns": start_ns,
        "end_ns": end_ns,
    }


def rows_to_report(params, rank_id, sync):
    # Rank 0 reports both groups; in cross-machine mode the group 1 command reports only its own rows.
    if rank_id == 0:
        return sync.gather()
    if params.group == 1:
        return [row for row in sync.gather() if row["rank_id"] == rank_id]
    return []
