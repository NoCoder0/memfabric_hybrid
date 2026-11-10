# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import CopyLayout, checked, prepare_copy, verify_copy


class TestTransfer003:
    """
    单节点双进程，由 rank0 发起 transfer_sync_read 单块同步读，源数据位于 rank1。
    将 rank1 对端 1 个 64 KiB HBM 块读至 rank0 本地 1 个 64 KiB HBM 块。
    两端以 register_memory 注册区间；源预置数据、目标填充不同数据并保留 guard。
    读取后校验目标数据、rank1 源数据与两端 guard 均正确。
    """

    APIS = ("transfer_sync_read", "register_memory")
    PROTOCOLS = ("device_rdma", "sdma")
    SOURCE_RANK = 1
    SEED = 103
    BLOCK_BYTES = 64 << 10

    def test_sync_single_read_hbm(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.buffers = prepare_copy(
            ctx, (CopyLayout("read_hbm", "hbm", "hbm", self.BLOCK_BYTES),), self.SOURCE_RANK, self.SEED
        )

    def run(self, ctx):
        if ctx.rank == 0:
            checked(
                "transfer_sync_read",
                ctx.engine.transfer_sync_read(
                    self.buffers.peer["session"], self.buffers.local_ptrs()[0], self.buffers.peer_ptrs()[0], 65536
                ),
            )
        verify_copy(ctx, self.buffers, "sync_single_read_hbm")

    def teardown(self, ctx):
        pass
