/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <gtest/gtest.h>
#include <string>

#define private public
#include "smem.h"
#include "smem_common_includes.h"
#undef private

using namespace ock::smem;

class SmemTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        (void)smem_get_and_clear_last_err_msg();
        SmLastError::GetAndClearCode(true);
    }

    void TearDown() override
    {
    }
};

TEST_F(SmemTest, GetLastErrMsg_InitiallyEmpty)
{
    const char *msg = smem_get_last_err_msg();
    EXPECT_NE(msg, nullptr);
}

TEST_F(SmemTest, GetLastErrCode_InitiallyZero)
{
    int32_t code = smem_get_last_err_code();
    EXPECT_EQ(code, 0); // 0
}

TEST_F(SmemTest, GetAndClearLastErrMsg_InitiallyEmpty)
{
    const char *msg = smem_get_and_clear_last_err_msg();
    EXPECT_NE(msg, nullptr);
}

TEST_F(SmemTest, SetLogLevel_Invalid_ReturnsError)
{
    int32_t ret = smem_set_log_level(99); // 99
    EXPECT_NE(ret, 0); // 0
}

TEST_F(SmemTest, SetLogLevel_Valid_ReturnsOk)
{
    int32_t ret = smem_set_log_level(0); // 0
    EXPECT_EQ(ret, 0); // 0
}

TEST_F(SmemTest, SetConfStoreTls_ReturnsOk)
{
    int32_t ret = smem_set_conf_store_tls(true, nullptr, 0); // 0
    EXPECT_EQ(ret, 0); // 0
}

TEST_F(SmemTest, SetExternLogger_NullFunc_ReturnsError)
{
    int32_t ret = smem_set_extern_logger(nullptr);
    EXPECT_NE(ret, 0); // 0
}

TEST_F(SmemTest, SetConfigStoreTlsKey_ReturnsOk)
{
    int32_t ret = smem_set_config_store_tls_key(nullptr, 0, nullptr, 0, nullptr); // 0, 0
    EXPECT_EQ(ret, 0); // 0
}

TEST_F(SmemTest, IsValidBackendOp_DefaultOp_ReturnsFalse)
{
    smem_conf_store_backend_op_t op{};
    // Zero-initialized op should be invalid (all function pointers are null)
    int32_t ret = smem_config_store_set_backend_op(&op);
    EXPECT_NE(ret, 0);
}

TEST_F(SmemTest, CreateConfigStore_NullUrl_ReturnsError)
{
    int32_t ret = smem_create_config_store(nullptr, 0);
    EXPECT_EQ(ret, ock::smem::SM_INVALID_PARAM);
}

TEST_F(SmemTest, DestroyConfigStore_NullUrl_NoCrash)
{
    smem_destroy_config_store(nullptr);
}

TEST_F(SmemTest, SetExternAlarm_NullAlarm_ReturnsError)
{
    int32_t ret = smem_set_extern_alarm(nullptr, nullptr);
    EXPECT_NE(ret, 0);
}

TEST_F(SmemTest, SetExternAlarm_Valid_ReturnsOk)
{
    auto testAlarm = [](uint16_t, const char *) {};
    auto testResume = [](uint16_t) {};
    int32_t ret = smem_set_extern_alarm(testAlarm, testResume);
    EXPECT_EQ(ret, 0);
}

TEST_F(SmemTest, ConfigStoreSetBackendOp_NullPtr_ReturnsInvalidParam)
{
    int32_t ret = smem_config_store_set_backend_op(nullptr);
    EXPECT_EQ(ret, ock::smem::SM_INVALID_PARAM);
}