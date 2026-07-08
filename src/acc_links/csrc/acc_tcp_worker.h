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
#ifndef ACC_LINKS_ACC_TCP_WORKER_H
#define ACC_LINKS_ACC_TCP_WORKER_H

#include <utility>

#include "acc_tcp_common.h"
#include "acc_tcp_link.h"
#include "acc_tcp_link_default.h"
#include "acc_tcp_request_context.h"
#include "acc_tcp_shared_buf.h"

namespace ock {
namespace acc {
using LinkBrokenHandlerInner = std::function<int32_t(const AccTcpLinkDefaultPtr &link)>;

/*
 * Worker is for epoll event from connection sockets
 */
class AccTcpWorker : public AccReferable {
public:
    explicit AccTcpWorker(AccTcpWorkerOptions options) : options_(std::move(options)) {}
    ~AccTcpWorker() override
    {
        Stop();
    }

    Result Start();
    void Stop(bool afterFork = false);

    Result AddLink(const AccTcpLinkDefaultPtr &link, uint32_t events) noexcept;
    Result ModifyLink(const AccTcpLinkDefaultPtr &link, uint32_t events) noexcept;
    Result RemoveLink(const AccTcpLinkDefaultPtr &link) noexcept;

    void RegisterNewRequestHandler(const AccNewReqHandler &h);
    void RegisterRequestSentHandler(const AccReqSentHandler &h);
    void RegisterLinkBrokenHandler(const LinkBrokenHandlerInner &h);

private:
    void SetPropertiesForThread();
    void RunInThread(std::atomic<bool> *started);
    Result ValidateOptions();
    void StopInner(bool afterFork);
    Result ProcessEvent(struct epoll_event &event) noexcept;
    Result ProcessPollIn(AccTcpLinkDefault *link) noexcept;
    Result ProcessPollOut(AccTcpLinkDefault *link) noexcept;
    Result ProcessPollWrNorm(AccTcpLinkDefault *link) noexcept;
    void ProcessBufferedRequest(AccTcpLinkDefault *link) noexcept;

private:
    int epollFD_ = -1;      /* epoll fd */
    bool needStop_ = false; /* if the worker need to be stopped */
    AccNewReqHandler newRequestHandle_ = nullptr;
    AccReqSentHandler requestSentHandle_ = nullptr;
    LinkBrokenHandlerInner linkBrokenHandle_ = nullptr;

    /* non-hot variables */
    std::mutex mutex_;
    AccTcpWorkerOptions options_;      /* worker options */
    std::atomic<bool> started_{false}; /* if the worker started */
    std::thread epollThread_;          /* thread */
    std::atomic<bool> threadStarted_{false};
};
using AccTcpWorkerPtr = AccRef<AccTcpWorker>;

inline void AccTcpWorker::RegisterNewRequestHandler(const AccNewReqHandler &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(newRequestHandle_ == nullptr);
    newRequestHandle_ = h;
}

inline void AccTcpWorker::RegisterRequestSentHandler(const AccReqSentHandler &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(requestSentHandle_ == nullptr);
    requestSentHandle_ = h;
}

inline void AccTcpWorker::RegisterLinkBrokenHandler(const LinkBrokenHandlerInner &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(linkBrokenHandle_ == nullptr);
    linkBrokenHandle_ = h;
}
} // namespace acc
} // namespace ock

#endif // ACC_LINKS_ACC_TCP_WORKER_H
