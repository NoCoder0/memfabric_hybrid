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

#include "zbal_test_constants.h"
#include "zbal_deepep_config.h"

using namespace zbal::adaptor::deep_ep;

/* ==================== get_low_latency_rdma_size_hint ==================== */

TEST(TestDeepEPConfig, LowLatencyRdmaSizeHintReturnsFirstParam)
{
    EXPECT_EQ(get_low_latency_rdma_size_hint(ZBAL_UT_NUM_100, ZBAL_UT_NUM_512, ZBAL_UT_NUM_4, ZBAL_UT_NUM_8),
              ZBAL_UT_NUM_100);
    EXPECT_EQ(get_low_latency_rdma_size_hint(0, ZBAL_UT_NUM_512, ZBAL_UT_NUM_4, ZBAL_UT_NUM_8), 0u);
    EXPECT_EQ(get_low_latency_rdma_size_hint(ZBAL_UT_NUM_256, 0, ZBAL_UT_NUM_1, ZBAL_UT_NUM_1), ZBAL_UT_NUM_256);
    EXPECT_EQ(get_low_latency_rdma_size_hint(ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, ZBAL_UT_NUM_16, ZBAL_UT_NUM_64),
              ZBAL_UT_SIZE_1KB);
}

/* ==================== get_value_from_env ==================== */

TEST(TestDeepEPConfig, GetValueFromEnv)
{
    const char *VAR = "ZBAL_DEEEEP_TEST_VAR";

    // env not set → returns default
    unsetenv(VAR);
    EXPECT_EQ(get_value_from_env(VAR, ZBAL_UT_NUM_42), ZBAL_UT_NUM_42);

    // valid numeric → returns parsed value
    setenv(VAR, "123", 1);
    EXPECT_EQ(get_value_from_env(VAR, ZBAL_UT_NUM_42), ZBAL_UT_NUM_123);

    // zero
    setenv(VAR, "0", 1);
    EXPECT_EQ(get_value_from_env(VAR, ZBAL_UT_NUM_42), 0u);

    // negative / non-digit leading / empty / non-numeric / trailing → all return default
    const char *rejected[] = {"-99", "  100", "", "abc123", "42abc"};
    for (const auto &s : rejected) {
        setenv(VAR, s, 1);
        EXPECT_EQ(get_value_from_env(VAR, ZBAL_UT_NUM_42), ZBAL_UT_NUM_42);
    }
    unsetenv(VAR);
}

/* ==================== Config struct ==================== */

TEST(TestDeepEPConfig, ConfigConstructor)
{
    Config cfg(ZBAL_UT_NUM_8, ZBAL_UT_NUM_16, ZBAL_UT_NUM_32, ZBAL_UT_NUM_64, ZBAL_UT_NUM_128);
    EXPECT_EQ(cfg.num_sms, ZBAL_UT_NUM_8);
    EXPECT_EQ(cfg.num_max_nvl_chunked_send_tokens, ZBAL_UT_NUM_16);
    EXPECT_EQ(cfg.num_max_nvl_chunked_recv_tokens, ZBAL_UT_NUM_32);
    EXPECT_EQ(cfg.num_max_rdma_chunked_send_tokens, ZBAL_UT_NUM_64);
    EXPECT_EQ(cfg.num_max_rdma_chunked_recv_tokens, ZBAL_UT_NUM_128);
}

TEST(TestDeepEPConfig, ConfigBufferSizeHints)
{
    Config cfg(ZBAL_UT_NUM_8, ZBAL_UT_NUM_16, ZBAL_UT_NUM_32, ZBAL_UT_NUM_64, ZBAL_UT_NUM_128);

    EXPECT_EQ(cfg.get_nvl_buffer_size_hint(ZBAL_UT_SIZE_1KB, ZBAL_UT_NUM_4), ZBAL_UT_SIZE_1KB);
    EXPECT_EQ(cfg.get_nvl_buffer_size_hint(ZBAL_UT_SIZE_2KB, ZBAL_UT_NUM_8), ZBAL_UT_SIZE_2KB);
    EXPECT_EQ(cfg.get_nvl_buffer_size_hint(0, 1), 0u);

    EXPECT_EQ(cfg.get_rdma_buffer_size_hint(ZBAL_UT_SIZE_4KB, ZBAL_UT_NUM_4), ZBAL_UT_SIZE_4KB);
    EXPECT_EQ(cfg.get_rdma_buffer_size_hint(ZBAL_UT_SIZE_8KB, ZBAL_UT_NUM_8), ZBAL_UT_SIZE_8KB);
    EXPECT_EQ(cfg.get_rdma_buffer_size_hint(0, 1), 0u);
}
