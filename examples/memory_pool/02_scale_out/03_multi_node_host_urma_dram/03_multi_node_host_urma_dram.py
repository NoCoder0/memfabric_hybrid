#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
import argparse
import ctypes
import os
import socket
import struct
import time

os.environ.setdefault("MF_HYBM_RDMA_SWAP_SPACE_SIZE", "64")

import memfabric_hybrid as mf
from memfabric_hybrid import bm

ONE_GIB = 1 << 30
COPY_BYTES = 4 * 1024 * 1024
STORE_PORT = 8572
CTRL_PORT = 9876
INT32_COUNT = COPY_BYTES // 4
Int32Array = ctypes.c_int32 * INT32_COUNT


def _fill_seq(arr, start=0):
    for i in range(INT32_COUNT):
        arr[i] = start + i


def _fill_const(arr, val):
    for i in range(INT32_COUNT):
        arr[i] = val


def _ptr(buf):
    return ctypes.addressof(buf)


def _equal(a, b):
    for i in range(INT32_COUNT):
        if a[i] != b[i]:
            return False
    return True


def _send(conn, msg: str):
    conn.sendall(struct.pack("!I", len(msg)))
    conn.sendall(msg.encode())


def _recv(conn) -> str:
    raw_len = conn.recv(4)
    msg_len = struct.unpack("!I", raw_len)[0]
    return conn.recv(msg_len).decode()


def _run_rank0(args):
    os.environ["MF_HOST_URMA_EID"] = args.eid

    store_url = f"tcp://{args.head_ip}:{STORE_PORT}"
    mf.set_log_level(1)
    assert mf.initialize() == 0
    bm_inited = False
    try:
        cfg = bm.BmConfig()
        cfg.rank_id = 0
        cfg.start_store = True
        cfg.set_nic(f"tcp://{args.head_ip}:10005")
        assert bm.initialize(store_url, 2, 0, cfg) == 0
        bm_inited = True

        handle = bm.create2(
            id=0,
            local_dram_size=ONE_GIB,
            max_dram_size=ONE_GIB,
            data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
        )
        print("[rank 0] join()", flush=True)
        assert handle.join() == 0

        gva_me = handle.peer_rank_ptr(0, bm.BmMemType.HOST)
        assert gva_me != 0

        # Write pattern A to local GVA
        pattern_a = Int32Array()
        _fill_seq(pattern_a)
        assert handle.copy_data(_ptr(pattern_a), gva_me, COPY_BYTES,
                                bm.BmCopyType.H2G, 0) == 0
        print("[rank 0] wrote pattern A to local GVA", flush=True)

        with socket.create_server(("0.0.0.0", CTRL_PORT)) as server:
            server.listen(1)
            conn, _ = server.accept()
            with conn:
                _send(conn, "RANK0_READY")
                msg = _recv(conn)
                assert msg == "REMOTE_WRITE_DONE"
                # Verify rank 1 wrote pattern C to our GVA
                buf = Int32Array()
                assert handle.copy_data(gva_me, _ptr(buf), COPY_BYTES,
                                        bm.BmCopyType.G2H, 0) == 0
                expected_c = Int32Array()
                _fill_const(expected_c, 0xCA)
                assert _equal(buf, expected_c), "Pattern C mismatch"
                print("[rank 0] verified pattern C from rank 1", flush=True)

                _send(conn, "RANK0_DONE")
                msg = _recv(conn)
                assert msg == "RANK1_READY"

                # Active read rank 1's pattern B
                gva_peer1 = handle.peer_rank_ptr(1, bm.BmMemType.HOST)
                assert gva_peer1 != 0
                buf2 = Int32Array()
                assert handle.copy_data(gva_peer1, _ptr(buf2), COPY_BYTES,
                                        bm.BmCopyType.G2H, 0) == 0
                expected_b = Int32Array()
                _fill_const(expected_b, 0xBE)
                assert _equal(buf2, expected_b), "Pattern B mismatch"
                print("[rank 0] verified pattern B from rank 1 (active read)", flush=True)

        # Loop 100x 4MiB reads
        for i in range(100):
            buf_loop = Int32Array()
            assert handle.copy_data(gva_me, _ptr(buf_loop), COPY_BYTES,
                                    bm.BmCopyType.G2H, 0) == 0
        print("[rank 0] completed 100x 4MiB loop", flush=True)

        assert handle.leave() == 0
        handle.destroy()
    finally:
        if bm_inited:
            bm.uninitialize(0)
        mf.uninitialize()
    print("[rank 0] cleanup done", flush=True)


def _run_rank1(args):
    os.environ["MF_HOST_URMA_EID"] = args.eid

    store_url = f"tcp://{args.head_ip}:{STORE_PORT}"
    mf.set_log_level(1)
    assert mf.initialize() == 0
    bm_inited = False
    try:
        cfg = bm.BmConfig()
        cfg.rank_id = 1
        cfg.start_store = False
        cfg.set_nic(f"tcp://{args.head_ip}:10005")
        assert bm.initialize(store_url, 2, 0, cfg) == 0
        bm_inited = True

        handle = bm.create2(
            id=0,
            local_dram_size=ONE_GIB,
            max_dram_size=ONE_GIB,
            data_op_type=bm.BmDataOpType.HOST_DEVICE_URMA,
        )
        print("[rank 1] joining", flush=True)
        assert handle.join() == 0

        gva_peer0 = handle.peer_rank_ptr(0, bm.BmMemType.HOST)
        gva_me = handle.peer_rank_ptr(1, bm.BmMemType.HOST)
        assert gva_me != 0 and gva_peer0 != 0

        sock = socket.socket()
        sock.connect((args.head_ip, CTRL_PORT))
        with sock as conn:
            msg = _recv(conn)
            assert msg == "RANK0_READY"

            # Read rank 0's pattern A via G2H
            buf = Int32Array()
            assert handle.copy_data(gva_peer0, _ptr(buf), COPY_BYTES,
                                    bm.BmCopyType.G2H, 0) == 0
            expected_a = Int32Array()
            _fill_seq(expected_a)
            # Debug: check first/last few values
            print("[rank 1] buf[0]=", buf[0], " expected[0]=", expected_a[0], flush=True)
            print("[rank 1] buf[1]=", buf[1], " expected[1]=", expected_a[1], flush=True)
            print("[rank 1] buf[-1]=", buf[INT32_COUNT - 1], " expected[-1]=", expected_a[INT32_COUNT - 1], flush=True)
            assert _equal(buf, expected_a), "Pattern A mismatch"
            print("[rank 1] verified pattern A from rank 0", flush=True)

            # Batch G2G: read 4 blocks from rank 0
            offsets = [0, INT32_COUNT // 4, INT32_COUNT // 2, 3 * INT32_COUNT // 4]
            batch_src = [gva_peer0 + off * 4 for off in offsets]
            batch_dst = [gva_me + off * 4 for off in offsets]
            batch_len = [COPY_BYTES // 4] * 4
            assert handle.copy_data_batch(batch_src, batch_dst, batch_len, len(batch_src),
                                          bm.BmCopyType.G2G, 0) == 0
            # Verify via G2H
            block_count = INT32_COUNT // 4
            BlockArray = ctypes.c_int32 * block_count
            for i, off in enumerate(offsets):
                buf_block = BlockArray()
                assert handle.copy_data(gva_me + off * 4, _ptr(buf_block),
                                        COPY_BYTES // 4, bm.BmCopyType.G2H, 0) == 0
                for j in range(block_count):
                    assert buf_block[j] == expected_a[off + j], f"Batch block {i} mismatch at offset {j}"
            print("[rank 1] verified batch G2G from rank 0", flush=True)

            # Write pattern C to rank 0's GVA via H2G
            pattern_c = Int32Array()
            _fill_const(pattern_c, 0xCA)
            assert handle.copy_data(_ptr(pattern_c), gva_peer0, COPY_BYTES,
                                    bm.BmCopyType.H2G, 0) == 0
            print("[rank 1] wrote pattern C to rank 0 GVA", flush=True)

            _send(conn, "REMOTE_WRITE_DONE")
            _ = _recv(conn)  # RANK0_DONE

            # Write pattern B to local GVA
            pattern_b = Int32Array()
            _fill_const(pattern_b, 0xBE)
            assert handle.copy_data(_ptr(pattern_b), gva_me, COPY_BYTES,
                                    bm.BmCopyType.H2G, 0) == 0
            print("[rank 1] wrote pattern B to local GVA", flush=True)

            _send(conn, "RANK1_READY")

        # Loop 100x 4MiB reads
        for i in range(100):
            buf_loop = Int32Array()
            assert handle.copy_data(gva_peer0, _ptr(buf_loop), COPY_BYTES,
                                    bm.BmCopyType.G2H, 0) == 0
        print("[rank 1] completed 100x 4MiB loop", flush=True)

        assert handle.leave() == 0
        handle.destroy()
    finally:
        if bm_inited:
            bm.uninitialize(0)
        mf.uninitialize()
    print("[rank 1] cleanup done", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Kunpeng H2H URMA example")
    parser.add_argument("--rank", type=int, required=True, choices=[0, 1])
    parser.add_argument("--head-ip", type=str, required=True)
    parser.add_argument("--eid", type=str, required=True,
                        help="32-char hex EID for local URMA endpoint")
    args = parser.parse_args()
    if args.rank == 0:
        _run_rank0(args)
    else:
        _run_rank1(args)


if __name__ == "__main__":
    main()
