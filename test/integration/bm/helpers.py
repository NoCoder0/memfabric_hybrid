#!/usr/bin/env python
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""BM case 的机械动作，不含任何 case 决策。"""

import ctypes
import logging
import os
import pdb
import re
import subprocess
import sys

ALIGNMENT = 4096

LOG = logging.getLogger(__name__)
# 与 SDK soc 判断一致：Ascend910B -> A2，Ascend910_93(910C) -> A3，Ascend910_95/Ascend950 -> A5。
_CHIP_RULES = (
    ("A2", ("Ascend910B",)),
    ("A3", ("Ascend910_93",)),
    ("A5", ("Ascend910_95", "Ascend950")),
)
_PROC_MEM_RE = re.compile(r"Process memory\(MB\):(\d+)")


class ForkedPdb(pdb.Pdb):
    def interaction(self, *args, **kwargs):
        _stdin = sys.stdin
        try:
            sys.stdin = open("/dev/stdin")
            pdb.Pdb.interaction(self, *args, **kwargs)
        finally:
            sys.stdin = _stdin


def maybe_pdb(ctx):
    if os.environ.get("BM_PDB_RANK") == str(ctx.rank):
        ForkedPdb().set_trace()


def expected(ctx, seed, block, block_bytes):
    """双 rank 各自独立生成期望数据，必须逐字节一致，故用 CPU generator 按同一 seed 重放。"""
    generator = ctx.torch.Generator(device="cpu").manual_seed(seed + block)
    return ctx.torch.randint(0, 256, (block_bytes,), dtype=ctx.torch.uint8, generator=generator)


def make_npu_buffer(ctx, block, block_bytes):
    """分配一个对齐 NPU buffer；调用者持有引用并决定何时注册和注销。"""
    ctx.step(f"allocate NPU block={block}")
    raw = ctx.torch.empty(block_bytes + ALIGNMENT - 1, dtype=ctx.torch.uint8, device="npu")
    offset = (-raw.data_ptr()) % ALIGNMENT
    view = raw[offset : offset + block_bytes]
    assert view.data_ptr() % ALIGNMENT == 0, "NPU buffer alignment failed"
    return raw, view


def unregister_buffers(ctx, registered):
    """注销已成功注册的 buffer，并返回未能注销的记录。"""
    errors = []
    retained = []
    for raw, view in registered:
        try:
            ctx.step("handle.unregister")
            ret = ctx.handle.unregister(view.data_ptr())
            if ret != 0:
                raise RuntimeError(f"handle.unregister failed, ret={ret}")
        except Exception as exc:
            LOG.exception("rank=%d handle.unregister failed", ctx.rank)
            retained.append((raw, view))
            errors.append(str(exc))
    return retained, errors


def make_host_buffers(ctx, count, block_bytes):
    assert ctx.handle.local_mem_size(ctx.bm.BmMemType.HOST) >= count * block_bytes, "Owner HOST pool is too small"
    gva = ctx.handle.peer_rank_ptr(ctx.rank, ctx.bm.BmMemType.HOST)
    assert gva != 0, "Owner HOST GVA is zero"
    buffers = []
    for block in range(count):
        ctx.step(f"owner gva_to_va LOCAL_HOST block={block}")
        va = ctx.handle.gva_to_va(gva + block * block_bytes, ctx.bm.BmMemType.LOCAL_HOST)
        assert va != 0, f"Owner LOCAL_HOST mapping failed, block={block}"
        memory = (ctypes.c_ubyte * block_bytes).from_address(va)
        buffers.append(ctx.torch.frombuffer(memory, dtype=ctx.torch.uint8))
    return buffers


def fill(buffer, values, poison):
    """poison 填充用于证明拷贝确实发生，而非碰巧通过校验。"""
    buffer.copy_(values.bitwise_not() if poison else values)


def verify(ctx, actual, expected_tensor, block, label):
    if not ctx.torch.equal(actual, expected_tensor):
        mismatch = actual.ne(expected_tensor)
        count = int(mismatch.sum().item())
        first = int(mismatch.nonzero()[0].item())
        raise AssertionError(f"{label} mismatch, block={block}, first_index={first}, mismatch_count={count}")


def detect_chip():
    """返回 A2/A3/A5；CANN 不可用时返回 None。"""
    try:
        import acl

        acl.init()
        try:
            soc = acl.get_soc_name()
        finally:
            acl.finalize()
    except Exception:
        LOG.warning("acl chip detection failed", exc_info=True)
        return None
    return _chip_from_soc(soc)


def _chip_from_soc(soc):
    for chip, markers in _CHIP_RULES:
        if any(marker in soc for marker in markers):
            return chip
    return None


def select_devices(count=2):
    """返回物理卡号列表；卡数或 npu-smi 不可用时返回 None；空闲优先，无足够空闲按进程占用最小挑。"""
    total = _device_count()
    if total is None:
        return None
    cards = {}
    for card in range(total):
        usage = _proc_mem(card)
        if usage is None:
            return None
        cards[card] = usage
    return _choose_devices(cards, count)


def _choose_devices(cards, count):
    idle = sorted(card for card, usage in cards.items() if not usage["processes"])
    if len(idle) >= count:
        return idle[:count]
    ranked = sorted(cards.items(), key=lambda item: (sum(item[1]["processes"]), item[0]))
    return [card for card, _ in ranked][:count]


def _device_count():
    try:
        import acl

        acl.init()
        try:
            value, ret = acl.rt.get_device_count()
        finally:
            acl.finalize()
    except Exception:
        LOG.warning("acl device_count failed", exc_info=True)
        return None
    if ret != 0:
        return None
    return int(value)


def _proc_mem(card):
    try:
        result = subprocess.run(
            ["npu-smi", "info", "-t", "proc-mem", "-i", str(card)],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    return _parse_proc_mem(result.stdout)


def _parse_proc_mem(output):
    if "No process in device." in output:
        return {"processes": []}
    return {"processes": [int(mem) for mem in _PROC_MEM_RE.findall(output)]}
