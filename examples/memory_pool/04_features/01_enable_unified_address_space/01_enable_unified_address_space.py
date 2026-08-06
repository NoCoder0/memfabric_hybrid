#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
#
# Note: unified_address_space is enforced to True by SmemBmConfigCheck (smem_bm.cpp:51);
# setting False is rejected. The "on/off comparison" in the README spec is aspirational;
# this sample demonstrates the ON path: gva_to_va address semantics + round-trip correctness.
import logging
import multiprocessing as mp

import torch

import memfabric_hybrid as mf
from memfabric_hybrid import bm

logging.basicConfig(level=logging.INFO, format="%(message)s")

ONE_GIB = 1 << 30  # 1GB
COPY_BYTES = 4 * 1024 * 1024  # 4MB int32 payload
STORE_URL = "tcp://127.0.0.1:8575"
WORLD_SIZE = 2

RANK_0, DEVICE_0 = 0, 0
RANK_1, DEVICE_1 = 1, 1


def _rank_main(rank_id: int, device_id: int, sync: mp.Barrier):
    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    bm_inited = False
    try:
        cfg = bm.BmConfig()
        cfg.rank_id = rank_id
        cfg.start_store = rank_id == RANK_0
        cfg.set_nic("tcp://127.0.0.1:10006")
        # enforced by SmemBmConfigCheck (smem_bm.cpp:51); False is rejected. Set explicitly for clarity.
        cfg.unified_address_space = True
        assert bm.initialize(STORE_URL, WORLD_SIZE, device_id, cfg) == 0, "bm.initialize failed"
        bm_inited = True

        handle = bm.create2(
            id=0,
            local_dram_size=ONE_GIB,
            max_dram_size=ONE_GIB,
            data_op_type=bm.BmDataOpType.DEVICE_RDMA,
        )
        assert handle.join() == 0, "join failed"

        gva_me = handle.peer_rank_ptr(rank_id, bm.BmMemType.HOST)
        assert gva_me != 0, "peer_rank_ptr(HOST) returned 0"
        # gva_to_va: convert GVA to process-local VA; with UAS=True (SVM), GVA is directly accessible.
        va_me = handle.gva_to_va(gva=gva_me)
        logging.info(f"[rank {rank_id}] uas=True gva=0x{gva_me:x} va=0x{va_me:x} gva==va:{gva_me == va_me}")

        peer = 1 - rank_id
        gva_peer = handle.peer_rank_ptr(peer, bm.BmMemType.HOST)
        assert gva_peer != 0, "peer_rank_ptr(peer HOST) returned 0"

        sync.wait()  # (1) both ranks ready

        # r0 -> r1 round-trip: H2G(r0) -> G2G(0->1) -> G2H(r1) verify
        if rank_id == RANK_0:
            src = torch.arange(COPY_BYTES // 4, dtype=torch.int32).contiguous()
            assert handle.copy_data(src.data_ptr(), gva_me, COPY_BYTES, bm.BmCopyType.H2G, 0) == 0, "H2G r0"
        sync.wait()  # (2) r0 done H2G

        if rank_id == RANK_0:
            assert handle.copy_data(gva_me, gva_peer, COPY_BYTES, bm.BmCopyType.G2G, 0) == 0, "G2G 0->1"
        sync.wait()  # (3) r0 done G2G

        if rank_id == RANK_1:
            exp = torch.arange(COPY_BYTES // 4, dtype=torch.int32).contiguous()
            got = torch.empty(COPY_BYTES // 4, dtype=torch.int32)
            assert handle.copy_data(gva_me, got.data_ptr(), COPY_BYTES, bm.BmCopyType.G2H, 0) == 0, "G2H r1"
            assert torch.equal(got, exp), "round-trip r0->r1 mismatch"
            logging.info(f"[rank {rank_id}] round-trip OK (r0->r1)")
        sync.wait()  # (4) r1 done verify

        assert handle.leave() == 0, "leave failed"
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        handle.destroy()
    finally:
        if bm_inited:
            bm.uninitialize(0)
        mf.uninitialize()


def main():
    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(WORLD_SIZE)
    p0 = mp.Process(target=_rank_main, args=(RANK_0, DEVICE_0, sync))
    p1 = mp.Process(target=_rank_main, args=(RANK_1, DEVICE_1, sync))
    p0.start()
    p1.start()
    p0.join()
    p1.join()
    if p0.exitcode != 0 or p1.exitcode != 0:
        raise RuntimeError(f"child rank failed: p0.exitcode={p0.exitcode}, p1.exitcode={p1.exitcode}")
    logging.info("01_enable_unified_address_space ok")


if __name__ == "__main__":
    main()
