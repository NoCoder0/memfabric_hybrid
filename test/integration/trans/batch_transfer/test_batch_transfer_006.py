# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import CopyLayout, checked, create_stream, destroy_stream, prepare_copy, verify_copy

SMALL_BLOCK_BYTES = 4 << 10
MEDIUM_BLOCK_BYTES = 8 << 10
LARGE_BLOCK_BYTES = 12 << 10

LAYOUT = (
    CopyLayout("async_batch_4k", "hbm", "hbm", SMALL_BLOCK_BYTES),
    CopyLayout("async_batch_8k", "hbm", "hbm", MEDIUM_BLOCK_BYTES),
    CopyLayout("async_batch_12k", "hbm", "hbm", LARGE_BLOCK_BYTES),
)


class TestBatchTransfer006:
    """
    单节点双进程，由 rank0 在 ACL stream 上发起 batch_transfer_async_read_submit 批量异步读，源在 rank1。
    将 rank1 HBM 的 3 个块读至 rank0 HBM：4 KiB、8 KiB、12 KiB，合计 24 KiB。
    两端以 batch_register_memory 注册全部区间；源预置数据、目标填充不同数据并保留 guard。
    stream 完成并销毁后，逐块校验目标数据、rank1 源数据与两端 guard 均正确。
    """

    APIS = ("batch_transfer_async_read_submit", "batch_register_memory")
    PROTOCOLS = ("sdma",)
    SOURCE_RANK = 1
    SEED = 204

    def test_async_batch_read(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.stream = None
        self.buffers = prepare_copy(ctx, LAYOUT, self.SOURCE_RANK, self.SEED, registration="batch")
        self.stream = create_stream(ctx) if ctx.rank == 0 else None

    def run(self, ctx):
        if ctx.rank == 0:
            try:
                checked(
                    "batch_transfer_async_read_submit",
                    ctx.engine.batch_transfer_async_read_submit(
                        self.buffers.peer["session"],
                        self.buffers.local_ptrs(),
                        self.buffers.peer_ptrs(),
                        self.buffers.lengths(),
                        self.stream,
                    ),
                )
            finally:
                destroy_stream(ctx, self.stream)
                self.stream = None
        verify_copy(ctx, self.buffers, "async_batch_read")

    def teardown(self, ctx):
        if self.stream is not None:
            destroy_stream(ctx, self.stream)
            self.stream = None
