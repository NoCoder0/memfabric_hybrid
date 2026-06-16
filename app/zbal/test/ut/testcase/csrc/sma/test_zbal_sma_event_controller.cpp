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

#define private public
#include "dl_cann_api.h"
#include "zbal_sma_device_pool.h"
#undef private

#include "test_zbal_def.h"
#include "zbal_sma_device.h"

using namespace zbal;
using namespace zbal::sma;
using namespace zbal::sma::device;

class TestEventController : public testing::Test {
protected:
    void SetUp() override
    {
        zbal::underapi::DlCannApi::pAclrtSynchronizeEvent = nullptr;
    }

    void TearDown() override
    {
        zbal::underapi::DlCannApi::pAclrtSynchronizeEvent = nullptr;
    }

    DeviceSMACachingAllocator allocator_;
    EventController controller_;
    DeviceBlockPool pool_;
};

/* ================================================================
* EventController::get
* ================================================================ */

TEST_F(TestEventController, Get_BasicAndCache)
{
    auto e0 = controller_.get(0);
    EXPECT_NE(e0.get(), nullptr);
    EXPECT_EQ(controller_.pools_[0].event_pool_.size(), 0u);

    auto e1 = controller_.get(1);
    EXPECT_NE(e1.get(), nullptr);
    EXPECT_NE(e0.get(), e1.get());
}

TEST_F(TestEventController, Get_CacheHit)
{
    {
        auto e = controller_.get(0);
    }
    EXPECT_EQ(controller_.pools_[0].event_pool_.size(), 1u);

    auto e = controller_.get(0);
    EXPECT_NE(e.get(), nullptr);
    EXPECT_EQ(controller_.pools_[0].event_pool_.size(), 0u);
}

TEST_F(TestEventController, Get_InvalidDevice)
{
    EXPECT_THROW(controller_.get(-1), std::runtime_error);

    int deviceCount = static_cast<int>(controller_.pools_.size());
    EXPECT_THROW(controller_.get(deviceCount), std::runtime_error);
}

/* ================================================================
* EventController::emptyCache
* ================================================================ */

TEST_F(TestEventController, EmptyCache)
{
    std::vector<ZEvent> events;
    for (int i = 0; i < ZBAL_TEST_NUMBER_THREE; i++) {
        events.push_back(controller_.get(0));
    }
    EXPECT_EQ(controller_.pools_[0].event_pool_.size(), 0u);

    events.clear();
    EXPECT_EQ(controller_.pools_[0].event_pool_.size(), 3u);

    controller_.emptyCache();
    EXPECT_EQ(controller_.pools_[0].event_pool_.size(), 0u);
}
