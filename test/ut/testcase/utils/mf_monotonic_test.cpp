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

#include "mf_monotonic_time.h"

using namespace ock::mf;

class MfMonotonicTest : public testing::Test {
public:
    static void SetUpTestCase() {}

    static void TearDownTestCase() {}

    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(MfMonotonicTest, time_test)
{
    ASSERT_EQ(MonotonicTime::TimeUs() != 0, true);
    ASSERT_EQ(MonotonicTime::TimeNs() != 0, true);
}

TEST_F(MfMonotonicTest, time_monotonic_increasing)
{
    uint64_t t1 = MonotonicTime::TimeUs();
    uint64_t t2 = MonotonicTime::TimeUs();
    EXPECT_GE(t2, t1);
}

TEST_F(MfMonotonicTest, time_ns_greater_than_us)
{
    uint64_t us = MonotonicTime::TimeUs();
    uint64_t ns = MonotonicTime::TimeNs();
    EXPECT_GE(ns, us);
}

TEST_F(MfMonotonicTest, mono_perf_trace_no_crash)
{
    MonoPerfTrace trace;
    trace.RecordStart();
    trace.RecordEnd();
}
