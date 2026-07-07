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

#ifndef LATENCY_STATS_TLS_AGGREGATOR_H
#define LATENCY_STATS_TLS_AGGREGATOR_H

/*
 * tls_aggregator.h - TLS aggregators (avg/max/percentile) for lock-free write paths.
 * Each thread gets a TLS-local accumulator via thread_local map; GetTlsPtr() returns a
 * per-thread pointer with zero overhead on subsequent calls. The sampler thread
 * collects all TLS data under mutex. Thread exit merges TLS data into global residuals.
 */

#include <mutex>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include "percentile.h"

namespace ock {
namespace mf {

/* AvgSample: sum/count pair; Average() = sum/count, Diff() = delta between snapshots. */
struct AvgSample {
    int64_t sum;
    int64_t count;
    AvgSample() : sum(0), count(0) {}
    void Add(int64_t val)
    {
        sum += val;
        ++count;
    }
    int64_t Average() const { return count > 0 ? sum / count : 0; }
    AvgSample Diff(const AvgSample& older) const
    {
        AvgSample r;
        r.sum = sum - older.sum;
        r.count = count - older.count;
        return r;
    }
};

/* MaxSample: max-value sample data, only records the maximum value seen. */
struct MaxSample {
    int64_t maxVal;
    MaxSample() : maxVal(0) {}
    void Update(int64_t val)
    {
        if (val > maxVal) {
            maxVal = val;
        }
    }
};

/* TLS average aggregator: lock-free write via per-thread AvgSample, snapshot under mutex. */
class TlsAvgAggregator {
public:
    TlsAvgAggregator() : globalSum_(0), globalCount_(0) {}
    ~TlsAvgAggregator()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto* ptr : tlsPtrs_) {
            delete ptr;
        }
    }

    /* Return per-thread AvgSample pointer (lazy allocation on first call). */
    AvgSample* GetTlsPtr()
    {
        struct Entry {
            AvgSample* data;
            TlsAvgAggregator* owner;
        };
        struct Registry {
            std::unordered_map<TlsAvgAggregator*, Entry> entries;
            ~Registry()
            {
                for (auto& [_, e] : entries) {
                    if (e.owner && e.data) {
                        e.owner->Reclaim(e.data);
                    }
                }
            }
        };
        static thread_local Registry registry;
        auto it = registry.entries.find(this);
        if (it != registry.entries.end()) {
            return it->second.data;
        }
        auto* data = new AvgSample();
        registry.entries[this] = {data, this};
        std::lock_guard<std::mutex> guard(mutex_);
        tlsPtrs_.push_back(data);
        return data;
    }

    /* SnapshotAll: read global residual + all TLS sums, return aggregate AvgSample. */
    AvgSample SnapshotAll()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        AvgSample total;
        total.sum = globalSum_;
        total.count = globalCount_;
        for (auto* ptr : tlsPtrs_) {
            total.sum += ptr->sum;
            total.count += ptr->count;
        }
        return total;
    }

    /* SnapshotAndReset: read all data and reset every accumulator to zero. */
    AvgSample SnapshotAndReset()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        AvgSample total;
        total.sum = globalSum_;
        total.count = globalCount_;
        for (auto* ptr : tlsPtrs_) {
            total.sum += ptr->sum;
            total.count += ptr->count;
            ptr->sum = 0;
            ptr->count = 0;
        }
        globalSum_ = 0;
        globalCount_ = 0;
        return total;
    }

    /* Reclaim: merge a dying thread's TLS data into global residuals and free memory. */
    void Reclaim(AvgSample* ptr)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        globalSum_ += ptr->sum;
        globalCount_ += ptr->count;
        auto it = std::find(tlsPtrs_.begin(), tlsPtrs_.end(), ptr);
        if (it != tlsPtrs_.end()) {
            tlsPtrs_.erase(it);
        }
        delete ptr;
    }

private:
    std::mutex mutex_;                 /* protects tlsPtrs_ and global residuals */
    std::vector<AvgSample*> tlsPtrs_; /* all active TLS data pointers */
    int64_t globalSum_;               /* residual sum from exited threads */
    int64_t globalCount_;             /* residual count from exited threads */
};

/* TLS max aggregator: same structure as TlsAvgAggregator, data is per-thread int64_t max. */
class TlsMaxAggregator {
public:
    TlsMaxAggregator() : globalMax_(0) {}
    ~TlsMaxAggregator()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto* ptr : tlsPtrs_) {
            delete ptr;
        }
    }

    int64_t* GetTlsPtr()
    {
        struct Entry {
            int64_t* data;
            TlsMaxAggregator* owner;
        };
        struct Registry {
            std::unordered_map<TlsMaxAggregator*, Entry> entries;
            ~Registry()
            {
                for (auto& [_, e] : entries) {
                    if (e.owner && e.data) {
                        e.owner->Reclaim(e.data);
                    }
                }
            }
        };
        static thread_local Registry registry;
        auto it = registry.entries.find(this);
        if (it != registry.entries.end()) {
            return it->second.data;
        }
        auto* data = new int64_t(0);
        registry.entries[this] = {data, this};
        std::lock_guard<std::mutex> guard(mutex_);
        tlsPtrs_.push_back(data);
        return data;
    }

    int64_t SnapshotAll()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        int64_t totalMax = globalMax_;
        for (auto* ptr : tlsPtrs_) {
            if (*ptr > totalMax) {
                totalMax = *ptr;
            }
        }
        return totalMax;
    }

    int64_t SnapshotAndReset()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        int64_t totalMax = globalMax_;
        for (auto* ptr : tlsPtrs_) {
            if (*ptr > totalMax) {
                totalMax = *ptr;
            }
            *ptr = 0;
        }
        globalMax_ = 0;
        return totalMax;
    }

    void Reclaim(int64_t* ptr)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (*ptr > globalMax_) {
            globalMax_ = *ptr;
        }
        auto it = std::find(tlsPtrs_.begin(), tlsPtrs_.end(), ptr);
        if (it != tlsPtrs_.end()) {
            tlsPtrs_.erase(it);
        }
        delete ptr;
    }

private:
    std::mutex mutex_;
    std::vector<int64_t*> tlsPtrs_;
    int64_t globalMax_;               /* residual max from exited threads */
};

/* TLS percentile aggregator: lock-free write, flush to global when TLS buckets are full. */
class TlsPercentileAggregator {
public:
    TlsPercentileAggregator() {}
    ~TlsPercentileAggregator()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto* ptr : tlsPtrs_) {
            delete ptr;
        }
    }

    /* GetTlsPtr: obtain the current thread's TlsPercentile pointer. */
    TlsPercentile* GetTlsPtr()
    {
        struct Entry {
            TlsPercentile* data;
            TlsPercentileAggregator* owner;
        };
        struct Registry {
            std::unordered_map<TlsPercentileAggregator*, Entry> entries;
            ~Registry()
            {
                for (auto& [_, e] : entries) {
                    if (e.owner && e.data) {
                        e.owner->Reclaim(e.data);
                    }
                }
            }
        };
        static thread_local Registry registry;
        auto it = registry.entries.find(this);
        if (it != registry.entries.end()) {
            return it->second.data;
        }
        auto* data = new TlsPercentile();
        registry.entries[this] = {data, this};
        std::lock_guard<std::mutex> guard(mutex_);
        tlsPtrs_.push_back(data);
        return data;
    }

    /* Merge TLS percentile data into global bucket and clear TLS (write path). */
    void FlushTlsToGlobal(TlsPercentile* tls)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        global_.MergeFromTls(*tls);
        tls->Clear();
    }

    /* Merge global bucket only (safe when TLS pointers may be dangling). */
    GlobalPercentile MergeGlobalOnly()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        GlobalPercentile result;
        result.MergeFromSame(global_);
        return result;
    }

    /* Flush all TLS percentile data into global and return merged snapshot. */
    GlobalPercentile FlushAllToGlobal()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto* ptr : tlsPtrs_) {
            global_.MergeFromTls(*ptr);
            ptr->Clear();
        }
        GlobalPercentile result;
        result.MergeFromSame(global_);
        global_.Clear();
        return result;
    }

    /* Reclaim: merge a dying thread's TLS percentile data into global bucket. */
    void Reclaim(TlsPercentile* ptr)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        global_.MergeFromTls(*ptr);
        auto it = std::find(tlsPtrs_.begin(), tlsPtrs_.end(), ptr);
        if (it != tlsPtrs_.end()) {
            tlsPtrs_.erase(it);
        }
        delete ptr;
    }

private:
    std::mutex mutex_;
    std::vector<TlsPercentile*> tlsPtrs_; /* active TLS percentile pointer list */
    GlobalPercentile global_;               /* global merged bucket (background aggregation result) */
};

} /* namespace mf */
} /* namespace ock */

#endif /* LATENCY_STATS_TLS_AGGREGATOR_H */
