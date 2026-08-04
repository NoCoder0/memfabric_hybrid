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

import argparse
import ctypes
import json
import os
import socket
import struct

import memfabric_hybrid as mf
from memfabric_hybrid import bm


STORE_PORT = 8574
CTRL_PORT = 9877
WORLD_SIZE = 2
HOST_RANK, NPU_RANK = 0, 1
POOL_BYTES = 8 << 20
ITEM_BYTES = 4 << 10
BATCH_COUNTS = (1, 999, 1000, 1001)
BYTE_ARRAY = ctypes.c_uint8 * POOL_BYTES


def _read_exact(conn: socket.socket, size: int) -> bytes:
    chunks = []
    while size:
        chunk = conn.recv(size)
        if not chunk:
            raise RuntimeError("control connection closed before the message was complete")
        chunks.append(chunk)
        size -= len(chunk)
    return b"".join(chunks)


def _send(conn: socket.socket, message):
    payload = json.dumps(message).encode("utf-8")
    conn.sendall(struct.pack("!I", len(payload)) + payload)


def _recv(conn: socket.socket):
    payload_size = struct.unpack("!I", _read_exact(conn, 4))[0]
    return json.loads(_read_exact(conn, payload_size).decode("utf-8"))


def _config(rank_id: int, head_ip: str):
    cfg = bm.BmConfig()
    cfg.rank_id = rank_id
    cfg.start_store = rank_id == HOST_RANK
    cfg.auto_ranking = False
    cfg.set_nic(f"tcp://{head_ip}:10005")
    return cfg


def _create_host_handle(args):
    store_url = f"tcp://{args.head_ip}:{args.store_port}"
    ret = bm.initialize(store_url, WORLD_SIZE, 0, _config(HOST_RANK, args.head_ip))
    assert ret == 0, f"host bm.initialize failed, ret={ret}"
    handle = bm.create2(
        id=0,
        local_dram_size=POOL_BYTES,
        max_dram_size=POOL_BYTES,
        local_hbm_size=0,
        max_hbm_size=0,
        data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
    )
    assert handle.join() == 0, "host handle.join failed"
    return handle


def _create_npu_handle(args, device_id: int):
    store_url = f"tcp://{args.head_ip}:{args.store_port}"
    ret = bm.initialize(store_url, WORLD_SIZE, device_id, _config(NPU_RANK, args.head_ip))
    assert ret == 0, f"NPU bm.initialize failed, ret={ret}"
    handle = bm.create2(
        id=0,
        local_dram_size=0,
        max_dram_size=0,
        local_hbm_size=POOL_BYTES,
        max_hbm_size=POOL_BYTES,
        data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
    )
    assert handle.join() == 0, "NPU handle.join failed"
    return handle


def _host_pattern() -> ctypes.Array:
    block = bytes(index % 251 for index in range(ITEM_BYTES))
    return BYTE_ARRAY.from_buffer_copy(block * (POOL_BYTES // ITEM_BYTES))


def _run_host(args):
    os.environ["MF_HOST_URMA_EID"] = args.eid
    mf.set_log_level(1)
    assert mf.initialize() == 0, "host mf.initialize failed"
    bm_initialized = False
    handle = None
    try:
        handle = _create_host_handle(args)
        bm_initialized = True
        host_gva = handle.peer_rank_ptr(HOST_RANK, bm.BmMemType.HOST)
        assert host_gva != 0, "host fixed GVA allocation failed"
        host_view = handle.gva_to_va(host_gva, bm.BmMemType.LOCAL_HOST)
        assert host_view == host_gva, f"host fixed GVA/view mismatch: gva=0x{host_gva:x}, view=0x{host_view:x}"
        pattern = _host_pattern()
        ret = handle.copy_data(ctypes.addressof(pattern), host_gva, POOL_BYTES, bm.BmCopyType.H2G, 0)
        assert ret == 0, f"host source initialization failed, gva=0x{host_gva:x}, ret={ret}"

        with socket.create_server((args.bind_ip, args.ctrl_port)) as server:
            conn, _ = server.accept()
            with conn:
                _send(conn, {"host_gva": host_gva, "pool_bytes": POOL_BYTES, "item_bytes": ITEM_BYTES})
                message = _recv(conn)
                assert message == "READY", f"unexpected NPU handshake: {message}"
                message = _recv(conn)
                assert message == "COPY_DONE", f"unexpected NPU completion: {message}"
                _send(conn, "HOST_RELEASE")
        assert handle.leave() == 0, "host handle.leave failed"
        handle.destroy()
        handle = None
        print("[host] fixed-GVA route and source lifetime checks passed", flush=True)
    finally:
        if handle is not None:
            handle.destroy()
        if bm_initialized:
            bm.uninitialize(0)
        mf.uninitialize()


def _run_batch(handle, host_gva: int, device, count: int, offset: int):
    import torch
    import torch_npu
    import mf_acc_offload

    total_bytes = count * ITEM_BYTES
    destination = torch.full((total_bytes,), 0xCD, dtype=torch.uint8).npu()
    src_addresses = [host_gva + offset + index * ITEM_BYTES for index in range(count)]
    dst_addresses = [destination.data_ptr() + index * ITEM_BYTES for index in range(count)]
    src_ptrs = torch.tensor(src_addresses, dtype=torch.int64).npu()
    dst_ptrs = torch.tensor(dst_addresses, dtype=torch.int64).npu()
    len_ptrs = torch.full((count,), ITEM_BYTES, dtype=torch.int64).npu()
    ret = mf_acc_offload.sparse_copy_urma(src_ptrs, dst_ptrs, len_ptrs, count, device)
    assert ret == 0, f"NPU sparse_copy_urma failed, count={count}, ret={ret}"
    torch.npu.synchronize()
    expected = (torch.arange(total_bytes, dtype=torch.int32) % 251).to(torch.uint8)
    assert torch.equal(destination.cpu(), expected), f"NPU data mismatch, count={count}, offset={offset}"
    print(f"[NPU] sparse_copy_urma OK count={count} offset={offset} bytes={total_bytes}", flush=True)


def _run_npu(args):
    import torch
    import torch_npu

    torch.npu.set_device(args.device_id)
    mf.set_log_level(1)
    assert mf.initialize() == 0, "NPU mf.initialize failed"
    bm_initialized = False
    handle = None
    try:
        handle = _create_npu_handle(args, args.device_id)
        bm_initialized = True
        peer_gva = handle.peer_rank_ptr(HOST_RANK, bm.BmMemType.HOST)
        assert peer_gva != 0, "NPU remote Host GVA lookup failed"
        import_view = handle.gva_to_va(peer_gva, bm.BmMemType.LOCAL_HOST)
        assert import_view == peer_gva, (
            f"Host GVA/import view equality failed: gva=0x{peer_gva:x}, view=0x{import_view:x}"
        )
        with socket.create_connection((args.head_ip, args.ctrl_port)) as conn:
            route_info = _recv(conn)
            assert route_info["host_gva"] == peer_gva, (
                f"peer GVA differs from host metadata: local=0x{peer_gva:x}, remote=0x{route_info['host_gva']:x}"
            )
            assert route_info["item_bytes"] == ITEM_BYTES and route_info["pool_bytes"] == POOL_BYTES
            print(
                f"[NPU] route peer={HOST_RANK} exported_gva=0x{peer_gva:x} "
                f"import_view=0x{import_view:x} device={args.device_id}",
                flush=True,
            )
            _send(conn, "READY")
            for count in BATCH_COUNTS:
                if count == 1:
                    offset = 2 * ITEM_BYTES
                elif count == 999:
                    offset = ITEM_BYTES
                else:
                    offset = POOL_BYTES - count * ITEM_BYTES
                _run_batch(handle, peer_gva, torch.device(f"npu:{args.device_id}"), count, offset)
            _send(conn, "COPY_DONE")
            message = _recv(conn)
            assert message == "HOST_RELEASE", f"unexpected host completion: {message}"
        assert handle.leave() == 0, "NPU handle.leave failed"
        handle.destroy()
        handle = None
        print("[NPU] Host-DDR sparse_copy_urma checks passed", flush=True)
    finally:
        if handle is not None:
            handle.destroy()
        if bm_initialized:
            bm.uninitialize(0)
        mf.uninitialize()


def main():
    parser = argparse.ArgumentParser(description="Host-DDR to NPU HBM sparse_copy_urma example")
    parser.add_argument("--rank", type=int, required=True, choices=(HOST_RANK, NPU_RANK))
    parser.add_argument("--head-ip", required=True, help="Config-store and control-plane peer address")
    parser.add_argument("--eid", help="Host URMA EID, 32 hex characters; required for --rank 0")
    parser.add_argument("--device-id", type=int, default=0, help="NPU device id used by --rank 1")
    parser.add_argument("--store-port", type=int, default=STORE_PORT)
    parser.add_argument("--ctrl-port", type=int, default=CTRL_PORT)
    parser.add_argument("--bind-ip", default="0.0.0.0", help="Host-side control listener bind address")
    args = parser.parse_args()
    if args.rank == HOST_RANK:
        if not args.eid:
            parser.error("--eid is required for --rank 0")
        _run_host(args)
    else:
        _run_npu(args)


if __name__ == "__main__":
    main()
