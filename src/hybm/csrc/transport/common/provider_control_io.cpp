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

#include "provider_control_io.h"
#include "mf_net.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <charconv>
#include <cstdlib>
#include <cerrno>
#include <algorithm>
#include <climits>
namespace ock::mf::transport::device::provider_control {
namespace {
bool Wait(int fd, short events, ProviderDeadline deadline)
{
    auto now = std::chrono::steady_clock::now();
    while (now < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd item{fd, events, 0};
        auto ret = poll(&item, 1, static_cast<int>(std::clamp<int64_t>(remaining, 1, INT_MAX)));
        if (ret > 0) {
            if ((item.revents & events) != 0) {
                return true;
            }
            // Report the hangup: callers log errno after a false return and would otherwise inherit the
            // previous syscall's value.
            if ((item.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                errno = ECONNRESET;
                return false;
            }
            continue;
        }
        if (ret < 0 && errno != EINTR) {
            return false;
        }
        now = std::chrono::steady_clock::now();
    }
    errno = ETIMEDOUT;
    return false;
}

enum class PeekState : uint8_t { IDLE, DATA, CLOSED };

// Probe without consuming: DATA means an unprompted frame is waiting, CLOSED covers FIN and hard errors.
PeekState PeekSocket(int fd)
{
    char byte = 0;
    const auto count = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (count > 0) {
        return PeekState::DATA;
    }
    if (count == 0) {
        return PeekState::CLOSED;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return PeekState::IDLE;
    }
    return PeekState::CLOSED;
}

int ListenOnPort(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, SOMAXCONN) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
} // namespace
bool SendAll(int fd, const void *buf, size_t len, ProviderDeadline deadline)
{
    auto data = static_cast<const uint8_t *>(buf);
    while (len != 0) {
        if (!Wait(fd, POLLOUT, deadline))
            return false;
        auto count = send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count < 0)
            return false; // errno already describes the failure
        if (count == 0) {
            errno = EPIPE;
            return false;
        }
        data += count;
        len -= static_cast<size_t>(count);
    }
    return true;
}
bool RecvAll(int fd, void *buf, size_t len, ProviderDeadline deadline)
{
    auto data = static_cast<uint8_t *>(buf);
    while (len != 0) {
        if (!Wait(fd, POLLIN, deadline))
            return false;
        auto count = recv(fd, data, len, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count < 0)
            return false; // errno already describes the failure
        if (count == 0) {
            // EOF mid-frame: distinct from the ETIMEDOUT Wait() reports.
            errno = ECONNRESET;
            return false;
        }
        data += count;
        len -= static_cast<size_t>(count);
    }
    return true;
}
int CreateListenSocket(uint16_t &port)
{
    if (port != 0) {
        return ListenOnPort(port);
    }
    constexpr uint16_t minAutoPort = 9000;
    int fd = -1;
    port = SelectRandomTcpPort(minAutoPort, UINT16_MAX, [&](uint16_t candidate) {
        fd = ListenOnPort(candidate);
        return fd >= 0;
    });
    return fd;
}
int ConnectToHost(const std::string &ip, uint16_t port, ProviderDeadline deadline)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int ret = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    int error = 0;
    socklen_t length = sizeof(error);
    if ((ret != 0 && errno != EINPROGRESS) || (ret != 0 && !Wait(fd, POLLOUT, deadline)) ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
bool ParsePort(std::string_view value, uint16_t &port)
{
    if (value.empty()) {
        return false;
    }
    uint16_t parsed = 0;
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed == 0) {
        return false;
    }
    port = parsed;
    return true;
}

bool ResolveListenPort(uint16_t &port)
{
    if (port != 0) {
        return true;
    }
    const char *configured = std::getenv("MEMFABRIC_PROVIDER_PORT");
    if (configured == nullptr || *configured == '\0') {
        return true;
    }
    return ParsePort(configured, port);
}

bool ParseUrl(const std::string &url, std::string &ip, uint16_t &port)
{
    auto value = url.rfind("tcp://", 0) == 0 ? url.substr(6) : url;
    auto split = value.rfind(':');
    if (split == std::string::npos || split == 0 || split + 1 == value.size())
        return false;
    ip = value.substr(0, split);
    in_addr address{};
    return inet_pton(AF_INET, ip.c_str(), &address) == 1 && ParsePort(std::string_view(value).substr(split + 1), port);
}
bool SocketAlive(int fd)
{
    return PeekSocket(fd) != PeekState::CLOSED;
}

bool SocketIdle(int fd)
{
    return PeekSocket(fd) == PeekState::IDLE;
}
} // namespace ock::mf::transport::device::provider_control
