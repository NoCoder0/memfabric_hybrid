# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import helpers


class TestCopyDataBatch009:
    """
    G2G: 单节点双进程，由 rank0 发起批量拷贝。
    从 rank1 内存池 DRAM 的全局虚拟地址 GVA，批量拷贝到 rank0 内存池 DRAM 的全局虚拟地址 GVA。
    拷贝 4 个 block，每个 block 大小为 8 MiB，共 32 MiB。
    目标预先填充为与源数据不同的值，拷贝后逐块校验目标数据正确且源数据保持不变。
    """

    COUNT = 4
    BLOCK_BYTES = 8 << 20
    SEED = 8105

    def test_batch_copy_g2g(self, shared_pool):
        shared_pool.run(type(self))

    def setup(self, ctx):
        self.expected, self.buffers = [], []
        self.expected = [helpers.expected(ctx, self.SEED, block, self.BLOCK_BYTES) for block in range(self.COUNT)]
        self.buffers = helpers.make_host_buffers(ctx, self.COUNT, self.BLOCK_BYTES)
        for buffer, values in zip(self.buffers, self.expected):
            helpers.fill(buffer, values, poison=ctx.rank == 0)
        ctx.step("seed synchronization")
        ctx.sync()

    def run(self, ctx):
        if ctx.rank == 0:
            src_gva = ctx.handle.peer_rank_ptr(1, ctx.bm.BmMemType.HOST)
            dst_gva = ctx.handle.peer_rank_ptr(0, ctx.bm.BmMemType.HOST)
            assert src_gva != 0 and dst_gva != 0, "HOST GVA is zero"
            src_addrs = [src_gva + block * self.BLOCK_BYTES for block in range(self.COUNT)]
            dst_addrs = [dst_gva + block * self.BLOCK_BYTES for block in range(self.COUNT)]
            ctx.step("copy_data_batch G2G")
            ret = ctx.handle.copy_data_batch(
                src_addrs=src_addrs,
                dst_addrs=dst_addrs,
                sizes=[self.BLOCK_BYTES] * self.COUNT,
                count=self.COUNT,
                type=ctx.bm.BmCopyType.G2G,
                flags=0,
            )
            assert ret == 0, f"copy_data_batch failed, ret={ret}"
        ctx.step("copy completion synchronization")
        ctx.sync()
        for block, (buffer, values) in enumerate(zip(self.buffers, self.expected)):
            label = "unchanged source" if ctx.rank == 1 else "destination"
            ctx.step(f"verify {label} block={block}")
            helpers.verify(ctx, buffer.cpu(), values, block, label)
        ctx.step("verification synchronization")
        ctx.sync()

    def teardown(self, ctx):
        self.buffers.clear()
        self.expected.clear()
