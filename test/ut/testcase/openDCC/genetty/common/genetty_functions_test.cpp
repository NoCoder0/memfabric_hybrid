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

#include "common/genetty_functions.h"

using namespace ock::openDCC;

class GenettyFunctionsTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================
// Func::Trim
// ============================================================
TEST_F(GenettyFunctionsTest, Trim_NoBlank)
{
    EXPECT_EQ(Func::Trim("abc"), "abc");
    EXPECT_EQ(Func::Trim("hello-world"), "hello-world");
    EXPECT_EQ(Func::Trim("a=b"), "a=b");
}

TEST_F(GenettyFunctionsTest, Trim_LeadingBlank)
{
    EXPECT_EQ(Func::Trim("   abc"), "abc");
    EXPECT_EQ(Func::Trim("\tabc"), "abc");
    EXPECT_EQ(Func::Trim("\nabc"), "abc");
    EXPECT_EQ(Func::Trim("\rabc"), "abc");
}

TEST_F(GenettyFunctionsTest, Trim_TrailingBlank)
{
    EXPECT_EQ(Func::Trim("abc   "), "abc");
    EXPECT_EQ(Func::Trim("abc\t"), "abc");
    EXPECT_EQ(Func::Trim("abc\n"), "abc");
    EXPECT_EQ(Func::Trim("abc\r"), "abc");
}

TEST_F(GenettyFunctionsTest, Trim_BothSides)
{
    EXPECT_EQ(Func::Trim("   abc   "), "abc");
    EXPECT_EQ(Func::Trim(" \t\r\n abc \n\r\t "), "abc");
    EXPECT_EQ(Func::Trim(" a "), "a");
}

TEST_F(GenettyFunctionsTest, Trim_OnlyBlank)
{
    EXPECT_EQ(Func::Trim(""), "");
    EXPECT_EQ(Func::Trim(" "), "");
    EXPECT_EQ(Func::Trim("    "), "");
    EXPECT_EQ(Func::Trim(" \t\r\n"), "");
}

TEST_F(GenettyFunctionsTest, Trim_KeepInternalBlank)
{
    EXPECT_EQ(Func::Trim("  hello world  "), "hello world");
    EXPECT_EQ(Func::Trim(" a  b "), "a  b");
    EXPECT_EQ(Func::Trim("\ta\tb\t"), "a\tb");
}

TEST_F(GenettyFunctionsTest, Trim_Idempotent)
{
    const std::string src = "  a b  ";
    auto once = Func::Trim(src);
    auto twice = Func::Trim(once);
    EXPECT_EQ(once, twice);
    EXPECT_EQ(once, "a b");
}

TEST_F(GenettyFunctionsTest, Trim_SourceNotChanged)
{
    const std::string src = "  abc  ";
    auto trimmed = Func::Trim(src);
    EXPECT_EQ(trimmed, "abc");
    /* the input must be kept as is */
    EXPECT_EQ(src, "  abc  ");
}
