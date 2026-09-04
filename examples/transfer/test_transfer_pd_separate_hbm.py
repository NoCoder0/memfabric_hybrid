#!/usr/bin/env python3
# coding=utf-8
"""PD 分离场景：HBM + HOST_RDMA 读写双向验证 demo。

功能:
  使用 TransferEngine (PD 分离模式)，
  两端各自在 HBM 上分配 buffer，通过 HOST_RDMA 跨机读写。
  一次运行自动完成两个相位:
    写相位: Prefill --transfer_sync_write--> Decode, Decode 端校验数据
    读相位: Decode 重新填充特征值后, Prefill --transfer_sync_read--> 拉回本地校验

参数说明:
  --role          Decode=接收端(先启动) / Prefill=发送端
  --src-unique-id 本端会话地址 "ip:port", 跨机时必须用可路由 IP, 不能用 127.0.0.1
  --dst-unique-id 对端会话地址(仅 Prefill 需要), 必须与对端 src-unique-id 完全一致
  --store-url     配置存储地址, 格式 tcp://ip:port (仅协议前缀参与建链)
  --nic           RDMA 数据面地址 tcp://<本机RDMA网卡IP>:<端口>
  --npu-id        NPU 设备号, 默认取最后一张卡
  --buffer-bytes  单 buffer 大小(字节), 默认 1MB
  --batch-count   batch buffer 数量, 默认 8
  --fill-val      写相位填充基准值, 默认 42
  --log-level     0 debug / 1 info / 2 warn / 3 error, 默认 3
  --wait-timeout  Decode 等待数据的超时秒数, 默认 120

用法示例:
  # 终端1 (Decode)
  python3 test_transfer_pd_separate_hbm.py --role Decode \
      --src-unique-id "127.0.0.1:50051" --store-url tcp://127.0.0.1:8000 \
      --nic "tcp://<RDMA_IP>:7000" --npu-id 0

  # 终端2 (Prefill)
  python3 test_transfer_pd_separate_hbm.py --role Prefill \
      --src-unique-id "127.0.0.1:50052" --store-url tcp://127.0.0.1:8000 \
      --dst-unique-id "127.0.0.1:50051" \
      --nic "tcp://<RDMA_IP>:7001" --npu-id 1

结果判定:
  single=PASS batch=PASS  读写链路验证通过
"""

import argparse
import ctypes
import logging
import time

import acl
import numpy as np

from memfabric_hybrid import TransferEngine, set_log_level, set_conf_store_tls

logger = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')

PAGE = 2 * 1024 * 1024
SLOT = 64 * PAGE  # spacing between buffers (>> alignment granularity)
SINGLE_IDX = 0  # slot 0: single buffer
BATCH_IDX0 = 1  # slots 1..N: batch buffers

ACL_MEMCPY_HOST_TO_DEVICE = 1
ACL_MEMCPY_DEVICE_TO_HOST = 2


def read_expect(fill_val, i):
    """Value Decode puts into slot i for the read phase."""
    return fill_val + 50 + i + 1


def alloc_hbm(total_bytes):
    """Allocate HBM via acl.rt.malloc (PD 分离场景的外部分配方式)."""
    dev_ptr, ret = acl.rt.malloc(total_bytes, 0)
    if ret != 0:
        raise RuntimeError(f"acl.rt.malloc failed, size={total_bytes}, ret={ret}")
    return int(dev_ptr)


def free_hbm(dev_ptr):
    """Free HBM allocated by alloc_hbm."""
    if dev_ptr == 0:
        return
    ret = acl.rt.free(dev_ptr)
    if ret != 0:
        logger.warning(f"acl.rt.free failed, ptr=0x{dev_ptr:x}, ret={ret}")


def copy_hbm(dev_ptr, arr, direction):
    """Copy data between host npu array and HBM."""
    if direction == ACL_MEMCPY_HOST_TO_DEVICE:
        ret = acl.rt.memcpy(dev_ptr, arr.nbytes, arr.ctypes.data, arr.nbytes, direction)
    else:
        ret = acl.rt.memcpy(arr.ctypes.data, arr.nbytes, dev_ptr, arr.nbytes, direction)
    if ret != 0:
        raise RuntimeError(f"acl.rt.memcpy failed, dir={direction}, ret={ret}")


def read_hbm_array(ptr, nbytes):
    """Read HBM content into a host uint64 numpy array."""
    out = np.zeros(nbytes // 8, dtype=np.uint64)
    copy_hbm(ptr, out, ACL_MEMCPY_DEVICE_TO_HOST)
    return out


def write_hbm_array(ptr, arr):
    """Write a host numpy array to HBM."""
    copy_hbm(ptr, arr, ACL_MEMCPY_HOST_TO_DEVICE)


def fill_hbm(ptr, nbytes, val):
    """Fill HBM region with a constant uint64 value."""
    arr = np.full(nbytes // 8, val, dtype=np.uint64)
    write_hbm_array(ptr, arr)


class HbmArena:
    """HBM arena: allocates contiguous HBM slots for single + batch buffers.

    This is the "PD 分离" pattern — each rank allocates its own HBM,
    then registers it into TransferEngine for RDMA access.
    """

    def __init__(self, nbytes, count):
        self.nbytes = nbytes
        self.count = count
        self.spacing = SLOT
        self.base = alloc_hbm(self.spacing * (1 + count))
        self.batch_ptrs = [self.base + self.spacing * (i + 1) for i in range(count)]

        # Zero-initialize all slots
        for p in [self.base] + self.batch_ptrs:
            fill_hbm(p, nbytes, 0)

        logger.info(f"HBM arena base=0x{self.base:x} slots={1 + count} size={nbytes}")
        logger.info(f"[DECODE-BASE-ADDR] 0x{self.base:x}")

    def free(self):
        """Explicitly release the HBM allocation."""
        if self.base != 0:
            free_hbm(self.base)
            self.base = 0

    @property
    def single_ptr(self):
        return self.base

    def read_single(self):
        return read_hbm_array(self.base, self.nbytes)

    def read_batch(self, i):
        return read_hbm_array(self.batch_ptrs[i], self.nbytes)

    def write_single(self, val):
        fill_hbm(self.base, self.nbytes, val)

    def write_batch(self, i, val):
        fill_hbm(self.batch_ptrs[i], self.nbytes, val)


def run_decode(args):
    acl.init()
    count, _ = acl.rt.get_device_count()
    device_id = args.npu_id if args.npu_id >= 0 else (count - 1)
    acl.rt.set_device(device_id)

    engine = TransferEngine()
    set_log_level(args.log_level)
    set_conf_store_tls(False, "")

    nbytes = args.buffer_bytes
    arena = HbmArena(nbytes, args.batch_count)

    init_kwargs = {
        "store_url": args.store_url,
        "session_id": args.src_unique_id,
        "role": "Decode",
        "device_id": device_id,
        "data_op_type": TransferEngine.TransDataOpType.HOST_RDMA,
        "nic": args.nic,
    }
    ret = engine.initialize(**init_kwargs)
    if ret != 0:
        logger.error(f"TransferEngine initialize failed, ret={ret}")
        raise RuntimeError(f"TransferEngine initialize failed, ret={ret}")
    logger.info("TransferEngine init success, role=Decode")

    if engine.register_memory(arena.single_ptr, nbytes) != 0:
        logger.error("register_memory failed")
        engine.unInitialize()
        arena.free()
        raise RuntimeError("register_memory failed")
    logger.info("register_memory success")

    if engine.batch_register_memory(arena.batch_ptrs, [nbytes] * args.batch_count) != 0:
        logger.error("batch_register_memory failed")
        engine.destroy()
        engine.unInitialize()
        arena.free()
        raise RuntimeError("batch_register_memory failed")
    logger.info("batch_register_memory success")

    # Wait for incoming data (written by Prefill)
    waited = 0
    while waited < args.wait_timeout:
        time.sleep(2)
        waited += 2
        if np.any(arena.read_single() != 0):
            logger.info(f"data received after {waited}s")
            break
    else:
        logger.error(f"no data received after {args.wait_timeout}s")
        engine.destroy()
        engine.unInitialize()
        arena.free()
        raise RuntimeError(f"no data received after {args.wait_timeout}s")

    # Verify write phase
    ok_single = bool(np.all(arena.read_single() == args.fill_val))
    time.sleep(10)
    ok_batch = all(bool(np.all(arena.read_batch(i) == args.fill_val + i + 1)) for i in range(args.batch_count))
    logger.info(f"[RESULT] write single={'PASS' if ok_single else 'FAIL'} batch={'PASS' if ok_batch else 'FAIL'}")

    # Refill buffers for read phase
    arena.write_single(read_expect(args.fill_val, 0))
    for i in range(args.batch_count):
        arena.write_batch(i, read_expect(args.fill_val, i + 1))
    logger.info("buffers refilled for read test, holding 60s...")
    time.sleep(60)

    # Cleanup: destroy engine first, then release HBM
    engine.destroy()
    engine.unInitialize()
    arena.free()
    logger.info("done")


def run_prefill(args):
    acl.init()
    count, _ = acl.rt.get_device_count()
    device_id = args.npu_id if args.npu_id >= 0 else (count - 1)
    acl.rt.set_device(device_id)

    engine = TransferEngine()
    set_log_level(args.log_level)
    set_conf_store_tls(False, "")

    nbytes = args.buffer_bytes
    spacing = SLOT

    # Local HBM arena (Prefill side)
    local_base = alloc_hbm(spacing * (1 + args.batch_count))
    single_ptr = local_base
    batch_ptrs = [local_base + spacing * (i + 1) for i in range(args.batch_count)]

    # Decode's HBM base address (provided via --dst-base-addr)
    dst_base = int(args.dst_base_addr, 16)
    peer_single = dst_base
    peer_batch = [dst_base + spacing * (i + 1) for i in range(args.batch_count)]

    # Fill local HBM with test data
    fill_hbm(single_ptr, nbytes, args.fill_val)
    for i, p in enumerate(batch_ptrs):
        fill_hbm(p, nbytes, args.fill_val + i + 1)

    logger.info(f"HBM src arena base=0x{local_base:x} peer base=0x{dst_base:x}")

    init_kwargs = {
        "store_url": args.store_url,
        "session_id": args.src_unique_id,
        "role": "Prefill",
        "device_id": device_id,
        "data_op_type": TransferEngine.TransDataOpType.HOST_RDMA,
        "nic": args.nic,
    }
    ret = engine.initialize(**init_kwargs)
    if ret != 0:
        logger.error(f"TransferEngine initialize failed, ret={ret}")
        raise RuntimeError(f"TransferEngine initialize failed, ret={ret}")
    logger.info("TransferEngine init success, role=Prefill")

    if engine.register_memory(single_ptr, nbytes) != 0:
        logger.error("register_memory failed")
        engine.destroy()
        engine.unInitialize()
        free_hbm(local_base)
        raise RuntimeError("register_memory failed")

    if engine.batch_register_memory(batch_ptrs, [nbytes] * args.batch_count) != 0:
        logger.error("batch_register_memory failed")
        engine.destroy()
        engine.unInitialize()
        free_hbm(local_base)
        raise RuntimeError("batch_register_memory failed")

    logger.info("waiting 10s for Decode to register...")
    time.sleep(10)

    # Write phase: push local HBM data to Decode's HBM
    ret = engine.transfer_sync_write(args.dst_unique_id, single_ptr, peer_single, nbytes)
    logger.info(f"transfer_sync_write ret={ret}")
    ret = engine.batch_transfer_sync_write(args.dst_unique_id, batch_ptrs, peer_batch, [nbytes] * args.batch_count)
    logger.info(f"batch_transfer_sync_write ret={ret}")

    # Wait for Decode to verify write phase and refill buffers
    time.sleep(25)

    # Read phase: pull Decode's refilled HBM data back to local
    ret = engine.transfer_sync_read(args.dst_unique_id, single_ptr, peer_single, nbytes)
    ok_read_single = ret == 0 and bool(np.all(read_hbm_array(single_ptr, nbytes) == read_expect(args.fill_val, 0)))
    logger.info(f"transfer_sync_read ret={ret}")

    ret = engine.batch_transfer_sync_read(args.dst_unique_id, batch_ptrs, peer_batch, [nbytes] * args.batch_count)
    ok_read_batch = ret == 0 and all(
        bool(np.all(read_hbm_array(batch_ptrs[i], nbytes) == read_expect(args.fill_val, i + 1)))
        for i in range(args.batch_count)
    )
    logger.info(f"batch_transfer_sync_read ret={ret}")

    logger.info(
        f"[RESULT] read single={'PASS' if ok_read_single else 'FAIL'} batch={'PASS' if ok_read_batch else 'FAIL'}"
    )

    time.sleep(20)
    # Cleanup: destroy engine first, then release HBM
    engine.destroy()
    engine.unInitialize()
    free_hbm(local_base)
    logger.info("done")


def main():
    parser = argparse.ArgumentParser(description="PD 分离场景 HBM + HOST_RDMA 传输验证")
    parser.add_argument("--role", required=True, choices=["Decode", "Prefill"])
    parser.add_argument("--src-unique-id", required=True)
    parser.add_argument("--dst-unique-id", default=None)
    parser.add_argument("--store-url", required=True)
    parser.add_argument("--nic", required=True)
    parser.add_argument(
        "--dst-base-addr",
        default=None,
        help="Decode's [DECODE-BASE-ADDR] value (required for Prefill)",
    )
    parser.add_argument("--npu-id", type=int, default=-1)
    parser.add_argument("--buffer-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--batch-count", type=int, default=8)
    parser.add_argument("--fill-val", type=int, default=42)
    parser.add_argument("--log-level", type=int, default=3, choices=[0, 1, 2, 3])
    parser.add_argument("--wait-timeout", type=int, default=120)
    args = parser.parse_args()

    # 验证 —dst-base-addr 只由 Decode 端打印，Prefill 端需要传入
    if args.role == "Prefill" and not args.dst_base_addr:
        parser.error("--dst-base-addr is required for Prefill (use the value Decode prints as [DECODE-BASE-ADDR])")

    if args.role == "Decode":
        run_decode(args)
    else:
        run_prefill(args)


if __name__ == "__main__":
    main()
