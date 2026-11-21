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
 * @file hcc_server_types.cpp
 * @brief Implementation of the internal HCC server types.
 */

#include "hcc_server_types.h"

#include <string>

namespace ock {
namespace openDCC {
void SafeCloseFd(int &fd) noexcept
{
    if (fd < 0) return;
    int32_t tmpFd = fd;
    if (__sync_bool_compare_and_swap(&fd, tmpFd, -1)) {
        ::close(tmpFd);
    }
}

std::ostream &operator<<(std::ostream &os, const TcpMessage &message) noexcept
{
    os << "tcp message [opcode: " << message.opcode << ", result: " << message.result
       << ", data size: " << message.dataSize << ", crc: " << message.crc
       << ", data: " << (message.data != nullptr) << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const TcpServerOptions &options) noexcept
{
    os << "tcp server options [ip: " << options.ip << ", port: " << options.port
       << ", start listener: " << options.startListener
       << ", worker thread count: " << static_cast<uint32_t>(options.workerThreadCount)
       << ", message handler: " << (options.messageHandler != nullptr)
       << ", socket created handler: " << (options.socketCreatedHandler != nullptr) << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const ExternalConnectionHandler &handler) noexcept
{
    os << "external connection handler [ip: " << handler.ip << ", port: " << handler.port
       << ", callback: " << (handler.callback != nullptr) << ", timeout ms: " << handler.timeoutMs
       << ", user ctx: " << handler.usrCtx << ", comm id: " << handler.commId_ << "]";
    return os;
}
} // namespace openDCC
} // namespace ock
