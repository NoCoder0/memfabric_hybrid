#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import multiprocessing as mp

import torch

import memfabric_hybrid as mf
from memfabric_hybrid import bm

ONE_GIB = 1 << 30
STORE_URL = "tcp://127.0.0.1:8572"
WORLD_SIZE = 2
ITEM_BYTES = 256
MAX_BATCH = 1001
SOURCE_OFFSET = 4 * 1024 * 1024
DESTINATION_OFFSET = 16 * 1024 * 1024
RANK_0, DEVICE_0 = 0, 0
RANK_1, DEVICE_1 = 1, 1


def _write_local_pattern(handle, local_hbm: int, rank_id: int) -> torch.Tensor:
    element_count = MAX_BATCH * ITEM_BYTES // 4
    pattern = (torch.arange(element_count, dtype=torch.int32) * 17 + rank_id * 101).contiguous()
    source = local_hbm + SOURCE_OFFSET
    assert handle.copy_data(pattern.data_ptr(), source, pattern.numel() * 4, bm.BmCopyType.H2G, 0) == 0
    print("_write_local_pattern " + str(rank_id))
    return pattern


def _read_peer_single(handle, local_hbm: int, peer_hbm: int, peer_rank: int) -> None:
    ret = handle.copy_data(
        peer_hbm + SOURCE_OFFSET,
        local_hbm + DESTINATION_OFFSET,
        ITEM_BYTES,
        bm.BmCopyType.G2G,
        0,
    )
    assert ret == 0, f"rank read peer single item failed: peer={peer_rank}, ret={ret}"
    print("_read_peer_single " + str(peer_rank))
    actual = torch.empty(ITEM_BYTES // 4, dtype=torch.int32)
    ret = handle.copy_data(local_hbm + DESTINATION_OFFSET, actual.data_ptr(), ITEM_BYTES, bm.BmCopyType.G2H, 0)
    print("_read_peer_single " + str(peer_rank))
    assert ret == 0, f"single-item G2H verification failed: peer={peer_rank}, ret={ret}"
    expected = torch.arange(actual.numel(), dtype=torch.int32) * 17 + peer_rank * 101
    assert torch.equal(actual, expected), f"single-item BatchCopy data mismatch: peer={peer_rank}"
    print("_read_peer_single " + str(peer_rank))


def _read_peer_batch(handle, local_hbm: int, peer_hbm: int, peer_rank: int, batch_size: int) -> None:
    sources = [peer_hbm + SOURCE_OFFSET + index * ITEM_BYTES for index in range(batch_size)]
    destinations = [local_hbm + DESTINATION_OFFSET + index * ITEM_BYTES for index in range(batch_size)]
    sizes = [ITEM_BYTES] * batch_size
    ret = handle.copy_data_batch(sources, destinations, sizes, batch_size, bm.BmCopyType.G2G, 0)
    assert ret == 0, f"rank read peer batch failed: peer={peer_rank}, batch_size={batch_size}, ret={ret}"

    actual = torch.empty(batch_size * ITEM_BYTES // 4, dtype=torch.int32)
    ret = handle.copy_data(
        local_hbm + DESTINATION_OFFSET, actual.data_ptr(), batch_size * ITEM_BYTES, bm.BmCopyType.G2H, 0
    )
    assert ret == 0, f"G2H verification copy failed: peer={peer_rank}, batch_size={batch_size}, ret={ret}"
    expected = torch.arange(actual.numel(), dtype=torch.int32) * 17 + peer_rank * 101
    assert torch.equal(actual, expected), f"BatchCopy data mismatch: peer={peer_rank}, batch_size={batch_size}"


def _rank_main(rank_id: int, device_id: int, sync: mp.Barrier) -> None:
    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    bm_initialized = False
    try:
        config = bm.BmConfig()
        config.rank_id = rank_id
        config.start_store = rank_id == RANK_0
        config.set_nic("tcp://127.0.0.1:10005")
        assert bm.initialize(STORE_URL, WORLD_SIZE, device_id, config) == 0, "bm.initialize failed"
        bm_initialized = True

        handle = bm.create2(
            id=0,
            local_dram_size=0,
            max_dram_size=0,
            local_hbm_size=ONE_GIB,
            max_hbm_size=ONE_GIB,
            data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
        )
        assert handle.join() == 0, "join failed"

        peer_rank = 1 - rank_id
        local_hbm = handle.peer_rank_ptr(rank_id, bm.BmMemType.DEVICE)
        peer_hbm = handle.peer_rank_ptr(peer_rank, bm.BmMemType.DEVICE)
        assert local_hbm != 0 and peer_hbm != 0, "peer_rank_ptr DEVICE failed"
        print("_rank_main " + str(peer_rank))
        _write_local_pattern(handle, local_hbm, rank_id)
        sync.wait()

        _read_peer_single(handle, local_hbm, peer_hbm, peer_rank)
        sync.wait()
        # for batch_size in (1, 999, 1000, 1001):
        #     _read_peer_batch(handle, local_hbm, peer_hbm, peer_rank, batch_size)
        #     sync.wait()
        # print(f"[rank {rank_id}] HOST_DEVICE_URMA BatchCopy checks passed", flush=True)

        # sync.wait()
        assert handle.leave() == 0, "leave failed"
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        handle.destroy()
    finally:
        if bm_initialized:
            bm.uninitialize(0)
        mf.uninitialize()


def main() -> None:
    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(WORLD_SIZE)
    processes = [
        mp.Process(target=_rank_main, args=(RANK_0, DEVICE_0, sync)),
        mp.Process(target=_rank_main, args=(RANK_1, DEVICE_1, sync)),
    ]
    for process in processes:
        process.start()
    for process in processes:
        process.join()
    exit_codes = [process.exitcode for process in processes]
    if any(code != 0 for code in exit_codes):
        raise RuntimeError(f"child rank failed: exit_codes={exit_codes}")
    print("04_single_node_multi_device_batch_copy: all ranks OK", flush=True)


if __name__ == "__main__":
    main()
