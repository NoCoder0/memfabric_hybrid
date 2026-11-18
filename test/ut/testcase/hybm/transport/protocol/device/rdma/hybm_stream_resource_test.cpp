/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <gtest/gtest.h>
#include <unistd.h>
#include <mockcpp/mockcpp.hpp>
#include "vendor/vendor_api.h"
#include "vendor/ascend/dl_hal_api.h"
#include "hybm_stream.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::mf;

namespace {
int failedAllocationType;
bool failStreamIdFree;
uint32_t normalQueueFrees;
uint32_t streamIdFrees;
uint32_t synchronizationSleeps;
uint32_t synchronizationHeadQueries;
} // namespace

class HybmStreamResourceTest : public testing::Test {
protected:
    void SetUp() override
    {
        failedAllocationType = -1;
        failStreamIdFree = false;
        normalQueueFrees = 0;
        streamIdFrees = 0;
        MOCKER_CPP(&ock::mf::AscendApiImpl::GetSocName, ock::mf::AscendSocType(*)(ock::mf::AscendApiImpl *))
            .stubs()
            .will(returnValue(ock::mf::AscendSocType::ASCEND_910B));
        MOCKER(DlHalApi::HalResourceIdAlloc)
            .stubs()
            .will(invoke(+[](uint32_t, halResourceIdInputInfo *, halResourceIdOutputInfo *out) {
                out->resourceId = 901;
                return BM_OK;
            }));
        MOCKER(DlHalApi::HalSqCqAllocate)
            .stubs()
            .will(invoke(+[](uint32_t, halSqCqInputInfo *in, halSqCqOutputInfo *out) {
                if (static_cast<int>(in->type) == failedAllocationType) {
                    return -1;
                }
                out->sqId = 21;
                out->cqId = in->type == DRV_NORMAL_TYPE ? 22 : 23;
                return BM_OK;
            }));
        MOCKER(DlHalApi::HalResourceConfig).stubs().will(returnValue(BM_OK));
        MOCKER(DlHalApi::HalSqCqFree).stubs().will(invoke(+[](uint32_t, halSqCqFreeInfo *info) {
            EXPECT_EQ(info->type, DRV_NORMAL_TYPE);
            ++normalQueueFrees;
            return BM_OK;
        }));
        MOCKER(DlHalApi::HalResourceIdFree).stubs().will(invoke(+[](uint32_t, halResourceIdInputInfo *in) {
            EXPECT_EQ(in->resourceId, 901U);
            ++streamIdFrees;
            return failStreamIdFree ? -1 : BM_OK;
        }));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

TEST_F(HybmStreamResourceTest, NormalQueueAllocationFailureReleasesStreamId)
{
    failedAllocationType = DRV_NORMAL_TYPE;
    HybmStream stream(0, 0, 0);
    EXPECT_NE(stream.Initialize(), BM_OK);
    EXPECT_EQ(stream.GetId(), UINT32_MAX);
    EXPECT_EQ(streamIdFrees, 1U);
    EXPECT_EQ(normalQueueFrees, 0U);
}

TEST_F(HybmStreamResourceTest, LogicQueueAllocationFailureReleasesNormalQueueAndStreamId)
{
    failedAllocationType = DRV_LOGIC_TYPE;
    HybmStream stream(0, 0, 0);
    EXPECT_NE(stream.Initialize(), BM_OK);
    EXPECT_EQ(stream.GetId(), UINT32_MAX);
    EXPECT_EQ(normalQueueFrees, 1U);
    EXPECT_EQ(streamIdFrees, 1U);
}

TEST_F(HybmStreamResourceTest, FailedStreamIdFreeDoesNotRepeatSuccessfulQueueFree)
{
    HybmStream stream(0, 0, 0);
    ASSERT_EQ(stream.Initialize(), BM_OK);
    failStreamIdFree = true;
    stream.Destroy();
    EXPECT_EQ(stream.GetId(), 901U);
    failStreamIdFree = false;
    stream.Destroy();
    EXPECT_EQ(stream.GetId(), UINT32_MAX);
    EXPECT_EQ(normalQueueFrees, 1U);
    EXPECT_EQ(streamIdFrees, 2U);
}

TEST_F(HybmStreamResourceTest, FailedCompletionQueryCannotReportSuccessfulSynchronization)
{
    MOCKER(DlHalApi::HalSqTaskSend).stubs().will(returnValue(BM_OK));
    MOCKER(DlHalApi::HalSqCqQuery).stubs().will(invoke(+[](uint32_t, halSqCqQueryInfo *query) {
        if (query->prop == DRV_SQCQ_PROP_SQ_HEAD) {
            query->value[0] = 1;
            return BM_OK;
        }
        return -1;
    }));
    HybmStream stream(0, 0, 0);
    ASSERT_EQ(stream.Initialize(), BM_OK);
    StreamTask task{};
    task.type = STREAM_TASK_TYPE_NOTIFY;
    ASSERT_EQ(stream.SubmitTasks(task), BM_OK);
    EXPECT_NE(stream.Synchronize(), BM_OK);
}

TEST_F(HybmStreamResourceTest, CompletedSynchronizationDoesNotSleepAgain)
{
    synchronizationSleeps = 0;
    MOCKER(DlHalApi::HalSqTaskSend).stubs().will(returnValue(BM_OK));
    MOCKER(DlHalApi::HalSqCqQuery).stubs().will(invoke(+[](uint32_t, halSqCqQueryInfo *query) {
        query->value[0] = query->prop == DRV_SQCQ_PROP_SQ_HEAD ? 1U : 0U;
        return BM_OK;
    }));
    MOCKER(usleep).stubs().will(invoke(+[](useconds_t) {
        ++synchronizationSleeps;
        return 0;
    }));
    HybmStream stream(0, 0, 0);
    ASSERT_EQ(stream.Initialize(), BM_OK);
    StreamTask task{};
    task.type = STREAM_TASK_TYPE_NOTIFY;
    ASSERT_EQ(stream.SubmitTasks(task), BM_OK);
    EXPECT_EQ(stream.Synchronize(), BM_OK);
    EXPECT_EQ(synchronizationSleeps, 0U);
}

TEST_F(HybmStreamResourceTest, PendingSynchronizationStillSleepsBeforeCheckingAgain)
{
    synchronizationSleeps = 0;
    synchronizationHeadQueries = 0;
    MOCKER(DlHalApi::HalSqTaskSend).stubs().will(returnValue(BM_OK));
    MOCKER(DlHalApi::HalSqCqQuery).stubs().will(invoke(+[](uint32_t, halSqCqQueryInfo *query) {
        query->value[0] = 0;
        if (query->prop == DRV_SQCQ_PROP_SQ_HEAD) {
            query->value[0] = synchronizationHeadQueries++ == 0 ? 0U : 1U;
        }
        return BM_OK;
    }));
    MOCKER(usleep).stubs().will(invoke(+[](useconds_t) {
        ++synchronizationSleeps;
        return 0;
    }));
    HybmStream stream(0, 0, 0);
    ASSERT_EQ(stream.Initialize(), BM_OK);
    StreamTask task{};
    task.type = STREAM_TASK_TYPE_NOTIFY;
    ASSERT_EQ(stream.SubmitTasks(task), BM_OK);
    EXPECT_EQ(stream.Synchronize(), BM_OK);
    EXPECT_EQ(synchronizationHeadQueries, 2U);
    EXPECT_EQ(synchronizationSleeps, 1U);
}
