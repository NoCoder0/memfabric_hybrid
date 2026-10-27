# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Parent process: spawn the local ranks, collect their results and return one exit code."""

import logging
import multiprocessing as mp
import socket
import time
from collections import Counter
from queue import Empty

from . import output

LOGGER = logging.getLogger(__name__)
_POLL_SEC = 0.2
_ABORT_GRACE_SEC = 30  # time for surviving ranks to see the failure and clean up on their own
_REAP_TIMEOUT_SEC = 5


class BenchmarkRunner:
    """One CLI invocation: the local rank processes, the result queue and the final report."""

    def __init__(self, params, worker):
        self.params = params
        self.worker = worker  # top-level function(params, rank_id, local_index, queue); spawn pickles it
        self.ctx = mp.get_context("spawn")
        self.queue = self.ctx.Queue()
        self.processes = []

    def run(self):
        try:
            output.print_start(self.params)
            if self.params.group == 1 and not _store_reachable(self.params.sync_host, self.params.sync_port):
                output.print_peer_hint(self.params.sync_host)
            self._spawn()
            results, errors = self._collect()
        except BaseException:
            LOGGER.exception("benchmark parent failed while starting or collecting rank processes")
            raise
        finally:
            try:
                self._reap_processes()
            finally:
                self.queue.close()
                self.queue.join_thread()
        if errors:
            for rank_id, message in sorted(errors, key=lambda item: item[0]):
                LOGGER.error("benchmark failed: rank=%s error=%s", rank_id, message)
            print("[benchmark] FAILED: initialization, transfer, verification or cleanup failed.", flush=True)
            return 1
        output.print_matrix(results, self.params)
        return 0

    def _spawn(self):
        for index in range(self.params.local_rank_size):
            rank_id = self.params.rank_start + index
            process = self.ctx.Process(target=self.worker, args=(self.params, rank_id, index, self.queue))
            process.start()
            self.processes.append(process)

    def _collect(self):
        results, errors = [], []
        reported = set()
        abort_at = None
        while any(process.exitcode is None for process in self.processes):
            self._drain(results, errors, reported, timeout=_POLL_SEC)
            if abort_at is None and any(process.exitcode not in (None, 0) for process in self.processes):
                abort_at = time.monotonic() + _ABORT_GRACE_SEC
            if abort_at is not None and time.monotonic() >= abort_at:
                # SIGTERM alone may leave a surviving rank stuck in a native call.
                self._reap_processes()
                break
        for index, process in enumerate(self.processes):
            process.join()
            if process.exitcode != 0:
                errors.append((self.params.rank_start + index, f"process exited with code {process.exitcode}"))
        self._drain(results, errors, reported, timeout=0)
        if not errors:
            errors.extend(self._check_results(results, reported))
        return results, errors

    def _check_results(self, results, reported):
        """Exit code zero is insufficient: every worker and planned copy must report once."""
        params = self.params
        local_ranks = set(range(params.rank_start, params.rank_start + params.local_rank_size))
        errors = [(rank, "missing worker report") for rank in sorted(local_ranks - reported)]
        # Rank 0 reports the whole world. The separate group 1 command reports only its own initiators.
        result_ranks = local_ranks if params.group == 1 else range(params.world_size)
        expected = {
            (phase.name, size, rank)
            for phase in params.phases
            for size in params.block_sizes
            for rank in result_ranks
            if params.group_of(rank) == phase.initiator_group
        }
        actual = Counter((row["direction"], row["copy_size"], row["rank_id"]) for row in results)
        for direction, size, rank in sorted(expected - actual.keys()):
            errors.append((rank, f"missing result: direction={direction} block_size_bytes={size}"))
        for (direction, size, rank), count in actual.items():
            if (direction, size, rank) not in expected or count != 1:
                errors.append((rank, f"unexpected result: direction={direction} block_size_bytes={size} count={count}"))
        return errors

    def _reap_processes(self):
        """Reap started workers before closing IPC, including partial spawn and interruption."""
        for index, process in enumerate(self.processes):
            if process.exitcode is None:
                LOGGER.error(
                    "terminating unfinished rank during parent cleanup: rank=%s", self.params.rank_start + index
                )
                process.terminate()
        for index, process in enumerate(self.processes):
            process.join(timeout=_REAP_TIMEOUT_SEC)
            if process.exitcode is None:
                LOGGER.error(
                    "killing rank after cleanup timeout: rank=%s timeout_sec=%s",
                    self.params.rank_start + index,
                    _REAP_TIMEOUT_SEC,
                )
                process.kill()
                process.join()

    def _drain(self, results, errors, reported, timeout):
        while True:
            try:
                message = self.queue.get(timeout=timeout) if timeout else self.queue.get_nowait()
            except Empty:
                return
            rank = message["rank_id"]
            if (
                rank in reported
                or not self.params.rank_start <= rank < self.params.rank_start + self.params.local_rank_size
            ):
                errors.append((rank, "duplicate or unexpected worker report"))
            elif "error" in message:
                errors.append((rank, message["error"]))
            else:
                results.extend(message["results"])
            reported.add(rank)
            timeout = 0


def _store_reachable(host, port):
    """One connect-and-close probe of the --ip machine's sync store; nothing is sent."""
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except OSError:
        return False
