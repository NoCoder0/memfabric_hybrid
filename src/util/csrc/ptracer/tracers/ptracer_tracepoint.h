/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef MEM_FABRIC_PTRACER_TRACEPOINT_H
#define MEM_FABRIC_PTRACER_TRACEPOINT_H

#include "ptracer_utils.h"
#include "latency_recorder.h"

namespace ock {
namespace mf {
namespace tracer {
class Tracepoint {
public:
    __always_inline void TraceBegin(const std::string &tpName)
    {
        bool expectVal = false;
        if (nameValid_.compare_exchange_weak(expectVal, true)) {
            name_ = tpName;
            if (rec_.load(std::memory_order_relaxed) == nullptr) {
                auto *r = new (std::nothrow) LatencyRecorder(PTRACER_DUMP_INTERVAL_SEC);
                if (r != nullptr) {
                    LatencyRecorder *expected = nullptr;
                    if (rec_.compare_exchange_strong(expected, r,
                        std::memory_order_release, std::memory_order_relaxed)) {
                        r->StartSampling();
                    } else {
                        delete r;
                    }
                }
            }
        }
        begin_.fetch_add(1u, std::memory_order_relaxed);
    }

    __always_inline void TraceEnd(uint64_t diff, int32_t goodBadExecution)
    {
        if (goodBadExecution != 0) { /* ignore the time cost counting for bad execution */
            badEnd_.fetch_add(1u, std::memory_order_relaxed);
            return;
        }

        auto *r = rec_.load(std::memory_order_acquire);
        if (r != nullptr) {
            *r << static_cast<int64_t>(diff);
        }
        goodEnd_.fetch_add(1u, std::memory_order_relaxed);
    }

    __always_inline void Reset()
    {
        begin_ = 0;
        goodEnd_ = 0;
        badEnd_ = 0;
    }

    __always_inline const std::string &GetName() const
    {
        return name_;
    }

    __always_inline uint64_t GetBegin() const
    {
        return begin_.load(std::memory_order_relaxed);
    }

    __always_inline uint64_t GetGoodEnd() const
    {
        return goodEnd_.load(std::memory_order_relaxed);
    }

    __always_inline uint64_t GetBadEnd() const
    {
        return badEnd_.load(std::memory_order_relaxed);
    }

    __always_inline bool Valid(const bool needTotal) const
    {
        if (needTotal) {
            return nameValid_ && begin_.load(std::memory_order_relaxed) > 0;
        } else {
            return nameValid_ && begin_.load(std::memory_order_relaxed) > previousBegin_;
        }
    }

    void UpdatePreviousData()
    {
        previousBegin_ = begin_.load(std::memory_order_relaxed);
        previousGoodEnd_ = goodEnd_.load(std::memory_order_relaxed);
        previousBadEnd_ = badEnd_.load(std::memory_order_relaxed);
    }

    std::string ToPeriodString()
    {
        auto beginGap = begin_.load(std::memory_order_relaxed) - previousBegin_;
        auto goodEndGap = goodEnd_.load(std::memory_order_relaxed) - previousGoodEnd_;
        auto badEndGap = badEnd_.load(std::memory_order_relaxed) - previousBadEnd_;
        UpdatePreviousData();
        return Func::FormatString(name_, beginGap, goodEndGap, badEndGap, rec_.load(std::memory_order_relaxed));
    }

    std::string ToTotalString()
    {
        auto beginGap = begin_.load(std::memory_order_relaxed);
        auto goodEndGap = goodEnd_.load(std::memory_order_relaxed);
        auto badEndGap = badEnd_.load(std::memory_order_relaxed);
        return Func::FormatString(name_, beginGap, goodEndGap, badEndGap, rec_.load(std::memory_order_relaxed));
    }

private:
    std::string name_;
    std::atomic<bool> nameValid_{false};
    std::atomic_uint_fast64_t begin_{0};
    std::atomic_uint_fast64_t goodEnd_{0};
    std::atomic_uint_fast64_t badEnd_{0};

    uint64_t previousBegin_{0};
    uint64_t previousGoodEnd_{0};
    uint64_t previousBadEnd_{0};
    std::atomic<LatencyRecorder *> rec_{nullptr};
};

class TracepointCollection {
public:
    static __always_inline Tracepoint **GetTracepoints()
    {
        static Tracepoint **tracePoints = CreateInstance();
        return tracePoints;
    }

    static __always_inline Tracepoint *GetTracepoint(uint32_t tpId)
    {
        auto tracePoints = GetTracepoints();
        if (PTRACER_UNLIKELY(tracePoints == nullptr)) {
            return nullptr;
        }

        uint32_t moduleId = GetModuleId(tpId);
        uint32_t traceId = GetTraceId(tpId);
        if (PTRACER_UNLIKELY(moduleId >= static_cast<uint32_t>(MAX_MODULE_COUNT) ||
                            traceId >= static_cast<uint32_t>(MAX_TRACE_ID_COUNT))) {
            return nullptr;
        }

        return &tracePoints[moduleId][traceId];
    }

    static __always_inline void TraceBegin(uint32_t tpId, const std::string &tpName)
    {
        auto tracepoint = GetTracepoint(tpId);
        if (PTRACER_UNLIKELY(tracepoint == nullptr)) {
            return;
        }

        tracepoint->TraceBegin(tpName);
    }

    static __always_inline void TraceEnd(uint32_t tpId, const uint64_t &diff, int32_t goodBadExecution)
    {
        auto tracepoint = GetTracepoint(tpId);
        if (PTRACER_UNLIKELY(tracepoint == nullptr)) {
            return;
        }

        tracepoint->TraceEnd(diff, goodBadExecution);
    }

public:
    static constexpr int32_t MAX_MODULE_COUNT = 64;
    static constexpr int32_t MAX_TRACE_ID_COUNT = 1024;

private:
    static __always_inline Tracepoint **CreateInstance()
    {
        auto **instance = new (std::nothrow) Tracepoint *[MAX_MODULE_COUNT];
        if (instance == nullptr) {
            return nullptr;
        }

        for (int32_t i = 0; i < MAX_MODULE_COUNT; ++i) {
            instance[i] = nullptr;
        }

        int32_t ret = 0;
        uint16_t i = 0;
        for (i = 0; i < MAX_MODULE_COUNT; ++i) {
            instance[i] = new (std::nothrow) Tracepoint[MAX_TRACE_ID_COUNT];
            if (instance[i] == nullptr) {
                ret = -1;
                break;
            }
        }

        if (ret != 0) {
            for (uint16_t j = 0; j < i; ++j) {
                delete[] instance[j];
            }
            delete[] instance;
            return nullptr;
        }
        return instance;
    }

    static __always_inline uint32_t GetModuleId(uint32_t tpId)
    {
        return ((tpId >> 16) & 0xFFFF);
    }

    static __always_inline uint32_t GetTraceId(uint32_t tpId)
    {
        return (tpId & 0xFFFF);
    }
};
} // namespace tracer
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_PTRACER_TRACEPOINT_H
