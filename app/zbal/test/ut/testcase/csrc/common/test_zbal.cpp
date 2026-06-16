/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of MulanPSL2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the MulanPSL v2 for more details.
 */
#include <gtest/gtest.h>

#include <string>
#include <atomic>

#include "zbal_test_constants.h"
#include "test_zbal_def.h"
#include "zbal_common_includes.h"
#include "zbal_bootstrap_default.h"

using namespace zbal;
using namespace zbal::bootstrap;

namespace {
std::atomic<int> g_logCallCount{0};
std::string g_lastLogMsg;
int g_lastLogLevel = -1;

void test_logger_func(int level, const char *msg)
{
    g_logCallCount++;
    g_lastLogLevel = level;
    if (msg != nullptr) {
        g_lastLogMsg = msg;
    }
}

void reset_logger_state()
{
    g_logCallCount = 0;
    g_lastLogMsg.clear();
    g_lastLogLevel = -1;
}
} // namespace

class TestZBALApi : public testing::Test {
public:
    void SetUp() override
    {
        ZBLastError::GetAndClear(true);
        reset_logger_state();
    }

    void TearDown() override
    {
        Bootstrap::Destroy();
        ZBLastError::GetAndClear(true);
    }
};

TEST_F(TestZBALApi, VersionReturnsValidString)
{
    const char *version = zbal_version();
    EXPECT_TRUE(version != nullptr);
    EXPECT_TRUE(strlen(version) > 0);
}

TEST_F(TestZBALApi, SetLoggerWithValidFunction)
{
    EXPECT_TRUE(zbal_set_logger(test_logger_func) == Z_OK);
    EXPECT_TRUE(g_logCallCount == 0);

    OutLogger::Instance().Log(INFO_LEVEL, "test message");
    EXPECT_TRUE(g_logCallCount == 1);
    EXPECT_TRUE(g_lastLogLevel == INFO_LEVEL);
    EXPECT_TRUE(g_lastLogMsg.find("test message") != std::string::npos);
}

TEST_F(TestZBALApi, SetLoggerWithNullFunction)
{
    EXPECT_TRUE(zbal_set_logger(nullptr) == Z_INVALID_PARAM);
}

TEST_F(TestZBALApi, SetLoggerLevelWithValidLevels)
{
    EXPECT_TRUE(zbal_set_logger_level(DEBUG_LEVEL) == Z_OK);
    EXPECT_TRUE(OutLogger::Instance().GetLogLevel() == DEBUG_LEVEL);

    EXPECT_TRUE(zbal_set_logger_level(INFO_LEVEL) == Z_OK);
    EXPECT_TRUE(OutLogger::Instance().GetLogLevel() == INFO_LEVEL);

    EXPECT_TRUE(zbal_set_logger_level(WARN_LEVEL) == Z_OK);
    EXPECT_TRUE(OutLogger::Instance().GetLogLevel() == WARN_LEVEL);

    EXPECT_TRUE(zbal_set_logger_level(ERROR_LEVEL) == Z_OK);
    EXPECT_TRUE(OutLogger::Instance().GetLogLevel() == ERROR_LEVEL);
}

TEST_F(TestZBALApi, SetLoggerLevelWithInvalidLevel)
{
    EXPECT_TRUE(zbal_set_logger_level(-1) == Z_INVALID_PARAM);
    EXPECT_TRUE(zbal_set_logger_level(ZBAL_UT_NUM_100) == Z_INVALID_PARAM);
}

TEST_F(TestZBALApi, GetLastErrorMsgInitiallyEmpty)
{
    const char *msg = zbal_get_last_error_msg();
    EXPECT_TRUE(msg == nullptr || strlen(msg) == 0);
}

TEST_F(TestZBALApi, GetLastErrorMsgDoesNotClear)
{
    ZBAL_LOG_AND_SET_LAST_ERROR("test error message");

    const char *msg1 = zbal_get_last_error_msg();
    EXPECT_TRUE(msg1 != nullptr);
    EXPECT_TRUE(std::string(msg1).find("test error message") != std::string::npos);

    const char *msg2 = zbal_get_last_error_msg();
    EXPECT_TRUE(msg2 != nullptr);
    EXPECT_TRUE(std::string(msg2).find("test error message") != std::string::npos);
}

TEST_F(TestZBALApi, GetAndClearLastErrorMsgClearsAfterRead)
{
    ZBAL_LOG_AND_SET_LAST_ERROR("test clear message");

    const char *msg1 = zbal_get_and_clear_last_error_msg();
    EXPECT_TRUE(msg1 != nullptr);
    EXPECT_TRUE(std::string(msg1).find("test clear message") != std::string::npos);

    const char *msg2 = zbal_get_last_error_msg();
    EXPECT_TRUE(msg2 == nullptr || strlen(msg2) == 0);
}

TEST_F(TestZBALApi, SetLoggerLevelWithBootstrap)
{
    zbal_bootstrap_options_t options;
    bzero(&options, sizeof(zbal_bootstrap_options_t));
    options.btType = BOOT_BY_MEMFABRIC;
    options.worldSize = 1;
    options.rankId = 0;
    options.deviceId = 0;
    options.deviceMemorySize = ZBAL_UT_SIZE_1KB * ZBAL_UT_SIZE_1KB;
    options.commGroupCap = ZBAL_UT_NUM_16;
    options.commMetaSpaceSize = ZBAL_UT_SIZE_1KB;

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    EXPECT_TRUE(zbal_set_logger_level(DEBUG_LEVEL) == Z_OK);
    EXPECT_TRUE(OutLogger::Instance().GetLogLevel() == DEBUG_LEVEL);
}