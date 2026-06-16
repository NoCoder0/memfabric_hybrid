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
#include <cstring>
#include <limits>

#include "zbal_test_constants.h"

#define private   public
#define protected public
#include "zbal_sma_config.h"
#undef private
#undef protected

using namespace zbal::sma;

class TestSMAConfig : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}

    SMAConfig config_;
};

TEST_F(TestSMAConfig, LexArgsEmptyString)
{
    std::vector<std::string> config;
    config_.lexArgs("", config);
    EXPECT_TRUE(config.empty());
}

TEST_F(TestSMAConfig, LexArgsSimpleKeyValue)
{
    std::vector<std::string> config;
    config_.lexArgs("max_split_size_mb:100", config);
    ASSERT_EQ(config.size(), ZBAL_UT_NUM_3);
    EXPECT_EQ(config[0], "max_split_size_mb");
    EXPECT_EQ(config[ZBAL_UT_NUM_1], ":");
    EXPECT_EQ(config[ZBAL_UT_NUM_2], "100");
}

TEST_F(TestSMAConfig, LexArgsMultipleOptions)
{
    std::vector<std::string> config;
    config_.lexArgs("max_split_size_mb:100,garbage_collection_threshold:0.5", config);
    ASSERT_EQ(config.size(), ZBAL_UT_NUM_7);
    EXPECT_EQ(config[ZBAL_UT_NUM_4], "garbage_collection_threshold");
    EXPECT_EQ(config[ZBAL_UT_NUM_6], "0.5");
}

TEST_F(TestSMAConfig, LexArgsWhitespaceSkipped)
{
    std::vector<std::string> config;
    config_.lexArgs("  max_split_size_mb : 100  ,  use_sma_allocator : True  ", config);
    ASSERT_EQ(config.size(), ZBAL_UT_NUM_7);
    EXPECT_EQ(config[ZBAL_UT_NUM_6], "True");
}

TEST_F(TestSMAConfig, LexArgsBracketedValues)
{
    std::vector<std::string> config;
    config_.lexArgs("small_heap_size:[1024]", config);
    ASSERT_EQ(config.size(), ZBAL_UT_NUM_5);
    EXPECT_EQ(config[ZBAL_UT_NUM_2], "[");
    EXPECT_EQ(config[ZBAL_UT_NUM_3], "1024");
    EXPECT_EQ(config[ZBAL_UT_NUM_4], "]");
}

TEST_F(TestSMAConfig, ConsumeTokenValid)
{
    std::vector<std::string> config = {"key", ":", "val", ",", "next"};
    EXPECT_NO_THROW(config_.consumeToken(config, ZBAL_UT_NUM_1, ':'));
    EXPECT_NO_THROW(config_.consumeToken(config, ZBAL_UT_NUM_3, ','));
}

TEST_F(TestSMAConfig, DefaultMaxSplitSizeIsMax)
{
    config_.parseEnv(nullptr);
    EXPECT_EQ(config_.max_split_size_, std::numeric_limits<size_t>::max());
}

TEST_F(TestSMAConfig, DefaultGCThresholdIsZero)
{
    config_.parseEnv(nullptr);
    EXPECT_EQ(config_.garbage_collection_threshold_, 0.0);
}

TEST_F(TestSMAConfig, DefaultUseSMAAllocatorTrue)
{
    config_.parseEnv(nullptr);
    EXPECT_TRUE(config_.use_sma_allocator_);
}

TEST_F(TestSMAConfig, DefaultUseVMMForStaticMemoryFalse)
{
    config_.parseEnv(nullptr);
    EXPECT_FALSE(config_.use_vmm_for_static_memory_);
}

TEST_F(TestSMAConfig, ParseMaxSplitSizeValid)
{
    config_.parseEnv("max_split_size_mb:100");
    EXPECT_EQ(config_.max_split_size_, 100u * kMB);
}

TEST_F(TestSMAConfig, ParseMaxSplitSizeClamped)
{
    config_.parseEnv("max_split_size_mb:25");
    EXPECT_GE(config_.max_split_size_, kLargeBuffer);
}

TEST_F(TestSMAConfig, ParseGCThreshold)
{
    config_.parseEnv("garbage_collection_threshold:0.75");
    EXPECT_DOUBLE_EQ(config_.garbage_collection_threshold_, 0.75);
}

TEST_F(TestSMAConfig, ParseUseSMAAllocatorFalse)
{
    config_.parseEnv("use_sma_allocator:False");
    EXPECT_FALSE(config_.use_sma_allocator_);
}

TEST_F(TestSMAConfig, ParseUseSMAAllocatorTrue)
{
    config_.parseEnv("use_sma_allocator:True");
    EXPECT_TRUE(config_.use_sma_allocator_);
}

TEST_F(TestSMAConfig, ParseUseVMMForStaticMemoryTrue)
{
    config_.parseEnv("use_vmm_for_static_memory:True");
    EXPECT_TRUE(config_.use_vmm_for_static_memory_);
}

TEST_F(TestSMAConfig, ParseSmallHeapSize)
{
    config_.parseEnv("small_heap_size:2048");
    EXPECT_EQ(config_.small_heap_size_, 2048u);
}

TEST_F(TestSMAConfig, ParseSmallHeapThreshold)
{
    config_.parseEnv("small_heap_threshold:512");
    EXPECT_EQ(config_.small_heap_threshold_, 512u);
}

TEST_F(TestSMAConfig, ParseSegmentSizeMb)
{
    config_.parseEnv("segment_size_mb:64");
    EXPECT_EQ(config_.segment_size_mb_, 64u * kMB);
}

TEST_F(TestSMAConfig, ParseMultipleOptions)
{
    config_.parseEnv("max_split_size_mb:200,use_sma_allocator:False,"
                     "garbage_collection_threshold:0.25,small_heap_size:1024,"
                     "use_vmm_for_static_memory:True,segment_size_mb:32,"
                     "small_heap_threshold:256");
    EXPECT_EQ(config_.max_split_size_, 200u * kMB);
    EXPECT_FALSE(config_.use_sma_allocator_);
    EXPECT_DOUBLE_EQ(config_.garbage_collection_threshold_, 0.25);
    EXPECT_EQ(config_.small_heap_size_, 1024u);
    EXPECT_TRUE(config_.use_vmm_for_static_memory_);
    EXPECT_EQ(config_.segment_size_mb_, 32u * kMB);
    EXPECT_EQ(config_.small_heap_threshold_, 256u);
}

TEST_F(TestSMAConfig, StaticMaxSplitSize)
{
    EXPECT_EQ(SMAConfig::max_split_size(), std::numeric_limits<size_t>::max());
}

TEST_F(TestSMAConfig, StaticGCThreshold)
{
    EXPECT_DOUBLE_EQ(SMAConfig::garbage_collection_threshold(), 0.0);
}

TEST_F(TestSMAConfig, StaticUseSMAAllocator)
{
    EXPECT_TRUE(SMAConfig::use_sma_allocator());
}

TEST_F(TestSMAConfig, StaticUseVMMForStaticMemory)
{
    EXPECT_FALSE(SMAConfig::use_vmm_for_static_memory());
}

TEST_F(TestSMAConfig, StaticSmallHeapSize)
{
    EXPECT_EQ(SMAConfig::small_heap_size(), kSmallHeapSize);
}

TEST_F(TestSMAConfig, StaticSmallHeapThreshold)
{
    EXPECT_EQ(SMAConfig::small_heap_threshold(), kSmallThreshold);
}

TEST_F(TestSMAConfig, StaticSegmentSizeMb)
{
    EXPECT_EQ(SMAConfig::segment_size_mb(), kSmallBuffer);
}
