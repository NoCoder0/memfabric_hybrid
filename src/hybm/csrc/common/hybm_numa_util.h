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
#ifndef MEM_FABRIC_HYBRID_HYBM_NUMA_UTIL_H
#define MEM_FABRIC_HYBRID_HYBM_NUMA_UTIL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <sched.h>

#include "hybm_def.h"

namespace ock {
namespace mf {

enum class NumaBindPolicy {
    OFF = 0,
    AUTO = 1,
    MANUAL = 2,
};

struct NumaBindPolicyInfo {
    NumaBindPolicy policy{NumaBindPolicy::OFF};
    uint32_t numaIndex{0};
    int32_t cpuGroupId{-1};
    bool isManual{false};
    bool valid{true};
};

class HybmNumaUtil {
public:
    static NumaBindPolicyInfo GetNumaBindPolicyInfo(uint32_t flags, int32_t deviceId = -1);

private:
};

class CpuAffinityGuard {
public:
    explicit CpuAffinityGuard(int32_t cpuGroupId);
    ~CpuAffinityGuard();

    CpuAffinityGuard(const CpuAffinityGuard &) = delete;
    CpuAffinityGuard &operator=(const CpuAffinityGuard &) = delete;

    bool Enabled() const;

private:
    bool enabled_ = false;
    cpu_set_t oldMask_{};
};
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_NUMA_UTIL_H
