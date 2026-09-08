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

#ifndef MF_HYBRID_HOST_HCOM_SUBMIT_POOL_H
#define MF_HYBRID_HOST_HCOM_SUBMIT_POOL_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "hybm_logger.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
namespace transport {
namespace host {

// 常驻 worker 池：multi-link batch 把每个 ep 的分片任务分发给固定 worker 并发提交，
// 避免每轮新建线程。任务数必须等于 Start 时的 worker 数（调用方为不活跃 ep 补 no-op 任务）。
class HostSubmitPool {
public:
    HostSubmitPool() noexcept = default;
    ~HostSubmitPool()
    {
        Stop();
    }

    HostSubmitPool(const HostSubmitPool &) = delete;
    HostSubmitPool &operator=(const HostSubmitPool &) = delete;

    // 幂等启动 workerCount 个常驻线程（每线程绑定一个固定槽位）
    void Start(uint32_t workerCount)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ || stop_) {
            return;
        }
        started_ = true;
        for (uint32_t i = 0; i < workerCount; ++i) {
            threads_.emplace_back([this, i]() { WorkerLoop(i); });
        }
    }

    // 投递一整轮任务并阻塞等待全部完成；任一任务非 BM_OK 则整体返回 BM_ERROR。
    Result RunTasks(std::vector<std::function<Result()>> tasks)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!started_ || stop_) {
            BM_LOG_ERROR("HostSubmitPool not started");
            return BM_NOT_INITIALIZED;
        }
        if (tasks.size() != threads_.size()) {
            BM_LOG_ERROR("HostSubmitPool task count " << tasks.size() << " mismatch workers " << threads_.size());
            return BM_INVALID_PARAM;
        }
        roundTasks_ = std::move(tasks);
        finished_ = 0;
        roundFailed_ = false;
        hasRound_ = true;
        workersCv_.notify_all();
        doneCv_.wait(lock, [this]() { return finished_ >= roundTasks_.size(); });
        hasRound_ = false;
        return roundFailed_ ? BM_ERROR : BM_OK;
    }

private:
    void WorkerLoop(uint32_t idx)
    {
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            workersCv_.wait(lock, [this]() { return stop_ || hasRound_; });
            if (stop_) {
                return;
            }
            auto task = roundTasks_[idx];
            lock.unlock();

            Result ret = task();

            lock.lock();
            if (ret != BM_OK) {
                roundFailed_ = true;
            }
            ++finished_;
            if (finished_ >= roundTasks_.size()) {
                doneCv_.notify_one();
            }
        }
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            stop_ = true;
        }
        workersCv_.notify_all();
        for (auto &t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

private:
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable workersCv_;
    std::condition_variable doneCv_;
    std::vector<std::function<Result()>> roundTasks_;
    size_t finished_{0};
    bool hasRound_{false};
    bool roundFailed_{false};
    bool started_{false};
    bool stop_{false};
};
} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HOST_HCOM_SUBMIT_POOL_H
