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
#include <cstdint>

#include "mf_env_util.h"

using namespace ock::mf;

class MfEnvUtilTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================
// MfEnvUtil::GetUint
// ============================================================
TEST_F(MfEnvUtilTest, GetUint_Valid)
{
    uint64_t val = 0;
    EXPECT_TRUE(MfEnvUtil::GetUint("1024", val));
    EXPECT_EQ(val, 1024U);
}

TEST_F(MfEnvUtilTest, GetUint_Empty)
{
    uint64_t val = 0;
    EXPECT_FALSE(MfEnvUtil::GetUint("", val));
}

TEST_F(MfEnvUtilTest, GetUint_InvalidStr)
{
    uint64_t val = 0;
    EXPECT_FALSE(MfEnvUtil::GetUint("abc", val));
}

TEST_F(MfEnvUtilTest, GetUint_Zero)
{
    uint64_t val = 100; // non-zero initial
    // GetUint rejects 0 because parsedValue == 0 check
    EXPECT_FALSE(MfEnvUtil::GetUint("0", val));
    EXPECT_EQ(val, 100U); // unchanged
}

// ============================================================
// MfEnvUtil::GetOptionalUint
// ============================================================
TEST_F(MfEnvUtilTest, GetOptionalUint_Valid)
{
    uint64_t val = 0;
    EXPECT_TRUE(MfEnvUtil::GetOptionalUint("2048", val));
    EXPECT_EQ(val, 2048U);
}

TEST_F(MfEnvUtilTest, GetOptionalUint_Empty)
{
    uint64_t val = 0;
    EXPECT_FALSE(MfEnvUtil::GetOptionalUint("", val));
}

TEST_F(MfEnvUtilTest, GetOptionalUint_Invalid)
{
    uint64_t val = 0;
    EXPECT_FALSE(MfEnvUtil::GetOptionalUint("abc", val));
}

TEST_F(MfEnvUtilTest, GetOptionalUint_Zero)
{
    uint64_t val = 0;
    // GetOptionalUint allows 0 (no parsedValue == 0 check)
    EXPECT_TRUE(MfEnvUtil::GetOptionalUint("0", val));
    EXPECT_EQ(val, 0U);
}

// ============================================================
// MfEnvUtil::GetUintOrDefault
// ============================================================
TEST_F(MfEnvUtilTest, GetUintOrDefault_Valid)
{
    uint64_t val = MfEnvUtil::GetUintOrDefault<uint64_t>("4096", 999);
    EXPECT_EQ(val, 4096U);
}

TEST_F(MfEnvUtilTest, GetUintOrDefault_Empty_ReturnsDefault)
{
    uint64_t val = MfEnvUtil::GetUintOrDefault<uint64_t>("", 999);
    EXPECT_EQ(val, 999U);
}

TEST_F(MfEnvUtilTest, GetUintOrDefault_Invalid_ReturnsDefault)
{
    uint64_t val = MfEnvUtil::GetUintOrDefault<uint64_t>("abc", 999);
    EXPECT_EQ(val, 999U);
}

TEST_F(MfEnvUtilTest, GetUintOrDefault_Zero_ReturnsDefault)
{
    uint64_t val = MfEnvUtil::GetUintOrDefault<uint64_t>("0", 999);
    EXPECT_EQ(val, 999U);
}

// ============================================================
// MfEnvUtil::GetOptionalUintOrDefault
// ============================================================
TEST_F(MfEnvUtilTest, GetOptionalUintOrDefault_Valid)
{
    uint64_t val = MfEnvUtil::GetOptionalUintOrDefault<uint64_t>("8192", 999);
    EXPECT_EQ(val, 8192U);
}

TEST_F(MfEnvUtilTest, GetOptionalUintOrDefault_Empty_ReturnsDefault)
{
    uint64_t val = MfEnvUtil::GetOptionalUintOrDefault<uint64_t>("", 999);
    EXPECT_EQ(val, 999U);
}

TEST_F(MfEnvUtilTest, GetOptionalUintOrDefault_Invalid_ReturnsDefault)
{
    uint64_t val = MfEnvUtil::GetOptionalUintOrDefault<uint64_t>("xyz", 999);
    EXPECT_EQ(val, 999U);
}

TEST_F(MfEnvUtilTest, GetOptionalUintOrDefault_Zero_Allowed)
{
    uint64_t val = MfEnvUtil::GetOptionalUintOrDefault<uint64_t>("0", 999);
    EXPECT_EQ(val, 0U);
}
