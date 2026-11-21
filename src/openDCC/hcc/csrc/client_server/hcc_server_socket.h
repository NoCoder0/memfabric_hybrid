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
#ifndef MF_OPENDCC_HCC_SERVER_SOCKET_H
#define MF_OPENDCC_HCC_SERVER_SOCKET_H

#include "hcc_server_types.h"
#include "common/hcc_define.h"

#include <atomic>
#include <deque>
#include <netinet/in.h>
#include <thread>
#include <utility>
#include <vector>

namespace ock {
namespace openDCC {

enum SocketStage {
    STAGE_CONNECTING, /* doing async connection at client */
    STAGE_CONNECTED,  /* connected */
};

/**
 * @brief One TCP connection, supports blocking send and receive of TcpMessage.
 */
class TcpSocket : public Referable {
public:
    TcpSocket(int fd, bool client) noexcept : fd_(fd), client_(client) {}

    /**
     * @brief Connect to an IPv4 server
     *
     * @param host             [in] IPv4 address of the server
     * @param port             [in] port of the server
     * @return HCC_OK if successful, negative error code if failed
     */
    Result Connect(const std::string &host, uint16_t port);

    /**
     * @brief Close the socket
     */
    void Disconnect();

    /**
     * @brief Blocking send one message
     *
     * @param message          [in] message to be sent, data is owned by the caller
     * @return HCC_OK if successful, negative error code if failed
     */
    Result BlockSend(const TcpMessage &message) noexcept;

    /**
     * @brief Blocking receive one message
     *
     * @param message          [out] message received, data is allocated and owned by the message
     * @return HCC_OK if successful, negative error code if failed
     */
    Result BlockReceive(TcpMessage &message) noexcept;

    /**
     * @brief Fill the local ip:port string from the socket fd, a warning is logged if it fails
     *
     * @param fd               [in] socket fd
     */
    void SetMyIpPort(int fd) noexcept;

    /**
     * @brief Fill the peer ip:port string from the socket fd, a warning is logged if it fails
     *
     * @param fd               [in] socket fd
     */
    void SetPeerIpPort(int fd) noexcept;

    /**
     * @brief Get the connection description, formatted as "myIp:myPort to peerIp:peerPort"
     *
     * @return connection description
     */
    std::string GetConnString() noexcept;

    /**
     * @brief Get the fd of the socket
     *
     * @return socket fd, -1 if the socket is not valid
     */
    int GetFd() const noexcept;

private:
    /* set the socket fd blocking or non-blocking */
    static Result SetBlocking(int fd, bool blocking) noexcept;
    /* set FD_CLOEXEC on the socket fd, best effort */
    static void SetCloseOnExec(int fd) noexcept;
    /* read one complete message from the fd */
    Result ReadOneMessage(int fd, TcpMessage &message) noexcept;

private:
    SocketStage stage_ = STAGE_CONNECTING; /* socket stage */
    int fd_ = -1;                          /* fd of the socket */
    bool client_ = true;                   /* true if the socket is created by Connect */
    std::string myIpPort_;                 /* local ip and port, format "ip:port" */
    std::string peerIpPort_;               /* peer ip and port, format "ip:port" */

    friend struct TcpServerWork;
    friend class TcpServer;
    friend class TcpServerWorker;
};

ALWAYS_INLINE std::string TcpSocket::GetConnString() noexcept
{
    return myIpPort_ + " to " + peerIpPort_;
}

ALWAYS_INLINE int TcpSocket::GetFd() const noexcept
{
    return fd_;
}

/**
 * @brief One socket event submitted by the poll thread to a worker.
 */
struct TcpServerWork {
    TcpSocketPtr socket;          /* socket which triggered the event */
    bool connectionEvent = false; /* connection event or data event */
    short revents = 0;            /* poll events of the socket */
};

/**
 * @brief Worker thread that reads one message from the socket and calls the message handler.
 */
class TcpServerWorker final : public Referable {
public:
    /**
     * @brief Create a worker with the given index
     *
     * @param index            [in] worker index
     */
    explicit TcpServerWorker(uint8_t index) noexcept : index_(index) {}

    /**
     * @brief Stop the worker thread
     */
    ~TcpServerWorker() noexcept override;

private:
    /* start the worker thread */
    Result Start() noexcept;
    /* stop the worker thread */
    void Stop() noexcept;

    /* wait for and process queued works */
    void RunInThread() noexcept;
    /* process one work item */
    void ProcessWork(const TcpServerWork &work) noexcept;

private:
    TcpMessageHandler handler_;                              /* callback to process received message */
    TcpSocketCreatedHandler socketCreatedHandler_ = nullptr; /* socket created handler triggered by connect/accept */
    std::mutex mutex_;                                       /* mutex protecting the work queue and state */
    std::condition_variable cv_;                             /* condition variable for the work queue */
    std::deque<TcpServerWork> works_;                        /* pending socket events */
    bool stopping_ = false;                                  /* worker is stopping */
    bool running_ = false;                                   /* worker thread is running */
    const uint8_t index_;                                    /* worker index */
    std::thread thread_;                                     /* worker thread */

    friend class TcpServer;
};

/**
 * @brief TCP server that listens for connections, polls socket events and dispatches them to workers.
 */
class TcpServer final : public Referable {
public:
    /**
     * @brief Create an idle TCP server
     */
    TcpServer() = default;

    /**
     * @brief Stop the server and release all resources
     */
    ~TcpServer() noexcept override;

    /**
     * @brief Start the TCP server
     *
     * @param options          [in] server options
     * @return HCC_OK if successful, negative error code if failed
     */
    Result Start(const TcpServerOptions &options) noexcept;

    /**
     * @brief Stop the TCP server, close all sockets and join threads
     */
    void Stop() noexcept;

    /**
     * @brief Connect the server to a peer asynchronously through a new TCP connection
     *
     * @param host             [in] IPv4 address of the peer
     * @param port             [in] port of the peer
     * @param fd               [out] fd of the created socket, -1 if failed
     * @return HCC_OK if successful, negative error code if failed
     */
    Result ConnectToPeerAsync(const std::string &host, uint16_t port, int &fd) noexcept;

private:
    /* validate server options before starting */
    Result VerifyOptions(const TcpServerOptions &options) noexcept;

    /* create, bind and listen on the given ipv4 address */
    Result ListenIpv4(const std::string &host, uint16_t port) noexcept;

    /* start the listen socket and poll thread */
    Result StartPollThread() noexcept;
    /* stop the poll thread and close the listen fd */
    void StopPollThread() noexcept;
    /* poll socket events and dispatch them */
    void RunInThread() noexcept;

    /* create and start all worker threads */
    Result StartWorkersWithThread() noexcept;
    /* stop all worker threads */
    void StopWorkersWithThread() noexcept;

    /* dispatch one poll event */
    Result HandleOneEvent(int fd, short revents, const TcpSocketPtr &socket) noexcept;
    /* finish the async connect and report it */
    Result HandleConnectEvent(int fd, const TcpSocketPtr &socket, short revents) noexcept;
    /* accept one pending connection */
    Result Accept() noexcept;
    /* submit one work item to a worker */
    Result SubmitWork(const TcpServerWork &work) noexcept;

    /* add a socket to the poll list, caller holds the lock */
    void AddFdAndSocketLocked(int fd, const TcpSocketPtr &socket) noexcept;
    /* remove and close one socket, caller holds the lock */
    void RemoveSocketLocked(int fd) noexcept;
    /* close and clear all sockets, caller holds no lock */
    void ClearAllSocketsUnlocked() noexcept;

private:
    TcpMessageHandler msgHandler_ = nullptr;                 /* callback to process received message */
    TcpSocketCreatedHandler socketCreatedHandler_ = nullptr; /* socket created handler triggered by connect/accept */
    std::vector<std::pair<int, TcpSocketPtr>> fds_;          /* socket fds and sockets to poll */
    bool fdsChanged_ = false;                                /* fds changed and the poll list needs rebuild */
    std::vector<TcpServerWorkerPtr> workers_;                /* worker threads */
    std::mutex mutex_;                                       /* mutex protecting server state */
    bool started_ = false;                                   /* server is started */
    uint8_t workerThreadCount_ = 2;                          /* worker thread count */
    int32_t listenFd_ = -1;                                  /* listen fd, -1 if not listening */
    TcpServerOptions options_;                               /* server options */
    std::atomic<bool> pollStopping_{false};                  /* poll thread should stop */
    std::thread thread_;                                     /* poll thread */
};

} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_SERVER_SOCKET_H
