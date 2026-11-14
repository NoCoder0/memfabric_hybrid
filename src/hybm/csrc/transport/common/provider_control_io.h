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

#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
namespace ock::mf::transport::device {
using ProviderDeadline = std::chrono::steady_clock::time_point;
namespace provider_control {
bool SendAll(int fd, const void *buf, size_t len, ProviderDeadline deadline);
bool RecvAll(int fd, void *buf, size_t len, ProviderDeadline deadline);
int CreateListenSocket(uint16_t &port);
int ConnectToHost(const std::string &ip, uint16_t port, ProviderDeadline deadline);
bool ParsePort(std::string_view value, uint16_t &port);
bool ResolveListenPort(uint16_t &port);
bool ParseUrl(const std::string &url, std::string &ip, uint16_t &port);
// SocketIdle additionally requires that no frame is pending: a control link that only expects replies at
// known points treats an unprompted frame as a protocol violation.
bool SocketAlive(int fd);
bool SocketIdle(int fd);
} // namespace provider_control
} // namespace ock::mf::transport::device
