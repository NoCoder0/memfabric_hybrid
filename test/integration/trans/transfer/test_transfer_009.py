# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.


class TestTransfer009:
    """
    单节点双进程，验证 SDMA 默认参数的 initialize 与 get_rpc_port 行为，不发起传输。
    每个 rank 关闭已有引擎后，以默认参数创建新的 TRANS 引擎。
    校验两个 rank 的会话端口均非 0，并通过双进程同步确认初始化完成。
    用例不分配或注册数据块，因此不涉及数据和 guard 校验。
    """

    APIS = ("initialize", "get_rpc_port")
    PROTOCOLS = ("sdma",)

    def test_default_initialize(self, trans_pool):
        trans_pool.run(type(self))

    def setup(self, ctx):
        ctx.close_engine()

    def run(self, ctx):
        ctx.start_engine(use_defaults=True)
        if ctx.session().split(":", 1)[1] == "0":
            raise AssertionError("default initialize returned port 0")
        ctx.sync()

    def teardown(self, ctx):
        pass
