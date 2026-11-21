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
#ifndef OPEN_DCC_TOOLS_ODCC_CMD_SOCKET_H
#define OPEN_DCC_TOOLS_ODCC_CMD_SOCKET_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <chrono>

#include <cerrno>
#include <cstdint>
#include <string>

namespace ock {
namespace openDCC {
using SocketDeadline = std::chrono::steady_clock::time_point;

inline int WaitSocket(int fd, short events, uint32_t timeoutMs, SocketDeadline deadline) noexcept
{
    for (;;) {
        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (timeoutMs != 0 && remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        pollfd descriptor{fd, events, 0};
        int result = ::poll(&descriptor, 1, timeoutMs == 0 ? -1 : static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            errno = ETIMEDOUT;
        }
        return result > 0 ? 0 : -1;
    }
}

inline size_t TransferSocket(int fd, void *buffer, size_t size, bool sending, uint32_t timeoutMs) noexcept
{
    if (fd < 0 || (buffer == nullptr && size != 0)) {
        errno = EINVAL;
        return 0;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto data = static_cast<char *>(buffer);
    size_t transferred = 0;
    while (transferred < size) {
        if (WaitSocket(fd, sending ? POLLOUT : POLLIN, timeoutMs, deadline) != 0) {
            break;
        }
        auto count = sending ? ::send(fd, data + transferred, size - transferred, MSG_NOSIGNAL | MSG_DONTWAIT)
                             : ::recv(fd, data + transferred, size - transferred, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = ECONNRESET;
            }
            break;
        }
        transferred += static_cast<size_t>(count);
    }
    return transferred;
}

class SocketClient {
public:
    int32_t Connect(const std::string &ip, uint16_t port, uint32_t timeoutMs = 0) noexcept;
    size_t Send(void *buffer, size_t size) noexcept;
    size_t Receive(void *buffer, size_t size) noexcept;
    void Close() noexcept;

private:
    uint32_t timeoutMs_ = 0; /* zero means no timeout */
    int fd_ = -1; /* fd after connected */
};

class SocketServer {
public:
    int32_t ListenAndAccept(const std::string &ip, uint16_t port, uint32_t timeoutMs = 0) noexcept;
    size_t Send(void *buffer, size_t size) noexcept;
    size_t Receive(void *buffer, size_t size) noexcept;
    void Close() noexcept;

private:
    int32_t Accept(uint32_t timeoutMs) noexcept;

private:
    uint32_t timeoutMs_ = 0; /* zero means no timeout */
    int listenPort_ = 9010; /* listen port */
    int listenFd_ = -1;     /* listen fd */
    int acceptedFd_ = -1;   /* accept fd */
};

/**
 * @brief Connect to the given tcp server, the previous connection is closed first
 *
 * @param ip               [in] ip of the server
 * @param port             [in] port of the server
 * @param timeoutMs        [in] connection and transfer timeout, zero means no timeout
 * @return 0 if successful, -1 if failed with errno set
 */
inline int32_t SocketClient::Connect(const std::string &ip, uint16_t port, uint32_t timeoutMs) noexcept
{
    if (ip.empty() || port == 0) {
        errno = EINVAL;
        return -1;
    }

    Close();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        errno = EINVAL;
        return -1;
    }

    timeoutMs_ = timeoutMs;
    if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        ::close(fd);
        return -1;
    }
    int result = ::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    if (result < 0 && errno == EINPROGRESS) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        result = WaitSocket(fd, POLLOUT, timeoutMs, deadline);
        int error = 0;
        socklen_t length = sizeof(error);
        if (result == 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
            result = -1;
        }
        if (result == 0 && error != 0) {
            errno = error;
            result = -1;
        }
    }
    if (result < 0) {
        ::close(fd);
        return -1;
    }

    fd_ = fd;
    return 0;
}

/**
 * @brief Send exactly the given bytes, blocking until all of them are sent or an error occurs
 *
 * @param buffer           [in] data to be sent
 * @param size             [in] size of the data
 * @return the bytes actually sent, less than size if failed
 */
inline size_t SocketClient::Send(void *buffer, size_t size) noexcept
{
    return TransferSocket(fd_, buffer, size, true, timeoutMs_);
}

/**
 * @brief Receive exactly the given bytes, blocking until all of them are received or an error occurs
 *
 * @param buffer           [out] buffer to store the received data
 * @param size             [in] size of the buffer
 * @return the bytes actually received, less than size if the peer closed or failed
 */
inline size_t SocketClient::Receive(void *buffer, size_t size) noexcept
{
    return TransferSocket(fd_, buffer, size, false, timeoutMs_);
}

inline void SocketClient::Close() noexcept
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

/**
 * @brief Listen on the given ip and port, then block until one client is accepted, the previous listener
 *        and connection are closed first so that this function can be called repeatedly
 *
 * @param ip               [in] ip to be listened, e.g. "0.0.0.0"
 * @param port             [in] port to be listened
 * @param timeoutMs        [in] accept and transfer timeout, zero means no timeout
 * @return 0 if successful, -1 if failed with errno set
 */
inline int32_t SocketServer::ListenAndAccept(const std::string &ip, uint16_t port, uint32_t timeoutMs) noexcept
{
    if (ip.empty() || port == 0) {
        errno = EINVAL;
        return -1;
    }

    /* close the previous listener and connection, so that listening again is supported */
    Close();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int reuse = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ::close(fd);
        return -1;
    }

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        errno = EINVAL;
        return -1;
    }

    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        return -1;
    }

    listenPort_ = static_cast<int>(port);
    listenFd_ = fd;

    return Accept(timeoutMs);
}

inline int32_t SocketServer::Accept(uint32_t timeoutMs) noexcept
{
    timeoutMs_ = timeoutMs;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    if (::fcntl(listenFd_, F_SETFL, O_NONBLOCK) < 0) {
        Close();
        return -1;
    }
    for (;;) {
        if (WaitSocket(listenFd_, POLLIN, timeoutMs, deadline) != 0) {
            Close();
            return -1;
        }
        acceptedFd_ = ::accept(listenFd_, nullptr, nullptr);
        if (acceptedFd_ >= 0) {
            return 0;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            Close();
            return -1;
        }
    }
}

/**
 * @brief Send exactly the given bytes to the accepted client, blocking until all of them are sent
 *
 * @param buffer           [in] data to be sent
 * @param size             [in] size of the data
 * @return the bytes actually sent, less than size if failed
 */
inline size_t SocketServer::Send(void *buffer, size_t size) noexcept
{
    return TransferSocket(acceptedFd_, buffer, size, true, timeoutMs_);
}

/**
 * @brief Receive exactly the given bytes from the accepted client, blocking until all of them are received
 *
 * @param buffer           [out] buffer to store the received data
 * @param size             [in] size of the buffer
 * @return the bytes actually received, less than size if the peer closed or failed
 */
inline size_t SocketServer::Receive(void *buffer, size_t size) noexcept
{
    return TransferSocket(acceptedFd_, buffer, size, false, timeoutMs_);
}

inline void SocketServer::Close() noexcept
{
    if (acceptedFd_ >= 0) {
        ::close(acceptedFd_);
        acceptedFd_ = -1;
    }

    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}
} // namespace openDCC
} // namespace ock

#endif // OPEN_DCC_TOOLS_ODCC_CMD_SOCKET_H
