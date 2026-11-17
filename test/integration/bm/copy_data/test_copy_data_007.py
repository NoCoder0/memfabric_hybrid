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


class TestCopyData007:
    """
    GH2H: 单节点双进程，由 rank0 发起单次拷贝。
    从 rank1 内存池 DRAM 的全局虚拟地址 GVA，单次拷贝到 rank0 内存池 DRAM 的本地虚拟地址 VA。
    拷贝 1 个 大小为 8 MiB 的 block
    目标预先填充为与源数据不同的值，拷贝后校验目标数据正确且源数据保持不变。
    """

    COUNT = 1
    BLOCK_BYTES = 8 << 20
    SEED = 11983

    def test_copy_gh2h(self, shared_pool):
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
            peer_gva = ctx.handle.peer_rank_ptr(1, ctx.bm.BmMemType.HOST)
            assert peer_gva != 0, "Remote HOST GVA is zero"
            local = self.buffers[0].data_ptr()
            ctx.step("copy_data GH2H")
            ret = ctx.handle.copy_data(
                src_ptr=peer_gva, dst_ptr=local, size=self.BLOCK_BYTES, type=ctx.bm.BmCopyType.GH2H, flags=0
            )
            assert ret == 0, f"copy_data failed, ret={ret}"
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
