#!/usr/bin/env python3
# coding=utf-8
"""PD 分离 + DP 多实例场景：HBM 全矩阵（P×D）跨实例读写验证 demo。

对齐 sglang 开启 DP（--dp-size > 1）后的 PD 分离拓扑：**每个 Prefill 实例都会与
每个 Decode 实例建立连接**（P_i ↔ D_j 全矩阵），KV 池被多对端访问/注册。

本脚本把该拓扑压到最小可验证形态：

  - Decode 侧：N 个独立实例（D rank 0..N-1），各自进程 / 各自 NPU 设备 / 各自内嵌
    store server / 各自 HBM arena；arena 被所有 P 依序复用（pair 串行，无并发写）。
  - Prefill 侧：N 个独立实例（P rank 0..N-1），各自进程 / 各自 NPU 设备；每个 P_i
    与**全部** D_j 建连并完成一轮完整的 push/pull 校验（值同时编码 src P rank 与
    dst D rank，跨实例串扰必然不匹配）。
  - 连接矩阵：P_i 对 D_0..D_{N-1} 各有一条 smem 连接（懒建连 + 注册重放），
    共 N×N 条 —— 这正是多 P 进程共享同一批 D store 的 Whitelist 压测形态。

结果呈现（终端直接给结论，完整日志落在 log 文件）：

  - launcher 模式：启动器逐个打印 "P{r}/D{r}  PASS|FAIL"，并给出 "RESULT: PASS|FAIL"；
  - 手动模式：实例自身打印 "[P{r}] PASS" / "[D{r}] PASS"，main 同样给出 "RESULT" 行；
  - 任一 (P,D) 对校验失败 ⇒ 失败实例的日志尾部回显到终端，脚本以非 0 退出码结束。

参数说明:

  --role            Prefill=发起端 / Decode=接收端（两侧均有 DP 多实例；
                    建议【先启动 Decode】——控制服务端在 P 侧，为被动 accept，
                    D 的连接带主动重试，容忍 P 晚起）
  --local-ip        本机 IPv4，默认 127.0.0.1；跨机须传对端可达的本机网卡 IP
  --prefill-ip      Prefill 机器 IPv4（仅 Decode 侧需要），跨机须传真实 Prefill IP
  --prefill-ips     多 P 节点时传逗号分隔的全部 Prefill 节点 IP（仅 Decode 侧），设置后覆盖 --prefill-ip；
                    全局 p_rank r 由节点 r//ranks_per_node 承载、端口 base + r%ranks_per_node
  --control-port    控制通道基础端口，默认 29598；P rank r 实际使用 base+r
  --dp-size         实例数（P 与 D 各 N 个，N×N 全矩阵），默认 2
  --p-rank          Prefill 本实例 rank；缺省时一条命令拉起全部 P 实例
  --dp-rank         Decode 本实例 rank；缺省时一条命令拉起全部 D 实例
  --npu-id          整块设备起始号（launcher 模式必须 >=0）：P 块占起始起的 ceil(N/K) 张，
                    D 块紧随其后；K = --instances-per-card（默认 1，即 2*N 张卡）
  --instances-per-card  单卡承载的实例数（P 块与 D 块仍分卡；>1 时同卡多进程，
                    用于在少量卡上把 store rank 顶到 16+ 还原撞键环境）
  --data-op-type    数据面类型，覆盖 transfer 适配器已暴露的全部协议（默认 HOST_RDMA）：
                      HOST_RDMA    主机侧 RDMA（跨机），需 --nic
                      SDMA         设备侧 SDMA（单机自测 / 同超节点跨机），无需 --nic
                      DEVICE_RDMA  设备侧 RDMA（跨机），无需 --nic
                      DEVICE_URMA  设备侧 URMA（跨机，A5），无需 --nic
                      DEVICE_UBOE  设备侧 UBoE（跨机），无需 --nic
                    注：trans 核心另支持 HOST_URMA / HOST_TCP，但 Python 适配器未暴露
                    对应枚举，如需覆盖须先扩展 pytransfer 绑定。
  --nic             HOST_RDMA 数据面地址 tcp://<本机RDMA网卡IP>:<端口>（仅 HOST_RDMA 生效，
                    device 类协议忽略）；launcher 按 P_r=base+r、D_j=base+dp_size+j 偏移端口
  --buffer-bytes    单 buffer 大小(字节)，默认 1MB
  --batch-count     每 rank batch buffer 数量，默认 8
  --fill-val        写相位填充基准值，默认 42
  --log-level       0 debug / 1 info / 2 warn / 3 error，默认 1
  --timeout         控制通道超时秒数，默认 120

用法示例（单机 2 P × 2 D：P 用卡 0/1，D 用卡 2/3；先启动 Decode）:

  # 终端1 (Decode，先启动，一条命令拉起 D0/D1)
  python3 test_transfer_pd_separate_hbm_dp.py --role Decode --dp-size 2 \\
      --prefill-ip 127.0.0.1 --npu-id 0 --nic "tcp://127.0.0.1:9000"

  # 终端2 (Prefill，一条命令拉起 P0/P1，各自持 2 条连接)
  python3 test_transfer_pd_separate_hbm_dp.py --role Prefill --dp-size 2 \\
      --local-ip 127.0.0.1 --npu-id 0 --nic "tcp://127.0.0.1:7000"

最简 1 P × 1 D 用法（原 test_transfer_pd_separate_hbm.py 场景已合入本脚本：旧版手工复制
[DECODE-BASE-ADDR]、--dst-unique-id 与 sleep 同步相位，均由控制协议自动完成；launcher 模式
P 用卡 0、D 用卡 1）:

  # 终端1 (Decode，先启动)
  python3 test_transfer_pd_separate_hbm_dp.py --role Decode --dp-size 1 \\
      --npu-id 0 --nic "tcp://<RDMA_IP>:9000"

  # 终端2 (Prefill)
  python3 test_transfer_pd_separate_hbm_dp.py --role Prefill --dp-size 1 \\
      --npu-id 0 --nic "tcp://<RDMA_IP>:7000"

多 P 节点 × 多 D 节点（如 2 P 机 × 2 D 机）：launcher 只支持单节点，两侧都改用手动模式逐实例拉起
（显式 --p-rank / --dp-rank），D 侧用 --prefill-ips 传全部 P 节点 IP。单机可用 127.0.0.1/127.0.0.2
等回环 IP 模拟多节点；跨机时 SDMA 需同超节点、HOST_RDMA 需加 --nic，DEVICE_RDMA / DEVICE_URMA /
DEVICE_UBOE 走设备侧传输无需 --nic（把 --data-op-type 换成对应值即可）。先启动全部 D 实例:

  # P 节点0: python3 test_transfer_pd_separate_hbm_dp.py --role Prefill --p-rank 0 \\
      --local-ip <P0_IP> --npu-id 0 --dp-size 2 --data-op-type SDMA
  # P 节点1: ... --role Prefill --p-rank 1 --local-ip <P1_IP> --npu-id 1 --dp-size 2 --data-op-type SDMA
  # D 节点0: python3 test_transfer_pd_separate_hbm_dp.py --role Decode --dp-rank 0 \\
      --local-ip <D0_IP> --npu-id 2 --dp-size 2 --prefill-ips <P0_IP>,<P1_IP> --data-op-type SDMA
  # D 节点1: ... --role Decode --dp-rank 1 --local-ip <D1_IP> --npu-id 3 --dp-size 2 \\
      --prefill-ips <P0_IP>,<P1_IP> --data-op-type SDMA

结果判定:

  终端最后一行 "RESULT: PASS" 表示全矩阵读写链路验证通过；"RESULT: FAIL — <明细>" 表示
  失败（退出码非 0），失败实例的日志尾部会一并回显，完整日志见
  /tmp/pd_matrix_<control_port>/<p|d>_<rank>.log。

注意（同机多实例的两个约束）:

  1. Prefill 与 Decode 不要共用同一张卡（两侧各自 acl.rt.malloc 抢显存，且该约束与 PD 用例一致）；
     共需 2*dp_size 张卡。
  2. HOST_RDMA 下每个 entity 的 HCOM 端口 = nic 基础端口 + 实例内偏移，被占用时框架在
     +1..+8 范围内重试；同机多实例 / 单进程多连接时基础端口要留出余量，否则 bind 会失败。
"""

import argparse
import contextlib
import ipaddress
import json
import logging
import os
import socket
import subprocess
import sys
import time

import acl
import numpy as np

from memfabric_hybrid import TransferEngine, TransDataOpType, set_conf_store_tls, set_log_level

logger = logging.getLogger(__name__)

PAGE = 2 * 1024 * 1024
SLOT = 64 * PAGE  # spacing between buffers (>> alignment granularity)

ACL_MEMCPY_HOST_TO_DEVICE = 1
ACL_MEMCPY_DEVICE_TO_HOST = 2

ROLE_PREFILL = "Prefill"
ROLE_DECODE = "Decode"

DEFAULT_CONTROL_PORT = 29598
DEFAULT_LOG_LEVEL = 1
MAX_CONTROL_MESSAGE_BYTES = 64 * 1024
LOG_LEVELS = {0: logging.DEBUG, 1: logging.INFO, 2: logging.WARNING, 3: logging.ERROR}
# transfer 适配器已暴露的全部数据面协议
DATA_OP_TYPES = {
    "SDMA": TransDataOpType.SDMA,
    "HOST_RDMA": TransDataOpType.HOST_RDMA,
    "DEVICE_RDMA": TransDataOpType.DEVICE_RDMA,
    "DEVICE_URMA": TransDataOpType.DEVICE_URMA,
    "DEVICE_UBOE": TransDataOpType.DEVICE_UBOE,
}
# 仅主机侧 RDMA 把 nic 作为 transUrl；device 类协议由设备侧传输，忽略 nic
NIC_REQUIRED_TYPES = {"HOST_RDMA"}

# 失败实例回显到终端的日志尾部行数（完整日志仍保留在实例 log 文件中）
LOG_TAIL_LINES = 20

# 值同时编码 src P rank 与 dst D rank（外加槽位），任意维度串扰必然不匹配
P_VALUE_STRIDE = 1000
D_VALUE_STRIDE = 1000 * 1000
# 读相位的值带整体上移，避免与写相位的值混淆（读回旧值也能被识别）
READ_PHASE_OFFSET = 500000


def write_expect(fill_val, p_rank, d_rank, slot):
    """写相位：P_src 写向 D_dst 的期望值。slot 0 = single，i>0 = batch[i-1]。"""
    return fill_val + P_VALUE_STRIDE * p_rank + D_VALUE_STRIDE * d_rank + slot


def read_expect(fill_val, p_rank, d_rank, slot):
    """读相位：D 重新填充、待 P 拉回的值。"""
    return fill_val + READ_PHASE_OFFSET + P_VALUE_STRIDE * p_rank + D_VALUE_STRIDE * d_rank + slot


def _ipv4(value):
    try:
        address = ipaddress.ip_address(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid IP address: {value}") from error
    if address.version != 4:
        raise argparse.ArgumentTypeError("MemFabric test requires a numeric IPv4 address")
    return str(address)


def resolve_device_id(npu_id):
    """解析设备号：-1 表示最后一张卡（与 PD 用例一致）。"""
    count, _ = acl.rt.get_device_count()
    if count <= 0:
        raise RuntimeError(f"no NPU device available, deviceCount={count}")
    device_id = npu_id if npu_id >= 0 else (count - 1)
    if device_id >= count:
        raise RuntimeError(f"invalid npu id, npuId={npu_id}, deviceCount={count}")
    return device_id


# ---------------------------- HBM helpers (acl) ----------------------------


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
    """Copy data between host numpy array and HBM."""
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

    PD 分离模式下每个实例各自分配 HBM，再注册进自己的 TransferEngine。
    全矩阵模式下 D 的 arena 被 N 个 P 依序复用（pair 串行，无并发写）。
    """

    def __init__(self, nbytes, count):
        self.nbytes = nbytes
        self.count = count
        self.spacing = SLOT
        self.base = alloc_hbm(self.spacing * (1 + count))
        self.batch_ptrs = [self.base + self.spacing * (i + 1) for i in range(count)]

        for p in [self.base] + self.batch_ptrs:
            fill_hbm(p, nbytes, 0)

        logger.info(f"HBM arena base=0x{self.base:x} slots={1 + count} size={nbytes}")

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


# ---------------------------- control channel (plain TCP JSON) ----------------------------


def send_json(sock, message):
    sock.sendall(json.dumps(message).encode() + b"\n")


def recv_json(sock):
    payload = bytearray()
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("control connection closed before a complete message")
        payload.extend(chunk)
        if len(payload) > MAX_CONTROL_MESSAGE_BYTES:
            raise ValueError("control message too large")
        newline = payload.find(b"\n")
        if newline >= 0:
            return json.loads(payload[:newline])


def expect_phase(message, phase):
    """校验控制消息 phase；对端报错时直接抛出对端错误。"""
    if isinstance(message, dict) and message.get("phase") == "error":
        raise RuntimeError(f"peer reported error: {message.get('error', message)}")
    got = message.get("phase") if isinstance(message, dict) else type(message).__name__
    if got != phase:
        raise RuntimeError(f"protocol error: expected phase {phase!r}, got {got!r}: {message}")


def connect_with_retry(host, port, timeout):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            remaining = max(deadline - time.monotonic(), 0.1)
            return socket.create_connection((host, port), timeout=min(5.0, remaining))
        except OSError as error:
            last_error = error
            time.sleep(0.5)
    raise TimeoutError(f"timed out connecting to {host}:{port}: {last_error}")


# ---------------------------- engine ----------------------------


def init_engine(role, args, device_id):
    """初始化 TransferEngine 并返回 (engine, session)。

    store_server_role 用默认值 "Decode"：Decode 实例各自内嵌 store server（绑定自己的
    session 端口），Prefill 是纯 client —— 只有 client 模式（handle_ == nullptr）才会走
    "按目标 session 懒建连 + 重放已注册内存" 的多连接路径，这正是本用例要覆盖的。
    store_url 仅提供协议前缀，真实 store 地址取自对端 session。
    """
    engine = TransferEngine()
    ret = engine.initialize(
        store_url=f"tcp://{args.local_ip}",
        session_id=args.local_ip,
        role=role,
        device_id=device_id,
        data_op_type=DATA_OP_TYPES[args.data_op_type],
        nic=args.nic or "",
    )
    if ret != 0:
        logger.error(f"TransferEngine initialize failed, ret={ret}, role={role}, deviceId={device_id}")
        raise RuntimeError(f"TransferEngine initialize failed, ret={ret}, role={role}, deviceId={device_id}")
    session = f"{args.local_ip}:{engine.get_rpc_port()}"
    logger.info(f"engine ready: role={role} session={session} dataOpType={args.data_op_type}")
    return engine, session


def register_arena(engine, arena, tag):
    """注册 arena 的 single + batch。"""
    if engine.register_memory(arena.single_ptr, arena.nbytes) != 0:
        logger.error(f"{tag} register_memory failed, addr=0x{arena.single_ptr:x}, size={arena.nbytes}")
        return False
    if engine.batch_register_memory(arena.batch_ptrs, [arena.nbytes] * arena.count) != 0:
        logger.error(f"{tag} batch_register_memory failed, count={arena.count}, size={arena.nbytes}")
        return False
    logger.info(f"{tag} register_memory success, single=0x{arena.single_ptr:x} batch={arena.count}")
    return True


def teardown_engine(engine, arena):
    """先销毁引擎再释放 HBM（与 PD 用例一致的收尾顺序）。"""
    engine.destroy()
    engine.unInitialize()
    if arena is not None:
        arena.free()


# ---------------------------- pair protocol (per (P_i, D_j) connection) ----------------------------


def verify_write_phase(arena, args, p_rank):
    """D 侧校验写相位：single 与 batch 必须逐槽等于 (P_src=p_rank, D_self) 的专属值。"""
    d_rank = args.dp_rank
    ok_single = bool(np.all(arena.read_single() == write_expect(args.fill_val, p_rank, d_rank, 0)))
    ok_batch = all(
        bool(np.all(arena.read_batch(i) == write_expect(args.fill_val, p_rank, d_rank, i + 1)))
        for i in range(arena.count)
    )
    logger.info(
        f"[D{d_rank}] write phase (from P{p_rank}) single={'PASS' if ok_single else 'FAIL'} "
        f"batch={'PASS' if ok_batch else 'FAIL'} "
        f"(expect single={write_expect(args.fill_val, p_rank, d_rank, 0)})"
    )
    return ok_single, ok_batch


def refill_for_read(arena, args, p_rank):
    """读相位前重填 arena：值带 (P_src, D_self) 标签，P 拉回后据此判断来源。"""
    d_rank = args.dp_rank
    arena.write_single(read_expect(args.fill_val, p_rank, d_rank, 0))
    for i in range(arena.count):
        arena.write_batch(i, read_expect(args.fill_val, p_rank, d_rank, i + 1))


def serve_pair_as_decode(conn, args, engine, arena, p_rank):
    """D 侧单个 pair 的服务流程：init → arena_ready → 校验写 → 重填 → final。

    arena 由本 D 实例的全部 pair 依序复用（控制连接逐条处理，天然串行）。
    返回本 pair 是否通过。
    """
    d_rank = args.dp_rank
    init_message = recv_json(conn)
    expect_phase(init_message, "init")
    if int(init_message.get("p_rank", -1)) != p_rank or int(init_message.get("d_rank", -1)) != d_rank:
        raise RuntimeError(f"pair mismatch: init p={init_message.get('p_rank')} d={init_message.get('d_rank')}")
    if int(init_message.get("dp_size", -1)) != args.dp_size:
        raise RuntimeError(f"dp_size mismatch: prefill={init_message.get('dp_size')} local={args.dp_size}")

    send_json(
        conn,
        {
            "phase": "arena_ready",
            "p_rank": p_rank,
            "d_rank": d_rank,
            "session": args.session,
            "single_base": arena.single_ptr,
            "batch_bases": arena.batch_ptrs,
        },
    )

    expect_phase(recv_json(conn), "verify_write")
    ok_single, ok_batch = verify_write_phase(arena, args, p_rank)
    send_json(
        conn,
        {
            "phase": "write_result",
            "p_rank": p_rank,
            "d_rank": d_rank,
            "ok": bool(ok_single and ok_batch),
        },
    )

    expect_phase(recv_json(conn), "refill_read")
    refill_for_read(arena, args, p_rank)
    send_json(conn, {"phase": "refilled", "p_rank": p_rank, "d_rank": d_rank})

    final = recv_json(conn)
    expect_phase(final, "final")
    logger.info(f"[D{d_rank}] pair (P{p_rank},D{d_rank}) final: {final.get('detail', '')}")
    return bool(final.get("ok"))


# ---------------------------- Decode (DP instance, serves all P pairs) ----------------------------


def resolve_prefill_endpoints(args):
    """把全局 p_rank 映射到 (Prefill 节点 IP, 控制端口)。

    单节点（默认）全部 rank 落在 --prefill-ip，端口 base+r 与 P 侧 launcher 对齐；多 P 节点时
    （--prefill-ips）按 ranks_per_node 均分：全局 p_rank r → 节点 r//ranks_per_node，
    端口 base + r%ranks_per_node —— 与 P 侧手动模式（--p-rank r --control-port base+local_rank）对应。
    """
    ips = [s.strip() for s in args.prefill_ips.split(",") if s.strip()] if args.prefill_ips else [args.prefill_ip]
    ranks_per_node = (args.dp_size + len(ips) - 1) // len(ips)
    return [(ips[rank // ranks_per_node], args.control_port + rank % ranks_per_node) for rank in range(args.dp_size)]


def run_decode_instance(args):
    set_log_level(args.log_level)
    set_conf_store_tls(False, "")

    acl.init()
    device_id = resolve_device_id(args.npu_id)
    acl.rt.set_device(device_id)

    engine, session = init_engine(ROLE_DECODE, args, device_id)
    arena = HbmArena(args.buffer_bytes, args.batch_count)
    register_arena(engine, arena, f"[D{args.dp_rank}]")
    args.session = session

    # 依次与每个 P 实例完成一个 pair（控制连接逐条处理，串行无并发写）
    endpoints = resolve_prefill_endpoints(args)
    failures = []
    for p_rank in range(args.dp_size):
        prefill_ip, control_port = endpoints[p_rank]
        try:
            logger.info(f"[D{args.dp_rank}] connecting P{p_rank} at {prefill_ip}:{control_port}")
            with connect_with_retry(prefill_ip, control_port, args.timeout) as conn:
                conn.settimeout(args.timeout)
                send_json(conn, {"phase": "hello", "dp_rank": args.dp_rank, "p_rank": p_rank})
                if not serve_pair_as_decode(conn, args, engine, arena, p_rank):
                    failures.append({"p_rank": p_rank, "d_rank": args.dp_rank})
        except Exception as error:
            logger.error(f"[D{args.dp_rank}] pair (P{p_rank},D{args.dp_rank}) failed: {type(error).__name__}: {error}")
            failures.append({"p_rank": p_rank, "d_rank": args.dp_rank, "error": str(error)})

    teardown_engine(engine, arena)
    logger.info(f"[D{args.dp_rank}] cleanup done")
    if failures:
        raise RuntimeError(f"DP instance {args.dp_rank} verification failed: {failures}")
    logger.info(f"[D{args.dp_rank}] PASS")


# ---------------------------- Prefill (instance, connects to all D) ----------------------------


def push_to_peer(engine, local, peer, args, p_rank, d_rank):
    """向 D_dst 写 single + batch（值编码 src/dst），返回失败明细。

    懒建连后首笔传输可能早于 transport 注册完成（ret=-1），对首笔做有限重试。
    """
    nbytes = local.nbytes
    failures = []
    local.write_single(write_expect(args.fill_val, p_rank, d_rank, 0))
    ret = -1
    for attempt in range(3):
        ret = engine.transfer_sync_write(peer["session"], local.single_ptr, peer["single"], nbytes)
        if ret == 0:
            break
        logger.warning(f"[P{p_rank}] write single retry {attempt + 1}, d_rank={d_rank}, ret={ret}")
        time.sleep(0.2)
    if ret != 0:
        logger.error(f"[P{p_rank}] write single failed, d_rank={d_rank}, ret={ret}")
        failures.append({"p_rank": p_rank, "d_rank": d_rank, "slot": "single", "ret": ret})
    for i in range(local.count):
        local.write_batch(i, write_expect(args.fill_val, p_rank, d_rank, i + 1))
    ret = engine.batch_transfer_sync_write(peer["session"], local.batch_ptrs, peer["batch"], [nbytes] * local.count)
    if ret != 0:
        logger.error(f"[P{p_rank}] write batch failed, d_rank={d_rank}, ret={ret}")
        failures.append({"p_rank": p_rank, "d_rank": d_rank, "slot": "batch", "ret": ret})
    return failures


def _slot_matches(ptr, nbytes, expect):
    return bool(np.all(read_hbm_array(ptr, nbytes) == expect))


def pull_from_peer(engine, local, peer, args, p_rank, d_rank):
    """从 D_dst 读 single + batch 并立即校验，返回失败明细。

    读回仍写入 local 同一个 buffer：若某次读静默失败，校验会拿到上一次的值而失败。
    首笔读同样可能早于 transport 就绪，做有限重试（重试间重填期望值，防脏数据假通过）。
    """
    nbytes = local.nbytes
    failures = []
    ret = -1
    for attempt in range(3):
        ret = engine.transfer_sync_read(peer["session"], local.single_ptr, peer["single"], nbytes)
        if ret == 0 and _slot_matches(local.single_ptr, nbytes, read_expect(args.fill_val, p_rank, d_rank, 0)):
            break
        logger.warning(f"[P{p_rank}] read single retry {attempt + 1}, d_rank={d_rank}, ret={ret}")
        time.sleep(0.2)
    if ret != 0 or not _slot_matches(local.single_ptr, nbytes, read_expect(args.fill_val, p_rank, d_rank, 0)):
        logger.error(f"[P{p_rank}] read single failed, d_rank={d_rank}, ret={ret}")
        failures.append({"p_rank": p_rank, "d_rank": d_rank, "slot": "single", "ret": ret})
    ret = engine.batch_transfer_sync_read(peer["session"], local.batch_ptrs, peer["batch"], [nbytes] * local.count)
    bad_slots = [
        i
        for i in range(local.count)
        if not _slot_matches(local.batch_ptrs[i], nbytes, read_expect(args.fill_val, p_rank, d_rank, i + 1))
    ]
    if ret != 0 or bad_slots:
        logger.error(f"[P{p_rank}] read batch failed, d_rank={d_rank}, ret={ret}, badSlots={bad_slots}")
        failures.append({"p_rank": p_rank, "d_rank": d_rank, "slot": "batch", "ret": ret, "badSlots": bad_slots})
    return failures


def run_pair_as_prefill(engine, local, conn, args, d_rank):
    """P 侧单个 pair 的驱动流程：init → arena_ready → push → 校验 → pull → final。

    返回 (本 pair 是否通过, 失败明细)。
    """
    p_rank = args.p_rank
    send_json(
        conn,
        {
            "phase": "init",
            "dp_size": args.dp_size,
            "p_rank": p_rank,
            "d_rank": d_rank,
            "buffer_bytes": args.buffer_bytes,
            "batch_count": args.batch_count,
        },
    )

    ready = recv_json(conn)
    expect_phase(ready, "arena_ready")
    batch_bases = ready.get("batch_bases", [])
    valid = (
        int(ready.get("d_rank", -1)) == d_rank
        and int(ready.get("p_rank", -1)) == p_rank
        and isinstance(ready.get("session"), str)
        and int(ready.get("single_base", 0)) != 0
        and len(batch_bases) == args.batch_count
    )
    if not valid:
        raise RuntimeError(f"invalid arena_ready from D{d_rank}: {ready}")
    peer = {"session": ready["session"], "single": int(ready["single_base"]), "batch": batch_bases}
    logger.info(f"[P{p_rank}] pair (P{p_rank},D{d_rank}) arena ready, session={ready['session']}")

    failures = push_to_peer(engine, local, peer, args, p_rank, d_rank)
    send_json(conn, {"phase": "verify_write"})
    write_result = recv_json(conn)
    expect_phase(write_result, "write_result")
    write_ok = not failures and bool(write_result.get("ok"))

    send_json(conn, {"phase": "refill_read"})
    expect_phase(recv_json(conn), "refilled")
    read_failures = pull_from_peer(engine, local, peer, args, p_rank, d_rank)
    read_ok = not read_failures

    pair_ok = write_ok and read_ok
    detail = f"pair(P{p_rank},D{d_rank}) write={'PASS' if write_ok else 'FAIL'} read={'PASS' if read_ok else 'FAIL'}"
    send_json(conn, {"phase": "final", "ok": pair_ok, "detail": detail})
    logger.info(f"[P{p_rank}] {detail}")
    return pair_ok, failures + read_failures


def run_prefill_instance(args):
    """P 实例主流程：init → 注册 → 依次与每个 D 完成 pair → 聚合结论。"""
    acl.init()
    device_id = resolve_device_id(args.npu_id)
    acl.rt.set_device(device_id)
    set_log_level(args.log_level)
    set_conf_store_tls(False, "")

    engine, _ = init_engine(ROLE_PREFILL, args, device_id)
    local = HbmArena(args.buffer_bytes, args.batch_count)
    register_arena(engine, local, f"[P{args.p_rank}]")

    # 关键顺序：控制通道是裸 TCP，此刻还没有任何指向 D 的 smem 连接；register 只落在
    # adapter 台账里，每个 pair 懒建连时由重放把这份注册补投到对应的新 entity。
    failures = []
    served = set()
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.local_ip, args.control_port))
        server.listen(args.dp_size)
        server.settimeout(args.timeout)

        for _ in range(args.dp_size):
            conn, peer = server.accept()
            conn.settimeout(args.timeout)
            try:
                hello = recv_json(conn)
                d_rank = hello.get("dp_rank") if isinstance(hello, dict) else None
                valid = (
                    isinstance(hello, dict)
                    and hello.get("phase") == "hello"
                    and isinstance(d_rank, int)
                    and 0 <= d_rank < args.dp_size
                    and d_rank not in served
                )
                if not valid:
                    raise RuntimeError(f"invalid Decode hello from {peer}: {hello}")
                logger.info(f"[P{args.p_rank}] D{d_rank} connected from {peer[0]}:{peer[1]}")
                pair_ok, pair_failures = run_pair_as_prefill(engine, local, conn, args, d_rank)
                served.add(d_rank)
                if not pair_ok:
                    failures.extend(pair_failures)
            except Exception as error:
                with contextlib.suppress(OSError):
                    send_json(conn, {"phase": "error", "error": f"{type(error).__name__}: {error}"})
                raise
            finally:
                conn.close()

    teardown_engine(engine, local)
    if len(served) != args.dp_size or failures:
        raise RuntimeError(f"P instance {args.p_rank} verification failed: served={sorted(served)}, {failures}")
    logger.info(f"[P{args.p_rank}] PASS: pairs={sorted(served)}")


# ---------------------------- launchers (one command, N instances per side) ----------------------------


def offset_nic_port(nic, offset):
    """把 nic 的端口按偏移量平移：同机多实例的 HCOM 端口需互相错开。"""
    if not nic:
        return ""
    prefix, _, port_text = nic.rpartition(":")
    if not port_text.isdigit():
        return nic
    return f"{prefix}:{int(port_text) + offset}"


def _instance_argv(base_argv, extras):
    return [sys.executable, os.path.abspath(__file__)] + list(base_argv) + extras


def _strip_args(argv, names):
    """剔除 argv 中的 --name value / --name=value 形式的参数。"""
    result = []
    skip_next = False
    for item in argv:
        if skip_next:
            skip_next = False
            continue
        if item in names:
            skip_next = True
            continue
        if any(item.startswith(f"{name}=") for name in names):
            continue
        result.append(item)
    return result


def _instance_log_dir(args):
    """每个实例的输出单独落盘，避免多进程共享终端互相 interleaved、多轮日志混在一起。"""
    log_dir = os.path.join("/tmp", f"pd_matrix_{args.control_port}")
    os.makedirs(log_dir, exist_ok=True)
    return log_dir


def _validate_device_budget(args):
    """launcher 模式提前校验设备预算：P 块 + D 块，共 2*ceil(dp_size/per_card) 张卡。

    两个 launcher 的 --npu-id 语义都是"整块设备的起始号"，
    传 1 会导致 D 侧最高位实例越界（如 npuId=8, deviceCount=8）。
    instances_per_card > 1 时同卡多实例（各进程独立 ACL 上下文/地址空间，arena 分区）。
    """
    acl.init()
    count, ret = acl.rt.get_device_count()
    if ret != 0 or count <= 0:
        raise RuntimeError(f"acl.rt.get_device_count failed, ret={ret}, count={count}")
    per_card = max(1, args.instances_per_card)
    cards = 2 * ((args.dp_size + per_card - 1) // per_card)
    if args.npu_id < 0 or args.npu_id + cards > count:
        raise RuntimeError(
            f"device budget insufficient: need {cards} cards "
            f"([{args.npu_id}, {args.npu_id + cards})) but deviceCount={count}；"
            f"两个 launcher 的 --npu-id 都应传整块起始号（8 卡跑 dp-size 4 时两侧都传 0），"
            f"或减小 --dp-size / 增大 --instances-per-card"
        )
    logger.info(
        f"[launcher] device budget OK: {cards} cards [{args.npu_id}, {args.npu_id + cards}) of {count}, "
        f"instances_per_card={per_card}"
    )


def _log_tail(log_path, max_lines=LOG_TAIL_LINES):
    """读取实例日志末尾若干行，便于失败时直接在终端看到关键错误。"""
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as log_file:
            lines = log_file.read().splitlines()
    except OSError as error:
        print(f"[launcher] read log failed, path={log_path}, error={error}")
        return []
    return lines[-max_lines:]


def _report_instances(side, tag, children):
    """等待子进程退出，逐个打印 PASS/FAIL 并回显失败者日志尾部；返回是否全部通过。

    打印用 print 而非 logger，保证 --log-level 调高（如 3=ERROR）时结论依然可见。
    """
    outcomes = [(rank, child.wait(), log_path) for rank, child, log_path in children]
    passed_count = sum(1 for _, code, _ in outcomes if code == 0)
    print(f"[launcher] ===== {side} summary: {passed_count}/{len(outcomes)} PASS =====")
    for rank, code, log_path in outcomes:
        if code == 0:
            print(f"[launcher] {tag}{rank}  PASS  exit=0  log={log_path}")
            continue
        print(f"[launcher] {tag}{rank}  FAIL  exit={code}  log={log_path}")
        for line in _log_tail(log_path):
            print(f"[launcher]   | {line}")
    all_passed = passed_count == len(outcomes)
    print(f"[launcher] {side} overall: {'PASS' if all_passed else 'FAIL'}")
    return all_passed, outcomes


def launch_prefill_instances(args):
    """Prefill 启动器：一条命令拉起全部 P 实例子进程，并在终端汇总 PASS/FAIL。"""
    _validate_device_budget(args)
    base_argv = _strip_args(sys.argv[1:], ("--p-rank", "--npu-id", "--nic", "--control-port"))
    log_dir = _instance_log_dir(args)
    children = []
    for rank in range(args.dp_size):
        device = args.npu_id + rank // args.instances_per_card
        extras = [
            "--p-rank",
            str(rank),
            "--npu-id",
            str(device),
            "--control-port",
            str(args.control_port + rank),
        ]
        if args.nic:
            extras += ["--nic", offset_nic_port(args.nic, rank)]
        command = _instance_argv(base_argv, extras)
        log_path = os.path.join(log_dir, f"p_{rank}.log")
        logger.info(
            f"[launcher] starting P instance {rank}, npu={device}, controlPort={args.control_port + rank}, log={log_path}"
        )
        with open(log_path, "w") as log_file:
            children.append((rank, subprocess.Popen(command, stdout=log_file, stderr=subprocess.STDOUT), log_path))

    all_passed, outcomes = _report_instances("Prefill", "P", children)
    if not all_passed:
        failed = ", ".join(f"P{rank}(exit={code})" for rank, code, _ in outcomes if code != 0)
        raise RuntimeError(f"Prefill instances failed: {failed}")


def launch_decode_instances(args):
    """Decode 启动器：一条命令拉起全部 D 实例子进程，并在终端汇总 PASS/FAIL。"""
    _validate_device_budget(args)
    base_argv = _strip_args(sys.argv[1:], ("--dp-rank", "--npu-id", "--nic"))
    log_dir = _instance_log_dir(args)
    p_cards = (args.dp_size + args.instances_per_card - 1) // args.instances_per_card
    children = []
    for rank in range(args.dp_size):
        device = args.npu_id + p_cards + rank // args.instances_per_card
        extras = [
            "--dp-rank",
            str(rank),
            "--npu-id",
            str(device),
        ]
        if args.nic:
            extras += ["--nic", offset_nic_port(args.nic, args.dp_size + rank)]
        command = _instance_argv(base_argv, extras)
        log_path = os.path.join(log_dir, f"d_{rank}.log")
        logger.info(f"[launcher] starting DP instance {rank}, npu={device}, log={log_path}")
        with open(log_path, "w") as log_file:
            children.append((rank, subprocess.Popen(command, stdout=log_file, stderr=subprocess.STDOUT), log_path))

    all_passed, outcomes = _report_instances("Decode", "D", children)
    if not all_passed:
        failed = ", ".join(f"D{rank}(exit={code})" for rank, code, _ in outcomes if code != 0)
        raise RuntimeError(f"Decode instances failed: {failed}")


# ---------------------------- entry ----------------------------


def _validate_args(parser, args):
    if args.dp_size < 1:
        parser.error("--dp-size must be >= 1")
    for name, rank in (("--p-rank", args.p_rank), ("--dp-rank", args.dp_rank)):
        if rank is not None and not 0 <= rank < args.dp_size:
            parser.error(f"{name} must be in [0, {args.dp_size})")
    if args.buffer_bytes <= 0 or args.buffer_bytes % 8 != 0:
        parser.error("--buffer-bytes must be a positive multiple of 8")
    if args.prefill_ips:
        ips = [s.strip() for s in args.prefill_ips.split(",") if s.strip()]
        if not ips:
            parser.error("--prefill-ips must contain at least one valid IPv4")
        if len(ips) > args.dp_size:
            parser.error(f"--prefill-ips has {len(ips)} nodes but dp-size is only {args.dp_size}")
        for ip in ips:
            try:
                ipaddress.IPv4Address(ip)
            except ValueError:
                parser.error(f"invalid IPv4 in --prefill-ips: {ip}")
    if args.batch_count < 1:
        parser.error("--batch-count must be >= 1")
    if args.data_op_type in NIC_REQUIRED_TYPES and not args.nic:
        parser.error(f"--nic is required for {args.data_op_type}, format tcp://<ip>:<port>")
    # -1（最后一张卡）只在手动模式有意义：launcher 模式需要起始卡号才能给各实例分卡
    # launcher 模式 ⟺ 当前 role 对应的 rank 未显式指定（与 _dispatch 的判定保持一致）
    is_launcher = args.p_rank is None if args.role == ROLE_PREFILL else args.dp_rank is None
    if args.npu_id < 0 and is_launcher:
        parser.error("--npu-id must be >= 0 in launcher mode (it is the start device id)")


def _parse_args():
    parser = argparse.ArgumentParser(description="PD 分离 + DP 多实例 HBM 全矩阵（P×D）读写验证")
    parser.add_argument("--role", type=str, required=True, choices=[ROLE_DECODE, ROLE_PREFILL])
    parser.add_argument("--local-ip", type=_ipv4, default="127.0.0.1", help="Numeric IPv4 of this machine")
    parser.add_argument("--prefill-ip", type=_ipv4, default="127.0.0.1", help="Prefill machine IPv4 (Decode only)")
    parser.add_argument(
        "--prefill-ips",
        type=str,
        default="",
        help="Comma-separated Prefill node IPs (Decode only, multi-node); overrides --prefill-ip when set",
    )
    parser.add_argument(
        "--control-port",
        type=int,
        default=DEFAULT_CONTROL_PORT,
        help="TCP metadata channel base port; P rank r uses base+r",
    )
    parser.add_argument("--dp-size", type=int, default=2, help="Instance count (N P instances x N D instances)")
    parser.add_argument("--p-rank", type=int, default=None, help="Prefill rank; omit to spawn all P instances")
    parser.add_argument("--dp-rank", type=int, default=None, help="Decode rank; omit to spawn all D instances")
    parser.add_argument(
        "--npu-id",
        type=int,
        default=0,
        help="Start device id of the whole block; -1 = last (manual mode only)",
    )
    parser.add_argument(
        "--instances-per-card",
        type=int,
        default=1,
        help="Instances sharing one NPU card (P block and D block stay on separate cards)",
    )
    parser.add_argument(
        "--data-op-type",
        type=str,
        default="HOST_RDMA",
        choices=list(DATA_OP_TYPES),
        help="Data plane protocol; see the module docstring for the full list and per-protocol requirements",
    )
    parser.add_argument(
        "--nic",
        type=str,
        default="",
        help="host RDMA address tcp://ip:port (required for HOST_RDMA; ignored by device-side protocols)",
    )
    parser.add_argument("--buffer-bytes", type=int, default=1024 * 1024, help="Bytes per buffer")
    parser.add_argument("--batch-count", type=int, default=8, help="Batch buffers per rank")
    parser.add_argument("--fill-val", type=int, default=42, help="Base value of the write phase")
    parser.add_argument("--log-level", type=int, default=DEFAULT_LOG_LEVEL, choices=sorted(LOG_LEVELS))
    parser.add_argument("--timeout", type=int, default=120, help="Control channel timeout in seconds")
    args = parser.parse_args()
    _validate_args(parser, args)
    return args


def _dispatch(args):
    """按 role / rank 选择 launcher 或单实例执行路径。"""
    if args.role == ROLE_PREFILL:
        if args.p_rank is None:
            launch_prefill_instances(args)
        else:
            run_prefill_instance(args)
    elif args.dp_rank is None:
        launch_decode_instances(args)
    else:
        run_decode_instance(args)


def main():
    args = _parse_args()
    logging.basicConfig(level=LOG_LEVELS[args.log_level], format="%(asctime)s %(levelname)s %(message)s")
    try:
        _dispatch(args)
    except RuntimeError as error:
        print(f"RESULT: FAIL — {error}")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
