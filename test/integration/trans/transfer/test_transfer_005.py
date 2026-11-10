# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import CopyLayout, checked, create_stream, destroy_stream, prepare_copy, verify_copy


class TestTransfer005:
    """
    单节点双进程，由 rank0 在 ACL stream 上发起 transfer_async_write_submit 单块异步写。
    将 rank0 本地 1 个 64 KiB HBM 块写入 rank1 对端 1 个 64 KiB HBM 块。
    两端以 register_memory 注册区间；源预置数据、目标填充不同数据并保留 guard。
    stream 完成并销毁后，校验目标数据、源数据与两端 guard 均正确。
    """

    APIS = ("transfer_async_write_submit", "register_memory")
    PROTOCOLS = ("sdma",)
    SOURCE_RANK = 0
    SEED = 201
    BLOCK_BYTES = 64 << 10

    def test_async_single_write(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.stream = None
        self.buffers = prepare_copy(
            ctx, (CopyLayout("async_write", "hbm", "hbm", self.BLOCK_BYTES),), self.SOURCE_RANK, self.SEED
        )
        self.stream = create_stream(ctx) if ctx.rank == 0 else None

    def run(self, ctx):
        if ctx.rank == 0:
            try:
                checked(
                    "transfer_async_write_submit",
                    ctx.engine.transfer_async_write_submit(
                        self.buffers.peer["session"],
                        self.buffers.local_ptrs()[0],
                        self.buffers.peer_ptrs()[0],
                        65536,
                        self.stream,
                    ),
                )
            finally:
                destroy_stream(ctx, self.stream)
                self.stream = None
        ctx.sync()
        verify_copy(ctx, self.buffers, "async_single_write")

    def teardown(self, ctx):
        if self.stream is not None:
            destroy_stream(ctx, self.stream)
            self.stream = None
