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

#ifndef ACC_LINKS_ACC_HTTP_LISTENER_H
#define ACC_LINKS_ACC_HTTP_LISTENER_H

#include "acc_tcp_listener.h"

namespace ock {
namespace acc {

class AccHttpListener : public AccTcpListener {
public:
    AccHttpListener(std::string ip, uint16_t port, bool reusePort, bool enableTls = false, SSL_CTX *sslCtx = nullptr)
        : AccTcpListener(std::move(ip), port, reusePort, enableTls, sslCtx)
    {}

    ~AccHttpListener() override = default;

protected:
    void ProcessNewConnection(int fd, struct sockaddr_in addressIn) noexcept override;
    Result StartAcceptThread() noexcept override;
};

} // namespace acc
} // namespace ock

#endif // ACC_LINKS_ACC_HTTP_LISTENER_H
