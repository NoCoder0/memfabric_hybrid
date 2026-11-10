# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import CopyLayout, checked, prepare_copy, verify_copy

SMALL_BLOCK_BYTES = 4 << 10
MEDIUM_BLOCK_BYTES = 8 << 10
LARGE_BLOCK_BYTES = 12 << 10

LAYOUT = (
    CopyLayout("batch_4k", "hbm", "host", SMALL_BLOCK_BYTES),
    CopyLayout("batch_8k", "host", "hbm", MEDIUM_BLOCK_BYTES),
    CopyLayout("batch_12k", "hbm", "host", LARGE_BLOCK_BYTES),
)


class TestBatchTransfer001:
    """
    单节点双进程，由 rank0 发起 batch_transfer_sync_write 批量写。
    将 rank0 的 3 个块写入 rank1：HBM→HOST 4 KiB、HOST→HBM 8 KiB、HBM→HOST 12 KiB。
    两端以 batch_register_memory 注册全部区间；源预置数据、目标填充不同数据并保留 guard。
    写入后逐块校验目标数据、源数据与两端 guard 均正确。
    """

    APIS = ("batch_transfer_sync_write", "batch_register_memory")
    PROTOCOLS = ("device_rdma",)
    SOURCE_RANK = 0
    SEED = 105

    def test_sync_batch_write(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.buffers = prepare_copy(ctx, LAYOUT, self.SOURCE_RANK, self.SEED, registration="batch")

    def run(self, ctx):
        if ctx.rank == 0:
            checked(
                "batch_transfer_sync_write",
                ctx.engine.batch_transfer_sync_write(
                    self.buffers.peer["session"],
                    self.buffers.local_ptrs(),
                    self.buffers.peer_ptrs(),
                    self.buffers.lengths(),
                ),
            )
        ctx.sync()
        verify_copy(ctx, self.buffers, "sync_batch_write")

    def teardown(self, ctx):
        pass
