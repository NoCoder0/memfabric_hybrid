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

#include <net/if.h>
#include <sys/time.h>
#include <pthread.h>

#include "acc_common_util.h"
#include "mf_ipv4_validator.h"
#include "acc_http_link_default.h"
#include "acc_http_listener.h"

namespace ock {
namespace acc {

namespace {
/* Enable TCP keepalive (dead-connection probing at the TCP layer) on accepted
 * sockets. This is distinct from HTTP keep-alive (connection reuse); HTTP
 * keep-alive links benefit from TCP keepalive to detect half-open peers. */
constexpr int TCP_NODELAY_ON = 1;
} // namespace

Result AccHttpListener::StartAcceptThread() noexcept
{
    threadName_ = "http_accept_poll";
    pollTimeoutMs_ = HTTP_LISTENER_POLL_TIMEOUT_MS;
    return AccTcpListener::StartAcceptThread();
}

void AccHttpListener::ProcessNewConnection(int fd, struct sockaddr_in addressIn) noexcept
{
    std::string ipPort = inet_ntoa(addressIn.sin_addr);
    ipPort += ":";
    ipPort += std::to_string(ntohs(addressIn.sin_port));

    SSL *ssl = nullptr;

    if (enableTls_) {
        auto ret = AccTcpSslHelper::NewSslLink(true, fd, sslCtx_, ssl);
        if (ret != ACC_OK) {
            LOG_ERROR("Failed to new connection ssl link, fd=" << fd << ", ipPort=" << ipPort);
            SafeCloseFd(fd);
            return;
        }
    }

    LOG_INFO("Connected from " << ipPort << " successfully, ssl " << (enableTls_ ? "enable" : "disable"));
    AccTcpLinkDefaultPtr newLink;
    if (linkFactory_) {
        newLink = linkFactory_(fd, ipPort, ssl);
    } else {
        newLink = AccConvert<AccHttpLinkDefault, AccTcpLinkDefault>(AccMakeRef<AccHttpLinkDefault>(fd, ipPort, ssl));
    }
    if (newLink == nullptr) {
        LOG_ERROR("Failed to create listener http link object, probably out of memory, fd=" << fd <<
            ", ipPort=" << ipPort);
        if (ssl != nullptr) {
            if (AccCommonUtil::SslShutdownHelper(ssl) != ACC_OK) {
                LOG_ERROR("shut down ssl failed!");
            }
            OpenSslApiWrapper::SslFree(ssl);
            ssl = nullptr;
        }
        SafeCloseFd(fd);
        return;
    }

    AccConnReq req{};
    auto result = connHandler_(req, newLink.Get());
    if (result != ACC_OK) {
        LOG_ERROR("ProcessNewConnection: connHandler_ non-ok result=" << result << ", ipPort=" << ipPort);
        return;
    }
}

} // namespace acc
} // namespace ock