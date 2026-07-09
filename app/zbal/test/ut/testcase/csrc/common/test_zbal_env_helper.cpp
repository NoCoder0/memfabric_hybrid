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
#include <cstdlib>
#include <string>

#include "zbal_env_helper.h"

using namespace zbal;

class TestZBALEnvHelper : public testing::Test {
public:
    void SetUp() override
    {
        unsetenv(ENV_NAME_PROF_SWITCH);
        unsetenv(ENV_NAME_PROF_TRACING_COUNT);
        unsetenv(ENV_NAME_PROF_DIR);
        unsetenv(ENV_NAME_OP_DEFUALT_STREAM);
    }

    void TearDown() override
    {
        unsetenv(ENV_NAME_PROF_SWITCH);
        unsetenv(ENV_NAME_PROF_TRACING_COUNT);
        unsetenv(ENV_NAME_PROF_DIR);
        unsetenv(ENV_NAME_OP_DEFUALT_STREAM);
    }
};

TEST_F(TestZBALEnvHelper, DefaultInitialValues)
{
    EXPECT_FALSE(EnvHelper::PROF_ENABLED);
    EXPECT_FALSE(EnvHelper::OP_DEFAULT_STREAM);
    EXPECT_EQ(EnvHelper::PROF_TRACING_MAX_COUNT, 0u);
    EXPECT_EQ(EnvHelper::PROF_DIR, "/home/");
}

TEST_F(TestZBALEnvHelper, InitializeWithDefaultEnv)
{
    EnvHelper::Initialize();

    EXPECT_FALSE(EnvHelper::PROF_ENABLED);
    EXPECT_FALSE(EnvHelper::OP_DEFAULT_STREAM);
    EXPECT_EQ(EnvHelper::PROF_TRACING_MAX_COUNT, 0u);
}

TEST_F(TestZBALEnvHelper, InitializeWithProfEnabled)
{
    setenv(ENV_NAME_PROF_SWITCH, "1", 1);

    EnvHelper::Initialize();

    EXPECT_TRUE(EnvHelper::PROF_ENABLED);
    EXPECT_FALSE(EnvHelper::OP_DEFAULT_STREAM);
    EXPECT_EQ(EnvHelper::PROF_TRACING_MAX_COUNT, 0u);
}

TEST_F(TestZBALEnvHelper, InitializeWithProfDisabled)
{
    setenv(ENV_NAME_PROF_SWITCH, "0", 1);

    EnvHelper::Initialize();

    EXPECT_FALSE(EnvHelper::PROF_ENABLED);
}

TEST_F(TestZBALEnvHelper, InitializeWithProfTracingCount)
{
    setenv(ENV_NAME_PROF_TRACING_COUNT, "100", 1);

    EnvHelper::Initialize();

    EXPECT_EQ(EnvHelper::PROF_TRACING_MAX_COUNT, 100u);
}

TEST_F(TestZBALEnvHelper, InitializeWithProfTracingCountZero)
{
    setenv(ENV_NAME_PROF_TRACING_COUNT, "0", 1);

    EnvHelper::Initialize();

    EXPECT_EQ(EnvHelper::PROF_TRACING_MAX_COUNT, 0u);
}

TEST_F(TestZBALEnvHelper, InitializeWithProfDir)
{
    setenv(ENV_NAME_PROF_DIR, "/tmp/test_prof", 1);

    EnvHelper::Initialize();

    EXPECT_EQ(EnvHelper::PROF_DIR, "/tmp/test_prof");
}

TEST_F(TestZBALEnvHelper, InitializeWithOpDefaultStreamEnabled)
{
    setenv(ENV_NAME_OP_DEFUALT_STREAM, "1", 1);

    EnvHelper::Initialize();

    EXPECT_TRUE(EnvHelper::OP_DEFAULT_STREAM);
}

TEST_F(TestZBALEnvHelper, InitializeWithOpDefaultStreamDisabled)
{
    setenv(ENV_NAME_OP_DEFUALT_STREAM, "0", 1);

    EnvHelper::Initialize();

    EXPECT_FALSE(EnvHelper::OP_DEFAULT_STREAM);
}

TEST_F(TestZBALEnvHelper, InitializeWithInvalidProfTracingCount)
{
    setenv(ENV_NAME_PROF_TRACING_COUNT, "invalid", 1);

    EnvHelper::Initialize();

    EXPECT_EQ(EnvHelper::PROF_TRACING_MAX_COUNT, 0u);
}

TEST_F(TestZBALEnvHelper, InitializeWithAllEnvSet)
{
    setenv(ENV_NAME_PROF_SWITCH, "1", 1);
    setenv(ENV_NAME_PROF_TRACING_COUNT, "500", 1);
    setenv(ENV_NAME_PROF_DIR, "/opt/zbal/prof", 1);
    setenv(ENV_NAME_OP_DEFUALT_STREAM, "1", 1);

    EnvHelper::Initialize();

    EXPECT_TRUE(EnvHelper::PROF_ENABLED);
    EXPECT_TRUE(EnvHelper::OP_DEFAULT_STREAM);
    EXPECT_EQ(EnvHelper::PROF_TRACING_MAX_COUNT, 500u);
    EXPECT_EQ(EnvHelper::PROF_DIR, "/opt/zbal/prof");
}

TEST_F(TestZBALEnvHelper, DumpEnvDoesNotCrash)
{
    EnvHelper::Initialize();

    EXPECT_NO_THROW(EnvHelper::DumpEnv());
}

TEST_F(TestZBALEnvHelper, InitializeMultipleTimes)
{
    setenv(ENV_NAME_PROF_SWITCH, "1", 1);
    EnvHelper::Initialize();
    EXPECT_TRUE(EnvHelper::PROF_ENABLED);

    unsetenv(ENV_NAME_PROF_SWITCH);
    EnvHelper::Initialize();
    EXPECT_FALSE(EnvHelper::PROF_ENABLED);
}
