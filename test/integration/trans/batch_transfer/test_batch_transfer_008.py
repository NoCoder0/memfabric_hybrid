# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import (
    CopyLayout,
    create_stream,
    destroy_stream,
    prepare_copy,
    verify_rejected,
)

SOURCE_RANK = 0
SEED = 208
REJECTED_RET = -1
SMALL_BLOCK_BYTES = 4 << 10
MEDIUM_BLOCK_BYTES = 8 << 10
LAYOUT = (
    CopyLayout("async_batch_rejected_4k", "hbm", "hbm", SMALL_BLOCK_BYTES),
    CopyLayout("async_batch_rejected_8k", "hbm", "hbm", MEDIUM_BLOCK_BYTES),
)


class TestBatchTransfer008:
    """
    单节点双进程，由 rank0 在 ACL stream 上调用 batch_transfer_async_write_submit 的参数异常路径。
    准备 2 个已批量注册的 HBM→HBM 块，大小分别为 4 KiB 和 8 KiB，源在 rank0。
    传入少一个目标地址，使地址向量长度不等，接口必须返回 -1，并完成 stream 销毁。
    失败后校验源、目标和两端 guard 均保持预置值，未发生部分异步写入。
    """

    APIS = ("batch_transfer_async_write_submit", "batch_register_memory")
    PROTOCOLS = ("sdma",)

    def test_async_batch_write_mismatch(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.stream = None
        self.buffers = prepare_copy(ctx, LAYOUT, SOURCE_RANK, SEED, registration="batch")
        self.stream = create_stream(ctx) if ctx.rank == SOURCE_RANK else None

    def run(self, ctx):
        if ctx.rank == SOURCE_RANK:
            try:
                result = ctx.engine.batch_transfer_async_write_submit(
                    self.buffers.peer["session"],
                    self.buffers.local_ptrs(),
                    self.buffers.peer_ptrs()[:-1],
                    self.buffers.lengths(),
                    self.stream,
                )
                if result != REJECTED_RET:
                    raise AssertionError(
                        f"batch_transfer_async_write_submit returned {result}, expected {REJECTED_RET}"
                    )
            finally:
                destroy_stream(ctx, self.stream)
                self.stream = None
        verify_rejected(ctx, self.buffers, "async_batch_write_mismatch")

    def teardown(self, ctx):
        if self.stream is not None:
            destroy_stream(ctx, self.stream)
            self.stream = None
