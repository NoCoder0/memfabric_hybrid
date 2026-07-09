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

#ifndef LATENCY_STATS_WINDOW_SAMPLER_H
#define LATENCY_STATS_WINDOW_SAMPLER_H

/*
 * window_sampler.h - Time-windowed snapshot storage and background sampler thread.
 * WindowSampler<T> stores timestamped snapshots and supports diff/range queries within
 * a sliding window. PercentileWindowSampler merges in-window GlobalPercentile snapshots.
 * SamplerThread runs registered callbacks every ~1s (SAMPLE_SLEEP_STEPS x 100ms).
 */

#include <mutex>
#include <cstdint>
#include <ctime>
#include <deque>
#include <vector>
#include <thread>
#include <condition_variable>
#include <functional>
#include "tls_aggregator.h"
#include "percentile.h"

namespace ock {
namespace mf {

constexpr int64_t USEC_PER_SEC = 1000000;
constexpr int64_t NSEC_PER_USEC = 1000;
constexpr time_t DEFAULT_WINDOW_SIZE_SEC = 10;
constexpr size_t MIN_SAMPLES_FOR_DIFF = 2;
constexpr time_t PRUNE_BUFFER_SEC = 60;
constexpr int32_t SAMPLE_SLEEP_USEC = 100000;
constexpr int32_t SAMPLE_SLEEP_STEPS = 10;

/* Monotonic clock in microseconds; used for snapshot timestamps and window boundaries. */
inline int64_t NowUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * USEC_PER_SEC + ts.tv_nsec / NSEC_PER_USEC;
}

/* Data snapshot tagged with a microsecond timestamp for window queries. */
template<typename T>
struct TimedSample {
    T data;
    int64_t timeUs;

    TimedSample() : data(), timeUs(0) {}
};

/*
 * Time-windowed snapshot store. Stores TimedSample<T> history; old entries beyond
 * (windowSize_ + PRUNE_BUFFER_SEC) are auto-pruned. T must support Diff().
 */
template<typename T>
class WindowSampler {
public:
    explicit WindowSampler(time_t windowSizeSec)
        : windowSize_(windowSizeSec > 0 ? windowSizeSec : DEFAULT_WINDOW_SIZE_SEC)
    {}

    /* Store a new snapshot with the current timestamp. */
    void TakeSnapshot(const T &data)
    {
        TimedSample<T> snap;
        snap.data = data;
        snap.timeUs = NowUs();
        std::lock_guard<std::mutex> guard(mutex_);
        history_.push_back(snap);
        PruneOld();
    }

    /* Compute latest.Diff(oldest) within windowSec; false if < MIN_SAMPLES_FOR_DIFF snapshots or
     * no sample falls within the window. */
    bool GetWindowDiff(time_t windowSec, TimedSample<T> *result)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (history_.size() < MIN_SAMPLES_FOR_DIFF) {
            return false;
        }

        int64_t cutoffUs = NowUs() - static_cast<int64_t>(windowSec) * USEC_PER_SEC;
        size_t oldestIdx = 0;
        bool found = false;
        for (size_t i = 0; i < history_.size(); ++i) {
            if (history_[i].timeUs >= cutoffUs) {
                oldestIdx = i;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }

        TimedSample<T> &oldest = history_[oldestIdx];
        TimedSample<T> &latest = history_.back();

        result->data = latest.data.Diff(oldest.data);
        result->timeUs = latest.timeUs - oldest.timeUs;
        return true;
    }

    /* GetWindowDiff: use the default windowSize_ configured at construction. */
    bool GetWindowDiff(TimedSample<T> *result)
    {
        return GetWindowDiff(windowSize_, result);
    }

    /* GetLatest: return the most recent snapshot data, or false if empty. */
    bool GetLatest(T *result)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (history_.empty()) {
            return false;
        }
        *result = history_.back().data;
        return true;
    }

    /* Return all snapshots within windowSec (used for max-latency queries). */
    std::vector<TimedSample<T>> GetSamplesInWindow(time_t windowSec)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<TimedSample<T>> samples;
        int64_t cutoffUs = NowUs() - static_cast<int64_t>(windowSec) * USEC_PER_SEC;
        for (auto &s : history_) {
            if (s.timeUs >= cutoffUs) {
                samples.push_back(s);
            }
        }
        return samples;
    }

    time_t WindowSize() const
    {
        return windowSize_;
    }

private:
    /* Remove entries older than windowSize_ + PRUNE_BUFFER_SEC. */
    void PruneOld()
    {
        int64_t cutoffUs = NowUs() - static_cast<int64_t>(windowSize_ + PRUNE_BUFFER_SEC) * USEC_PER_SEC;
        while (!history_.empty() && history_.front().timeUs < cutoffUs) {
            history_.pop_front();
        }
    }

    time_t windowSize_;                  /* configured window duration in seconds */
    std::deque<TimedSample<T>> history_; /* chronological snapshot series */
    std::mutex mutex_;                   /* protects history_ */
};

/*
 * Percentile-specific window sampler. TakeSnapshot() calls FlushAllToGlobal() on the
 * aggregator; GetPercentile() merges in-window snapshots into a CombinedPercentile.
 */
class PercentileWindowSampler {
public:
    PercentileWindowSampler(TlsPercentileAggregator &agg, time_t windowSec)
        : agg_(agg), windowSec_(windowSec > 0 ? windowSec : DEFAULT_WINDOW_SIZE_SEC)
    {}

    /* Flush all TLS data to global, capture snapshot, and store in history. */
    void TakeSnapshot()
    {
        GlobalPercentile snap;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            snap = agg_.FlushAllToGlobal();
        }
        TimedSample<GlobalPercentile> timedSnap;
        timedSnap.data = snap;
        timedSnap.timeUs = NowUs();
        std::lock_guard<std::mutex> guard(historyMutex_);
        history_.push_back(timedSnap);
        PruneOld();
    }

    /* Query percentile by merging all in-window GlobalPercentile snapshots. */
    uint32_t GetPercentile(double ratio)
    {
        CombinedPercentile combined;
        int64_t cutoffUs = NowUs() - static_cast<int64_t>(windowSec_) * USEC_PER_SEC;
        std::lock_guard<std::mutex> guard(historyMutex_);
        for (auto &snap : history_) {
            if (snap.timeUs >= cutoffUs) {
                combined.MergeFromAny(snap.data);
            }
        }
        return combined.GetPercentile(ratio);
    }

    /* GetSamplesInWindow: return raw GlobalPercentile snapshots within the window. */
    std::vector<GlobalPercentile> GetSamplesInWindow()
    {
        std::lock_guard<std::mutex> guard(historyMutex_);
        std::vector<GlobalPercentile> result;
        int64_t cutoffUs = NowUs() - static_cast<int64_t>(windowSec_) * USEC_PER_SEC;
        for (auto &s : history_) {
            if (s.timeUs >= cutoffUs) {
                result.push_back(s.data);
            }
        }
        return result;
    }

private:
    void PruneOld()
    {
        int64_t cutoffUs = NowUs() - static_cast<int64_t>(windowSec_ + PRUNE_BUFFER_SEC) * USEC_PER_SEC;
        while (!history_.empty() && history_.front().timeUs < cutoffUs) {
            history_.pop_front();
        }
    }

    TlsPercentileAggregator &agg_;                      /* reference to the TLS percentile aggregator */
    time_t windowSec_;                                  /* window duration in seconds */
    std::deque<TimedSample<GlobalPercentile>> history_; /* snapshot history */
    std::mutex historyMutex_;                           /* protects history_ for read queries */
    std::mutex mutex_;                                  /* protects flush operation */
};

/*
 * Background thread running registered samplers every ~1s. Sleeps in SAMPLE_SLEEP_STEPS
 * x 100ms intervals, checking stop_ between sleeps for responsive shutdown.
 */
class SamplerThread {
public:
    SamplerThread() : running_(false), stop_(false) {}
    ~SamplerThread()
    {
        Stop();
    }

    /* Shared singleton — all LatencyRecorders register on the same thread. */
    static SamplerThread &GetSharedInstance()
    {
        static SamplerThread instance;
        return instance;
    }

    /* Start: launch the sampling thread. Mutex-protected; no-op if already running. */
    void Start()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_) {
            return;
        }
        stop_ = false;
        running_ = true;
        thread_ = std::thread([this]() { RunLoop(); });
    }

    /* Stop: signal the thread to stop and wait for it to join. */
    void Stop()
    {
        if (!running_) {
            return;
        }
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
        running_ = false;
    }

    /* AddSampler: register a sampler function to be called each iteration. */
    void AddSampler(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        samplers_.push_back(fn);
    }

private:
    /* Main loop: execute all samplers, then sleep in steps with stop_ checks. */
    void RunLoop()
    {
        while (!stop_) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                for (auto &fn : samplers_) {
                    fn();
                }
            }
            for (int i = 0; i < SAMPLE_SLEEP_STEPS && !stop_; ++i) {
                usleep(SAMPLE_SLEEP_USEC);
            }
        }
    }

    std::thread thread_;
    bool running_;
    bool stop_;
    std::mutex mutex_;                            /* protects samplers_ list */
    std::vector<std::function<void()>> samplers_; /* registered sampler functions */
};

} /* namespace mf */
} /* namespace ock */

#endif /* LATENCY_STATS_WINDOW_SAMPLER_H */
