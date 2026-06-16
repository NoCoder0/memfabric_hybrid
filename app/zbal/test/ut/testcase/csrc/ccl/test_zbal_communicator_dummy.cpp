/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <gtest/gtest.h>

#include "zbal_test_constants.h"

#define private   public
#define protected public
#include "zbal_communicator.h"
#undef private
#undef protected

#include "zbal_communicator_dummy.h"
#include "zbal_comm_group_meta.h"
#include "zbal_comm_types.h"

using namespace zbal;
using namespace zbal::operators;

class TestZBALCommunicatorDummy : public testing::Test {
public:
    void SetUp() override
    {
        opt_.name = "test_dummy";
        opt_.worldSize = ZBAL_UT_WORLD_SIZE;
        opt_.groupSize = ZBAL_UT_GROUP_SIZE;
        opt_.myWorldRank = ZBAL_UT_WORLD_RANK;
        opt_.myGroupRank = ZBAL_UT_GROUP_RANK;
        opt_.deviceId = ZBAL_UT_DEVICE_ID;
    }

    void TearDown() override
    {
        Communicator::DestroyAll();
    }

    CommGroupOptions opt_;
};

TEST_F(TestZBALCommunicatorDummy, Construction)
{
    {
        CommunicatorDummy world(opt_, true, nullptr);
        EXPECT_TRUE(world.IsWorldGroup());
        CommunicatorDummy nonWorld(opt_, false, nullptr);
        EXPECT_FALSE(nonWorld.IsWorldGroup());
    }
    {
        const char *testNames[] = {"g", "moe_ep_12", "a_very_long_communicator_name", ""};
        for (auto *nm : testNames) {
            opt_.name = nm;
            CommunicatorDummy comm(opt_, false, nullptr);
            EXPECT_EQ(comm.Name(), std::string(nm));
        }
    }
    {
        CommunicatorDummy comm(opt_, false, nullptr);
        EXPECT_EQ(comm.GroupId(), UINT16_MAX);
    }
    {
        CommunicatorDummy comm(opt_, false, nullptr);
        const auto &m = comm.GetMetaInfo();
        EXPECT_EQ(m.groupSize, ZBAL_UT_NUM_0);
        EXPECT_EQ(m.myGroupRank, ZBAL_UT_NUM_0);
        EXPECT_EQ(m.groupIndex, ZBAL_UT_NUM_0);
        EXPECT_EQ(m.waitSymbol, ZBAL_UT_NUM_0);
        EXPECT_EQ(m.myMetaGva, ZBAL_UT_NUM_0);
    }
    {
        CommunicatorDummy worldGrp(opt_, true, nullptr);
        EXPECT_TRUE(worldGrp.IsWorldGroup());
        CommunicatorDummy subGrp(opt_, false, CommunicatorPtr(&worldGrp));
        EXPECT_FALSE(subGrp.IsWorldGroup());
    }
    {
        CommGroupOptions optA;
        optA.name = "comm_a";
        optA.groupSize = ZBAL_UT_NUM_4;
        optA.myGroupRank = ZBAL_UT_NUM_0;
        CommGroupOptions optB;
        optB.name = "comm_b";
        optB.groupSize = ZBAL_UT_NUM_8;
        optB.myGroupRank = ZBAL_UT_NUM_3;
        CommunicatorDummy commA(optA, true, nullptr);
        CommunicatorDummy commB(optB, false, nullptr);
        EXPECT_TRUE(commA.IsWorldGroup());
        EXPECT_FALSE(commB.IsWorldGroup());
        EXPECT_EQ(commA.Name(), "comm_a");
        EXPECT_EQ(commB.Name(), "comm_b");
        EXPECT_NE(&commA.GetMetaInfo(), &commB.GetMetaInfo());
    }
}

TEST_F(TestZBALCommunicatorDummy, Initialize)
{
    CommunicatorDummy comm(opt_, false, nullptr);
    EXPECT_EQ(comm.Initialize(), Z_OK);

    const auto &m1 = comm.GetMetaInfo();
    const auto &m2 = comm.GetMetaInfo();
    EXPECT_EQ(&m1, &m2);

    comm.UnInitialize();
    EXPECT_EQ(comm.Initialize(), Z_OK);
    comm.UnInitialize();
    SUCCEED();
}

TEST_F(TestZBALCommunicatorDummy, StaticApi)
{
    Communicator::DestroyAll();

    {
        ZBALInitStateExt ext;
        ext.worldSize = ZBAL_UT_NUM_1;
        ext.worldRankId = ZBAL_UT_NUM_0;
        ext.deviceId = ZBAL_UT_NUM_0;
        ext.commMetaSpaceSize = ZBAL_UT_COMM_META_SPACE_SIZE;
        ext.commGroupCap = ZBAL_UT_COMM_GROUP_CAP;
        uintptr_t base = 0x10000;
        ext.myCommMetaDeviceGva = reinterpret_cast<void *>(base);
        ext.metaSizeOfDevice = ZBAL_UT_NUM_512 * ZBAL_UT_SIZE_1KB * ZBAL_UT_COMM_GROUP_CAP;
        ext.gvaDevice = reinterpret_cast<void *>(0x20000);
        GroupMetaArranger::Instance().UnInitialize();
        zbal_comm_options_t apiOpt{};
        apiOpt.name = const_cast<char *>("world_dummy");
        apiOpt.backendType = ZBAL_BACK_BUTT;
        apiOpt.isWorldGroup = true;
        apiOpt.groupSize = ZBAL_UT_NUM_1;
        apiOpt.groupRankId = ZBAL_UT_NUM_0;
        zbal_comm_t comm = nullptr;
        auto result = Communicator::Create(apiOpt, &comm, ext);
        EXPECT_NE(result, Z_OK);
        Communicator::DestroyAll();
    }
    {
        EXPECT_NE(Communicator::Destroy(nullptr, ZBAL_UT_NUM_0), Z_OK);
    }
    {
        Communicator::DestroyAll();
        zbal_comm_t comm = nullptr;
        EXPECT_NE(Communicator::Lookup("no_such_comm", &comm), Z_OK);
    }
    {
        Communicator::DestroyAll();
        zbal_comm_t comm = nullptr;
        EXPECT_NE(Communicator::GetGlobalComm(&comm), Z_OK);
    }
    {
        Communicator::DestroyAll();
        EXPECT_EQ(Communicator::Count(), ZBAL_UT_NUM_0);
    }
    {
        Communicator::DestroyAll();
        EXPECT_NO_THROW(Communicator::DumpAllComm());
    }
    {
        zbal_comm_property_t prop{};
        EXPECT_NE(Communicator::GetCommProperty(nullptr, &prop), Z_OK);
    }
}

TEST_F(TestZBALCommunicatorDummy, CreateDestroyInner)
{
    Communicator::DestroyAll();
    EXPECT_EQ(Communicator::gWorldCommunicator.Get(), nullptr);
    EXPECT_EQ(Communicator::CreateInner(ZBAL_BACK_BUTT, opt_, false), nullptr);

    CommunicatorPtr nullComm(nullptr);
    EXPECT_NE(Communicator::DestroyInner(nullComm), Z_OK);

    CommunicatorDummy comm(opt_, false, nullptr);
    CommunicatorPtr commPtr(&comm);
    EXPECT_EQ(Communicator::DestroyInner(commPtr), Z_OK);
    commPtr = nullptr;
}

TEST_F(TestZBALCommunicatorDummy, Operations)
{
    CommunicatorDummy comm(opt_, false, nullptr);

    EXPECT_NO_THROW(comm.ConstructCommGroupInfo(opt_));
    EXPECT_NO_THROW(comm.DumpProfilingTrace());
    EXPECT_NO_THROW(comm.SignalDumpTrace());

    AutoReleaseGroupId groupId(ZBAL_UT_NUM_128, ZBAL_UT_NUM_4, ZBAL_UT_NUM_0, ZBAL_UT_NUM_0, "test_group");
    EXPECT_EQ(comm.AssignGatherGroupId(groupId), Z_OK);

    int buf[ZBAL_UT_NUM_16] = {};
    EXPECT_EQ(comm.AllReduce(buf, buf, nullptr, ZBAL_UT_NUM_16, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, nullptr), 0);
    EXPECT_EQ(comm.ReduceScatter(buf, buf, ZBAL_UT_NUM_16, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, nullptr), 0);
    EXPECT_EQ(comm.AllGather(buf, buf, ZBAL_UT_NUM_16, ZBAL_DATA_TYPE_INT32, nullptr), 0);
    EXPECT_EQ(comm.Broadcast(buf, ZBAL_UT_NUM_16, ZBAL_DATA_TYPE_INT32, ZBAL_UT_NUM_0, nullptr), 0);
    EXPECT_EQ(comm.Scatter(buf, buf, ZBAL_UT_NUM_16, ZBAL_DATA_TYPE_INT32, ZBAL_UT_NUM_0, nullptr), 0);
    EXPECT_EQ(comm.Gather(buf, buf, ZBAL_UT_NUM_16, ZBAL_DATA_TYPE_INT32, ZBAL_UT_NUM_0, nullptr), 0);
    EXPECT_EQ(comm.Barrier(nullptr), 0);
    EXPECT_EQ(comm.Send(buf, ZBAL_DATA_TYPE_INT32, ZBAL_UT_NUM_0, nullptr), 0);
    EXPECT_EQ(comm.Recv(buf, ZBAL_UT_NUM_16, ZBAL_DATA_TYPE_INT32, ZBAL_UT_NUM_0, nullptr), 0);

    {
        uint64_t sendCounts[ZBAL_UT_NUM_4] = {ZBAL_UT_NUM_4, ZBAL_UT_NUM_4, ZBAL_UT_NUM_4, ZBAL_UT_NUM_4};
        uint64_t recvCounts[ZBAL_UT_NUM_4] = {ZBAL_UT_NUM_4, ZBAL_UT_NUM_4, ZBAL_UT_NUM_4, ZBAL_UT_NUM_4};
        EXPECT_EQ(comm.AlltoAllV(buf, buf, sendCounts, recvCounts, nullptr, ZBAL_DATA_TYPE_INT32, nullptr), 0);
    }

    {
        zbal_tensor_info_t sendTokens{};
        EXPECT_EQ(comm.DispatchNormalNotify(&sendTokens, ZBAL_UT_NUM_4, ZBAL_UT_NUM_2, nullptr, nullptr, nullptr,
                                            nullptr, nullptr, nullptr, ZBAL_UT_NUM_0),
                  0);
    }
    {
        zbal_tensor_info_t sendTokens{};
        int totalRecvData = ZBAL_UT_NUM_42;
        zbal_tensor_info_t totalRecv{};
        totalRecv.data = &totalRecvData;
        EXPECT_EQ(comm.DispatchNormalNotify(&sendTokens, ZBAL_UT_NUM_4, ZBAL_UT_NUM_2, nullptr, &totalRecv, nullptr,
                                            nullptr, nullptr, nullptr, ZBAL_UT_NUM_0),
                  0);
        EXPECT_EQ(totalRecvData, ZBAL_UT_NUM_42);
    }

    {
        zbal_tensor_info_t t{};
        EXPECT_EQ(comm.DispatchNormalLayout(&t, ZBAL_UT_NUM_4, ZBAL_UT_NUM_2, ZBAL_UT_NUM_2, &t, &t, &t, &t, nullptr,
                                            ZBAL_UT_NUM_0),
                  0);
        EXPECT_EQ(comm.DispatchNormal(&t, &t, &t, &t, &t, ZBAL_UT_NUM_2, NO_QUANT, &t, &t, nullptr, ZBAL_UT_NUM_0), 0);
        EXPECT_EQ(comm.CombineNormal(&t, &t, &t, &t, &t, &t, ZBAL_UT_NUM_2, &t, nullptr, ZBAL_UT_NUM_0), 0);
        EXPECT_EQ(comm.DispatchLowLatency(&t, &t, ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, ZBAL_UT_NUM_0, ZBAL_UT_NUM_0,
                                          ZBAL_UT_NUM_0, ZBAL_UT_NUM_0, ZBAL_UT_NUM_0, &t, &t, &t, &t, &t, &t, nullptr,
                                          nullptr, ZBAL_UT_NUM_0),
                  0);
        EXPECT_EQ(comm.CombineLowLatency(&t, &t, &t, &t, &t, &t, ZBAL_UT_NUM_2, nullptr, ZBAL_UT_NUM_0), 0);
    }
}
