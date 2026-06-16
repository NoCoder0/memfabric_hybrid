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
#include <thread>
#include <vector>

#include "zbal_test_constants.h"
#include "zbal_init_state.h"

using namespace zbal;

class TestZBALInitState : public testing::Test {
public:
    void SetUp() override
    {
        ZBALInitState::Instance().Reset();
    }

    void TearDown() override
    {
        ZBALInitState::Instance().Reset();
    }
};

/*
 * Reset truly zeroes all state including ext_ fields.
 */
TEST_F(TestZBALInitState, ResetClearsAllState)
{
    auto &state = ZBALInitState::Instance();
    constexpr uint16_t worldSize = ZBAL_UT_NUM_8;
    constexpr uint16_t worldRankId = ZBAL_UT_NUM_3;
    constexpr uint16_t deviceId = ZBAL_UT_NUM_2;
    state.Bootstrapped(true);
    state.SmaInitialized(true);
    state.CommunicatorCreated(ZBAL_UT_NUM_5);
    state.ext_.worldSize = worldSize;
    state.ext_.worldRankId = worldRankId;
    state.ext_.deviceId = deviceId;
    state.ext_.commMetaSpaceSize = ZBAL_UT_NUM_512;
    state.ext_.commGroupCap = ZBAL_UT_NUM_128;
    state.ext_.localDeviceMemSize = ZBAL_UT_SIZE_1KB * ZBAL_UT_SIZE_1KB;

    state.Reset();

    EXPECT_FALSE(state.Bootstrapped());
    EXPECT_FALSE(state.SmaInitialized());
    EXPECT_FALSE(state.HasCommunicator());
    EXPECT_EQ(state.ext_.worldSize, 0);
    EXPECT_EQ(state.ext_.worldRankId, 0);
    EXPECT_EQ(state.ext_.deviceId, 0);
}

/*
 * Full lifecycle: bootstrap -> create comms -> destroy comms -> reset.
 */
TEST_F(TestZBALInitState, FullLifecycle)
{
    auto &state = ZBALInitState::Instance();

    state.Bootstrapped(true);
    EXPECT_TRUE(state.Bootstrapped());

    state.SmaInitialized(true);
    EXPECT_TRUE(state.SmaInitialized());

    state.CommunicatorCreated();
    state.CommunicatorCreated(ZBAL_UT_NUM_3);
    EXPECT_TRUE(state.HasCommunicator());

    state.CommunicatorDestroy();
    state.CommunicatorDestroy(ZBAL_UT_NUM_2);
    state.CommunicatorDestroy();
    EXPECT_FALSE(state.HasCommunicator());

    EXPECT_TRUE(state.Bootstrapped());
    EXPECT_TRUE(state.SmaInitialized());

    state.Reset();
    EXPECT_FALSE(state.Bootstrapped());
}

/*
 * CommunicatorDestroy when count is already 0: goes negative,
 * HasCommunicator returns false (> 0 check).
 */
TEST_F(TestZBALInitState, DestroyBelowZero)
{
    auto &state = ZBALInitState::Instance();

    state.CommunicatorDestroy();
    state.CommunicatorDestroy();
    EXPECT_FALSE(state.HasCommunicator());
}

/*
 * Concurrent CommunicatorCreated/CommunicatorDestroy from multiple threads
 * verifies atomic counter integrity.
 */
TEST_F(TestZBALInitState, ConcurrentCommunicatorOps)
{
    auto &state = ZBALInitState::Instance();
    constexpr int kNumThreads = ZBAL_UT_THREAD_COUNT;
    constexpr int kOpsPerThread = ZBAL_UT_OPS_PER_THREAD_1K;

    std::vector<std::thread> threads;
    for (int i = 0; i < kNumThreads; i++) {
        threads.emplace_back([&state]() {
            for (int j = 0; j < kOpsPerThread; j++) {
                state.CommunicatorCreated();
            }
            for (int j = 0; j < kOpsPerThread; j++) {
                state.CommunicatorDestroy();
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    EXPECT_FALSE(state.HasCommunicator());
}

/*
 * Bootstrap flag is independent of communicator state.
 */
TEST_F(TestZBALInitState, BootstrapIndependentOfCommunicator)
{
    auto &state = ZBALInitState::Instance();

    state.Bootstrapped(true);
    EXPECT_FALSE(state.HasCommunicator());

    state.CommunicatorCreated();
    state.Bootstrapped(false);
    EXPECT_TRUE(state.HasCommunicator());
    EXPECT_FALSE(state.Bootstrapped());
}

/*
 * Toggle Bootstrapped flag multiple times to verify setter works correctly.
 */
TEST_F(TestZBALInitState, BootstrappedToggleMultipleTimes)
{
    auto &state = ZBALInitState::Instance();

    EXPECT_FALSE(state.Bootstrapped());
    state.Bootstrapped(true);
    EXPECT_TRUE(state.Bootstrapped());
    state.Bootstrapped(false);
    EXPECT_FALSE(state.Bootstrapped());
    state.Bootstrapped(true);
    EXPECT_TRUE(state.Bootstrapped());
    state.Bootstrapped(false);
    EXPECT_FALSE(state.Bootstrapped());
}

/*
 * Toggle SmaInitialized flag multiple times.
 */
TEST_F(TestZBALInitState, SmaFlagToggleMultipleTimes)
{
    auto &state = ZBALInitState::Instance();

    EXPECT_FALSE(state.SmaInitialized());
    state.SmaInitialized(true);
    EXPECT_TRUE(state.SmaInitialized());
    state.SmaInitialized(false);
    EXPECT_FALSE(state.SmaInitialized());
    state.SmaInitialized(true);
    EXPECT_TRUE(state.SmaInitialized());
}

/*
 * CommunicatorCreated with non-default count > 1.
 */
TEST_F(TestZBALInitState, CommunicatorCreatedWithLargeCount)
{
    auto &state = ZBALInitState::Instance();

    state.CommunicatorCreated(ZBAL_UT_NUM_10);
    EXPECT_TRUE(state.HasCommunicator());

    state.CommunicatorDestroy(ZBAL_UT_NUM_10);
    EXPECT_FALSE(state.HasCommunicator());
}

/*
 * CommunicatorCreated incrementally and HasCommunicator checks at each step.
 */
TEST_F(TestZBALInitState, CommunicatorCreatedIncremental)
{
    auto &state = ZBALInitState::Instance();

    state.CommunicatorCreated(1);
    EXPECT_TRUE(state.HasCommunicator());

    state.CommunicatorCreated(1);
    EXPECT_TRUE(state.HasCommunicator());

    state.CommunicatorCreated(1);
    EXPECT_TRUE(state.HasCommunicator());

    state.CommunicatorDestroy(1);
    EXPECT_TRUE(state.HasCommunicator());

    state.CommunicatorDestroy(1);
    EXPECT_TRUE(state.HasCommunicator());

    state.CommunicatorDestroy(1);
    EXPECT_FALSE(state.HasCommunicator());
}

/*
 * Reset without any prior state set is safe and sets everything to default.
 */
TEST_F(TestZBALInitState, ResetOnCleanState)
{
    auto &state = ZBALInitState::Instance();

    // Already clean from SetUp, reset again
    EXPECT_NO_THROW(state.Reset());

    EXPECT_FALSE(state.Bootstrapped());
    EXPECT_FALSE(state.SmaInitialized());
    EXPECT_FALSE(state.HasCommunicator());
}

/*
 * Verify ext_ gva pointers are zeroed by Reset.
 */
TEST_F(TestZBALInitState, ResetClearsGVAPointers)
{
    auto &state = ZBALInitState::Instance();

    state.ext_.gvaDevice = reinterpret_cast<void *>(0xDEADBEEF);
    state.ext_.myCommMetaDeviceGva = reinterpret_cast<void *>(0xCAFEBABE);
    state.ext_.mySMAGva = reinterpret_cast<void *>(0xFEEDFACE);

    state.Reset();

    EXPECT_EQ(state.ext_.gvaDevice, nullptr);
    EXPECT_EQ(state.ext_.myCommMetaDeviceGva, nullptr);
    EXPECT_EQ(state.ext_.mySMAGva, nullptr);
}

/*
 * Sma flag is independent of Bootstrapped flag.
 */
TEST_F(TestZBALInitState, SmaIndependentOfBootstrap)
{
    auto &state = ZBALInitState::Instance();

    state.SmaInitialized(true);
    EXPECT_TRUE(state.SmaInitialized());
    EXPECT_FALSE(state.Bootstrapped());

    state.Bootstrapped(true);
    state.SmaInitialized(false);
    EXPECT_TRUE(state.Bootstrapped());
    EXPECT_FALSE(state.SmaInitialized());
}

/*
 * Communicator count is independent of both bootstrap and sma flags.
 */
TEST_F(TestZBALInitState, CommunicatorIndependentOfFlags)
{
    auto &state = ZBALInitState::Instance();

    state.CommunicatorCreated(ZBAL_UT_NUM_3);
    EXPECT_TRUE(state.HasCommunicator());
    EXPECT_FALSE(state.Bootstrapped());
    EXPECT_FALSE(state.SmaInitialized());

    state.CommunicatorDestroy(ZBAL_UT_NUM_3);
    EXPECT_FALSE(state.HasCommunicator());
}

/*
 * Verify that Bootstrapped flag setter with the same value works.
 */
TEST_F(TestZBALInitState, BootstrappedSetSameValue)
{
    auto &state = ZBALInitState::Instance();

    state.Bootstrapped(false);
    EXPECT_FALSE(state.Bootstrapped());

    state.Bootstrapped(true);
    state.Bootstrapped(true); // same value again
    EXPECT_TRUE(state.Bootstrapped());
}
