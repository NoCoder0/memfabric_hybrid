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
import torch_npu

import memfabric_hybrid as mf
import mf_acc_offload
from memfabric_hybrid import bm
import os


STORE_URL = "tcp://127.0.0.1:8573"
WORLD_SIZE = 2
POOL_BYTES = 1 << 30
#ITEM_BYTES = 4 << 10
ITEM_BYTES=10
BATCH_COUNTS = [1, 999, 1000, 1001]
#BATCH_COUNTS = [1]
RANK_0, DEVICE_0 = 0, 0
RANK_1, DEVICE_1 = 1, 1

LOCA_EIDS={
        0:"000000000045050000100000df00b605",
        1:"000000000045050000100000df00d606"
        }

def _pattern(size: int, rank_id: int) -> torch.Tensor:
    return ((torch.arange(size, dtype=torch.int32) + rank_id * 101) % 251).to(torch.uint8).contiguous()


def _create_handle(rank_id: int, device_id: int):
    cfg = bm.BmConfig()
    cfg.rank_id = rank_id
    cfg.start_store = rank_id == RANK_0
    cfg.auto_ranking = False
    cfg.set_nic("tcp://127.0.0.1:10005")
    assert bm.initialize(STORE_URL, WORLD_SIZE, device_id, cfg) == 0, f"rank {rank_id}: bm.initialize failed"

    # HOST_DEVICE_URMA is the shared protocol that selects the Device URMA manager
    # on Ascend and enables the production BatchCopy route publisher.
    handle = bm.create2(
        id=0,
        local_dram_size=0,
        max_dram_size=0,
        local_hbm_size=POOL_BYTES,
        max_hbm_size=POOL_BYTES,
        data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
    )
    print(f"[rank {rank_id}]  bm.create2 success", flush=True)
    ret = handle.join()
    print(f"[rank {rank_id}]  joinjoin {ret}", flush=True)
    assert  ret == 0, f"rank {rank_id}: bm handle.join failed"
    return handle


def _write_local_pattern(handle, local_gva: int, rank_id: int) -> torch.Tensor:
    pattern = _pattern(POOL_BYTES, rank_id)
    # Initialization only. The data under test below is read exclusively through
    # mf_acc_offload.sparse_copy_urma, never through the batch BM copy API.
    print(f"rank {rank_id} _write local_pattern is {pattern}")
    ret = handle.copy_data(pattern.data_ptr(), local_gva, POOL_BYTES, bm.BmCopyType.H2G, 0)
    assert ret == 0, f"rank {rank_id}: H2G source initialization failed, gva=0x{local_gva:x}, ret={ret}"
    return pattern


def _run_batch(
    peer_gva: int, peer_pattern: torch.Tensor, device: torch.device, count: int, offset: int, rank_id: int
):
    total_bytes = count * ITEM_BYTES
    destination = torch.full((total_bytes,), 0xCD, dtype=torch.uint8).npu()
    src_addresses = [peer_gva + offset + index * ITEM_BYTES for index in range(count)]
    dst_addresses = [destination.data_ptr() + index * ITEM_BYTES for index in range(count)]
    lengths = [ITEM_BYTES] * count
    src_ptrs = torch.tensor(src_addresses, dtype=torch.int64).npu()
    print(f"rank id {rank_id} src_addresses is ")
    print(' '.join(f'0x{x:02x}' for x in src_addresses[0:10]))
    dst_ptrs = torch.tensor(dst_addresses, dtype=torch.int64).npu()
    print(f"rank id {rank_id} dst_addresses is")
    print(' '.join(f'0x{x:02x}' for x in dst_addresses[0:10]))
    len_ptrs = torch.tensor(lengths, dtype=torch.int64).npu()

    ret = mf_acc_offload.sparse_copy_urma(src_ptrs, dst_ptrs, len_ptrs, count, device)
    assert ret == 0, f"rank {rank_id}: sparse_copy_urma failed, count={count}, ret={ret}"
    torch.npu.synchronize()
    expected = peer_pattern[offset : offset + total_bytes]
    print(f"rank id {rank_id} expected peer_pattern is {expected}")
    actual = destination.cpu()
    print(f"rank id {rank_id} actual is {actual}")
    assert torch.equal(actual, expected), f"rank {rank_id}: data mismatch, count={count}, offset={offset}"
    print(f"[rank {rank_id}] sparse_copy_urma OK count={count} offset={offset} bytes={total_bytes}", flush=True)
    print(f"\n")


def _rank_main(rank_id: int, device_id: int, sync: mp.Barrier):
    torch.npu.set_device(device_id)
    mf.set_log_level(3)
    mf_initialized = mf.initialize() == 0
    assert mf_initialized, f"rank {rank_id}: mf.initialize failed"
    bm_initialized = False
    handle = None
    os.environ["USE_LOCAL_EID"]=LOCA_EIDS[device_id]
    try:
        handle = _create_handle(rank_id, device_id)
        print(f"[rank {rank_id}]  _create_handle success", flush=True)
        bm_initialized = True
        peer_rank = 1 - rank_id
        local_gva = handle.peer_rank_ptr(rank_id, bm.BmMemType.DEVICE)
        peer_gva = handle.peer_rank_ptr(peer_rank, bm.BmMemType.DEVICE)
        print(f"[rank {rank_id}]  local_gva {local_gva:#x}")
        print(f"[rank {rank_id}]  peer_gva {peer_gva:#x}")

        assert local_gva != 0 and peer_gva != 0, f"rank {rank_id}: DEVICE GVA lookup failed"
        local_pattern = _write_local_pattern(handle, local_gva, rank_id)
        print(f"[rank {rank_id}]  _write_local_pattern success", flush=True)
        sync.wait()

        peer_pattern = _pattern(POOL_BYTES, peer_rank)
        for count in BATCH_COUNTS:
            if count == 1:
                offset = 2 * ITEM_BYTES
            elif count == 999:
                offset = ITEM_BYTES
            else:
                offset = POOL_BYTES - count * ITEM_BYTES
            _run_batch(peer_gva, peer_pattern, torch.device(f"npu:{device_id}"), count, offset, rank_id)
            sync.wait()

        assert local_pattern.numel() == POOL_BYTES, f"rank {rank_id}: local pattern lifetime check failed"
        assert handle.leave() == 0, f"rank {rank_id}: handle.leave failed"
        handle.destroy()
        handle = None
    finally:
        if handle is not None:
            handle.destroy()
        if bm_initialized:
            bm.uninitialize(0)
        if mf_initialized:
            mf.uninitialize()


def main():
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
    print("01_single_node_multi_device_urma: all ranks OK", flush=True)


if __name__ == "__main__":
    main()
