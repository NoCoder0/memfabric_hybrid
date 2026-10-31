# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Native resources owned by one disposable rank process, kept alive until that process exits."""

import logging
import mmap

import torch

import memfabric_hybrid as mf
from memfabric_hybrid import bm

from ..common.native import configure_native_logging, require_ok

LOGGER = logging.getLogger(__name__)
_ALIGNMENT = 4096
_GIB = 1024**3
_PROTOCOL_TO_DATA_OP = {
    "sdma": bm.BmDataOpType.SDMA,
    "device_rdma": bm.BmDataOpType.DEVICE_RDMA,
    "device_urma": bm.BmDataOpType.DEVICE_URMA,
    "host_rdma": bm.BmDataOpType.HOST_RDMA,
    "host_urma": bm.BmDataOpType.HOST_URMA,
    "host_tcp": bm.BmDataOpType.HOST_TCP,
}
_POOL_MEM = {"device": bm.BmMemType.DEVICE, "host": bm.BmMemType.HOST}


class BmRank:
    """MF/BM resources with process lifetime; native teardown races with asynchronous peer callbacks."""

    def __init__(self, params, rank_id, local_index):
        self.params, self.rank_id = params, rank_id
        self.device_index = params.device_indices[local_index]  # logical id for torch.npu and BM
        self.device_id = params.devices[local_index]  # physical id, for reports only
        self.handle = None
        self.buffers = {}  # local mem type -> (raw tensor kept alive, aligned view)
        self.registered = []

    def initialize(self):
        torch.npu.set_device(self.device_index)
        configure_native_logging(self.params.log)
        require_ok(mf.initialize(), "mf.initialize")
        self._initialize_bm()
        self.handle = self._create_pool()
        require_ok(self.handle.join(), "handle.join")

    def slot(self, rank_id, mem):
        """GVA base of `rank_id`'s HBM or DRAM pool slot."""
        gva = self.handle.peer_rank_ptr(rank_id, _POOL_MEM[mem])
        if not gva:
            raise RuntimeError(f"pool slot address is unavailable: rank={rank_id} mem={mem}")
        return gva

    def buffer(self, mem):
        """Aligned, registered local working buffer of payload size, allocated on first use."""
        if mem not in self.buffers:
            size = self.params.payload_size
            if mem == "device":
                raw = torch.zeros((size + _ALIGNMENT) // 4, dtype=torch.int32, device="npu")
            else:
                # Dedicated pages for Host registration, independent of the CPU allocator.
                raw = torch.frombuffer(mmap.mmap(-1, size + _ALIGNMENT), dtype=torch.int32)
            offset = (-raw.data_ptr() % _ALIGNMENT) // 4
            view = raw[offset : offset + size // 4]
            self.buffers[mem] = (raw, view)
            if self.params.protocol != "sdma":
                require_ok(self.handle.register(view.data_ptr(), size), f"handle.register {mem} size={size}")
                self.registered.append(view.data_ptr())
        return self.buffers[mem][1]

    def unregister_buffers(self):
        while self.registered:
            require_ok(self.handle.unregister(self.registered.pop()), "handle.unregister")

    def _initialize_bm(self):
        cfg = bm.BmConfig()
        cfg.auto_ranking = False
        cfg.rank_id = self.rank_id
        cfg.start_store = self.rank_id == 0
        cfg.set_nic(self.params.nic)
        ret = bm.initialize(self.params.store_url, self.params.world_size, self.device_index, cfg)
        require_ok(ret, f"bm.initialize store={self.params.store_url} world_size={self.params.world_size}")
        if bm.bm_rank_id() != self.rank_id:
            raise RuntimeError(f"actual BM rank {bm.bm_rank_id()} does not match requested rank {self.rank_id}")

    def _create_pool(self):
        hbm = self.params.local_hbm_size
        dram = max(4 * _GIB, -(-self.params.payload_size // _GIB) * _GIB)
        handle = bm.create2(
            id=0,
            local_dram_size=dram,
            max_dram_size=dram,
            local_hbm_size=hbm,
            max_hbm_size=hbm,
            data_op_type=_PROTOCOL_TO_DATA_OP[self.params.protocol],
            enable_56bits_gva=True,
        )
        if handle is None:
            raise RuntimeError(f"bm.create2 failed: hbm={hbm} detail={mf.get_last_err_msg()}")
        return handle
