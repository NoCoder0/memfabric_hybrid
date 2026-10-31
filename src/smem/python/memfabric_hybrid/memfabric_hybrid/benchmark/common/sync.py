# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Store-based barriers, configuration agreement and result exchange for all benchmark ranks."""

import json
import logging
import time
from datetime import timedelta

LOGGER = logging.getLogger(__name__)
_POLL_SEC = 0.001  # short: concurrent pairs should start their timed copies together


class PeerFailedError(RuntimeError):
    """Another rank already reported its own failure through the store."""


class BenchmarkSync:
    """One TCPStore shared by every rank of both groups; rank 0 hosts it and leaves last."""

    def __init__(self, params, rank_id):
        from torch.distributed import TCPStore

        self.rank_id = rank_id
        self.world_size = params.world_size
        self.timeout = params.sync_timeout
        # Only the machine that does not own --ip can be left waiting for the other one to start.
        self.remedy = f"; run the same command on {params.sync_host}" if params.group == 1 else ""
        try:
            self.store = TCPStore(
                params.sync_host,
                params.sync_port,
                self.world_size,
                rank_id == 0,
                timedelta(seconds=self.timeout),
                wait_for_workers=False,
            )
        except Exception as error:  # noqa: BLE001 - torch raises several store error types
            raise TimeoutError(
                f"rank={rank_id} cannot reach {params.sync_host}:{params.sync_port} within {self.timeout:g}s{self.remedy}"
            ) from error

    def barrier(self, stage):
        self.store.set(f"{stage}/{self.rank_id}", "ready")
        self._wait([f"{stage}/{rank}" for rank in range(self.world_size)], stage)
        # Acknowledge the release so rank 0 cannot drop the store while a peer still polls the final keys.
        self.store.set(f"{stage}/ack/{self.rank_id}", "done")
        if self.rank_id == 0:
            self._wait([f"{stage}/ack/{rank}" for rank in range(self.world_size)], f"{stage} acknowledgements")

    def agree(self, signature):
        """Fail fast, before BM join, when the commands were started with different settings."""
        encoded = json.dumps(signature, sort_keys=True)
        self.store.set(f"config/{self.rank_id}", encoded)
        self.barrier("configuration ready")
        local = json.loads(encoded)  # compare JSON forms: tuples come back as lists
        for rank in range(self.world_size):
            actual = json.loads(self.store.get(f"config/{rank}"))
            if actual != local:
                diff = {key: (actual.get(key), local.get(key)) for key in local if actual.get(key) != local.get(key)}
                raise ValueError(f"benchmark configuration mismatch with rank={rank}: (theirs, ours)={diff}")

    def publish(self, results):
        self.store.set(f"results/{self.rank_id}", json.dumps(results))

    def exchange(self, stage, metadata, peer_rank):
        self.store.set(f"{stage}/metadata/{self.rank_id}", json.dumps(metadata))
        self.barrier(stage)
        return json.loads(self.store.get(f"{stage}/metadata/{peer_rank}"))

    def gather(self):
        return [row for rank in range(self.world_size) for row in json.loads(self.store.get(f"results/{rank}"))]

    def fail(self, message):
        try:
            self.store.set("failure", f"rank={self.rank_id}: {message}")
        except Exception as error:  # noqa: BLE001 - the store may already be gone with its host
            LOGGER.error("failed to notify peers: rank=%s error=%s", self.rank_id, error)

    def finish_failure(self):
        """Keep rank 0's store alive until peers stop using it, with a bound for crashed peers."""
        try:
            self.store.set(f"failure exit/{self.rank_id}", "ready")
            if self.rank_id != 0:
                return
            keys = [f"failure exit/{rank}" for rank in range(self.world_size)]
            deadline = time.monotonic() + min(self.timeout, 5.0)
            while not self.store.check(keys):
                if time.monotonic() >= deadline:
                    LOGGER.warning("failure exit acknowledgements incomplete: rank=%s", self.rank_id)
                    return
                time.sleep(_POLL_SEC)
        except Exception as error:  # noqa: BLE001 - the store host may itself have crashed
            LOGGER.warning("failed to acknowledge rank exit: rank=%s error=%s", self.rank_id, error)

    def _wait(self, keys, stage):
        deadline = time.monotonic() + self.timeout
        while not self.store.check(keys):
            if self.store.check(["failure"]):
                raise PeerFailedError(f"peer failed: {self.store.get('failure').decode()}")
            if time.monotonic() >= deadline:
                raise TimeoutError(f"rank={self.rank_id} timed out at {stage} after {self.timeout:g}s{self.remedy}")
            time.sleep(_POLL_SEC)
