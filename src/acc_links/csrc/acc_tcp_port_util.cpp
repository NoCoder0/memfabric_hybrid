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
#include "acc_tcp_port_util.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "acc_includes.h"
#include "acc_tcp_common.h"
#include "mf_net.h"

namespace ock {
namespace acc {
namespace {

int BindTcpPortV4(int &sockfd, uint16_t port)
{
    sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        int err = errno;
        LOG_ERROR("bind ipv4 tcp port failed, port=" << port << ", errno=" << err << ", errstr=" << std::strerror(err));
        SafeCloseFd(sockfd);
        return -1;
    }
    return 0;
}

int BindTcpPortV6(int &sockfd, uint16_t port)
{
    sockfd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;
    if (::bind(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        int err = errno;
        LOG_ERROR("bind ipv6 tcp port failed, port=" << port << ", errno=" << err << ", errstr=" << std::strerror(err));
        SafeCloseFd(sockfd);
        return -1;
    }
    return 0;
}

bool SupportsIpv6() noexcept
{
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    SafeCloseFd(fd);
    return true;
}

} // namespace

uint16_t AccFindAvailableTcpPort(int &sockfd, uint16_t minPort, uint16_t maxPort)
{
    if (sockfd >= 0) {
        SafeCloseFd(sockfd);
    }
    if (minPort == 0 || maxPort < minPort) {
        LOG_ERROR("invalid port range, minPort=" << minPort << ", maxPort=" << maxPort);
        return 0;
    }

    const bool supportsIpv6 = SupportsIpv6();
    const auto port = mf::SelectRandomTcpPort(minPort, maxPort, [&](uint16_t candidate) {
        return BindTcpPortV4(sockfd, candidate) == 0 || (supportsIpv6 && BindTcpPortV6(sockfd, candidate) == 0);
    });
    if (port == 0) {
        LOG_ERROR("no available tcp port found in range [" << minPort << "," << maxPort << "]");
    }
    return port;
}
} // namespace acc
} // namespace ock
