# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import CopyBuffers, CopyLayout, checked, prepare_copy, verify_copy


class TestTransfer011:
    """
    单节点双进程，验证 destroy、unInitialize、initialize 后同一 HOST 区间可重新 register_memory。
    两端保留 1 个 64 KiB 块：rank0 HBM 源和 rank1 HOST 目标；销毁并重建引擎后重新注册原区间。
    随后由 rank0 发起 transfer_sync_write，将 HBM 源写入 rank1 HOST 目标。
    源预置数据、目标填充不同数据并保留 guard；写入后校验数据和两端 guard 均正确。
    """

    APIS = ("destroy", "unInitialize", "initialize", "register_memory", "transfer_sync_write")
    PROTOCOLS = ("device_rdma", "device_urma")
    SOURCE_RANK = 0
    SEED = 402
    BLOCK_BYTES = 64 << 10

    def test_host_reregister_after_recreate(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.buffers = prepare_copy(
            ctx, (CopyLayout("recreate_host", "hbm", "host", self.BLOCK_BYTES),), self.SOURCE_RANK, self.SEED
        )
        self.block = self.buffers.blocks["recreate_host"]
        ctx.close_engine()
        ctx.start_engine()
        ctx.retain(self.block)
        checked("register_memory", ctx.engine.register_memory(self.block.region_ptr, self.block.capacity))
        ctx.sync()
        descriptor = {"session": ctx.session(), "ptrs": {self.block.name: self.block.ptr}}
        self.buffers = CopyBuffers(
            self.buffers.blocks, ctx.exchange(descriptor), self.SOURCE_RANK, self.SEED, self.buffers.layout
        )
        ctx.sync()

    def run(self, ctx):
        if ctx.rank == 0:
            checked(
                "transfer_sync_write",
                ctx.engine.transfer_sync_write(
                    self.buffers.peer["session"], self.block.ptr, self.buffers.peer_ptrs()[0], self.block.size
                ),
            )
        verify_copy(ctx, self.buffers, "host_reregister_after_recreate")

    def teardown(self, ctx):
        pass
