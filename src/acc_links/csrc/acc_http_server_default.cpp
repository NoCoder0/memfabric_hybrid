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

#include "acc_http_server_default.h"

#include <pthread.h>
#include <vector>

#include "acc_http_link_default.h"
#include "acc_http_listener.h"

namespace ock {
namespace acc {

namespace {
constexpr uint32_t HTTP_IDLE_CHECK_INTERVAL_MS = 1000;
} // namespace

Result AccHttpServerDefault::Start(const AccHttpServerOptions &opt, const AccTlsOption &tlsOption)
{
    AccTcpServerOptions tcpOpt;
    AccCommonServerOptions &commOpt = tcpOpt;
    commOpt = opt;
    /* HTTP has no magic/version handshake; tcpOpt keeps its own zero defaults */
    maxBodySize_ = opt.maxBodySize;
    return AccTcpServerDefault::Start(tcpOpt, tlsOption);
}

Result AccHttpServerDefault::ValidateHandler() const
{
    if (httpHandlers_.empty()) {
        LOG_ERROR("Invalid param, no http handler is registered");
        return ACC_INVALID_PARAM;
    }
    if (linkBrokenHandle_ == nullptr) {
        LOG_ERROR("Invalid param, link broken handler is not set");
        return ACC_INVALID_PARAM;
    }
    return ACC_OK;
}

void AccHttpServerDefault::RegisterHttpHandler(AccHttpMethod method, const std::string &path,
                                               const AccHttpReqHandler &handler)
{
    ASSERT_RET_VOID(!path.empty() && (path[0] == '/' || path == "*"));
    std::string key = AccHttpMethodToString(method) + ":" + path;
    ASSERT_RET_VOID(handler != nullptr);
    ASSERT_RET_VOID(httpHandlers_.find(key) == httpHandlers_.end());
    httpHandlers_[key] = handler;
}

/* create an AccHttpListener with a link factory that produces
 * AccHttpLinkDefault instances instead of AccTcpLinkComplexDefault */
Result AccHttpServerDefault::StartListener()
{
    if (!options_.enableListener) {
        return ACC_OK;
    }

    auto tmpListener = AccMakeRef<AccHttpListener>(options_.listenIp, options_.listenPort, options_.reusePort,
                                                   tlsOption_.enableTls, sslCtx_);
    ASSERT_RETURN(tmpListener.Get() != nullptr, ACC_NEW_OBJECT_FAIL);

    tmpListener->RegisterNewConnectionHandler(
        std::bind(&AccHttpServerDefault::HandleNewConnection, this, std::placeholders::_1, std::placeholders::_2));
    linkFactory_ = [](int fd, const std::string &ipPort, SSL *ssl) -> AccTcpLinkDefaultPtr {
        return AccConvert<AccHttpLinkDefault, AccTcpLinkDefault>(AccMakeRef<AccHttpLinkDefault>(fd, ipPort, ssl));
    };
    tmpListener->RegisterLinkFactory(linkFactory_);

    auto result = tmpListener->Start();
    if (result != ACC_OK) {
        LOG_ERROR("Failed to start HTTP listener, result: " << result);
        return result;
    }

    listener_ = AccConvert<AccHttpListener, AccTcpListener>(tmpListener);
    return ACC_OK;
}

/* start worker threads and register HandleHttpRequest (not HandleNewRequest)
 * as the worker callback so HTTP requests are dispatched through the route table */
Result AccHttpServerDefault::StartWorkers()
{
    AccTcpWorkerOptions workerOptions;
    workerOptions.threadPriority = options_.workerThreadPriority;
    workerOptions.cpuId = -1;
    workerOptions.pollingTimeoutMs = options_.workerPollTimeoutMs;
    workerOptions.name_ = "HttpWrk";
    for (uint16_t i = 0; i < options_.workerCount; i++) {
        if (options_.workerStartCpuId != -1) {
            workerOptions.cpuId = options_.workerStartCpuId + i;
        }
        workerOptions.index = i;

        auto tmpWorker = AccMakeRef<AccTcpWorker>(workerOptions);
        ASSERT_RETURN(tmpWorker.Get() != nullptr, ACC_NEW_OBJECT_FAIL);
        tmpWorker->RegisterNewRequestHandler(
            std::bind(&AccHttpServerDefault::HandleHttpRequest, this, std::placeholders::_1));
        tmpWorker->RegisterRequestSentHandler(std::bind(&AccTcpServerDefault::HandleRequestSent, this,
                                                        std::placeholders::_1, std::placeholders::_2,
                                                        std::placeholders::_3));
        tmpWorker->RegisterLinkBrokenHandler(
            std::bind(&AccHttpServerDefault::HandleLinkBroken, this, std::placeholders::_1));
        workers_.push_back(tmpWorker);
    }

    for (auto &item : workers_) {
        auto result = item->Start();
        if (result != ACC_OK) {
            LOG_ERROR("Failed to start HTTP worker, result: " << result);
            StopAndCleanWorkers();
            return result;
        }
    }

    idleCheckRunning_ = true;
    idleCheckThread_ = std::thread(&AccHttpServerDefault::IdleCheckLoop, this);

    return ACC_OK;
}

/* accept a new connection: reset HTTP state, select a worker, initialize the link,
 * invoke the user's new-link callback, then register the link with the worker's epoll */
Result AccHttpServerDefault::HandleNewConnection(const AccConnReq &req, const AccTcpLinkDefaultPtr &newLink)
{
    ASSERT_RETURN(newLink.Get() != nullptr, ACC_INVALID_PARAM);
    auto httpLink = AccConvert<AccTcpLinkDefault, AccHttpLinkDefault>(newLink);
    httpLink->ResetHttpState();
    httpLink->SetMaxBodySize(maxBodySize_);

    auto workIndex = WorkerSelect();
    if (workIndex == ACC_ERROR) {
        LOG_ERROR("Failed to select available worker for " << newLink->ShortName()
                                                           << ", workerCount=" << workers_.size());
        return ACC_ERROR;
    }

    auto &worker = workers_[workIndex];
    auto result = newLink->Initialize(options_.linkSendQueueSize, workIndex, worker.Get());
    if (UNLIKELY(result != ACC_OK)) {
        LOG_ERROR("Failed to initialize the link from " << newLink->ShortName() << ", result " << result);
        return ACC_ERROR;
    }

    if (newLinkHandle_ != nullptr) {
        result = newLinkHandle_(req, newLink.Get());
        if (UNLIKELY(result != ACC_OK)) {
            LOG_ERROR("New link handle callback returned error: " << result);
            return result;
        }
    }

    newLink->EnableNoBlocking();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!started_) {
            LOG_WARN("The server is being destroyed or has been destroyed. can't receive new connection.");
            return ACC_ERROR;
        }
        auto iter = connectedLinks_.find(newLink->Id());
        if (iter != connectedLinks_.end()) {
            LOG_ERROR("Failed to handle new connection as found duplicated link id " << newLink->Id());
            return ACC_ERROR;
        }
        result = worker->AddLink(newLink, EPOLLIN | EPOLLOUT | EPOLLET);
        if (UNLIKELY(result != ACC_OK)) {
            LOG_ERROR("Failed to add link to worker, result: " << result);
            return result;
        }
        connectedLinks_.emplace(newLink->Id(), newLink);
    }

    {
        std::lock_guard<std::mutex> g(httpLinksMutex_);
        httpLinks_.emplace(newLink->Id(), newLink);
    }

    return ACC_OK;
}

/* lookup the route in httpHandlers_, fall back to "METHOD:*", create an
 * AccHttpRequestContext and invoke the matched handler.
 * If no handler matches, send a plain 404 response. */
void AccHttpServerDefault::Stop()
{
    StopIdleCheck();
    AccTcpServerDefault::Stop();
    {
        std::lock_guard<std::mutex> g(httpLinksMutex_);
        httpLinks_.clear();
    }
}

void AccHttpServerDefault::StopAfterFork()
{
    idleCheckRunning_ = false;
    if (idleCheckThread_.joinable()) {
        idleCheckThread_.detach();
    }
    AccTcpServerDefault::StopAfterFork();
}

Result AccHttpServerDefault::HandleHttpRequest(const AccTcpRequestContext &context)
{
    auto httpLink = AccConvert<AccTcpLinkComplex, AccHttpLinkDefault>(context.Link());
    if (httpLink.Get() == nullptr) {
        LOG_ERROR("Http request handler called on non-HTTP link, linkId=" << context.Link()->Id());
        return ACC_LINK_MSG_INVALID;
    }

    /* CONNECT is not supported (RFC 7231 §4.3.6) */
    if (httpLink->method_ == "CONNECT") {
        LOG_ERROR("CONNECT method not supported on " << httpLink->ShortName() << ", method=" << httpLink->method_);
        httpLink->needClose_ = true;
        httpLink->SendHttpResponse(static_cast<int16_t>(AccHttpStatusCode::METHOD_NOT_ALLOWED),
                                   AccHttpStatusText(AccHttpStatusCode::METHOD_NOT_ALLOWED), "text/plain", nullptr, {},
                                   true);
        return ACC_LINK_MSG_INVALID;
    }

    std::string key = httpLink->method_ + ":" + httpLink->path_;
    auto it = httpHandlers_.find(key);
    if (it == httpHandlers_.end()) {
        it = httpHandlers_.find(httpLink->method_ + ":*");
    }
    if (it == httpHandlers_.end()) {
        /* distinguish 405 (path exists for another method) from 404 (path unknown) */
        httpLink->needClose_ = true;
        if (IsPathRegistered(httpLink->path_)) {
            LOG_ERROR("Method " << httpLink->method_ << " not allowed for path " << httpLink->path_);
            httpLink->SendHttpResponse(static_cast<int16_t>(AccHttpStatusCode::METHOD_NOT_ALLOWED),
                                       AccHttpStatusText(AccHttpStatusCode::METHOD_NOT_ALLOWED), "text/plain", nullptr,
                                       {}, true);
        } else {
            LOG_ERROR("No handler for " << key);
            httpLink->SendHttpResponse(static_cast<int16_t>(AccHttpStatusCode::NOT_FOUND),
                                       AccHttpStatusText(AccHttpStatusCode::NOT_FOUND), "text/plain", nullptr, {},
                                       true);
        }
        return ACC_LINK_MSG_INVALID;
    }

    auto reqCtx = AccMakeRef<AccHttpRequestContext>(context.Link(), httpLink->data_);
    if (reqCtx.Get() == nullptr) {
        LOG_ERROR("Failed to create AccHttpRequestContext on "
                  << httpLink->ShortName() << ", method=" << httpLink->method_ << ", path=" << httpLink->path_);
        return ACC_MALLOC_FAIL;
    }

    return it->second(*reqCtx.Get());
}

Result AccHttpServerDefault::HandleLinkBroken(const AccTcpLinkDefaultPtr &link)
{
    {
        std::lock_guard<std::mutex> g(httpLinksMutex_);
        httpLinks_.erase(link->Id());
    }
    return AccTcpServerDefault::HandleLinkBroken(link);
}

void AccHttpServerDefault::StopIdleCheck()
{
    idleCheckRunning_ = false;
    idleCheckCv_.notify_one();
    if (idleCheckThread_.joinable()) {
        idleCheckThread_.join();
    }
}

void AccHttpServerDefault::IdleCheckLoop()
{
    pthread_setname_np(pthread_self(), "HttpIdleChk");
    LOG_INFO("IdleCheckLoop thread started");
    while (idleCheckRunning_) {
        std::unique_lock<std::mutex> lock(cvMutex_);
        idleCheckCv_.wait_for(lock, std::chrono::milliseconds(HTTP_IDLE_CHECK_INTERVAL_MS));
        if (!idleCheckRunning_) {
            break;
        }
        std::vector<AccTcpLinkDefaultPtr> snapshot;
        {
            std::lock_guard<std::mutex> g(httpLinksMutex_);
            snapshot.reserve(httpLinks_.size());
            for (auto &[id, link] : httpLinks_) {
                snapshot.push_back(link);
            }
        }
        for (auto &link : snapshot) {
            (void)AccConvert<AccTcpLinkDefault, AccHttpLinkDefault>(link)->IsIdleExpired();
        }
        for (auto &link : snapshot) {
            if (link->HasPendingCleanup()) {
                (void)HandleLinkBroken(link);
            }
        }
    }
    LOG_INFO("IdleCheckLoop thread stopped");
}

bool AccHttpServerDefault::IsPathRegistered(const std::string &path) const
{
    for (const auto &entry : httpHandlers_) {
        auto colonPos = entry.first.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }
        std::string registeredPath = entry.first.substr(colonPos + 1);
        if (registeredPath == path || registeredPath == "*") {
            return true;
        }
    }
    return false;
}

} // namespace acc
} // namespace ock
