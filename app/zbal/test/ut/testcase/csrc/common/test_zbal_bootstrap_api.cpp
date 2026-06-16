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

#include <cstring>

#include "zbal_test_constants.h"
#include "zbal_common_includes.h"
#include "zbal_init_state.h"

using namespace zbal;

class TestZBALBootstrapApi : public testing::Test {
public:
    void SetUp() override
    {
        ZBALInitState::Instance().Reset();
        ZBLastError::GetAndClear(true);
    }

    void TearDown() override
    {
        ZBALInitState::Instance().Reset();
        ZBLastError::GetAndClear(true);
    }
};

TEST_F(TestZBALBootstrapApi, OptionsInitNullPointer)
{
    EXPECT_EQ(zbal_bootstrap_options_init(nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALBootstrapApi, OptionsInitDefaults)
{
    zbal_bootstrap_options_t options;
    std::memset(&options, 0xFF, sizeof(options));

    EXPECT_EQ(zbal_bootstrap_options_init(&options), Z_OK);

    EXPECT_EQ(options.btType, BOOT_BY_MEMFABRIC);
    EXPECT_EQ(options.startConfigServer, 0u);
    EXPECT_EQ(options.commMetaSpaceSize, static_cast<uint16_t>(COMM_META_SPACE_SIZE_DEFAULT));
    EXPECT_EQ(options.commGroupCap, static_cast<uint16_t>(COMM_GROUP_COUNT_CAP_DEFAULT));
    EXPECT_EQ(options.worldSize, 0u);
    EXPECT_EQ(options.rankId, 0u);
    EXPECT_EQ(options.deviceId, 0u);
}

TEST_F(TestZBALBootstrapApi, OptionsInitTwiceIsIdempotent)
{
    zbal_bootstrap_options_t options;
    std::memset(&options, 0xFF, sizeof(options));

    EXPECT_EQ(zbal_bootstrap_options_init(&options), Z_OK);
    EXPECT_EQ(zbal_bootstrap_options_init(&options), Z_OK);

    EXPECT_EQ(options.btType, BOOT_BY_MEMFABRIC);
}

TEST_F(TestZBALBootstrapApi, BootstrapNullOptions)
{
    zbal_bootstrap_output_t out;
    EXPECT_EQ(zbal_bootstrap(nullptr, &out), Z_INVALID_PARAM);
}

TEST_F(TestZBALBootstrapApi, BootstrapNullOutput)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    EXPECT_EQ(zbal_bootstrap(&options, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALBootstrapApi, UnbootstrapSucceedsWhenNothingInitialized)
{
    EXPECT_EQ(zbal_unbootstrap(0), Z_OK);
}

TEST_F(TestZBALBootstrapApi, UnbootstrapWithFlags)
{
    EXPECT_EQ(zbal_unbootstrap(0xFFFFFFFF), Z_OK);
}

TEST_F(TestZBALBootstrapApi, UnbootstrapWithActiveCommunicator)
{
    ZBALInitState::Instance().CommunicatorCreated(1);
    EXPECT_EQ(zbal_unbootstrap(0), Z_CANNOT_UNBOOTSTRAP);
}

TEST_F(TestZBALBootstrapApi, UnbootstrapWithSmaInitialized)
{
    ZBALInitState::Instance().SmaInitialized(true);
    EXPECT_EQ(zbal_unbootstrap(0), Z_CANNOT_UNBOOTSTRAP);
}

TEST_F(TestZBALBootstrapApi, BootstrapCreateFailsWithoutMemfabric)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    zbal_bootstrap_output_t output;
    EXPECT_EQ(zbal_bootstrap(&options, &output), Z_ERROR);
}

TEST_F(TestZBALBootstrapApi, UnbootstrapWithBothBlocks)
{
    ZBALInitState::Instance().CommunicatorCreated(1);
    ZBALInitState::Instance().SmaInitialized(true);
    EXPECT_EQ(zbal_unbootstrap(0), Z_CANNOT_UNBOOTSTRAP);
}

TEST_F(TestZBALBootstrapApi, BootstrapWithZeroWorldSize)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    options.worldSize = 0;

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);
    EXPECT_NE(result, Z_OK);
}

TEST_F(TestZBALBootstrapApi, BootstrapWithNonDefaultBtType)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    options.btType = BOOT_BY_MEMFABRIC;

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);
    EXPECT_NE(result, Z_OK);
}

TEST_F(TestZBALBootstrapApi, BootstrapWithCustomMetaSpaceSize)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    options.commMetaSpaceSize = ZBAL_UT_SIZE_2KB;

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);
    EXPECT_NE(result, Z_OK);
}

TEST_F(TestZBALBootstrapApi, BootstrapWithCustomCommGroupCap)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    options.commGroupCap = ZBAL_UT_NUM_64;

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);
    EXPECT_NE(result, Z_OK);
}

TEST_F(TestZBALBootstrapApi, BootstrapFailureDoesNotInitializeState)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);

    EXPECT_NE(result, Z_OK);
    EXPECT_FALSE(ZBALInitState::Instance().Bootstrapped());
    EXPECT_FALSE(ZBALInitState::Instance().SmaInitialized());
}

TEST_F(TestZBALBootstrapApi, MultipleUnbootstrapCalls)
{
    EXPECT_EQ(zbal_unbootstrap(0), Z_OK);
    EXPECT_EQ(zbal_unbootstrap(0), Z_OK);
    EXPECT_EQ(zbal_unbootstrap(0), Z_OK);
}

TEST_F(TestZBALBootstrapApi, BootstrapTriggersLogMessages)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);
    (void)result;
}

TEST_F(TestZBALBootstrapApi, UnbootstrapResetsState)
{
    auto &state = ZBALInitState::Instance();
    state.Bootstrapped(true);
    state.ext_.worldSize = ZBAL_UT_NUM_8;
    state.ext_.worldRankId = ZBAL_UT_NUM_4;
    state.ext_.deviceId = 0;

    state.Reset();

    EXPECT_FALSE(state.Bootstrapped());
    EXPECT_EQ(state.ext_.worldSize, 0u);
}

TEST_F(TestZBALBootstrapApi, BootstrapRepeatedCallsAllFail)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);

    for (int i = 0; i < ZBAL_UT_NUM_3; i++) {
        zbal_bootstrap_output_t output;
        EXPECT_EQ(zbal_bootstrap(&options, &output), Z_ERROR);
    }
}

TEST_F(TestZBALBootstrapApi, BootstrapWithTinyMetaSpace)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    options.commMetaSpaceSize = ZBAL_UT_NUM_1;

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);
    EXPECT_NE(result, Z_OK);
}

TEST_F(TestZBALBootstrapApi, BootstrapWithZeroCommGroupCap)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    options.commGroupCap = 0;

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);
    EXPECT_NE(result, Z_OK);
}

TEST_F(TestZBALBootstrapApi, BootstrapWithMaximumValues)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);
    options.worldSize = ZBAL_RANK_COUNT_MAX_LIMIT;
    options.deviceId = ZBAL_DEVICE_COUNT_MAX_LIMIT - 1;
    options.deviceMemorySize = ZBAL_MEMORY_SIZE_CAP - 1;
    options.commGroupCap = COMM_GROUP_COUNT_CAP_MAX - 1;

    zbal_bootstrap_output_t output;
    int32_t result = zbal_bootstrap(&options, &output);
    EXPECT_EQ(result, Z_ERROR);
}

TEST_F(TestZBALBootstrapApi, BootstrapFailureOutputStaysZeroed)
{
    zbal_bootstrap_options_t options;
    zbal_bootstrap_options_init(&options);

    zbal_bootstrap_output_t output;
    std::memset(&output, 0, sizeof(output));

    int32_t result = zbal_bootstrap(&options, &output);
    EXPECT_NE(result, Z_OK);

    EXPECT_EQ(output.deviceGva, nullptr);
    EXPECT_EQ(output.allocatedDeviceMemorySize, 0u);
}

TEST_F(TestZBALBootstrapApi, UnbootstrapResetsBootstrappedFlag)
{
    auto &state = ZBALInitState::Instance();
    state.Bootstrapped(true);
    EXPECT_TRUE(state.Bootstrapped());

    EXPECT_EQ(zbal_unbootstrap(0), Z_OK);
    EXPECT_FALSE(state.Bootstrapped());
}

TEST_F(TestZBALBootstrapApi, UnbootstrapResetsSmaFlag)
{
    auto &state = ZBALInitState::Instance();
    state.SmaInitialized(true);
    EXPECT_TRUE(state.SmaInitialized());

    state.SmaInitialized(false);
    EXPECT_EQ(zbal_unbootstrap(0), Z_OK);
    EXPECT_FALSE(state.SmaInitialized());
}

TEST_F(TestZBALBootstrapApi, UnbootstrapAfterPartialSetup)
{
    auto &state = ZBALInitState::Instance();

    state.Bootstrapped(true);
    state.CommunicatorCreated(ZBAL_UT_NUM_2);
    state.SmaInitialized(true);

    EXPECT_EQ(zbal_unbootstrap(0), Z_CANNOT_UNBOOTSTRAP);

    state.CommunicatorDestroy(ZBAL_UT_NUM_2);
    EXPECT_TRUE(state.HasCommunicator() == false);

    EXPECT_EQ(zbal_unbootstrap(0), Z_CANNOT_UNBOOTSTRAP);

    state.SmaInitialized(false);
    EXPECT_FALSE(state.SmaInitialized());

    EXPECT_EQ(zbal_unbootstrap(0), Z_OK);
}
