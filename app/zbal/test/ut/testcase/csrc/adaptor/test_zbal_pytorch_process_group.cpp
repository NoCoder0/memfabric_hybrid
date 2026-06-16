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

#include <memory>
#include <string>
#include <vector>
#include <cstdlib>

#define private   public
#define protected public

#include "zbal_test_constants.h"
#include "zbal_pytorch_process_group.h"
#include "zbal_pytorch_process_group_impl.h"
#include "zbal_init_state.h"
#include "zbal_defines.h"
#include "zbal_env_helper.h"
#include "zbal_functions.h"

#undef private
#undef protected

namespace zbal {
namespace adaptor {
namespace pytorch_npu {

using namespace zbal;

class TestPGZBAL : public ::testing::Test {
protected:
    void SetUp() override
    {
        ZBALInitState::Instance().Bootstrapped(false);
        unsetenv(ENV_NAME_HCCL_OP);
        ProcessGroupZBALImpl::hcclOp_.clear();
    }

    void SetupImplWithHccl(const std::string &ops)
    {
        setenv(ENV_NAME_HCCL_OP, ops.c_str(), 0);
        ProcessGroupZBALImpl::hcclOp_.clear();
        ProcessGroupZBALImpl::hcclOp_ = Func::GetEnvSplitByComma(ENV_NAME_HCCL_OP);
    }

    void CleanupHcclEnv()
    {
        unsetenv(ENV_NAME_HCCL_OP);
        ProcessGroupZBALImpl::hcclOp_.clear();
    }
};

TEST_F(TestPGZBAL, Options)
{
    {
        ProcessGroupZBAL::Options opts(false);
        EXPECT_FALSE(opts.isHighPriorityStream);
        EXPECT_EQ(opts.opTimeout, WORKER_MAX_TIMEOUT);
        EXPECT_TRUE(opts.globalRanksInGroup.empty());
        EXPECT_TRUE(opts.groupId.empty());
    }
    {
        ProcessGroupZBAL::Options opts(true);
        EXPECT_TRUE(opts.isHighPriorityStream);
    }
    EXPECT_FALSE(ProcessGroupZBAL::Options::create()->isHighPriorityStream);
    EXPECT_TRUE(ProcessGroupZBAL::Options::create(true)->isHighPriorityStream);
    {
        auto opts = ProcessGroupZBAL::Options::create(true, std::chrono::milliseconds(ZBAL_UT_NUM_5000));
        EXPECT_TRUE(opts->isHighPriorityStream);
        EXPECT_EQ(opts->opTimeout, WORKER_MAX_TIMEOUT);
    }
}

TEST_F(TestPGZBAL, Constructor)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);

    EXPECT_THROW(
        { c10::make_intrusive<ProcessGroupZBAL>(store, -1, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create()); },
        std::runtime_error);
    EXPECT_THROW(
        { c10::make_intrusive<ProcessGroupZBAL>(store, 0, 0, ProcessGroupZBAL::Options::create()); },
        std::runtime_error);
    EXPECT_THROW(
        {
            c10::make_intrusive<ProcessGroupZBAL>(store, ZBAL_UT_NUM_5, ZBAL_UT_NUM_5,
                                                  ProcessGroupZBAL::Options::create());
        },
        std::runtime_error);

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        EXPECT_NE(pg, nullptr);
        EXPECT_EQ(pg->getBackendName(), "zbal");
        EXPECT_FALSE(pg->getZBALCommName().empty());
        EXPECT_EQ(pg->myWorldRank_, 0);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, 1, ProcessGroupZBAL::Options::create());
        EXPECT_NE(pg, nullptr);
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {0, ZBAL_UT_NUM_2, ZBAL_UT_NUM_4, ZBAL_UT_NUM_6};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, ZBAL_UT_NUM_2, ZBAL_UT_NUM_4, opts);
        EXPECT_NE(pg, nullptr);
        EXPECT_EQ(pg->myWorldRank_, ZBAL_UT_NUM_4);
    }
    {
        auto pg =
            c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create(true));
        EXPECT_NE(pg, nullptr);
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->opTimeout = std::chrono::milliseconds(ZBAL_UT_NUM_3000000);
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, opts);
        EXPECT_EQ(pg->opTimeout_, std::chrono::milliseconds(WORKER_MAX_TIMEOUT));
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->opTimeout = std::chrono::milliseconds(ZBAL_UT_NUM_100);
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, opts);
        EXPECT_LE(pg->opTimeout_.count(), std::chrono::milliseconds(WORKER_MAX_TIMEOUT).count());
    }
}

TEST_F(TestPGZBAL, GroupCounterAndCommNames)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);

    auto refPg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
    uint64_t c0 = refPg->GetNextGroupCounter();
    EXPECT_EQ(refPg->GetNextGroupCounter(), c0 + 1);

    std::vector<std::string> names;
    for (int i = 0; i < ZBAL_UT_NUM_3; ++i) {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        names.push_back(pg->getZBALCommName());
    }
    EXPECT_NE(names[0], names[1]);
    EXPECT_NE(names[1], names[ZBAL_UT_NUM_2]);

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_4, ProcessGroupZBAL::Options::create());
        EXPECT_NE(pg->getZBALCommName().find("zbal_"), std::string::npos);
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {0, ZBAL_UT_NUM_2, ZBAL_UT_NUM_4};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_3, opts);
        EXPECT_NE(pg->getZBALCommName().find("zbal_"), std::string::npos);
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {0, ZBAL_UT_NUM_2, ZBAL_UT_NUM_5};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_3, opts);
        EXPECT_NE(pg->getZBALCommName().find("zbal_"), std::string::npos);
    }
}

TEST_F(TestPGZBAL, ConstructP2pCommName)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        EXPECT_EQ(pg->ConstructP2pCommName(1), "0:1");
        EXPECT_EQ(pg->ConstructP2pCommName(0), "0:0");
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {ZBAL_UT_NUM_10, ZBAL_UT_NUM_20, ZBAL_UT_NUM_30, ZBAL_UT_NUM_40};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_4, opts);
        EXPECT_EQ(pg->ConstructP2pCommName(ZBAL_UT_NUM_2), "20:30");
        EXPECT_EQ(pg->ConstructP2pCommName(0), "10:20");
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {ZBAL_UT_NUM_100, ZBAL_UT_NUM_200};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_2, opts);
        EXPECT_EQ(pg->ConstructP2pCommName(0), "100:200");
    }
}

TEST_F(TestPGZBAL, ConstructCommName)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, 1, ProcessGroupZBAL::Options::create());
        std::string name = pg->ConstructCommName();
        EXPECT_NE(name.find("zbal_"), std::string::npos);
        EXPECT_NE(name.find("0:0:1"), std::string::npos);
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {ZBAL_UT_NUM_5};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, 1, opts);
        std::string name = pg->ConstructCommName();
        EXPECT_NE(name.find("zbal_"), std::string::npos);
        EXPECT_NE(name.find("5:5:1"), std::string::npos);
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {0, ZBAL_UT_NUM_2, ZBAL_UT_NUM_5};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_3, opts);
        std::string name = pg->ConstructCommName();
        EXPECT_NE(name.find("zbal_"), std::string::npos);
        EXPECT_NE(name.find("0:5:2"), std::string::npos);
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {0, ZBAL_UT_NUM_4, ZBAL_UT_NUM_8, ZBAL_UT_NUM_12};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_4, opts);
        std::string name = pg->ConstructCommName();
        EXPECT_NE(name.find("zbal_"), std::string::npos);
        EXPECT_NE(name.find("0:12:4"), std::string::npos);
    }
}

TEST_F(TestPGZBAL, PrepareResources)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());

    std::vector<at::Device> multiDevices = {
        at::Device(c10::DeviceType::PrivateUse1, 0),
        at::Device(c10::DeviceType::PrivateUse1, 1),
    };
    std::string groupName = "test_group";
    EXPECT_EQ(pg->PrepareResources(groupName, multiDevices), Z_INVALID_PARAM);

    EnvHelper::OP_DEFAULT_STREAM = false;
    std::vector<at::Device> devices = {at::Device(c10::DeviceType::PrivateUse1, 0)};
    groupName = "test_group_pool";
    EXPECT_EQ(pg->PrepareResources(groupName, devices), Z_OK);

    EnvHelper::OP_DEFAULT_STREAM = true;
    groupName = "test_group_default";
    EXPECT_EQ(pg->PrepareResources(groupName, devices), Z_OK);
    EnvHelper::OP_DEFAULT_STREAM = false;
}

TEST_F(TestPGZBAL, DestructorAndInitCommunicator)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        EXPECT_EQ(pg->groupComm_, nullptr);
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {0, 1};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, opts);
    }

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        pg->groupComm_ = reinterpret_cast<zbal_comm_t>(1);
        EXPECT_EQ(pg->initCommunicator(), Z_OK);
        pg->groupComm_ = nullptr;
    }

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        std::string groupName = "0:1";
        auto fakeComm = reinterpret_cast<zbal_comm_t>(0xdead);
        pg->groupP2pComms_[groupName] = fakeComm;
        EXPECT_EQ(pg->initP2pCommunicator(1, groupName), Z_OK);
        pg->groupP2pComms_.clear();
    }

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_4, ProcessGroupZBAL::Options::create());
        std::string name = pg->getZBALCommName();
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name.find("zbal_"), std::string::npos);
    }

    SUCCEED();
}

TEST_F(TestPGZBAL, PrepareCommunicatorAndInitP2pComm)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);

    ZBALInitState::Instance().Bootstrapped(true);
    EXPECT_THROW(
        { c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create()); },
        std::runtime_error);
    ZBALInitState::Instance().Bootstrapped(false);

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        EXPECT_NE(pg, nullptr);
    }

    ZBALInitState::Instance().Bootstrapped(true);
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {0, 1};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, opts);
        EXPECT_NE(pg, nullptr);
    }
    ZBALInitState::Instance().Bootstrapped(false);

    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {ZBAL_UT_NUM_10, ZBAL_UT_NUM_20, ZBAL_UT_NUM_30};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_3, opts);
        std::string gn = "20:30";
        EXPECT_EQ(pg->initP2pCommunicator(ZBAL_UT_NUM_2, gn), Z_CREATE_COMM_FAILED);
        pg->groupP2pComms_.clear();
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {ZBAL_UT_NUM_10, ZBAL_UT_NUM_20, ZBAL_UT_NUM_30, ZBAL_UT_NUM_40};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_4, opts);
        std::string gn = "10:30";
        EXPECT_EQ(pg->initP2pCommunicator(ZBAL_UT_NUM_2, gn), Z_CREATE_COMM_FAILED);
        pg->groupP2pComms_.clear();
    }
    {
        auto opts = ProcessGroupZBAL::Options::create();
        opts->globalRanksInGroup = {ZBAL_UT_NUM_10, ZBAL_UT_NUM_20, ZBAL_UT_NUM_30, ZBAL_UT_NUM_40};
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, ZBAL_UT_NUM_3, ZBAL_UT_NUM_4, opts);
        std::string gn = "20:40";
        EXPECT_EQ(pg->initP2pCommunicator(1, gn), Z_CREATE_COMM_FAILED);
        pg->groupP2pComms_.clear();
    }
}

TEST_F(TestPGZBAL, AllreduceErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());
    c10d::AllreduceOptions opts;
    opts.reduceOp = c10d::ReduceOp::SUM;

    auto sparseTensors = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
    EXPECT_THROW({ pg->allreduce(sparseTensors, opts); }, std::runtime_error);

    auto ncTensors = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).t()};
    EXPECT_THROW({ pg->allreduce(ncTensors, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, BroadcastErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());
    c10d::BroadcastOptions opts;
    opts.rootRank = 0;

    auto ncTensors = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).t()};
    EXPECT_THROW({ pg->broadcast(ncTensors, opts); }, std::runtime_error);

    auto sparseTensors = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
    EXPECT_THROW({ pg->broadcast(sparseTensors, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, AllgatherBaseErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());
    c10d::AllgatherOptions opts;

    at::Tensor input = at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
    at::Tensor outType = at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kInt);
    EXPECT_THROW({ pg->_allgather_base(outType, input, opts); }, std::runtime_error);

    at::Tensor outSize = at::ones({ZBAL_UT_NUM_3, ZBAL_UT_NUM_3}, at::kFloat);
    EXPECT_THROW({ pg->_allgather_base(outSize, input, opts); }, std::runtime_error);

    at::Tensor inNc = input.t();
    at::Tensor outOk = at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
    EXPECT_THROW({ pg->_allgather_base(outOk, inNc, opts); }, std::runtime_error);

    at::Tensor outNc = at::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_3}, at::kFloat).t();
    EXPECT_THROW({ pg->_allgather_base(outNc, input, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, ReduceScatterBaseErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());
    c10d::ReduceScatterOptions opts;
    opts.reduceOp = c10d::ReduceOp::SUM;

    at::Tensor output = at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);

    at::Tensor inType = at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kInt);
    EXPECT_THROW({ pg->_reduce_scatter_base(output, inType, opts); }, std::runtime_error);

    at::Tensor inSize = at::ones({ZBAL_UT_NUM_5, ZBAL_UT_NUM_3}, at::kFloat);
    EXPECT_THROW({ pg->_reduce_scatter_base(output, inSize, opts); }, std::runtime_error);

    at::Tensor outNc = output.t();
    at::Tensor inOk = at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
    EXPECT_THROW({ pg->_reduce_scatter_base(outNc, inOk, opts); }, std::runtime_error);

    at::Tensor inNc = at::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_3}, at::kFloat).t();
    EXPECT_THROW({ pg->_reduce_scatter_base(output, inNc, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, AlltoallBaseErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());
    c10d::AllToAllOptions opts;
    std::vector<int64_t> emptySplits;

    at::Tensor outType = at::ones({ZBAL_UT_NUM_4}, at::kInt);
    at::Tensor inType = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
    EXPECT_THROW({ pg->alltoall_base(outType, inType, emptySplits, emptySplits, opts); }, std::runtime_error);

    at::Tensor outZero = at::ones({0}, at::kFloat);
    at::Tensor inZero = at::ones({0}, at::kFloat);
    EXPECT_THROW({ pg->alltoall_base(outZero, inZero, emptySplits, emptySplits, opts); }, std::runtime_error);

    at::Tensor outNc = at::ones({ZBAL_UT_NUM_3, ZBAL_UT_NUM_4}, at::kFloat).t();
    at::Tensor in2d = at::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_3}, at::kFloat);
    EXPECT_THROW({ pg->alltoall_base(outNc, in2d, emptySplits, emptySplits, opts); }, std::runtime_error);

    at::Tensor out4 = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
    at::Tensor in4 = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
    std::vector<int64_t> outSplits2 = {ZBAL_UT_NUM_2, ZBAL_UT_NUM_2};
    std::vector<int64_t> negSplits = {1, -1};
    EXPECT_THROW({ pg->alltoall_base(out4, in4, outSplits2, negSplits, opts); }, std::runtime_error);

    std::vector<int64_t> sumMismatchSplits = {1, ZBAL_UT_NUM_2};
    EXPECT_THROW({ pg->alltoall_base(out4, in4, outSplits2, sumMismatchSplits, opts); }, std::runtime_error);

    at::Tensor out6 = at::ones({ZBAL_UT_NUM_6}, at::kFloat);
    at::Tensor in6 = at::ones({ZBAL_UT_NUM_6}, at::kFloat);
    std::vector<int64_t> badSplits = {ZBAL_UT_NUM_3, ZBAL_UT_NUM_2};
    EXPECT_THROW({ pg->alltoall_base(out6, in6, badSplits, badSplits, opts); }, std::runtime_error);

    at::Tensor out5 = at::ones({ZBAL_UT_NUM_5}, at::kFloat);
    at::Tensor in5 = at::ones({ZBAL_UT_NUM_5}, at::kFloat);
    EXPECT_THROW({ pg->alltoall_base(out5, in5, emptySplits, emptySplits, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, SendRecvErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());

    auto sendSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
    EXPECT_THROW({ pg->send(sendSparse, 1, 0); }, std::runtime_error);

    auto sendNc = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).t()};
    EXPECT_THROW({ pg->send(sendNc, 1, 0); }, std::runtime_error);

    auto recvNc = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).t()};
    EXPECT_THROW({ pg->recv(recvNc, 0, 0); }, std::runtime_error);

    auto recvSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
    EXPECT_THROW({ pg->recv(recvSparse, 0, 0); }, std::runtime_error);
}

TEST_F(TestPGZBAL, GatherErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());
    c10d::GatherOptions opts;
    opts.rootRank = 0;

    std::vector<std::vector<at::Tensor>> outEmpty;
    auto inSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
    EXPECT_THROW({ pg->gather(outEmpty, inSparse, opts); }, std::runtime_error);

    auto outSize2 = std::vector<std::vector<at::Tensor>>{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)},
                                                         {at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
    auto inOk = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
    EXPECT_THROW({ pg->gather(outSize2, inOk, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, ScatterErrorPaths)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);
    c10d::ScatterOptions opts;
    opts.rootRank = 0;

    auto pg1 = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
    std::vector<std::vector<at::Tensor>> inEmpty;
    auto outSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
    EXPECT_THROW({ pg1->scatter(outSparse, inEmpty, opts); }, std::runtime_error);

    auto outNc = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).t()};
    EXPECT_THROW({ pg1->scatter(outNc, inEmpty, opts); }, std::runtime_error);

    auto pg0 = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
    auto outOk = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
    auto inSize2 = std::vector<std::vector<at::Tensor>>{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)},
                                                        {at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
    EXPECT_THROW({ pg0->scatter(outOk, inSize2, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, AllgatherErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());
    c10d::AllgatherOptions opts;

    std::vector<std::vector<at::Tensor>> outNonEmpty = {{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}}};
    auto inSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
    EXPECT_THROW({ pg->allgather(outNonEmpty, inSparse, opts); }, std::runtime_error);

    std::vector<std::vector<at::Tensor>> outDiffSize = {
        {{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat), at::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_5}, at::kFloat)}}};
    auto inOk = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
    EXPECT_THROW({ pg->allgather(outDiffSize, inOk, opts); }, std::runtime_error);

    std::vector<std::vector<at::Tensor>> outSizeMismatch = {{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}}};
    EXPECT_THROW({ pg->allgather(outSizeMismatch, inOk, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, ReduceScatterErrorPaths)
{
    auto pg = c10::make_intrusive<ProcessGroupZBAL>(c10::intrusive_ptr<c10d::Store>(nullptr), 0, ZBAL_UT_NUM_2,
                                                    ProcessGroupZBAL::Options::create());
    c10d::ReduceScatterOptions opts;
    opts.reduceOp = c10d::ReduceOp::SUM;

    auto outSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
    auto inOk2 = std::vector<std::vector<at::Tensor>>{
        {{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat), at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}}};
    EXPECT_THROW({ pg->reduce_scatter(outSparse, inOk2, opts); }, std::runtime_error);

    auto outOk = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
    auto inDiffSize = std::vector<std::vector<at::Tensor>>{
        {{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat), at::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_5}, at::kFloat)}}};
    EXPECT_THROW({ pg->reduce_scatter(outOk, inDiffSize, opts); }, std::runtime_error);
}

TEST_F(TestPGZBAL, CollectivePaths)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);

    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        auto outG = std::vector<std::vector<at::Tensor>>{
            {at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
        auto inG = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        EXPECT_THROW({ pg->gather(outG, inG, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        std::vector<std::vector<at::Tensor>> outEmpty;
        auto inGnr = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        EXPECT_THROW({ pg->gather(outEmpty, inGnr, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        std::vector<std::vector<at::Tensor>> outEmpty;
        auto inGnr2 = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        EXPECT_THROW({ pg->gather(outEmpty, inGnr2, {.rootRank = 0}); }, std::runtime_error);

        auto outGnr = std::vector<std::vector<at::Tensor>>{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
        EXPECT_THROW({ pg->gather(outGnr, inGnr2, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_3, ProcessGroupZBAL::Options::create());
        auto outMr = std::vector<std::vector<at::Tensor>>{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
                                                           at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
                                                           at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
        auto inMr = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        EXPECT_THROW({ pg->gather(outMr, inMr, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        auto outS = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        auto inS = std::vector<std::vector<at::Tensor>>{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
                                                         at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
        EXPECT_THROW({ pg->scatter(outS, inS, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 1, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        auto outSnr = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        std::vector<std::vector<at::Tensor>> inEmpty;
        EXPECT_THROW({ pg->scatter(outSnr, inEmpty, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        auto sendOk = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        EXPECT_THROW({ pg->send(sendOk, 1, 0); }, std::runtime_error);

        auto recvOk = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        EXPECT_THROW({ pg->recv(recvOk, 0, 0); }, std::runtime_error);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        at::Tensor out4 = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
        at::Tensor in4 = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
        std::vector<int64_t> emptySplits;
        EXPECT_THROW({ pg->alltoall_base(out4, in4, emptySplits, emptySplits, {}); }, std::runtime_error);

        std::vector<int64_t> splits2 = {ZBAL_UT_NUM_2, ZBAL_UT_NUM_2};
        EXPECT_THROW({ pg->alltoall_base(out4, in4, splits2, splits2, {}); }, std::runtime_error);
    }
    {
        auto pg = c10::make_intrusive<ProcessGroupZBAL>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        auto outS2 = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        auto inS2_multi = std::vector<std::vector<at::Tensor>>{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)},
                                                               {at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
        EXPECT_THROW({ pg->scatter(outS2, inS2_multi, {.rootRank = 0}); }, std::runtime_error);
    }
}

TEST_F(TestPGZBAL, WorkZBAL)
{
    std::vector<at::Device> devices = {at::Device(c10::DeviceType::PrivateUse1, 0)};
    auto work = c10::make_intrusive<ProcessGroupZBAL::WorkZBAL>(devices, 0, c10d::OpType::ALLREDUCE);
    work->outputs_ = std::make_shared<std::vector<at::Tensor>>(
        std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)});

    EXPECT_NE(work, nullptr);
    EXPECT_EQ(work->rank_, 0);
    EXPECT_EQ(work->result().size(), 1u);
    EXPECT_EQ(work->devices_.size(), 1u);
    EXPECT_EQ(work->zbalEndEvents_->size(), 1u);

    EXPECT_FALSE(work->finishedNPUExecutionInternal());
    EXPECT_FALSE(work->isCompleted());
    EXPECT_FALSE(work->isSuccess());
    EXPECT_FALSE(work->finishedNPUExecution());

    work->future_ = c10::make_intrusive<at::ivalue::Future>(c10::ListType::create(c10::TensorType::get()), devices);
    work->future_->markCompleted(at::IValue(*work->outputs_));
    EXPECT_NE(work->getFuture(), nullptr);

    EXPECT_TRUE(work->wait(std::chrono::milliseconds(0)));
    work->synchronize();
    work->checkAndSetException();
    EXPECT_EQ(work->exception(), nullptr);
    work->checkAndThrowException();

    std::vector<at::Device> mdev = {
        at::Device(c10::DeviceType::PrivateUse1, 0),
        at::Device(c10::DeviceType::PrivateUse1, 1),
    };
    auto mwork = c10::make_intrusive<ProcessGroupZBAL::WorkZBAL>(mdev, 1, c10d::OpType::BROADCAST);
    EXPECT_EQ(mwork->devices_.size(), 2u);
    EXPECT_EQ(mwork->zbalEndEvents_->size(), 2u);
    EXPECT_FALSE(mwork->finishedNPUExecutionInternal());
    EXPECT_FALSE(mwork->isCompleted());
}

TEST_F(TestPGZBAL, WorkZBALEdgeCases)
{
    std::vector<at::Device> devices = {at::Device(c10::DeviceType::PrivateUse1, 0)};
    auto work = c10::make_intrusive<ProcessGroupZBAL::WorkZBAL>(devices, 0, c10d::OpType::ALLREDUCE);
    work->outputs_ = std::make_shared<std::vector<at::Tensor>>(
        std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)});

    EXPECT_NO_THROW({ work->finishedNPUExecutionInternal(); });

    work->barrierTensors_ = {};
    work->blockingWait_ = false;
    work->synchronize();
    SUCCEED();

    work->blockingWait_ = true;
    work->opTimeout_ = std::chrono::milliseconds(-1);
    work->workStartTime_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(ZBAL_UT_NUM_100);
    EXPECT_THROW({ work->synchronize(); }, std::runtime_error);
}

TEST_F(TestPGZBAL, ImplConstructor)
{
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);
    auto opts = ProcessGroupZBAL::Options::create();

    EXPECT_TRUE(ProcessGroupZBALImpl::hcclOp_.empty());
    {
        auto impl = c10::make_intrusive<ProcessGroupZBALImpl>(store, 0, ZBAL_UT_NUM_2, opts);
        EXPECT_NE(impl, nullptr);
        EXPECT_NE(impl->getZBALCommName().find("zbal_"), std::string::npos);
        EXPECT_EQ(impl->hcclGroup_, nullptr);
    }

    SetupImplWithHccl("allreduce,allgather,broadcast");
    {
        auto impl = c10::make_intrusive<ProcessGroupZBALImpl>(store, 0, ZBAL_UT_NUM_2, opts);
        EXPECT_NE(impl, nullptr);
        EXPECT_NE(impl->zbalGroup_, nullptr);
        EXPECT_NE(impl->hcclGroup_, nullptr);
    }
    CleanupHcclEnv();

    {
        auto store2 = c10::intrusive_ptr<c10d::Store>(nullptr);
        auto impl = c10::make_intrusive<ProcessGroupZBALImpl>(store2, 0, ZBAL_UT_NUM_4, opts);
        std::string name = impl->getZBALCommName();
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name.find("zbal_"), std::string::npos);
        EXPECT_EQ(impl->initCommunicator(), Z_CREATE_COMM_FAILED);
    }
}

TEST_F(TestPGZBAL, ImplRoutesToZbal)
{
    ZBALInitState::Instance().Bootstrapped(false);
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);
    auto impl = c10::make_intrusive<ProcessGroupZBALImpl>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());

    {
        auto tensors = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
        EXPECT_THROW({ impl->allreduce(tensors, c10d::AllreduceOptions()); }, std::runtime_error);
    }
    {
        at::Tensor out = at::ones({ZBAL_UT_NUM_3, ZBAL_UT_NUM_3}, at::kFloat);
        at::Tensor in = at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
        EXPECT_THROW({ impl->_allgather_base(out, in, c10d::AllgatherOptions()); }, std::runtime_error);
    }
    {
        at::Tensor out = at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
        at::Tensor in = at::ones({ZBAL_UT_NUM_5, ZBAL_UT_NUM_3}, at::kFloat);
        EXPECT_THROW({ impl->_reduce_scatter_base(out, in, {.reduceOp = c10d::ReduceOp::SUM}); }, std::runtime_error);
    }
    {
        std::vector<std::vector<at::Tensor>> outNonEmpty = {{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}}};
        auto inSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
        EXPECT_THROW({ impl->allgather(outNonEmpty, inSparse, c10d::AllgatherOptions()); }, std::runtime_error);
    }
    {
        std::vector<std::vector<at::Tensor>> outEmpty;
        auto inSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
        EXPECT_THROW({ impl->gather(outEmpty, inSparse, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto tensors = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).t()};
        EXPECT_THROW({ impl->broadcast(tensors, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto impl2 =
            c10::make_intrusive<ProcessGroupZBALImpl>(store, 1, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        auto outSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
        std::vector<std::vector<at::Tensor>> inEmpty;
        EXPECT_THROW({ impl2->scatter(outSparse, inEmpty, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto outSparse = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
        auto inOk = std::vector<std::vector<at::Tensor>>{{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
                                                           at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}}};
        EXPECT_THROW({ impl->reduce_scatter(outSparse, inOk, {.reduceOp = c10d::ReduceOp::SUM}); }, std::runtime_error);
    }
    {
        at::Tensor out = at::ones({ZBAL_UT_NUM_4}, at::kInt);
        at::Tensor in = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
        std::vector<int64_t> emptySplits;
        EXPECT_THROW(
            { impl->alltoall_base(out, in, emptySplits, emptySplits, c10d::AllToAllOptions()); }, std::runtime_error);
    }
    {
        auto tensors = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()};
        EXPECT_THROW({ impl->send(tensors, 1, 0); }, std::runtime_error);
    }
    {
        auto tensors = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).t()};
        EXPECT_THROW({ impl->recv(tensors, 0, 0); }, std::runtime_error);
    }
}

TEST_F(TestPGZBAL, ImplValidInputsReachingCollective)
{
    ZBALInitState::Instance().Bootstrapped(false);
    auto store = c10::intrusive_ptr<c10d::Store>(nullptr);
    auto impl = c10::make_intrusive<ProcessGroupZBALImpl>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());

    {
        auto out = std::vector<std::vector<at::Tensor>>{
            {at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
             at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
        auto in = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        EXPECT_THROW({ impl->allgather(out, in, c10d::AllgatherOptions()); }, std::runtime_error);
    }
    {
        auto out = std::vector<std::vector<at::Tensor>>{
            {at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
        auto in = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        EXPECT_THROW({ impl->gather(out, in, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto impl2 =
            c10::make_intrusive<ProcessGroupZBALImpl>(store, 1, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        auto outS = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        std::vector<std::vector<at::Tensor>> inEmpty;
        EXPECT_THROW({ impl2->scatter(outS, inEmpty, {.rootRank = 0}); }, std::runtime_error);
    }
    {
        auto out = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2 * ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        auto in = std::vector<std::vector<at::Tensor>>{{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
                                                        at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
        EXPECT_THROW({ impl->reduce_scatter(out, in, {.reduceOp = c10d::ReduceOp::SUM}); }, std::runtime_error);
    }
    {
        at::Tensor out = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
        at::Tensor in = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
        std::vector<int64_t> splits = {ZBAL_UT_NUM_2, ZBAL_UT_NUM_2};
        EXPECT_THROW({ impl->alltoall_base(out, in, splits, splits, c10d::AllToAllOptions()); }, std::runtime_error);
    }
}

TEST_F(TestPGZBAL, ImplRoutesToHccl)
{
    const char *ops[] = {"allreduce", "broadcast", "allgather", "alltoall", "reduce_scatter", "send", "recv"};
    for (const auto &op : ops) {
        ZBALInitState::Instance().Bootstrapped(false);
        SetupImplWithHccl(op);
        auto store = c10::intrusive_ptr<c10d::Store>(nullptr);
        auto impl =
            c10::make_intrusive<ProcessGroupZBALImpl>(store, 0, ZBAL_UT_NUM_2, ProcessGroupZBAL::Options::create());
        EXPECT_NE(impl->hcclGroup_, nullptr);

        auto t = std::vector<at::Tensor>{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)};
        if (std::string(op) == "allreduce") {
            EXPECT_ANY_THROW({ impl->allreduce(t, c10d::AllreduceOptions()); });
        } else if (std::string(op) == "broadcast") {
            EXPECT_ANY_THROW({ impl->broadcast(t, {.rootRank = 0}); });
        } else if (std::string(op) == "allgather") {
            at::Tensor out = at::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_3}, at::kFloat);
            EXPECT_ANY_THROW({ impl->_allgather_base(out, t[0], c10d::AllgatherOptions()); });
        } else if (std::string(op) == "alltoall") {
            at::Tensor out = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
            at::Tensor in = at::ones({ZBAL_UT_NUM_4}, at::kFloat);
            std::vector<int64_t> splits = {ZBAL_UT_NUM_2, ZBAL_UT_NUM_2};
            EXPECT_ANY_THROW({ impl->alltoall_base(out, in, splits, splits, c10d::AllToAllOptions()); });
        } else if (std::string(op) == "reduce_scatter") {
            at::Tensor out = at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
            at::Tensor in = at::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_3}, at::kFloat);
            EXPECT_ANY_THROW({ impl->_reduce_scatter_base(out, in, {.reduceOp = c10d::ReduceOp::SUM}); });
        } else if (std::string(op) == "send") {
            EXPECT_ANY_THROW({ impl->send(t, 1, 0); });
        } else if (std::string(op) == "recv") {
            EXPECT_ANY_THROW({ impl->recv(t, 0, 0); });
        }
        CleanupHcclEnv();
    }
}

} // namespace pytorch_npu
} // namespace adaptor
} // namespace zbal
