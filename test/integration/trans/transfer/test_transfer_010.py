# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import CopyLayout, checked, prepare_copy, verify_copy


class TestTransfer010:
    """
    单节点双进程，验证 destroy、unInitialize 的重复调用及 initialize 后重新传输。
    连续销毁两次，校验 get_rpc_port 返回 0；连续反初始化两次后重新创建引擎。
    两端注册 1 个 64 KiB HBM 块，由 rank0 发起 transfer_sync_write 写入 rank1。
    源预置数据、目标填充不同数据并保留 guard；写入后校验数据和两端 guard 均正确。
    """

    APIS = ("destroy", "unInitialize", "initialize", "get_rpc_port", "transfer_sync_write")
    PROTOCOLS = ("device_rdma", "sdma", "device_urma")
    SOURCE_RANK = 0
    SEED = 401
    BLOCK_BYTES = 64 << 10

    def test_destroy_uninitialize_recreate(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        if ctx.engine.destroy() is not None:
            raise AssertionError("destroy must return None")
        if ctx.engine.get_rpc_port() != 0:
            raise AssertionError("destroyed engine must report port 0")
        if ctx.engine.unInitialize() is not None:
            raise AssertionError("unInitialize must return None")
        ctx.engine = None
        ctx.retained.clear()
        ctx.start_engine()
        self.buffers = prepare_copy(
            ctx, (CopyLayout("recreate_hbm", "hbm", "hbm", self.BLOCK_BYTES),), self.SOURCE_RANK, self.SEED
        )

    def run(self, ctx):
        if ctx.rank == 0:
            checked(
                "transfer_sync_write",
                ctx.engine.transfer_sync_write(
                    self.buffers.peer["session"], self.buffers.local_ptrs()[0], self.buffers.peer_ptrs()[0], 65536
                ),
            )
        verify_copy(ctx, self.buffers, "destroy_uninitialize_recreate")

    def teardown(self, ctx):
        pass
