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
/**
 * @file hcc_server_types.h
 * @brief Internal type definitions used by the HCC server.
 */
#ifndef MF_OPENDCC_HCC_SERVER_TYPES_H
#define MF_OPENDCC_HCC_SERVER_TYPES_H

#include "hcc_types.h"

#include <cstdlib>

namespace ock {
namespace openDCC {

struct TcpMessage {
    uint16_t opcode = 0;   /* op code */
    int16_t result = 0;    /* result */
    uint32_t dataSize = 0; /* size of data to be sent or received */
    uint32_t crc = 0;      /* reserved for crc  */
    char *data = nullptr;  /* data to be sent or received, owned by this message */

    TcpMessage() = default;
    ~TcpMessage()
    {
        free(data);
    }

    TcpMessage(const TcpMessage &) = delete;
    TcpMessage &operator=(const TcpMessage &) = delete;
};

std::ostream &operator<<(std::ostream &os, const TcpMessage &message) noexcept;

class TcpSocket;
class TcpServer;
class TcpServerWorker;
using TcpSocketPtr = Ref<TcpSocket>;
using TcpServerPtr = Ref<TcpServer>;
using TcpServerWorkerPtr = Ref<TcpServerWorker>;

using TcpMessageHandler = std::function<int32_t(const TcpSocketPtr &socket, TcpMessage &msg, uint32_t)>;
using TcpSocketCreatedHandler = std::function<int32_t(const TcpSocketPtr &socket, uint32_t)>;

struct TcpServerOptions {
    std::string ip;                                         /* ip string */
    uint16_t port;                                          /* port for listen */
    bool startListener = false;                             /* start listen */
    uint8_t workerThreadCount = 4;                          /* thread count */
    TcpMessageHandler messageHandler = nullptr;             /* message handler */
    TcpSocketCreatedHandler socketCreatedHandler = nullptr; /* socket create handler */
};

std::ostream &operator<<(std::ostream &os, const TcpServerOptions &options) noexcept;

struct ExternalConnectionHandler : public Referable {
    void (*callback)(hcc_tunnel_t *tunnel_handle, uint64_t ctx, int32_t result) = nullptr; /* connect callback */
    uint64_t usrCtx = 0;    /* user context passed to the callback */
    uint64_t commId_ = 0;   /* communicator id */
    std::string ip;         /* peer ip address */
    uint32_t timeoutMs = 0; /* connect timeout, in milliseconds */
    uint16_t port = 0;      /* peer port */
};
using ExternalConnectionHandlerPtr = Ref<ExternalConnectionHandler>;

std::ostream &operator<<(std::ostream &os, const ExternalConnectionHandler &handler) noexcept;

void SafeCloseFd(int &fd) noexcept;
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_SERVER_TYPES_H
