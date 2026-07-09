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

#ifndef ACC_LINKS_ACC_HTTP_SERVER_H
#define ACC_LINKS_ACC_HTTP_SERVER_H

#include <functional>

#include "acc_def.h"
#include "acc_tcp_server.h"
#include "acc_http_request_context.h"

namespace ock {
namespace acc {

/**
 * @brief Handler for incoming HTTP requests
 *
 * @param context [in] HTTP request context providing method, URI, headers and Reply()
 * @return 0 on success, error code on failure
 */
using AccHttpReqHandler = std::function<int32_t(AccHttpRequestContext &context)>;

/**
 * @brief HTTP server with path-based routing
 *
 * AccHttpServer provides HTTP-specific server interface:
 * Start/Stop and route registration. It does not inherit from
 * AccTcpServer — users needing TCP-level methods (e.g.
 * RegisterLinkBrokenHandler) should dynamic_cast to AccTcpServer*.
 */
class ACC_API AccHttpServer : public virtual AccTcpServer {
public:
    /**
     * @brief Create an HTTP server
     *
     * @return shared pointer to the created server
     */
    static AccHttpServerPtr Create();

    /**
     * @brief Start the HTTP server with TLS option
     *
     * @param opt       [in] HTTP server options
     * @param tlsOption [in] TLS related options
     * @return ACC_OK if started successfully
     */
    virtual Result Start(const AccHttpServerOptions &opt, const AccTlsOption &tlsOption) = 0;

    /**
     * @brief Start the HTTP server without TLS
     *
     * @param opt [in] HTTP server options
     * @return ACC_OK if started successfully
     */
    Result Start(const AccHttpServerOptions &opt);

    /**
     * @brief Register a handler for a given HTTP method and URI path
     *
     * Wildcard path "*" matches any URI for the given method, acting as a
     * fallback. Duplicate (method, path) registrations are not allowed.
     *
     * @param method  [in] HTTP method (GET, POST, etc.)
     * @param path    [in] URI path, e.g. "/api/data", or "*" for wildcard
     * @param handler [in] callback invoked when a matching request arrives
     */
    virtual void RegisterHttpHandler(AccHttpMethod method, const std::string &path,
                                     const AccHttpReqHandler &handler) = 0;

    ~AccHttpServer() override = default;
};

inline Result AccHttpServer::Start(const AccHttpServerOptions &opt)
{
    return Start(opt, AccTlsOption());
}

} // namespace acc
} // namespace ock

#endif // ACC_LINKS_ACC_HTTP_SERVER_H
