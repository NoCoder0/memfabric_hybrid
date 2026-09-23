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
#ifndef MEMFABRIC_ACC_OFFLOAD_PARALLEL_COPY_POOL_H
#define MEMFABRIC_ACC_OFFLOAD_PARALLEL_COPY_POOL_H
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

namespace ock::offload {
// Copied from hostrdma_batch_bench.cpp: preserve worker partition, spin dispatch and affinity.
inline bool ParseCpuNumber(const std::string &text, int &cpu)
{
    try {
        size_t parsed = 0;
        const unsigned long value = std::stoul(text, &parsed);
        if (parsed != text.size() || value >= CPU_SETSIZE) {
            return false;
        }
        cpu = static_cast<int>(value);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

inline bool AppendCpuRange(const std::string &token, std::vector<int> &cpus)
{
    const size_t dash = token.find('-');
    int begin = 0;
    int end = 0;
    if (dash == std::string::npos) {
        if (!ParseCpuNumber(token, begin)) {
            return false;
        }
        end = begin;
    } else if (token.find('-', dash + 1) != std::string::npos ||
               !ParseCpuNumber(token.substr(0, dash), begin) || !ParseCpuNumber(token.substr(dash + 1), end) ||
               begin > end) {
        return false;
    }
    for (int cpu = begin; cpu <= end; ++cpu) {
        if (std::find(cpus.begin(), cpus.end(), cpu) != cpus.end()) {
            return false;
        }
        cpus.push_back(cpu);
    }
    return true;
}

inline bool ParseCpuList(const std::string &spec, std::vector<int> &cpus)
{
    size_t begin = 0;
    while (begin < spec.size()) {
        const size_t comma = spec.find(',', begin);
        const std::string token = spec.substr(begin, comma == std::string::npos ? comma : comma - begin);
        if (token.empty() || !AppendCpuRange(token, cpus)) {
            fprintf(stderr, "[ERROR] invalid --gather-cpus=%s\n", spec.c_str());
            return false;
        }
        if (comma == std::string::npos) {
            return true;
        }
        begin = comma + 1;
    }
    fprintf(stderr, "[ERROR] invalid --gather-cpus=%s\n", spec.c_str());
    return false;
}


inline void CpuRelax()
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield" : : : "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" : : : "memory");
#else
    std::this_thread::yield();
#endif
}

inline std::vector<int> GetGatherCpus(uint32_t threadCount)
{
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
        fprintf(stderr, "[ERROR] sched_getaffinity failed: %s\n", std::strerror(errno));
        return {};
    }
    std::vector<int> cpus;
    const int callerCpu = sched_getcpu();
    if (callerCpu >= 0 && CPU_ISSET(callerCpu, &affinity)) {
        cpus.push_back(callerCpu);
    }
    for (int cpu = 0; cpu < CPU_SETSIZE && cpus.size() < threadCount; ++cpu) {
        if (CPU_ISSET(cpu, &affinity) && cpu != callerCpu) {
            cpus.push_back(cpu);
        }
    }
    return cpus;
}


inline void PinGatherWorker(uint32_t workerIndex, int cpu)
{
    if (cpu < 0) {
        return;
    }
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    const int ret = pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] gather worker %u failed to bind CPU %d: %s\n", workerIndex, cpu,
                std::strerror(ret));
    }
}

class ParallelCopyPool {
public:
    explicit ParallelCopyPool(uint32_t threadCount, const std::vector<int> &workerCpus = {})
        : threadCount_(threadCount), workerCpus_(workerCpus), done_(threadCount + 1U)
    {
        workers_.reserve(threadCount_);
        for (uint32_t index = 0; index < threadCount_; ++index) {
            const int cpu = workerCpus_.empty() ? -1 : workerCpus_[index];
            workers_.emplace_back(&ParallelCopyPool::WorkerLoop, this, index, cpu);
        }
    }

    ~ParallelCopyPool()
    {
        stopping_.store(true, std::memory_order_release);
        generation_.fetch_add(1U, std::memory_order_release);
        for (auto &worker : workers_) {
            worker.join();
        }
    }

    void GatherAddresses(const uint64_t *sourceAddresses, uint32_t count, void *contiguous, uint64_t bytes)
    {
        sourceAddresses_ = sourceAddresses;
        destinations_ = nullptr;
        contiguous_ = static_cast<uint8_t *>(contiguous);
        segmentBytes_ = bytes;
        count_ = count;
        Dispatch(TaskType::GATHER);
    }

    void Scatter(const void *contiguous, const std::vector<void *> &destinations, uint64_t bytes)
    {
        sourceAddresses_ = nullptr;
        destinations_ = &destinations;
        contiguous_ = const_cast<uint8_t *>(static_cast<const uint8_t *>(contiguous));
        segmentBytes_ = bytes;
        count_ = static_cast<uint32_t>(destinations.size());
        Dispatch(TaskType::SCATTER);
    }

private:
    enum class TaskType : uint8_t { GATHER, SCATTER };

    void Dispatch(TaskType task)
    {
        task_ = task;
        done_.store(0U, std::memory_order_relaxed);
        generation_.fetch_add(1U, std::memory_order_release);
        while (done_.load(std::memory_order_acquire) != threadCount_ + 1U) {
            CpuRelax();
        }
    }

    void CopyPartition(uint32_t workerIndex)
    {
        const uint32_t begin = static_cast<uint32_t>(static_cast<uint64_t>(count_) * workerIndex / threadCount_);
        const uint32_t end =
            static_cast<uint32_t>(static_cast<uint64_t>(count_) * (workerIndex + 1U) / threadCount_);
        for (uint32_t index = begin; index < end; ++index) {
            auto *linear = contiguous_ + static_cast<uint64_t>(index) * segmentBytes_;
            if (task_ == TaskType::GATHER) {
                const void *source = reinterpret_cast<const void *>(sourceAddresses_[index]);
                std::memcpy(linear, source, segmentBytes_);
            } else {
                std::memcpy((*destinations_)[index], linear, segmentBytes_);
            }
        }
    }

    void WorkerLoop(uint32_t workerIndex, int cpu)
    {
        PinGatherWorker(workerIndex, cpu);
        uint64_t observedGeneration = 0;
        while (!stopping_.load(std::memory_order_acquire)) {
            auto generation = generation_.load(std::memory_order_acquire);
            while (generation == observedGeneration && !stopping_.load(std::memory_order_relaxed)) {
                CpuRelax();
                generation = generation_.load(std::memory_order_acquire);
            }
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            observedGeneration = generation;
            CopyPartition(workerIndex);
            const uint32_t finished = done_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            if (finished == threadCount_) {
                done_.store(threadCount_ + 1U, std::memory_order_release);
            }
        }
    }

    uint32_t threadCount_;
    std::vector<int> workerCpus_;
    std::vector<std::thread> workers_;
    std::atomic<uint64_t> generation_{0U};
    std::atomic<uint32_t> done_;
    std::atomic<bool> stopping_{false};
    TaskType task_{TaskType::GATHER};
    const uint64_t *sourceAddresses_{nullptr};
    const std::vector<void *> *destinations_{nullptr};
    uint8_t *contiguous_{nullptr};
    uint64_t segmentBytes_{0};
    uint32_t count_{0};
};


} // namespace ock::offload
#endif
