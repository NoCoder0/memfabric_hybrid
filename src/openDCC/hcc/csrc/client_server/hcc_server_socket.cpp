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
#include "hcc_server_socket.h"

#include "../common/hcc_logger.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace ock {
namespace openDCC {
namespace {
constexpr uint32_t MAX_TCP_MESSAGE_SIZE = 1U * 1024U * 1024U; /* max send receive size, 1MB */
constexpr size_t TCP_MESSAGE_HEADER_SIZE = offsetof(TcpMessage, data);
} // namespace

TcpServer::~TcpServer() noexcept
{
    Stop();
}

Result TcpServer::Start(const TcpServerOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        HCC_LOG_INFO("TcpServer already started");
        return HCC_OK;
    }

    /* step1: verify options */
    Result result = VerifyOptions(options);
    if (result != HCC_OK) {
        HCC_LOG_ERROR("Start tcp server failed, verify options failed, result(" << result << ")");
        return result;
    }

    /* step2: save options and handlers */
    options_ = options;
    workerThreadCount_ = options.workerThreadCount;
    msgHandler_ = options.messageHandler;
    socketCreatedHandler_ = options.socketCreatedHandler;

    /* step3: start worker threads */
    result = StartWorkersWithThread();
    if (result != HCC_OK) {
        HCC_LOG_ERROR("Start tcp server failed, start workers failed, result("
                      << result << "), worker count(" << static_cast<uint32_t>(workerThreadCount_) << ")");
        return result;
    }

    /* step5: start poll thread */
    result = StartPollThread();
    if (result != HCC_OK) {
        HCC_LOG_ERROR("Start tcp server failed, start poll thread failed, result("
                      << result << "), ip(" << options_.ip << "), port(" << options_.port << ")");
        StopWorkersWithThread();
        return result;
    }

    /* step4: mark inited before the poll thread starts */
    started_ = true;

    /* step6: log debug */
    HCC_LOG_DEBUG("Tcp server started, ip(" << options_.ip << "), port(" << options_.port << "), worker count("
                                            << static_cast<uint32_t>(workerThreadCount_) << ")");
    return HCC_OK;
}

void TcpServer::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            HCC_LOG_DEBUG("Tcp server not started");
            return;
        }
        /* release the lock before joining threads, they may still need it */
        started_ = false;
    }

    StopPollThread();
    StopWorkersWithThread();
    ClearAllSocketsUnlocked();

    HCC_LOG_DEBUG("Tcp server stopped");
}

Result TcpServer::ConnectToPeerAsync(const std::string &host, uint16_t port, int &fd) noexcept
{
    /* step0: reset the output fd */
    fd = -1;

    /* step1: validate ip and port */
    if (port == 0) {
        HCC_LOG_ERROR("Connect to (" << host << ":" << port << ") as port is invalid");
        return HCC_INVALID_PARAM;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        HCC_LOG_ERROR("Connect to (" << host << ":" << port << ") failed, ip/port is invalid");
        return HCC_INVALID_PARAM;
    }

    /* step2: create socket to tmp */
    int sockFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0) {
        HCC_LOG_ERROR("Connect (" << host << ":" << port << ") failed as create socket failed, errno(" << errno << ")");
        return HCC_ERROR;
    }

    /* set close on exec */
    TcpSocket::SetCloseOnExec(sockFd);

    /* set non-blocking */
    Result result = TcpSocket::SetBlocking(sockFd, false);
    if (result != HCC_OK) {
        SafeCloseFd(sockFd);
        return result;
    }

    /* step3: create socket object */
    auto tmpSocket = mf::MakeRef<TcpSocket>(sockFd, true);
    if (tmpSocket == nullptr) {
        SafeCloseFd(sockFd);
        HCC_LOG_ERROR("Connect (" << host << ":" << port
                                  << ") failed as create socket object failed, probably out of memory");
        return HCC_ERROR;
    }

    /* step4: connect in non-blocking mode */
    Result ret = connect(sockFd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) {
        /* connected successfully, probably happens when the server on the same host */
        result = TcpSocket::SetBlocking(sockFd, true);
        if (result != HCC_OK) {
            SafeCloseFd(sockFd);
            return result;
        }

        tmpSocket->stage_ = STAGE_CONNECTED;
        AddFdAndSocketLocked(sockFd, tmpSocket);

        TcpServerWork work;
        work.socket = tmpSocket;
        work.connectionEvent = true;
        work.revents = 0;

        SubmitWork(work);

        fd = sockFd;
        return HCC_OK;
    }

    /* step5: check if error */
    int connectErr = errno;
    if (connectErr != EINPROGRESS && connectErr != EINTR && connectErr != EALREADY) {
        /*
         * EINPROGRESS: connect in progress
         * EINTR:       interrupt
         * EALREADY:    already in connect
         */
        SafeCloseFd(sockFd);
        HCC_LOG_ERROR("Connect (" << host << ":" << port << ") failed, errno(" << connectErr << ")");
        return HCC_ERROR;
    }

    /* step6: added to fd set */
    tmpSocket->stage_ = STAGE_CONNECTING;
    AddFdAndSocketLocked(sockFd, tmpSocket);

    fd = sockFd;
    return HCC_OK;
}

Result TcpServer::VerifyOptions(const TcpServerOptions &options) noexcept
{
    /* step1: verify all member into option */
    HCC_ASSERT_MSG_RETURN(!options.ip.empty(), "Invalid param, ip is empty", HCC_INVALID_PARAM);
    HCC_ASSERT_MSG_RETURN(options.port != 0, "Invalid param, port is 0", HCC_INVALID_PARAM);
    HCC_ASSERT_MSG_RETURN(options.workerThreadCount > 0, "Invalid param, worker thread count is 0", HCC_INVALID_PARAM);

    if (options.messageHandler == nullptr) {
        HCC_LOG_ERROR("Verify options failed, message handler is null");
        return HCC_INVALID_PARAM;
    }

    if (options.socketCreatedHandler == nullptr) {
        HCC_LOG_ERROR("Verify options failed, socket created handler is null");
        return HCC_INVALID_PARAM;
    }

    return HCC_OK;
}

Result TcpServer::ListenIpv4(const std::string &host, uint16_t port) noexcept
{
    /* step1: check param */
    HCC_ASSERT_MSG_RETURN(!host.empty(), "Invalid param, ip is empty", HCC_INVALID_PARAM);
    HCC_ASSERT_MSG_RETURN(port != 0, "Invalid param, port is 0", HCC_INVALID_PARAM);

    /* step2: convert host and port to address */
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int ret = ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (ret != 1) {
        HCC_LOG_ERROR("Listen failed, invalid ip(" << host << "), ret(" << ret << ")");
        return HCC_INVALID_PARAM;
    }

    /* step3: create tmp socket fd */
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        HCC_LOG_ERROR("Listen failed, create socket failed, ip(" << host << "), port(" << port << "), errno(" << errno
                                                                 << ")");
        return HCC_ERROR;
    }

    ret = TcpSocket::SetBlocking(fd, false);
    if (ret != HCC_OK) {
        SafeCloseFd(fd);
        return ret;
    }

    int enable = 1;
    ret = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    if (ret != 0) {
        HCC_LOG_ERROR("Listen failed, set reuseaddr failed, errno(" << errno << ")");
        SafeCloseFd(fd);
        return HCC_ERROR;
    }

    /* step4: bind and listen, should set REUSABLE */
    ret = ::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    if (ret != 0) {
        HCC_LOG_ERROR("Listen failed, bind failed, ip(" << host << "), port(" << port << "), errno(" << errno << ")");
        SafeCloseFd(fd);
        return HCC_ERROR;
    }

    ret = ::listen(fd, SOMAXCONN);
    if (ret != 0) {
        HCC_LOG_ERROR("Listen failed, listen failed, ip(" << host << "), port(" << port << "), errno(" << errno << ")");
        SafeCloseFd(fd);
        return HCC_ERROR;
    }

    /* step5: assign to listenFd_ */
    listenFd_ = fd;
    HCC_LOG_DEBUG("Listen successfully, ip(" << host << "), port(" << port << "), fd(" << listenFd_ << ")");
    return HCC_OK;
}

Result TcpServer::StartPollThread() noexcept
{
    if (!options_.startListener) {
        return HCC_OK;
    }

    /* step1: start listen */
    Result result = ListenIpv4(options_.ip, options_.port);
    if (result != HCC_OK) {
        return result;
    }

    /* step2: create poll thread */
    pollStopping_ = false;
    try {
        thread_ = std::thread(&TcpServer::RunInThread, this);
    } catch (const std::system_error &e) {
        HCC_LOG_ERROR("Start poll thread failed, create thread failed, error(" << e.what() << ")");
        SafeCloseFd(listenFd_);
        return HCC_ERROR;
    }

    return HCC_OK;
}

void TcpServer::StopPollThread() noexcept
{
    pollStopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    SafeCloseFd(listenFd_);
}

void TcpServer::RunInThread() noexcept
{
    /* step1: set thread name to hcc_svr_epoll */
    (void)pthread_setname_np(pthread_self(), "hcc_svr_poll");

    /* step2: create fds and sockets */
    std::vector<struct pollfd> pollFds;
    std::vector<TcpSocketPtr> pollSockets;
    pollFds.reserve(32);
    pollSockets.reserve(32);
    pollFds.push_back({listenFd_, POLLIN, 0});
    pollSockets.push_back(nullptr);

    constexpr int pollTimeoutMs = 500;
    while (!pollStopping_) {
        {
            /* step1: update if fds updated */
            std::lock_guard<std::mutex> lock(mutex_);
            if (fdsChanged_) {
                pollFds.clear();
                pollSockets.clear();
                pollFds.push_back({listenFd_, POLLIN, 0});
                pollSockets.push_back(nullptr);

                for (const auto &item : fds_) {
                    short events = item.second->stage_ == STAGE_CONNECTING ? POLLOUT : POLLIN;
                    pollFds.push_back({item.first, events, 0});
                    pollSockets.push_back(item.second);
                }
                fdsChanged_ = false;
            }
        }

        /* step2: make polls */
        int ret = ::poll(pollFds.data(), pollFds.size(), pollTimeoutMs);
        if (ret < 0 && errno != EINTR) {
            HCC_LOG_ERROR("Tcp server poll failed, errno(" << errno << ")");
            break;
        }

        /* timeout */
        if (ret <= 0) {
            continue;
        }

        /* step3: check event and do handle accept or event */
        for (size_t i = 0; i < pollFds.size(); ++i) {
            Result result = HandleOneEvent(pollFds[i].fd, pollFds[i].revents, pollSockets[i]);
            if (result != HCC_OK) {
                HCC_LOG_DEBUG("Tcp server handle poll event failed, fd(" << pollFds[i].fd << "), result(" << result
                                                                         << ")");
            }
        }
    }

    /* step4: log info that thread is exiting */
    HCC_LOG_INFO("Tcp server poll thread exiting");
}

Result TcpServer::StartWorkersWithThread() noexcept
{
    workers_.reserve(workerThreadCount_);
    for (uint8_t i = 0; i < workerThreadCount_; ++i) {
        /* step1: create worker */
        auto worker = mf::MakeRef<TcpServerWorker>(i);
        if (worker == nullptr) {
            HCC_LOG_ERROR("Create worker(" << static_cast<uint32_t>(i) << ")/("
                                           << static_cast<uint32_t>(workerThreadCount_) << ") failed");
            StopWorkersWithThread();
            return HCC_MALLOC_FAILED;
        }

        /* step2: start worker */
        worker->handler_ = msgHandler_;
        worker->socketCreatedHandler_ = socketCreatedHandler_;
        Result result = worker->Start();
        if (result != HCC_OK) {
            HCC_LOG_ERROR("Create worker(" << static_cast<uint32_t>(i) << ")/("
                                           << static_cast<uint32_t>(workerThreadCount_) << ") failed, result(" << result
                                           << ")");
            StopWorkersWithThread();
            return result;
        }
        workers_.push_back(worker);
    }
    return HCC_OK;
}

void TcpServer::StopWorkersWithThread() noexcept
{
    for (auto &worker : workers_) {
        if (worker != nullptr) {
            worker->Stop();
        }
    }
    workers_.clear();
}

Result TcpServer::HandleOneEvent(int fd, short revents, const TcpSocketPtr &socket) noexcept
{
    /* step1: check if listen fd then call Accept */
    if (fd == listenFd_) {
        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            HCC_LOG_ERROR("Tcp server listen socket error, revents(" << revents << ")");
            return HCC_ERROR;
        }

        if ((revents & POLLIN) != 0) {
            return Accept();
        }

        return HCC_OK;
    }

    /* step2: process error case */
    if (socket == nullptr || (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        HCC_LOG_ERROR("Tcp server handle event failed as socket is null or has error");
        RemoveSocketLocked(fd);
        return HCC_OK;
    }

    /* step3: process connect event */
    if (socket->stage_ == STAGE_CONNECTING) {
        return HandleConnectEvent(fd, socket, revents);
    }

    /* step4: submit data work */
    if ((revents & POLLIN) == 0) {
        return HCC_OK;
    }

    TcpServerWork work;
    work.socket = socket;
    work.revents = revents;
    return SubmitWork(work);
}

Result TcpServer::HandleConnectEvent(int fd, const TcpSocketPtr &socket, short revents) noexcept
{
    if ((revents & POLLOUT) == 0) {
        return HCC_OK;
    }

    /* step1: check the connect result */
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        HCC_LOG_ERROR("Connect to peer failed, conn(" << socket->GetConnString() << "), errno("
                                                      << (err != 0 ? err : errno) << ")");
        RemoveSocketLocked(fd);
        return HCC_OK;
    }

    /* step2: refresh local and peer addresses */
    socket->SetMyIpPort(fd);
    socket->SetPeerIpPort(fd);

    /* step3: restore blocking mode and mark the socket connected */
    if (TcpSocket::SetBlocking(fd, true) != HCC_OK) {
        RemoveSocketLocked(fd);
        return HCC_OK;
    }

    /* step4: change to connect and set fds changed with lock */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        socket->stage_ = STAGE_CONNECTED;
        fdsChanged_ = true;
    }

    /* step5: report the connection event to the worker */
    TcpServerWork work;
    work.socket = socket;
    work.connectionEvent = true;
    work.revents = 0;
    return SubmitWork(work);
}

Result TcpServer::Accept() noexcept
{
    /* step1: accept one socket */
    struct sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    int fd = ::accept(listenFd_, reinterpret_cast<struct sockaddr *>(&peer), &peerLen);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return HCC_OK;
        }

        HCC_LOG_INFO("Accept failed, listen fd(" << listenFd_ << "), errno(" << errno << ")");
        return HCC_ERROR;
    }

    /* step2: set close-on-exec */
    TcpSocket::SetCloseOnExec(fd);

    /* step3: create socket object */
    TcpSocketPtr socket = mf::MakeRef<TcpSocket>(fd, false);
    if (socket == nullptr) {
        HCC_LOG_ERROR("Accept failed for peer port(" << ntohs(peer.sin_port) << "), probably out of memory");
        SafeCloseFd(fd);
        return HCC_MALLOC_FAILED;
    }
    socket->SetMyIpPort(fd);
    socket->SetPeerIpPort(fd);

    /* step4: set socket stage and add the socket to the poll set */
    socket->stage_ = STAGE_CONNECTED;
    AddFdAndSocketLocked(fd, socket);

    HCC_LOG_DEBUG("Accept new connection, conn(" << socket->GetConnString() << ")");
    return HCC_OK;
}

Result TcpServer::SubmitWork(const TcpServerWork &work) noexcept
{
    /* step1: check param */
    if (UNLIKELY(work.socket == nullptr || work.socket->fd_ < 0)) {
        HCC_LOG_WARN("Handle event failed, socket is invalid");
        return HCC_INVALID_PARAM;
    }

    /* step2: check workers */
    if (UNLIKELY(workers_.empty())) {
        HCC_LOG_WARN("Handle event failed, worker list is empty, conn(" << work.socket->GetConnString() << ")");
        return HCC_NOT_INITIALIZED;
    }

    /* step3: balance work and submit work */
    int fd = work.socket->fd_;
    auto &worker = workers_[static_cast<size_t>(fd) % workers_.size()];
    {
        std::lock_guard<std::mutex> lock(worker->mutex_);
        worker->works_.push_back(work);
    }

    /* step4: notify worker */
    worker->cv_.notify_one();
    HCC_LOG_DEBUG("Dispatch event for conn(" << work.socket->GetConnString() << "), worker count(" << workers_.size()
                                             << ")");
    return HCC_OK;
}

void TcpServer::AddFdAndSocketLocked(int fd, const TcpSocketPtr &socket) noexcept
{
    if (socket == nullptr) {
        HCC_LOG_ERROR("Add fd failed as socket is null");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    fds_.push_back({fd, socket});
    fdsChanged_ = true;
}

void TcpServer::RemoveSocketLocked(int fd) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = std::find_if(fds_.begin(), fds_.end(),
                             [fd](const std::pair<int, TcpSocketPtr> &item) { return item.first == fd; });
    if (iter != fds_.end()) {
        if (iter->second != nullptr) {
            iter->second->Disconnect();
        }
        fds_.erase(iter);
    }

    fdsChanged_ = true;
}

void TcpServer::ClearAllSocketsUnlocked() noexcept
{
    for (auto &item : fds_) {
        if (item.second != nullptr) {
            item.second->Disconnect();
        }
    }

    fds_.clear();
}

TcpServerWorker::~TcpServerWorker() noexcept
{
    Stop();
}

/**************************TcpServerWorker*****************************************/
Result TcpServerWorker::Start() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        HCC_LOG_DEBUG("Worker already started");
        return HCC_OK;
    }

    if (handler_ == nullptr) {
        HCC_LOG_ERROR("Failed to start TcpServerWorker as handler is nullptr");
        return HCC_INVALID_PARAM;
    }

    if (socketCreatedHandler_ == nullptr) {
        HCC_LOG_ERROR("Failed to start TcpServerWorker as socket create handler is nullptr");
        return HCC_INVALID_PARAM;
    }

    stopping_ = false;
    try {
        thread_ = std::thread(&TcpServerWorker::RunInThread, this);
    } catch (const std::system_error &e) {
        HCC_LOG_ERROR("Start tcp worker index(" << static_cast<uint32_t>(index_) << ") failed, error(" << e.what()
                                                << ")");
        return HCC_ERROR;
    }

    running_ = true;
    return HCC_OK;
}

void TcpServerWorker::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stopping_ = true;
    }

    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void TcpServerWorker::RunInThread() noexcept
{
    std::string threadName = "hcc_svr_wrk" + std::to_string(index_);
    (void)pthread_setname_np(pthread_self(), threadName.c_str());

    while (true) {
        /* step1: pick one work */
        TcpServerWork work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !works_.empty(); });
            if (stopping_ && works_.empty()) {
                break;
            }
            work = works_.front();
            works_.pop_front();
        }

        /* step2: check param */
        if (work.socket == nullptr || work.socket->fd_ < 0) {
            HCC_LOG_WARN("Invalid work, socket is null or fd is -1");
            continue;
        }

        /* step3: skip work without data event unless it is a connection event */
        if (!work.connectionEvent && (work.revents & POLLIN) == 0) {
            continue;
        }

        /* step4: process work */
        ProcessWork(work);
    }

    HCC_LOG_INFO("Tcp worker (" << static_cast<uint32_t>(index_) << ") exiting");
}

void TcpServerWorker::ProcessWork(const TcpServerWork &work) noexcept
{
    /* step0: process connection event, no need read message */
    if (work.connectionEvent) {
        auto result = socketCreatedHandler_(work.socket, 0);
        if (result != HCC_OK) {
            HCC_LOG_DEBUG("Call socket create handler function failed, result(" << result << ")");
        }

        return;
    }

    int fd = work.socket->fd_;
    /* step1: read data */
    TcpMessage message;
    Result readResult = work.socket->ReadOneMessage(fd, message);
    if (readResult != HCC_OK) {
        HCC_LOG_INFO("Tcp worker read failed, conn(" << work.socket->GetConnString() << "), result(" << readResult
                                                     << "), errno(" << errno << ")");
        /* NOTE: this should trigger poll failed, then trigger close socket */
        work.socket->Disconnect();
        return;
    }

    /* step3: call handler */
    int32_t result = handler_(work.socket, message, message.dataSize);
    if (result != HCC_OK) {
        HCC_LOG_DEBUG("Tcp worker process failed, conn(" << work.socket->GetConnString() << "), opcode("
                                                         << message.opcode << "), result(" << result << ")");
    }
}

/**************************TcpSocket*****************************************/
/* Best effort: failure to set FD_CLOEXEC does not affect the socket itself. */
void TcpSocket::SetCloseOnExec(int fd) noexcept
{
    if (fd < 0) {
        return;
    }

    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return;
    }

    (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

Result TcpSocket::SetBlocking(int fd, bool blocking) noexcept
{
    if (fd < 0) {
        HCC_LOG_ERROR("Set blocking failed, fd(" << fd << "), blocking(" << blocking << "), fd is invalid");
        return HCC_INVALID_PARAM;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        HCC_LOG_ERROR("Set blocking failed, fd(" << fd << "), blocking(" << blocking << "), get flags failed, errno("
                                                  << errno << ")");
        return HCC_ERROR;
    }

    int newFlags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    if (::fcntl(fd, F_SETFL, newFlags) < 0) {
        HCC_LOG_ERROR("Set blocking failed, fd(" << fd << "), blocking(" << blocking << "), set flags failed, errno("
                                                  << errno << ")");
        return HCC_ERROR;
    }

    return HCC_OK;
}

void TcpSocket::SetMyIpPort(int fd) noexcept
{
    struct sockaddr_in addr {};
    socklen_t addrLen = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &addrLen) != 0) {
        HCC_LOG_WARN("Set my ip port failed as getsockname failed, errno(" << errno << ")");
        return;
    }

    char ip[INET_ADDRSTRLEN] = {};
    (void)::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    myIpPort_ = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

void TcpSocket::SetPeerIpPort(int fd) noexcept
{
    struct sockaddr_in addr {};
    socklen_t addrLen = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<struct sockaddr *>(&addr), &addrLen) != 0) {
        HCC_LOG_WARN("Set peer ip port failed as getpeername failed, errno(" << errno << ")");
        return;
    }

    char ip[INET_ADDRSTRLEN] = {};
    (void)::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    peerIpPort_ = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

Result TcpSocket::ReadOneMessage(int fd, TcpMessage &message) noexcept
{
    /* step1: reset the message data */
    free(message.data);
    message.data = nullptr;
    message.dataSize = 0;

    /* step2: receive header */
    TcpMessage header{};
    ssize_t ret = ::recv(fd, &header, TCP_MESSAGE_HEADER_SIZE, MSG_WAITALL);
    if (ret != static_cast<ssize_t>(TCP_MESSAGE_HEADER_SIZE)) {
        HCC_LOG_WARN("Read message header failed, conn(" << GetConnString() << "), ret(" << ret << "), errno(" << errno
                                                         << ")");
        return HCC_ERROR;
    }

    /* step3: check body size */
    if (header.dataSize > MAX_TCP_MESSAGE_SIZE) {
        HCC_LOG_ERROR("Read message failed, message too large, conn(" << GetConnString() << "), data size("
                                                                      << header.dataSize << ")");
        return HCC_INVALID_PARAM;
    }

    message.opcode = header.opcode;
    message.result = header.result;
    message.dataSize = header.dataSize;
    message.crc = header.crc;
    /* dont receive data */
    if (message.dataSize == 0) {
        HCC_LOG_DEBUG("Receive message, conn(" << GetConnString() << "), opcode(" << message.opcode
                                               << "), no data need to be received");
        return HCC_OK;
    }

    /* step4: malloc data memory and receive data */
    message.data = static_cast<char *>(malloc(message.dataSize));
    if (message.data == nullptr) {
        HCC_LOG_ERROR("Read message failed as malloc failed, conn(" << GetConnString() << "), data size("
                                                                    << message.dataSize << ")");
        message.dataSize = 0;
        return HCC_MALLOC_FAILED;
    }

    ret = ::recv(fd, message.data, message.dataSize, MSG_WAITALL);
    if (ret != static_cast<ssize_t>(message.dataSize)) {
        HCC_LOG_WARN("Read message data failed, conn(" << GetConnString() << "), received(" << ret << "), data size("
                                                       << message.dataSize << "), errno(" << errno << ")");
        free(message.data);
        message.data = nullptr;
        message.dataSize = 0;
        return HCC_ERROR;
    }

    return HCC_OK;
}

Result TcpSocket::BlockSend(const TcpMessage &message) noexcept
{
    /* step1: check if fd is valid */
    if (fd_ < 0) {
        HCC_LOG_ERROR("Block send failed, socket is invalid, conn(" << GetConnString() << ")");
        return HCC_INVALID_PARAM;
    }

    /* step2: check size */
    if (message.dataSize > MAX_TCP_MESSAGE_SIZE) {
        HCC_LOG_ERROR("Block send failed, data size(" << message.dataSize << ") is invalid, conn(" << GetConnString()
                                                      << ")");
        return HCC_INVALID_PARAM;
    }

    if (message.dataSize > 0 && message.data == nullptr) {
        HCC_LOG_ERROR("Block send failed, data is null, data size(" << message.dataSize << "), conn(" << GetConnString()
                                                                    << ")");
        return HCC_INVALID_PARAM;
    }

    /* step3: send header */
    ssize_t ret = ::send(fd_, &message, TCP_MESSAGE_HEADER_SIZE, 0);
    if (ret != static_cast<ssize_t>(TCP_MESSAGE_HEADER_SIZE)) {
        HCC_LOG_WARN("Block send header failed, conn(" << GetConnString() << "), sent(" << ret << "), expect("
                                                       << TCP_MESSAGE_HEADER_SIZE << "), errno(" << errno << ")");
        return HCC_ERROR;
    }

    /* step4: send body */
    if (message.dataSize > 0) {
        ret = ::send(fd_, message.data, message.dataSize, 0);
        if (ret != static_cast<ssize_t>(message.dataSize)) {
            HCC_LOG_WARN("Block send data failed, conn(" << GetConnString() << "), sent(" << ret << "), data size("
                                                         << message.dataSize << "), errno(" << errno << ")");
            return HCC_ERROR;
        }
    }

    return HCC_OK;
}

Result TcpSocket::BlockReceive(TcpMessage &message) noexcept
{
    /* step1: check if fd is valid */
    if (fd_ < 0) {
        HCC_LOG_ERROR("Block receive failed, socket is invalid, conn(" << GetConnString() << ")");
        return HCC_INVALID_PARAM;
    }

    /* step2: receive one message */
    Result result = ReadOneMessage(fd_, message);
    if (result != HCC_OK) {
        HCC_LOG_ERROR("Block receive failed, conn(" << GetConnString() << "), result(" << result << "), errno(" << errno
                                                    << ")");
        return result;
    }

    return HCC_OK;
}

Result TcpSocket::Connect(const std::string &host, uint16_t port)
{
    /* step1: validate ip and port */
    if (port == 0) {
        HCC_LOG_ERROR("Connect to (" << host << ":" << port << ") as port is invalid");
        return HCC_INVALID_PARAM;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        HCC_LOG_ERROR("Connect to (" << host << ":" << port << ") failed, ip/port is invalid");
        return HCC_INVALID_PARAM;
    }

    /* step2: create socket to tmp */
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        HCC_LOG_ERROR("Connect (" << host << ":" << port << ") failed as create socket failed, errno(" << errno << ")");
        return HCC_ERROR;
    }

    SetCloseOnExec(fd);

    /* step3: try to connect and repeat 5 times, usleep(2000) as interval */
    constexpr int CONNECT_RETRY_TIMES = 5;
    constexpr int CONNECT_RETRY_INTERVAL_US = 2000;
    for (int i = 0; i < CONNECT_RETRY_TIMES; ++i) {
        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
            fd_ = fd;
            client_ = true;
            stage_ = STAGE_CONNECTED;
            SetMyIpPort(fd_);
            SetPeerIpPort(fd_);

            HCC_LOG_DEBUG("Connect successfully, conn(" << GetConnString() << ")");
            return HCC_OK;
        }

        if (i + 1 < CONNECT_RETRY_TIMES) {
            (void)::usleep(CONNECT_RETRY_INTERVAL_US);
        }
    }

    HCC_LOG_ERROR("Connect to (" << host << ":" << port << ") failed, errno(" << errno << "), retry times("
                                 << CONNECT_RETRY_TIMES << ")");
    SafeCloseFd(fd);
    return HCC_CONNECT_FAILED;
}

void TcpSocket::Disconnect()
{
    SafeCloseFd(fd_);
}

} // namespace openDCC
} // namespace ock
