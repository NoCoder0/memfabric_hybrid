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

#ifndef LATENCY_STATS_PERCENTILE_H
#define LATENCY_STATS_PERCENTILE_H

/*
 * percentile.h - Log2-bucketed reservoir sampling for percentile estimation.
 * Values are partitioned into 32 log2 buckets; each bucket retains a fixed-capacity
 * reservoir sample. Merge uses weighted random downsampling to preserve per-source
 * proportional representation.
 */

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace ock {
namespace mf {

constexpr size_t NUM_LOG_BUCKETS = 32;
constexpr size_t TLS_SAMPLE_SIZE = 30;
constexpr size_t GLOBAL_SAMPLE_SIZE = 254;
constexpr size_t COMBINED_SAMPLE_SIZE = 1022;
constexpr size_t BUCKET_0_UPPER_BOUND = 3;

inline uint32_t ThreadRand()
{
    static thread_local std::mt19937 gen(std::random_device{}());
    return static_cast<uint32_t>(gen());
}

inline size_t Log2BucketIndex(uint32_t x)
{
    if (x <= BUCKET_0_UPPER_BOUND) {
        return 0;
    }
    if (x >= (1u << (NUM_LOG_BUCKETS - 1))) {
        return NUM_LOG_BUCKETS - 1;
    }
    return static_cast<size_t>(30u - static_cast<uint32_t>(__builtin_clz(x)));
}

/* Return 1 with probability a/b; used for reservoir sampling replacement decisions. */
inline uint32_t RoundOfExpectation(uint32_t a, uint32_t b)
{
    if (b == 0) {
        return 0;
    }
    return a / b + (static_cast<uint32_t>(ThreadRand()) % b < a % b ? 1u : 0u);
}

/* Fixed-capacity reservoir sampling bucket with weighted merge downsampling. */
template<size_t CAP>
class SampleBucket {
public:
    SampleBucket() : numAdded_(0), numStored_(0), sorted_(false) {}

    /* Reservoir-sample a value: direct insert when not full, random replace when full. */
    bool Add(uint32_t val)
    {
        ++numAdded_;
        if (numStored_ < CAP) {
            samples_[numStored_++] = val;
            sorted_ = false;
            return true;
        }
        if (RoundOfExpectation(1, numAdded_)) {
            size_t pos = static_cast<size_t>(ThreadRand()) % numStored_;
            samples_[pos] = val;
            sorted_ = false;
        }
        return false;
    }

    /* Return value at rank (0 = smallest); lazy-sorts and caches sorted state. */
    uint32_t GetAt(size_t rank)
    {
        if (numStored_ == 0) {
            return 0;
        }
        if (rank >= numStored_) {
            rank = numStored_ - 1;
        }
        if (!sorted_) {
            std::sort(samples_, samples_ + numStored_);
            sorted_ = true;
        }
        return samples_[rank];
    }

    /* Merge same-capacity bucket via weighted downsampling (global-to-global). */
    void MergeFrom(const SampleBucket<CAP> &rhs)
    {
        if (rhs.numAdded_ == 0) {
            return;
        }
        if (numAdded_ == 0) {
            std::copy(rhs.samples_, rhs.samples_ + rhs.numStored_, samples_);
            numStored_ = rhs.numStored_;
            numAdded_ = rhs.numAdded_;
            sorted_ = false;
            return;
        }
        if (numAdded_ + rhs.numAdded_ <= CAP) {
            std::copy(rhs.samples_, rhs.samples_ + rhs.numStored_, samples_ + numStored_);
            numStored_ += rhs.numStored_;
            numAdded_ += rhs.numAdded_;
            sorted_ = false;
            return;
        }
        uint32_t total = numAdded_ + rhs.numAdded_;
        uint32_t keepSelf = RoundOfExpectation(numAdded_ * CAP, total);
        if (keepSelf > numStored_) {
            keepSelf = numStored_;
        }

        while (numStored_ > keepSelf) {
            size_t pos = static_cast<size_t>(ThreadRand()) % numStored_;
            samples_[pos] = samples_[numStored_ - 1];
            --numStored_;
        }

        uint32_t rhsNumStored = rhs.numStored_;
        if (rhsNumStored > CAP) {
            rhsNumStored = CAP;
        }
        uint32_t keepRhs = CAP - keepSelf;
        if (keepRhs > rhsNumStored) {
            keepRhs = rhsNumStored;
        }
        std::vector<uint32_t> rhsTmpVec(CAP);
        uint32_t *rhsTmp = rhsTmpVec.data();
        std::copy(rhs.samples_, rhs.samples_ + rhsNumStored, rhsTmp);
        for (uint32_t i = 0; i < keepRhs; ++i) {
            size_t idx = static_cast<size_t>(ThreadRand()) % (rhsNumStored - i);
            if (numStored_ < CAP) {
                samples_[numStored_++] = rhsTmp[idx];
            } else {
                samples_[static_cast<size_t>(ThreadRand()) % CAP] = rhsTmp[idx];
            }
            rhsTmp[idx] = rhsTmp[rhsNumStored - i - 1];
        }
        numAdded_ = total;
        sorted_ = false;
    }

    /* Merge different-capacity bucket via cross-CAP weighted downsampling (TLS-to-global). */
    template<size_t OTHER_CAP>
    void MergeFromDifferent(const SampleBucket<OTHER_CAP> &rhs)
    {
        if (rhs.NumAdded() == 0) {
            return;
        }
        if (numAdded_ == 0) {
            uint32_t rhsNumStored = rhs.NumStored();
            if (rhsNumStored > OTHER_CAP) {
                rhsNumStored = OTHER_CAP;
            }
            size_t toCopy = std::min(static_cast<size_t>(rhsNumStored), static_cast<size_t>(CAP));
            std::vector<uint32_t> rhsTmpVec(OTHER_CAP);
            uint32_t *rhsTmp = rhsTmpVec.data();
            for (uint32_t i = 0; i < rhsNumStored; ++i) {
                rhsTmp[i] = rhs.SampleAt(i);
            }
            for (size_t i = 0; i < toCopy; ++i) {
                samples_[i] = rhsTmp[i];
            }
            numStored_ = static_cast<uint32_t>(toCopy);
            numAdded_ = rhs.NumAdded();
            sorted_ = false;
            return;
        }
        uint32_t total = numAdded_ + rhs.NumAdded();
        uint32_t keepSelf = RoundOfExpectation(numAdded_ * CAP, total);
        if (keepSelf > numStored_) {
            keepSelf = numStored_;
        }
        while (numStored_ > keepSelf) {
            size_t pos = static_cast<size_t>(ThreadRand()) % numStored_;
            samples_[pos] = samples_[numStored_ - 1];
            --numStored_;
        }
        uint32_t keepRhs = CAP - keepSelf;
        if (keepRhs > rhs.NumStored()) {
            keepRhs = rhs.NumStored();
        }
        uint32_t rhsNumStored = rhs.NumStored();
        if (rhsNumStored > OTHER_CAP) {
            rhsNumStored = OTHER_CAP;
        }
        std::vector<uint32_t> rhsTmpVec(OTHER_CAP);
        uint32_t *rhsTmp = rhsTmpVec.data();
        for (uint32_t i = 0; i < rhsNumStored; ++i) {
            rhsTmp[i] = rhs.SampleAt(i);
        }
        for (uint32_t i = 0; i < keepRhs; ++i) {
            if (i >= rhsNumStored) {
                break;
            }
            size_t idx = static_cast<size_t>(ThreadRand()) % (rhsNumStored - i);
            if (numStored_ < CAP) {
                samples_[numStored_++] = rhsTmp[idx];
            } else {
                samples_[static_cast<size_t>(ThreadRand()) % CAP] = rhsTmp[idx];
            }
            rhsTmp[idx] = rhsTmp[rhsNumStored - i - 1];
        }
        numAdded_ = total;
        sorted_ = false;
    }

    uint32_t NumAdded() const
    {
        return numAdded_;
    }
    uint32_t NumStored() const
    {
        return numStored_;
    }
    uint32_t SampleAt(size_t i) const
    {
        return samples_[i];
    }
    bool Empty() const
    {
        return numStored_ == 0;
    }
    void Clear()
    {
        numAdded_ = 0;
        numStored_ = 0;
        sorted_ = false;
    }
    bool Full() const
    {
        return numStored_ >= CAP;
    }

private:
    uint32_t numAdded_;     /* total samples ever added (including replaced ones) */
    uint32_t numStored_;    /* currently stored samples (<= CAP) */
    bool sorted_;           /* cached sorted state to avoid redundant sorting */
    uint32_t samples_[CAP]; /* fixed-capacity sample array */
};

/*
 * Log2-bucketed percentile sampler. Query walks buckets in log2 order to locate
 * the target rank, then maps proportionally to stored samples within that bucket.
 * Variants: TlsPercentile (TLS_SAMPLE_SIZE), GlobalPercentile (GLOBAL_SAMPLE_SIZE),
 * CombinedPercentile (COMBINED_SAMPLE_SIZE).
 */
template<size_t BUCKET_CAP>
class PercentileSamples {
public:
    PercentileSamples() : totalAdded_(0)
    {
        std::fill_n(buckets_, NUM_LOG_BUCKETS, nullptr);
    }

    ~PercentileSamples()
    {
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (buckets_[i]) {
                delete buckets_[i];
            }
        }
    }

    PercentileSamples(const PercentileSamples &rhs) : totalAdded_(rhs.totalAdded_)
    {
        std::fill_n(buckets_, NUM_LOG_BUCKETS, nullptr);
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (rhs.buckets_[i] && !rhs.buckets_[i]->Empty()) {
                buckets_[i] = new SampleBucket<BUCKET_CAP>(*rhs.buckets_[i]);
            }
        }
    }

    PercentileSamples &operator=(const PercentileSamples &rhs)
    {
        if (this == &rhs) {
            return *this;
        }
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (buckets_[i]) {
                delete buckets_[i];
                buckets_[i] = nullptr;
            }
        }
        totalAdded_ = rhs.totalAdded_;
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (rhs.buckets_[i] && !rhs.buckets_[i]->Empty()) {
                buckets_[i] = new SampleBucket<BUCKET_CAP>(*rhs.buckets_[i]);
            }
        }
        return *this;
    }

    /* AddValue: add a uint32 value, auto-partitioned by log2 bucket index */
    void AddValue(uint32_t val)
    {
        size_t idx = Log2BucketIndex(val);
        GetBucket(idx).Add(val);
        ++totalAdded_;
    }

    /* AddValue64: add an int64 value; negative values are ignored,
       values exceeding uint32 range are clamped to uint32_max */
    void AddValue64(int64_t val)
    {
        if (val < 0) {
            return;
        }
        uint32_t uval;
        if (val > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
            uval = std::numeric_limits<uint32_t>::max();
        } else {
            uval = static_cast<uint32_t>(val);
        }
        AddValue(uval);
    }

    /* MergeFromTls: merge from a TLS-capacity sampler (TLS -> global) */
    void MergeFromTls(const PercentileSamples<TLS_SAMPLE_SIZE> &rhs)
    {
        totalAdded_ += rhs.TotalAdded();
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (rhs.HasBucket(i) && !rhs.BucketAt(i).Empty()) {
                GetBucket(i).MergeFromDifferent(rhs.BucketAt(i));
            }
        }
    }

    /* MergeFromSame: merge from a same-capacity sampler (global snapshot merge) */
    void MergeFromSame(const PercentileSamples &rhs)
    {
        totalAdded_ += rhs.totalAdded_;
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (rhs.buckets_[i] && !rhs.buckets_[i]->Empty()) {
                GetBucket(i).MergeFrom(*rhs.buckets_[i]);
            }
        }
    }

    /* MergeFromAny: merge from any-capacity sampler (window query combining snapshots) */
    template<size_t OTHER_CAP>
    void MergeFromAny(const PercentileSamples<OTHER_CAP> &rhs)
    {
        totalAdded_ += rhs.TotalAdded();
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (rhs.HasBucket(i) && !rhs.BucketAt(i).Empty()) {
                GetBucket(i).MergeFromDifferent(rhs.BucketAt(i));
            }
        }
    }

    /* Query approx. percentile at ratio: accumulate numAdded across buckets,
       locate target bucket, then map rank proportionally to stored samples. */
    uint32_t GetPercentile(double ratio)
    {
        if (totalAdded_ == 0) {
            return 0;
        }
        size_t n = static_cast<size_t>(std::ceil(ratio * totalAdded_));
        if (n > totalAdded_) {
            n = totalAdded_;
        }
        if (n == 0) {
            return 0;
        }

        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (!buckets_[i]) {
                continue;
            }
            SampleBucket<BUCKET_CAP> &bkt = *buckets_[i];
            if (n <= bkt.NumAdded()) {
                size_t rank = static_cast<size_t>(static_cast<double>(n) * bkt.NumStored() / bkt.NumAdded());
                if (rank > 0) {
                    --rank;
                }
                return bkt.GetAt(rank);
            }
            n -= bkt.NumAdded();
        }
        return std::numeric_limits<uint32_t>::max();
    }

    size_t TotalAdded() const
    {
        return totalAdded_;
    }
    bool HasBucket(size_t i) const
    {
        return buckets_[i] != nullptr;
    }
    const SampleBucket<BUCKET_CAP> &BucketAt(size_t i) const
    {
        return *buckets_[i];
    }

    void Clear()
    {
        totalAdded_ = 0;
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (buckets_[i]) {
                buckets_[i]->Clear();
            }
        }
    }

    bool Full() const
    {
        for (size_t i = 0; i < NUM_LOG_BUCKETS; ++i) {
            if (buckets_[i] && buckets_[i]->Full()) {
                return true;
            }
        }
        return false;
    }

private:
    /* GetBucket: lazily create a bucket; only allocated when data falls in that range */
    SampleBucket<BUCKET_CAP> &GetBucket(size_t idx)
    {
        if (!buckets_[idx]) {
            buckets_[idx] = new SampleBucket<BUCKET_CAP>;
        }
        return *buckets_[idx];
    }

    size_t totalAdded_;                                  /* total number of samples added */
    SampleBucket<BUCKET_CAP> *buckets_[NUM_LOG_BUCKETS]; /* log2 buckets (lazily created) */
};

using GlobalPercentile = PercentileSamples<GLOBAL_SAMPLE_SIZE>;
using TlsPercentile = PercentileSamples<TLS_SAMPLE_SIZE>;
using CombinedPercentile = PercentileSamples<COMBINED_SAMPLE_SIZE>;

} /* namespace mf */
} /* namespace ock */

#endif /* LATENCY_STATS_PERCENTILE_H */
