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

#ifndef LATENCY_STATS_LATENCY_RECORDER_H
#define LATENCY_STATS_LATENCY_RECORDER_H

/*
 * latency_recorder.h - Top-level facade orchestrating TLS aggregators (avg/max/percentile),
 * window samplers, and a background sampling thread. Write path is lock-free via per-thread
 * TLS slots; read path runs on the sampler thread under mutex.
 */

#include <mutex>
#include <cstdint>
#include <string>
#include "tls_aggregator.h"
#include "window_sampler.h"

namespace ock {
namespace mf {

/* Percentile query ratios for common statistical thresholds. */
constexpr double PERCENTILE_50 = 0.50;
constexpr double PERCENTILE_80 = 0.80;
constexpr double PERCENTILE_90 = 0.90;
constexpr double PERCENTILE_99 = 0.99;
constexpr double PERCENTILE_999 = 0.999;
constexpr double PERCENTILE_9999 = 0.9999;

/* Top-level class for recording and querying latency statistics with a sliding time window. */
class LatencyRecorder {
public:
    /*
     * Constructor: specify the time window in seconds (default 10).
     *   Creates all aggregators and window samplers. Does not start sampling yet.
     */
    explicit LatencyRecorder(time_t windowSec = DEFAULT_WINDOW_SIZE_SEC)
        : windowSec_(windowSec), avgWindow_(windowSec), maxWindow_(windowSec), pctWindow_(pctAgg_, windowSec),
          samplerStarted_(false)
    {}

    /* Destructor: the shared sampler thread outlives each recorder; no explicit stop needed. */
    ~LatencyRecorder() {}

    /* Record a latency value (us). Lock-free write path: TLS avg accumulation, TLS max
       update, TLS percentile sampling with occasional flush to global. */
    void Record(int64_t latencyUs)
    {
        if (latencyUs < 0) {
            return;
        }
        avgAgg_.GetTlsPtr()->Add(latencyUs);
        int64_t *tlsMax = maxAgg_.GetTlsPtr();
        if (latencyUs > *tlsMax) {
            *tlsMax = latencyUs;
        }
        TlsPercentile *tlsPct = pctAgg_.GetTlsPtr();
        if (tlsPct->Full()) {
            pctAgg_.FlushTlsToGlobal(tlsPct);
        }
        tlsPct->AddValue64(latencyUs);
    }

    /* operator<<: stream-style interface for recording latency values. */
    LatencyRecorder &operator<<(int64_t latencyUs)
    {
        Record(latencyUs);
        return *this;
    }

    /* Start background sampler thread (no-op if already started). */
    void StartSampling()
    {
        if (samplerStarted_) {
            return;
        }
        auto &st = SamplerThread::GetSharedInstance();
        st.AddSampler([this]() {
            AvgSample snap = avgAgg_.SnapshotAll();
            avgWindow_.TakeSnapshot(snap);
        });
        st.AddSampler([this]() {
            MaxSample snap;
            snap.maxVal = maxAgg_.SnapshotAndReset();
            maxWindow_.TakeSnapshot(snap);
        });
        st.AddSampler([this]() { pctWindow_.TakeSnapshot(); });
        st.Start();
        samplerStarted_ = true;
    }

    /* Windowed average latency (us); falls back to cumulative avg if no window data. */
    int64_t Latency()
    {
        TimedSample<AvgSample> result;
        if (avgWindow_.GetWindowDiff(&result) && result.data.count > 0) {
            return result.data.Average();
        }
        return avgAgg_.SnapshotAll().Average();
    }

    /* Maximum latency within the window; falls back to cumulative max. */
    int64_t MaxLatency()
    {
        auto samples = maxWindow_.GetSamplesInWindow(windowSec_);
        int64_t result = 0;
        for (auto &s : samples) {
            if (s.data.maxVal > result) {
                result = s.data.maxVal;
            }
        }
        if (result > 0) {
            return result;
        }
        return maxAgg_.SnapshotAll();
    }

    /* Queries per second within the window: (count * 1e6) / timeUs. */
    double Qps()
    {
        TimedSample<AvgSample> result;
        if (avgWindow_.GetWindowDiff(&result) && result.data.count > 0 && result.timeUs > 0) {
            return static_cast<double>(result.data.count) * static_cast<double>(USEC_PER_SEC) /
                   static_cast<double>(result.timeUs);
        }
        return 0.0;
    }

    /*
     * Count: return the cumulative total number of recorded values.
     *   This is a cumulative count (not windowed), representing all values
     *   ever recorded since the LatencyRecorder was created.
     */
    int64_t Count()
    {
        return avgAgg_.SnapshotAll().count;
    }

    /* Windowed percentile at the given ratio (merges in-window snapshots). */
    int64_t Percentile(double ratio)
    {
        return pctWindow_.GetPercentile(ratio);
    }

    /* Convenience percentile accessors for common ratios. */
    int64_t P50()
    {
        return Percentile(PERCENTILE_50);
    }
    int64_t P80()
    {
        return Percentile(PERCENTILE_80);
    }
    int64_t P90()
    {
        return Percentile(PERCENTILE_90);
    }
    int64_t P99()
    {
        return Percentile(PERCENTILE_99);
    }
    int64_t P999()
    {
        return Percentile(PERCENTILE_999);
    }
    int64_t P9999()
    {
        return Percentile(PERCENTILE_9999);
    }

    time_t WindowSize() const
    {
        return windowSec_;
    }

    /* Human-readable summary: latency, max, p50-p999, qps, count. */
    std::string DebugString()
    {
        std::string s;
        s += "latency=" + std::to_string(Latency());
        s += " max=" + std::to_string(MaxLatency());
        s += " p50=" + std::to_string(P50());
        s += " p80=" + std::to_string(P80());
        s += " p90=" + std::to_string(P90());
        s += " p99=" + std::to_string(P99());
        s += " p999=" + std::to_string(P999());
        s += " qps=" + std::to_string(static_cast<int64_t>(Qps()));
        s += " count=" + std::to_string(Count());
        return s;
    }

private:
    time_t windowSec_; /* window duration in seconds */

    /* Three TLS aggregators: avg, max, percentile */
    TlsAvgAggregator avgAgg_;        /* sum/count per thread, merged for average */
    TlsMaxAggregator maxAgg_;        /* per-thread max, merged for global max */
    TlsPercentileAggregator pctAgg_; /* per-thread percentile sampling */

    /* Three window samplers storing timestamped snapshots */
    WindowSampler<AvgSample> avgWindow_; /* avg snapshot history for windowed avg/qps */
    WindowSampler<MaxSample> maxWindow_; /* max snapshot history for windowed max */
    PercentileWindowSampler pctWindow_;  /* percentile snapshot history for windowed p99+ */

    bool samplerStarted_; /* flag to prevent double-start */
};

} /* namespace mf */
} /* namespace ock */

#endif /* LATENCY_STATS_LATENCY_RECORDER_H */
