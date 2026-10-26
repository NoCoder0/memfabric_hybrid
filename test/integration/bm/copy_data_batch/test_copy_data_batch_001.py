#!/usr/bin/env python
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of the Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import helpers


class TestCopyDataBatch001:
    """
    GH2L: 单节点双进程，由 rank0 发起批量拷贝。
    从 rank1 内存池 DRAM 的全局虚拟地址 GVA，批量拷贝到 rank0 本地 NPU HBM。
    拷贝 4 个 block，每个 block 大小为 8 MiB，共 32 MiB。
    目标预先填充为与源数据不同的值，拷贝后逐块校验目标数据正确且源数据保持不变。
    """

    COUNT = 4
    BLOCK_BYTES = 8 << 20
    SEED = 9341

    def test_batch_copy_gh2l(self, shared_pool):
        shared_pool.run(type(self))

    def setup(self, ctx):
        self.expected, self.buffers, self.owned_npu, self.registered = [], [], [], []
        self.expected = [helpers.expected(ctx, self.SEED, block, self.BLOCK_BYTES) for block in range(self.COUNT)]
        if ctx.rank == 0:
            for block in range(self.COUNT):
                raw, view = helpers.make_npu_buffer(ctx, block, self.BLOCK_BYTES)
                self.owned_npu.append((raw, view))
                ctx.step(f"handle.register NPU block={block}")
                ret = ctx.handle.register(view.data_ptr(), self.BLOCK_BYTES)
                assert ret == 0, f"register failed, block={block}, ret={ret}"
                self.registered.append((raw, view))
                self.buffers.append(view)
        else:
            self.buffers = helpers.make_host_buffers(ctx, self.COUNT, self.BLOCK_BYTES)
        for buffer, values in zip(self.buffers, self.expected):
            helpers.fill(buffer, values, poison=ctx.rank == 0)
        ctx.npu.synchronize()
        ctx.step("seed synchronization")
        ctx.sync()

    def run(self, ctx):
        if ctx.rank == 0:
            peer_gva = ctx.handle.peer_rank_ptr(1, ctx.bm.BmMemType.HOST)
            assert peer_gva != 0, "Remote HOST GVA is zero"
            remote = [peer_gva + block * self.BLOCK_BYTES for block in range(self.COUNT)]
            local = [buffer.data_ptr() for buffer in self.buffers]
            ctx.step("copy_data_batch GH2L")
            ret = ctx.handle.copy_data_batch(
                src_addrs=remote,
                dst_addrs=local,
                sizes=[self.BLOCK_BYTES] * self.COUNT,
                count=self.COUNT,
                type=ctx.bm.BmCopyType.GH2L,
                flags=0,
            )
            assert ret == 0, f"copy_data_batch failed, ret={ret}"
            ctx.npu.synchronize()
        ctx.step("copy completion synchronization")
        ctx.sync()
        for block, (buffer, values) in enumerate(zip(self.buffers, self.expected)):
            label = "unchanged source" if ctx.rank == 1 else "destination"
            ctx.step(f"verify {label} block={block}")
            helpers.verify(ctx, buffer.cpu(), values, block, label)
        ctx.step("verification synchronization")
        ctx.sync()

    def teardown(self, ctx):
        self.registered, errors = helpers.unregister_buffers(ctx, self.registered)
        if errors:
            self.owned_npu = self.registered
            self.buffers = [view for _, view in self.registered]
            raise RuntimeError("; ".join(errors))
        self.owned_npu.clear()
        self.buffers.clear()
        self.expected.clear()
