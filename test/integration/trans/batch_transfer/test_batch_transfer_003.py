# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import CopyLayout, prepare_copy, verify_rejected

SMALL_BLOCK_BYTES = 4 << 10
MEDIUM_BLOCK_BYTES = 8 << 10
LARGE_BLOCK_BYTES = 12 << 10

LAYOUT = (
    CopyLayout("batch_4k", "hbm", "host", SMALL_BLOCK_BYTES),
    CopyLayout("batch_8k", "host", "hbm", MEDIUM_BLOCK_BYTES),
    CopyLayout("batch_12k", "hbm", "host", LARGE_BLOCK_BYTES),
)


class TestBatchTransfer003:
    """
    单节点双进程，由 rank0 调用 batch_transfer_sync_write 的批量参数异常路径。
    准备 HBM→HOST 4 KiB、HOST→HBM 8 KiB、HBM→HOST 12 KiB 共 3 个已批量注册的块，源在 rank0。
    传入少一个目标地址，使地址向量长度不等，接口必须返回 -1。
    失败后逐块校验源、目标和两端 guard 均保持预置值，未发生部分写入。
    """

    APIS = ("batch_transfer_sync_write",)
    PROTOCOLS = ("device_rdma",)
    SOURCE_RANK = 0
    SEED = 107

    def test_sync_batch_write_mismatch(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.buffers = prepare_copy(ctx, LAYOUT, self.SOURCE_RANK, self.SEED, registration="batch")

    def run(self, ctx):
        if ctx.rank == 0:
            ret = ctx.engine.batch_transfer_sync_write(
                self.buffers.peer["session"],
                self.buffers.local_ptrs(),
                self.buffers.peer_ptrs()[:-1],
                self.buffers.lengths(),
            )
            if ret != -1:
                raise AssertionError(f"batch_transfer_sync_write mismatch returned {ret}, expected -1")
        verify_rejected(ctx, self.buffers, "sync_batch_write_mismatch")

    def teardown(self, ctx):
        pass
