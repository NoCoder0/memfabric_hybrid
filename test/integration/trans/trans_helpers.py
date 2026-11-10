# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

"""Mechanical buffer preparation and verification for TRANS cases."""

from dataclasses import dataclass

GUARD_BYTES = 64
REGISTER_ALIGNMENT = 2 * 1024 * 1024
POISON = 0x3C
GUARD = 0xA5


def checked(name, result, allow_none=False):
    """Raise with the actual native result; no retry or error suppression is allowed."""
    if result == 0 or (allow_none and result is None):
        return result
    raise RuntimeError(f"{name} failed, ret={result}")


@dataclass(frozen=True)
class CopyLayout:
    """One payload, declared from the source rank's perspective."""

    name: str
    source_memory: str
    target_memory: str
    size: int


class Block:
    """Owns an aligned registration region with payload guards."""

    def __init__(self, ctx, name, memory, size):
        device = "npu" if memory == "hbm" else "cpu"
        capacity = _align(size + 2 * GUARD_BYTES, REGISTER_ALIGNMENT)
        self.ctx, self.name, self.size, self.capacity = ctx, name, size, capacity
        self.raw = ctx.torch.empty(capacity + REGISTER_ALIGNMENT, dtype=ctx.torch.uint8, device=device)
        offset = (-self.raw.data_ptr()) % REGISTER_ALIGNMENT
        self.region = self.raw[offset : offset + capacity]
        self.region_ptr = self.region.data_ptr()
        self.ptr = self.region_ptr + GUARD_BYTES
        if self.region_ptr % REGISTER_ALIGNMENT:
            raise AssertionError(f"{name}: registration region is not aligned")

    def expected(self, seed):
        generator = self.ctx.torch.Generator(device="cpu").manual_seed(seed + _name_seed(self.name))
        return self.ctx.torch.randint(0, 256, (self.size,), dtype=self.ctx.torch.uint8, generator=generator)

    def set_source(self, seed):
        values = self.expected(seed)
        self.region.fill_(GUARD)
        self.region[GUARD_BYTES : GUARD_BYTES + self.size].copy_(values.to(self.region.device))

    def set_target(self):
        self.region.fill_(GUARD)
        self.region[GUARD_BYTES : GUARD_BYTES + self.size].fill_(POISON)

    def assert_payload(self, expected, label):
        actual = self.region[GUARD_BYTES : GUARD_BYTES + self.size].cpu()
        guard = self.ctx.torch.full((GUARD_BYTES,), GUARD, dtype=self.ctx.torch.uint8)
        if not self.ctx.torch.equal(actual, expected):
            raise AssertionError(f"{label}: payload mismatch")
        if not self.ctx.torch.equal(self.region[:GUARD_BYTES].cpu(), guard):
            raise AssertionError(f"{label}: prefix guard changed")
        suffix = self.region[GUARD_BYTES + self.size : GUARD_BYTES + self.size + GUARD_BYTES].cpu()
        if not self.ctx.torch.equal(suffix, guard):
            raise AssertionError(f"{label}: suffix guard changed")

    def assert_poison(self, label):
        expected = self.ctx.torch.full((self.size,), POISON, dtype=self.ctx.torch.uint8)
        self.assert_payload(expected, label)


@dataclass
class CopyBuffers:
    """Local blocks plus the peer session and payload pointers for one case."""

    blocks: dict
    peer: dict
    source_rank: int
    seed: int
    layout: tuple

    def local_ptrs(self):
        return [self.blocks[item.name].ptr for item in self.layout]

    def peer_ptrs(self):
        return [self.peer["ptrs"][item.name] for item in self.layout]

    def lengths(self):
        return [item.size for item in self.layout]


def prepare_copy(ctx, layout, source_rank, seed, registration="single"):
    """Allocate, seed, register, exchange descriptors, and synchronize one copy layout."""
    layout = tuple(layout)
    blocks = {}
    for item in layout:
        memory = item.source_memory if ctx.rank == source_rank else item.target_memory
        block = Block(ctx, item.name, memory, item.size)
        ctx.retain(block)
        if ctx.rank == source_rank:
            block.set_source(seed)
        else:
            block.set_target()
        blocks[item.name] = block
    _register(ctx, blocks, registration)
    ctx.npu.synchronize()
    ctx.sync()
    descriptor = {"session": ctx.session(), "ptrs": {name: block.ptr for name, block in blocks.items()}}
    peer = ctx.exchange(descriptor)
    ctx.sync()
    return CopyBuffers(blocks, peer, source_rank, seed, layout)


def verify_copy(ctx, buffers, case_name):
    """Verify source immutability, destination data, and guards without exchanging expected bytes."""
    ctx.sync()
    for item in buffers.layout:
        block = buffers.blocks[item.name]
        expected = block.expected(buffers.seed)
        label = f"{case_name} {item.name} rank={ctx.rank}"
        block.assert_payload(expected, f"source {label}" if ctx.rank == buffers.source_rank else f"target {label}")
    ctx.step(f"DATA_VERIFIED case={case_name} rank={ctx.rank}")
    ctx.sync()


def verify_rejected(ctx, buffers, case_name):
    """Check that an invalid vector call did not modify either source or target buffers."""
    ctx.sync()
    for item in buffers.layout:
        block = buffers.blocks[item.name]
        if ctx.rank == buffers.source_rank:
            block.assert_payload(block.expected(buffers.seed), f"source {case_name} {item.name}")
        else:
            block.assert_poison(f"target {case_name} {item.name}")
    ctx.step(f"DATA_VERIFIED case={case_name} rank={ctx.rank}")
    ctx.sync()


def create_stream(ctx):
    """Create a real ACL stream and retain it until the case synchronizes and destroys it."""
    stream, ret = ctx.acl.rt.create_stream()
    checked("acl.rt.create_stream", ret)
    if not stream:
        raise RuntimeError("acl.rt.create_stream returned a null stream")
    ctx.streams.append(stream)
    return stream


def destroy_stream(ctx, stream):
    """Confirm completion before destruction; retain resources if either operation fails."""
    checked("acl.rt.synchronize_stream", ctx.acl.rt.synchronize_stream(stream))
    checked("acl.rt.destroy_stream", ctx.acl.rt.destroy_stream(stream))
    ctx.streams.remove(stream)


def _register(ctx, blocks, registration):
    if registration == "single":
        for block in blocks.values():
            checked(f"register_memory({block.name})", ctx.engine.register_memory(block.region_ptr, block.capacity))
        return
    if registration == "batch":
        addresses = [block.region_ptr for block in blocks.values()]
        capacities = [block.capacity for block in blocks.values()]
        checked("batch_register_memory", ctx.engine.batch_register_memory(addresses, capacities))
        return
    raise ValueError(f"unknown registration mode: {registration}")


def _align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def _name_seed(name):
    return sum((index + 1) * ord(char) for index, char in enumerate(name))
