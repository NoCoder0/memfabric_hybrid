# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import CopyLayout, checked, prepare_copy, verify_copy


class TestTransfer007:
    """
    单节点双进程，验证已注册单块依次 unregister_memory、register_memory 后仍可传输。
    两端各保留 1 个 64 KiB HBM 块；rank0 先注销再重新注册本地区间，并发起 transfer_sync_write。
    源预置数据、目标填充不同数据并保留 guard，重新注册后写入 rank1 对端块。
    写入后校验目标数据、源数据与两端 guard 均正确。
    """

    APIS = ("register_memory", "unregister_memory", "transfer_sync_write")
    PROTOCOLS = ("device_rdma", "sdma")
    SOURCE_RANK = 0
    SEED = 301
    BLOCK_BYTES = 64 << 10

    def test_register_unregister_reregister(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.buffers = prepare_copy(
            ctx, (CopyLayout("reregister", "hbm", "hbm", self.BLOCK_BYTES),), self.SOURCE_RANK, self.SEED
        )
        self.block = self.buffers.blocks["reregister"]
        checked("unregister_memory", ctx.engine.unregister_memory(self.block.region_ptr))
        checked("register_memory", ctx.engine.register_memory(self.block.region_ptr, self.block.capacity))
        ctx.sync()

    def run(self, ctx):
        if ctx.rank == 0:
            checked(
                "transfer_sync_write",
                ctx.engine.transfer_sync_write(
                    self.buffers.peer["session"], self.block.ptr, self.buffers.peer_ptrs()[0], self.block.size
                ),
            )
        ctx.sync()
        verify_copy(ctx, self.buffers, "register_unregister_reregister")

    def teardown(self, ctx):
        pass
