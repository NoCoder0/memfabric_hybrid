# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

from ..trans_helpers import Block

BLOCK_BYTES = 4 << 10
REJECTED_RET = -1
BLOCK_NAME = "batch_register_rejected"


class TestBatchTransfer010:
    """
    单节点双进程，由 rank0 验证 batch_register_memory 的参数异常路径。
    两端各分配并保留 1 个 4 KiB HBM 块，填充目标毒值并保留 guard，不发起传输。
    传入一个地址和空长度向量，使向量长度不等，接口必须返回 -1。
    失败后校验两端块数据与 guard 均保持预置值，未产生可观察内存改写。
    """

    APIS = ("batch_register_memory",)
    PROTOCOLS = ("device_rdma", "sdma", "device_urma")

    def test_batch_register_memory_mismatch(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        self.block = ctx.retain(Block(ctx, BLOCK_NAME, "hbm", BLOCK_BYTES))
        self.block.set_target()

    def run(self, ctx):
        if ctx.rank == 0:
            result = ctx.engine.batch_register_memory([self.block.region_ptr], [])
            if result != REJECTED_RET:
                raise AssertionError(f"batch_register_memory returned {result}, expected {REJECTED_RET}")
        ctx.npu.synchronize()
        ctx.sync()
        self.block.assert_poison(f"batch_register_memory_mismatch rank={ctx.rank}")
        ctx.step(f"DATA_VERIFIED case=batch_register_memory_mismatch rank={ctx.rank}")
        ctx.sync()

    def teardown(self, ctx):
        pass
