/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
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

#include "smem.h"
#include "smem_acc_api.h"
#include "smem_config_store_api.h"
#include "smem_version.h"

namespace {
constexpr uint32_t VERSION_MAJOR_SHIFT = 16U;
constexpr uint32_t MEMFABRIC_ABI_VERSION =
    (static_cast<uint32_t>(VERSION_MAJOR) << VERSION_MAJOR_SHIFT) | static_cast<uint32_t>(VERSION_MINOR);
} // namespace

TEST(SmemRuntimeApiTest, GetAbiVersion)
{
    EXPECT_EQ(smem_get_abi_version(), MEMFABRIC_ABI_VERSION);
}

TEST(SmemRuntimeApiTest, RejectInvalidAccArguments)
{
    smem_acc_tcp_server_options_t tcpOptions{};
    smem_acc_http_server_options_t httpOptions{};
    smem_acc_connect_request_t request{};
    smem_acc_link_t link = nullptr;
    uint32_t bodyLength = 1U;

    EXPECT_NE(smem_acc_tcp_server_start(nullptr, &tcpOptions, nullptr), 0);
    EXPECT_NE(smem_acc_http_server_start(nullptr, &httpOptions, nullptr), 0);
    EXPECT_NE(smem_acc_server_load_tls_library(nullptr, nullptr), 0);
    EXPECT_NE(smem_acc_server_connect(nullptr, "127.0.0.1", 0U, &request, 0U, &link), 0);
    EXPECT_NE(smem_acc_http_server_register_handler(nullptr, 0U, "/", nullptr, nullptr), 0);
    EXPECT_EQ(smem_acc_link_retain(nullptr), nullptr);
    smem_acc_link_release(nullptr);
    EXPECT_EQ(smem_acc_link_id(nullptr), UINT32_MAX);
    smem_acc_link_set_context(nullptr, 0U);
    EXPECT_EQ(smem_acc_link_get_context(nullptr), 0U);
    EXPECT_NE(smem_acc_link_send(nullptr, 0, 0, 0U, nullptr, 0U), 0);
    EXPECT_NE(smem_acc_link_send_v2(nullptr, 0, 0, 0U, nullptr, 0U, nullptr, 0U), 0);
    EXPECT_EQ(smem_acc_http_request_body(nullptr, &bodyLength), nullptr);
    EXPECT_EQ(bodyLength, 0U);
    EXPECT_EQ(smem_acc_http_request_param(nullptr, "name"), nullptr);
    EXPECT_NE(smem_acc_http_request_visit_params(nullptr, nullptr, nullptr), 0);
    EXPECT_NE(smem_acc_http_request_reply(nullptr, 0U, nullptr, nullptr, 0U), 0);

    smem_acc_server_stop(nullptr);
    smem_acc_server_set_decrypt_handler(nullptr, nullptr, nullptr);
    smem_acc_server_set_new_link_handler(nullptr, nullptr, nullptr);
    smem_acc_server_set_request_handler(nullptr, 0, nullptr, nullptr);
    smem_acc_server_set_sent_handler(nullptr, 0, nullptr, nullptr);
    smem_acc_server_set_link_broken_handler(nullptr, nullptr, nullptr);
    smem_acc_server_destroy(nullptr);
}

TEST(SmemRuntimeApiTest, RejectInvalidConfigStoreArguments)
{
    EXPECT_EQ(smem_config_store_create_server(nullptr), nullptr);
    smem_config_store_destroy_server(nullptr, nullptr);
    smem_config_store_set_server_broken_handler(nullptr, nullptr, nullptr);
    EXPECT_EQ(smem_config_store_set_leader_handler(nullptr, nullptr, nullptr), -1);
}
