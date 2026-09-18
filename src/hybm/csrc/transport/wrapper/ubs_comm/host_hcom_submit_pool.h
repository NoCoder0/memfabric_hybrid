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

#include <pthread.h>
#include <sched.h>
#include <algorithm>
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

// 常驻 worker 池：multi-link / multi-rail batch 把每条 ep(rail) 的分片任务分发给固定 worker 并发提交，
// 避免每轮新建线程。任务数不必等于 worker 数：多出的 worker 补空转任务，任务数超过 worker 数才报错。
class HostSubmitPool {
public:
    HostSubmitPool() noexcept = default;
    ~HostSubmitPool()
    {
        Stop();
    }

    HostSubmitPool(const HostSubmitPool &) = delete;
    HostSubmitPool &operator=(const HostSubmitPool &) = delete;

    /* 幂等启动 workerCount 个常驻线程（每线程绑定一个固定槽位）。
       cpuBegin/cpuCount：可选绑核区段，第 i 个 worker 绑到 cpuBegin+i；cpuCount==0 表示不绑，
       落核交给内核调度器。提交是 CPU 密集的短任务，不绑核时可能被唤醒后落到忙轮询 worker 的核上
       互相抢占（提交变慢、完成回收也被推迟），所以多 rail 场景建议显式指定。 */
    void Start(uint32_t workerCount, uint32_t cpuBegin = 0, uint32_t cpuCount = 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ || stop_) {
            return;
        }
        started_ = true;
        for (uint32_t i = 0; i < workerCount; ++i) {
            const bool needBind = (i < cpuCount);
            const uint32_t cpu = cpuBegin + i;
            threads_.emplace_back([this, i, needBind, cpu]() {
                if (needBind) {
                    PinCurrentThread(cpu);
                }
                WorkerLoop(i);
            });
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
        if (tasks.size() > threads_.size()) {
            BM_LOG_ERROR("HostSubmitPool task count " << tasks.size() << " exceeds workers " << threads_.size());
            return BM_INVALID_PARAM;
        }
        /* 允许任务数少于 worker 数：不足的槽位补空转任务，worker 仍然一人一格。
           这样调用方不必把"任务条数"和"池子大小"绑死（多 rail / 多 ep 的条目数会随配置变）。 */
        tasks.resize(threads_.size(), []() { return BM_OK; });
        roundTasks_ = std::move(tasks);
        roundDone_.assign(threads_.size(), false);
        roundFailed_ = false;
        ++roundId_; // 发布新一轮：worker 认领该轮次后不会再重复取用同一轮任务
        workersCv_.notify_all();
        doneCv_.wait(lock, [this]() { return stop_ || AllRoundDone(); });
        return roundFailed_ ? BM_ERROR : BM_OK;
    }

private:
    // 本轮是否所有 worker 都已上报完成
    bool AllRoundDone() const
    {
        return std::all_of(roundDone_.begin(), roundDone_.end(), [](bool done) { return done; });
    }

    /* 把当前线程钉到指定核。失败只打 WARN 不返回错误：绑核是性能手段，不该让服务起不来。 */
    static void PinCurrentThread(uint32_t cpu)
    {
        cpu_set_t cpuSet;
        CPU_ZERO(&cpuSet);
        CPU_SET(static_cast<int>(cpu), &cpuSet);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpuSet), &cpuSet) != 0) {
            BM_LOG_WARN("Unable to bind submit worker to cpu " << cpu);
        }
    }

    void WorkerLoop(uint32_t idx)
    {
        uint64_t lastRound = 0;
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            workersCv_.wait(lock, [this, lastRound]() { return stop_ || roundId_ != lastRound; });
            if (stop_) {
                return;
            }
            lastRound = roundId_; // 认领本轮，保证同一轮任务只会被本 worker 取用一次
            auto task = roundTasks_[idx];
            lock.unlock();

            Result ret = BM_OK;
            try {
                ret = task();
            } catch (...) {
                ret = BM_ERROR; // 异常也必须上报，否则 RunTasks 会一直等不到该 worker
            }

            lock.lock();
            if (roundId_ == lastRound) { // 轮次已推进说明本轮早已判定完成，丢弃迟到上报以免污染下一轮
                if (ret != BM_OK) {
                    roundFailed_ = true;
                }
                roundDone_[idx] = true; // 每个 worker 只写自己那一位，重复执行无法替他人凑数
                if (AllRoundDone()) {
                    doneCv_.notify_all();
                }
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
        doneCv_.notify_all(); // 唤醒可能仍阻塞在 RunTasks 的调用线程
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
    std::vector<bool> roundDone_; // 每个 worker 一位：本轮是否已上报完成
    uint64_t roundId_{0};         // 轮次代号：worker 本地记录已认领的轮次
    bool roundFailed_{false};
    bool started_{false};
    bool stop_{false};
};
} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HOST_HCOM_SUBMIT_POOL_H
