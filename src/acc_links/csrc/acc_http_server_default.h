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

#ifndef ACC_LINKS_ACC_HTTP_SERVER_DEFAULT_H
#define ACC_LINKS_ACC_HTTP_SERVER_DEFAULT_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "acc_http_request_context.h"
#include "acc_http_server.h"
#include "acc_tcp_server_default.h"

namespace ock {
namespace acc {

/**
 * @brief HTTP server extending AccTcpServerDefault with path-based routing
 *
 * AccHttpServerDefault replaces the binary msgType dispatch with HTTP method + URI
 * route matching. On each new connection it creates AccHttpLinkDefault instances
 * (via the link factory) and registers HandleHttpRequest as
 * the worker callback. Routes are registered with RegisterHttpHandler().
 */
class AccHttpServerDefault : public AccTcpServerDefault, public AccHttpServer {
public:
    AccHttpServerDefault()
    {
        linkBlocking_ = false;
    }
    ~AccHttpServerDefault() override = default;

    Result Start(const AccHttpServerOptions &opt, const AccTlsOption &tlsOption) override;

    void Stop() override;
    void StopAfterFork() override;

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
    void RegisterHttpHandler(AccHttpMethod method, const std::string &path, const AccHttpReqHandler &handler) override;

protected:
    /**
     * @brief Validate that at least one HTTP handler and link broken handler are registered
     *
     * @return ACC_OK if valid, ACC_INVALID_PARAM otherwise
     */
    Result ValidateHandler() const override;

    /**
     * @brief Start the TCP listener with HTTP mode enabled
     *
     * Sets up the link factory to create AccHttpLinkDefault instances and creates
     * an AccHttpListener (no binary AccConnReq exchange).
     *
     * @return ACC_OK on success
     */
    Result StartListener() override;

    /**
     * @brief Start worker threads with HandleHttpRequest as the request callback
     *
     * @return ACC_OK on success
     */
    Result StartWorkers() override;

    /**
     * @brief Handle a new TCP connection from the listener
     *
     * Resets HTTP parsing state, initializes the link, and adds it to a worker.
     *
     * @param req     [in] connection request (unused for HTTP)
     * @param newLink [in] the newly accepted link (cast to AccHttpLinkDefault)
     * @return ACC_OK on success
     */
    Result HandleNewConnection(const AccConnReq &req, const AccTcpLinkDefaultPtr &newLink) override;
    Result HandleLinkBroken(const AccTcpLinkDefaultPtr &link) override;

private:
    /**
     * @brief Dispatch a received HTTP request to the registered handler
     *
     * Builds the route key "METHOD:/path", looks it up in httpHandlers_,
     * creates an AccHttpRequestContext, and invokes the handler.
     *
     * @param context [in] TCP request context containing the parsed HTTP data
     * @return ACC_OK on success, ACC_LINK_MSG_INVALID if no handler matches
     */
    Result HandleHttpRequest(const AccTcpRequestContext &context);

    /**
     * @brief Check whether any handler is registered for the given path (any method)
     *
     * @param path [in] URI path to check
     * @return true if at least one handler matches the path or wildcard "*"
     */
    bool IsPathRegistered(const std::string &path) const;

    void IdleCheckLoop();

    void StopIdleCheck();

    /* route table: key = "METHOD:/path", value = handler */
    std::unordered_map<std::string, AccHttpReqHandler> httpHandlers_;
    size_t maxBodySize_ = HTTP_DEFAULT_MAX_BODY_SIZE; /* cached from AccHttpServerOptions::maxBodySize */

    /* HTTP link tracking for keep-alive idle timeout detection */
    std::unordered_map<uint32_t, AccTcpLinkDefaultPtr> httpLinks_;
    std::mutex httpLinksMutex_;
    std::thread idleCheckThread_;
    std::atomic<bool> idleCheckRunning_{false};
    std::condition_variable idleCheckCv_;
    std::mutex cvMutex_;
};

using AccHttpServerDefaultPtr = AccRef<AccHttpServerDefault>;

} // namespace acc
} // namespace ock

#endif // ACC_LINKS_ACC_HTTP_SERVER_DEFAULT_H
