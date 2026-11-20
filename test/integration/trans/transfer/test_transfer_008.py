# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.


class TestTransfer008:
    """
    单节点双进程，验证 initialize 与 get_rpc_port 的初始化和参数边界行为，不发起传输。
    每个 rank 先检查未初始化实例端口为 0，再验证空 role、空 session 和超长 session 均不能初始化成功。
    随后调用 destroy、unInitialize，并以有效参数初始化新的引擎。
    最终校验两个 rank 的会话端口均非 0，且不涉及块注册、数据或 guard 校验。
    """

    APIS = ("initialize", "get_rpc_port")
    PROTOCOLS = ("device_rdma", "sdma", "device_urma")

    def test_initialize_and_rpc_port(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        ctx.close_engine()
        self.probe = ctx._engine_type()

    def run(self, ctx):
        if self.probe.get_rpc_port() != 0:
            raise AssertionError("get_rpc_port before initialize must return 0")
        for role, session in (("", "127.0.0.1:0"), ("Prefill", ""), ("Decode", "x" * 1024)):
            if self.probe.initialize("tcp://127.0.0.1", session, role, ctx.rank, ctx._data_op) == 0:
                raise AssertionError(f"initialize unexpectedly accepted role={role!r}, session={session!r}")
        self.probe.destroy()
        self.probe.unInitialize()
        ctx.start_engine()
        if ctx.session().split(":", 1)[1] == "0":
            raise AssertionError("initialized engine returned port 0")
        ctx.sync()

    def teardown(self, ctx):
        pass
