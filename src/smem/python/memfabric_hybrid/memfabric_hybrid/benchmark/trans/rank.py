# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Per-rank Trans engine and application-owned registered buffers."""

import logging
import mmap

import torch

from memfabric_hybrid import TransferEngine, offload

from ..common.native import configure_native_logging, require_ok

LOGGER = logging.getLogger(__name__)
_ALIGNMENT = 2 * 1024 * 1024  # Trans expands registration to whole 2 MiB pages.


class TransRank:
    def __init__(self, params, rank_id, local_index):
        self.params, self.rank_id = params, rank_id
        self.device_index = params.device_indices[local_index]
        self.device_id = params.devices[local_index]
        self.engine = None
        self.buffers = {}
        self.peer = None
        self.offload_initialized = False

    def initialize(self, sync):
        torch.npu.set_device(self.device_index)
        configure_native_logging(self.params.log)
        self.engine = TransferEngine()
        protocol = getattr(TransferEngine.TransDataOpType, self.params.protocol.upper())
        role = "Prefill" if self.params.group_of(self.rank_id) == 0 else "Decode"
        require_ok(
            self.engine.initialize(
                self.params.store_url,
                f"{self.params.local_ip}:0",
                role,
                self.device_index,
                protocol,
                nic=self.params.nic,
            ),
            f"TransferEngine.initialize rank={self.rank_id} protocol={self.params.protocol}",
        )
        self.endpoint = f"{self.params.local_ip}:{self.engine.get_rpc_port()}"
        self._register_buffers()
        metadata = {
            "endpoint": self.endpoint,
            "buffers": {mem: [view.data_ptr(), self.params.payload_size] for mem, (_, view) in self.buffers.items()},
        }
        self.peer = sync.exchange("trans buffers", metadata, self.params.peer_of(self.rank_id))
        LOGGER.info("Trans ready: rank=%s endpoint=%s peer=%s", self.rank_id, self.endpoint, self.peer["endpoint"])

    def _register_buffers(self):
        # Register once, before exchanging metadata; retain the backing allocations until destroy.
        memories = {mem for phase in self.params.phases for mem in (phase.local_mem, phase.peer_mem)}
        size = -(-self.params.payload_size // _ALIGNMENT) * _ALIGNMENT
        if self.params.protocol == "sdma" and "host" in memories:
            config = offload.OffloadConfig()
            config.device_id = self.device_index
            config.reserve_size = config.alloc_size = size + _ALIGNMENT
            config.world_size = self.params.world_size
            config.local_world_size = self.params.local_rank_size
            config.rank_id = self.rank_id
            config.scene = offload.Scene.SHARED
            config.store_url = self.params.store_url
            # SHARED exports the Fabric handle needed by Trans; LOCAL alone does not.
            require_ok(offload.initialize(config), f"offload.initialize rank={self.rank_id} size={size + _ALIGNMENT}")
            self.offload_initialized = True
        for mem in sorted(memories):
            if mem == "device":
                raw = torch.zeros((size + _ALIGNMENT) // 4, dtype=torch.int32, device="npu")
            elif self.offload_initialized:
                raw = offload.empty([(size + _ALIGNMENT) // 4], dtype=torch.int32)
            else:
                raw = torch.frombuffer(mmap.mmap(-1, size + _ALIGNMENT), dtype=torch.int32)
            offset = (-raw.data_ptr() % _ALIGNMENT) // 4
            view = raw[offset : offset + size // 4]
            self.buffers[mem] = (raw, view)
            require_ok(self.engine.register_memory(view.data_ptr(), size), f"register_memory mem={mem} size={size}")

    def buffer(self, mem):
        return self.buffers[mem][1]

    def close(self):
        if self.engine is not None:
            # deregister is currently a compatibility no-op. Never free buffers before destroy.
            self.engine.destroy()
        self.buffers.clear()
        if self.offload_initialized:
            offload.uninitialize()
            self.offload_initialized = False
        if self.engine is not None:
            self.engine.unInitialize()
            self.engine = None
