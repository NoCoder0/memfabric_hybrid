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

#ifndef MF_SMEM_ASYNC_CALLBACK_GUARD_H
#define MF_SMEM_ASYNC_CALLBACK_GUARD_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include "mf_env_define.h"
#include "mf_env_util.h"

namespace ock {
namespace smem {

class SmemAsyncCallbackGuard {
public:
    class Scope {
    public:
        explicit Scope(SmemAsyncCallbackGuard &guard) noexcept : guard_{guard} {}
        ~Scope()
        {
            guard_.Leave();
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        Scope(Scope &&) = delete;
        Scope &operator=(Scope &&) = delete;

    private:
        SmemAsyncCallbackGuard &guard_;
    };

    /* 回调入口调用：返回 true 表示可以安全使用 this，false 表示对象已关闭 */
    bool TryEnter() noexcept
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (closed_) {
            return false;
        }
        ++inflight_;
        return true;
    }

    /* 关闸并等待在飞回调退出；窗口内未退出返回 false。重复 detach 直接返回首次结果，不再重复等待 */
    bool Close(uint64_t timeoutMs) noexcept
    {
        std::unique_lock<std::mutex> lock{mutex_};
        if (detached_) {
            return drained_;
        }
        closed_ = true;
        drained_ = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return inflight_ == 0; });
        detached_ = true;
        return drained_;
    }

    /* 注册回调时放行：detach 过的对象再次被注册时，从这里重新开始计数 */
    void Open() noexcept
    {
        std::lock_guard<std::mutex> lock{mutex_};
        closed_ = false;
        detached_ = false;
        drained_ = false;
    }

private:
    void Leave() noexcept
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (inflight_ == 0) {
            return;
        }
        if (--inflight_ == 0) {
            cv_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_{false};
    bool detached_{false};
    bool drained_{false};
    uint32_t inflight_{0};
};

/* 在飞回调的耗时上限由建链时的 QP 就绪等待决定（MF_QP_READY_CHECK_TIMEOUT_BASE，默认 100s），
   排空窗口取该上限再加一段余量，保证超时只可能来自回调本身卡死，而不是窗口比回调短 */
constexpr uint64_t SMEM_QP_READY_TIMEOUT_DEFAULT_SEC = 100;
constexpr uint64_t SMEM_ASYNC_CALLBACK_DRAIN_MARGIN_MS = 30000;

inline uint64_t SmemGetCallbackDrainTimeoutMs() noexcept
{
    const uint64_t qpReadyTimeoutSec = mf::MfEnvUtil::GetOptionalUintOrDefault(mf::env::MF_QP_READY_CHECK_TIMEOUT_BASE,
                                                                               SMEM_QP_READY_TIMEOUT_DEFAULT_SEC);
    return qpReadyTimeoutSec * 1000ULL + SMEM_ASYNC_CALLBACK_DRAIN_MARGIN_MS;
}

} // namespace smem
} // namespace ock

#endif // MF_SMEM_ASYNC_CALLBACK_GUARD_H
