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

#define private public // Test access to heartbeat timeout parsing.
#include "smem_tcp_config_store_server.h"
#undef private

namespace ock {
namespace smem {
namespace {

constexpr uint32_t K_DEFAULT_HEARTBEAT_TIMEOUT_S = 60;
constexpr uint32_t K_HEARTBEAT_TIMEOUT_S = 15;
constexpr uint32_t K_MAX_HEARTBEAT_TIMEOUT_S = 3600;

TEST(AccStoreServerTest, HeartbeatTimeoutParsesSecondsAndZero)
{
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("0"), 0U);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("15"), K_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("3600"), K_MAX_HEARTBEAT_TIMEOUT_S);
}

TEST(AccStoreServerTest, HeartbeatTimeoutMissingOrInvalidUsesDefault)
{
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS(""), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("-1"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("abc"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("3601"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
}

} // namespace
} // namespace smem
} // namespace ock
