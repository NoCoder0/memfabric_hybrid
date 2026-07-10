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
#include <cstdint>
#include <cstddef>
#include <limits>

#include "percentile.h"

using namespace ock::mf;

constexpr size_t TEST_CAP = 4U;
constexpr uint32_t UNIFORM_COUNT = 100U;
constexpr uint32_t P50_LOWER = 40U;
constexpr uint32_t P50_UPPER = 60U;
constexpr uint32_t P99_LOWER = 90U;
constexpr double RATIO_P50 = 0.50;
constexpr double RATIO_P99 = 0.99;
// ============================================================
// Log2BucketIndex
// ============================================================

TEST(Log2BucketIndexTest, Bucket0_Covers0To3)
{
    EXPECT_EQ(Log2BucketIndex(0U), 0U);
    EXPECT_EQ(Log2BucketIndex(2U), 0U);
    EXPECT_EQ(Log2BucketIndex(3U), 0U);
}

TEST(Log2BucketIndexTest, BucketMapping_DoublingPattern)
{
    EXPECT_EQ(Log2BucketIndex(4U), 1U);
    EXPECT_EQ(Log2BucketIndex(7U), 1U);
    EXPECT_EQ(Log2BucketIndex(8U), 2U);
    EXPECT_EQ(Log2BucketIndex(15U), 2U);
    EXPECT_EQ(Log2BucketIndex(64U), 5U);
    EXPECT_EQ(Log2BucketIndex(127U), 5U);
}

TEST(Log2BucketIndexTest, HighValue_ClampedToLastBucket)
{
    EXPECT_EQ(Log2BucketIndex(1U << (NUM_LOG_BUCKETS - 1)), NUM_LOG_BUCKETS - 1);
    EXPECT_EQ(Log2BucketIndex(0xFFFFFFFFU), NUM_LOG_BUCKETS - 1);
}

// ============================================================
// SampleBucket
// ============================================================

TEST(SampleBucketTest, AddAndGetAt)
{
    SampleBucket<TEST_CAP> bucket;
    EXPECT_TRUE(bucket.Add(40U));
    EXPECT_TRUE(bucket.Add(10U));
    EXPECT_TRUE(bucket.Add(30U));
    EXPECT_TRUE(bucket.Add(20U));
    EXPECT_EQ(bucket.NumStored(), 4U);
    EXPECT_TRUE(bucket.Full());
    EXPECT_EQ(bucket.GetAt(0U), 10U);
    EXPECT_EQ(bucket.GetAt(3U), 40U);
    EXPECT_FALSE(bucket.Add(50U));
    EXPECT_EQ(bucket.NumAdded(), 5U);
}

TEST(SampleBucketTest, Clear_ResetsState)
{
    SampleBucket<TEST_CAP> bucket;
    bucket.Add(10U);
    bucket.Add(20U);
    bucket.Clear();
    EXPECT_EQ(bucket.NumStored(), 0U);
    EXPECT_EQ(bucket.NumAdded(), 0U);
    EXPECT_TRUE(bucket.Empty());
}

TEST(SampleBucketTest, MergeFrom_EmptySelf)
{
    SampleBucket<TEST_CAP> bucket;
    SampleBucket<TEST_CAP> rhs;
    rhs.Add(100U);
    rhs.Add(200U);
    bucket.MergeFrom(rhs);
    EXPECT_EQ(bucket.NumStored(), 2U);
    EXPECT_EQ(bucket.NumAdded(), 2U);
}

TEST(SampleBucketTest, MergeFrom_NoOverflow_Appends)
{
    SampleBucket<TEST_CAP> bucket;
    bucket.Add(10U);
    bucket.Add(20U);
    SampleBucket<TEST_CAP> rhs;
    rhs.Add(30U);
    rhs.Add(40U);
    bucket.MergeFrom(rhs);
    EXPECT_EQ(bucket.NumStored(), 4U);
    EXPECT_EQ(bucket.NumAdded(), 4U);
}

TEST(SampleBucketTest, MergeFrom_Overflow_Downsamples)
{
    SampleBucket<TEST_CAP> bucket;
    bucket.Add(10U);
    bucket.Add(20U);
    bucket.Add(30U);
    SampleBucket<TEST_CAP> rhs;
    rhs.Add(40U);
    rhs.Add(50U);
    rhs.Add(60U);
    bucket.MergeFrom(rhs);
    EXPECT_EQ(bucket.NumStored(), 4U);
    EXPECT_EQ(bucket.NumAdded(), 6U);
}

// ============================================================
// PercentileSamples
// ============================================================

using TestPercentile = PercentileSamples<TLS_SAMPLE_SIZE>;

TEST(PercentileSamplesTest, GetPercentile_EdgeCases)
{
    TestPercentile pct;
    EXPECT_EQ(pct.GetPercentile(RATIO_P50), 0U);
    pct.AddValue(100U);
    EXPECT_EQ(pct.GetPercentile(RATIO_P50), 100U);
    EXPECT_EQ(pct.GetPercentile(RATIO_P99), 100U);
}

TEST(PercentileSamplesTest, GetPercentile_Uniform100)
{
    TestPercentile pct;
    for (uint32_t v = 1U; v <= UNIFORM_COUNT; ++v) {
        pct.AddValue(v);
    }
    uint32_t p50 = pct.GetPercentile(RATIO_P50);
    EXPECT_GE(p50, P50_LOWER);
    EXPECT_LE(p50, P50_UPPER);
    uint32_t p99 = pct.GetPercentile(RATIO_P99);
    EXPECT_GE(p99, P99_LOWER);
}

TEST(PercentileSamplesTest, AddValue64_NegativeIgnored_Uint32MaxClamped)
{
    TestPercentile pct;
    pct.AddValue64(-100);
    EXPECT_EQ(pct.TotalAdded(), 0U);
    int64_t hugeVal = static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1000LL;
    pct.AddValue64(hugeVal);
    EXPECT_EQ(pct.TotalAdded(), 1U);
    EXPECT_EQ(pct.GetPercentile(RATIO_P50), std::numeric_limits<uint32_t>::max());
}

TEST(PercentileSamplesTest, Clear_ResetsTotal)
{
    TestPercentile pct;
    pct.AddValue(100U);
    pct.Clear();
    EXPECT_EQ(pct.TotalAdded(), 0U);
    EXPECT_EQ(pct.GetPercentile(RATIO_P50), 0U);
}

TEST(PercentileSamplesTest, MergeFromSame_CombinesCounts)
{
    TestPercentile pct1;
    pct1.AddValue(10U);
    pct1.AddValue(20U);
    TestPercentile pct2;
    pct2.AddValue(30U);
    pct2.AddValue(40U);
    pct1.MergeFromSame(pct2);
    EXPECT_EQ(pct1.TotalAdded(), 4U);
    uint32_t p50 = pct1.GetPercentile(RATIO_P50);
    EXPECT_GE(p50, 10U);
    EXPECT_LE(p50, 40U);
}
