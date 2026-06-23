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

#include "mf_num_util.h"
#include "hybm_dev_legacy_segment.h"
#include "hybm_def.h"

using namespace ock::mf;

class MFNumUtilTest : public testing::Test {
public:
    static void SetUpTestCase() {}

    static void TearDownTestCase() {}

    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(MFNumUtilTest, IsDigit_1)
{
    std::string str1 = "aaaa";
    EXPECT_FALSE(ock::mf::NumUtil::IsDigit(str1));

    std::string str2 = "";
    EXPECT_FALSE(ock::mf::NumUtil::IsDigit(str2));

    std::string str3 = "   ";
    EXPECT_FALSE(ock::mf::NumUtil::IsDigit(str3));

    std::string str4 = "\t  ";
    EXPECT_FALSE(ock::mf::NumUtil::IsDigit(str4));

    std::string str5 = "  123 ";
    EXPECT_FALSE(ock::mf::NumUtil::IsDigit(str5));

    std::string str6 = "  123";
    EXPECT_TRUE(ock::mf::NumUtil::IsDigit(str6));

    std::string str7 = "1234";
    EXPECT_TRUE(ock::mf::NumUtil::IsDigit(str7));
}

TEST_F(MFNumUtilTest, ExtractBits)
{
    uint32_t flags = 0b101010101010101010101010101010;

    EXPECT_EQ(ock::mf::NumUtil::ExtractBits(flags, UINT32_WIDTH, 1), UINT32_MAX);
    EXPECT_EQ(ock::mf::NumUtil::ExtractBits(flags, 0, UINT32_WIDTH), UINT32_MAX);
    EXPECT_EQ(ock::mf::NumUtil::ExtractBits(flags, 0, 0), UINT32_MAX);
    EXPECT_EQ(ock::mf::NumUtil::ExtractBits(flags, 1, UINT32_WIDTH), UINT32_MAX);

    EXPECT_EQ(ock::mf::NumUtil::ExtractBits(flags, HYBM_PERFORMANCE_MODE_FLAG_INDEX, HYBM_PERFORMANCE_MODE_FLAG_LEN),
              1);
    EXPECT_EQ(ock::mf::NumUtil::ExtractBits(flags, HYBM_BIND_NUMA_FLAG_INDEX, HYBM_BIND_NUMA_FLAG_LEN), 0b0101010);
}

TEST_F(MFNumUtilTest, GetReserveChunkSizetTest)
{
    auto ret = HybmDevLegacySegment::GetReserveChunkSize(127ULL * GB, 1ULL * GB);
    EXPECT_EQ(ret, 127ULL * GB);
    ret = HybmDevLegacySegment::GetReserveChunkSize(128ULL * GB, 1ULL * GB);
    EXPECT_EQ(ret, 128ULL * GB);
    ret = HybmDevLegacySegment::GetReserveChunkSize(256ULL * GB, 4ULL * GB);
    EXPECT_EQ(ret, 128ULL * GB);
    ret = HybmDevLegacySegment::GetReserveChunkSize(200ULL * GB, 4ULL * GB);
    EXPECT_EQ(ret, 100ULL * GB);
    ret = HybmDevLegacySegment::GetReserveChunkSize(0 * GB, 4ULL * GB);
    EXPECT_EQ(ret, 0 * GB);
        ret = HybmDevLegacySegment::GetReserveChunkSize(200ULL * GB, 2ULL * GB);
    EXPECT_EQ(ret, 100ULL * GB);
}

TEST_F(MFNumUtilTest, IsOverflowCheck_AddNoOverflow)
{
    uint64_t max = UINT64_MAX;
    EXPECT_FALSE(NumUtil::IsOverflowCheck(uint64_t(100), uint64_t(200), max, '+')); // 100 200
    EXPECT_FALSE(NumUtil::IsOverflowCheck(uint64_t(0), max, max, '+'));
}

TEST_F(MFNumUtilTest, IsOverflowCheck_AddOverflow)
{
    uint64_t max = UINT64_MAX;
    EXPECT_TRUE(NumUtil::IsOverflowCheck(max, uint64_t(1), max, '+'));
    EXPECT_TRUE(NumUtil::IsOverflowCheck(max, max, max, '+'));
}

TEST_F(MFNumUtilTest, IsOverflowCheck_MulNoOverflow)
{
    uint64_t max = UINT64_MAX;
    EXPECT_FALSE(NumUtil::IsOverflowCheck(uint64_t(2), uint64_t(3), max, '*')); // 2 3
    EXPECT_FALSE(NumUtil::IsOverflowCheck(uint64_t(0), max, max, '*'));
}

TEST_F(MFNumUtilTest, IsOverflowCheck_MulOverflow)
{
    uint64_t max = UINT64_MAX;
    EXPECT_TRUE(NumUtil::IsOverflowCheck(max, uint64_t(2), max, '*')); // 2
}

TEST_F(MFNumUtilTest, IsOverflowCheck_InvalidOp)
{
    uint64_t max = UINT64_MAX;
    EXPECT_TRUE(NumUtil::IsOverflowCheck(uint64_t(1), uint64_t(1), max, '-'));
}

TEST_F(MFNumUtilTest, IsOverflowCheck_SignedType)
{
    EXPECT_FALSE(NumUtil::IsOverflowCheck(int32_t(100), int32_t(200), int32_t(INT32_MAX), '+')); // 100 200
}

