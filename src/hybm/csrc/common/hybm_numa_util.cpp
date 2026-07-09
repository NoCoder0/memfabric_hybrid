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
#include "hybm_numa_util.h"

#include <bitset>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sched.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "dl_acl_api.h"
#include "hybm_logger.h"
#include "hybm_define.h"
#include "mf_num_util.h"

namespace ock {
namespace mf {

namespace {

bool Contains(std::initializer_list<int32_t> devices, int32_t deviceId)
{
    for (int32_t device : devices) {
        if (device == deviceId) {
            return true;
        }
    }
    return false;
}

int32_t ResolveCpuGroupFromDeviceImpl(int32_t deviceId)
{
    if (DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_950) {
        // A5环境中，deviceId 0/1/6/7 对应 cpulist1，deviceId 2/3/4/5 对应 cpulist0
        if (Contains({2, 3, 4, 5}, deviceId)) {
            return 0;
        }
        if (Contains({0, 1, 6, 7}, deviceId)) {
            return 1;
        }
    }
    return -1;
}

// 根据groupID找到对应NUMA nodes
std::vector<int32_t> ResolveNumaNodesFromCpuGroupImpl(int32_t cpuGroupId)
{
    // A5环境中，cpulist0对应numa node0/1，cpulist1对应numa node2/3
    if (DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_950) {
        if (cpuGroupId == 0) {
            return {0, 1};
        }
        if (cpuGroupId == 1) {
            return {2, 3};
        }
    }
    return {};
}

bool ParseCpuListImpl(const std::string &cpuList, std::vector<int32_t> &cpus)
{
    std::stringstream cpuListStream(cpuList);
    std::string cpuRange;
    while (std::getline(cpuListStream, cpuRange, ',')) {
        if (cpuRange.empty()) {
            continue;
        }

        const auto dashPos = cpuRange.find('-');
        const int32_t firstCpu = std::stoi(dashPos == std::string::npos ? cpuRange : cpuRange.substr(0, dashPos));
        const int32_t lastCpu = dashPos == std::string::npos ? firstCpu : std::stoi(cpuRange.substr(dashPos + 1));
        if (firstCpu < 0 || lastCpu < firstCpu) {
            return false;
        }
        for (int32_t cpu = firstCpu; cpu <= lastCpu; ++cpu) {
            cpus.emplace_back(cpu);
        }
    }
    return !cpus.empty();
}

bool GetCpuList(int32_t numaNode, std::vector<int32_t> &cpus)
{
    const std::string cpuListPath = "/sys/devices/system/node/node" + std::to_string(numaNode) + "/cpulist";
    std::ifstream cpuListFile(cpuListPath);
    if (!cpuListFile.is_open()) {
        BM_LOG_WARN("CpuAffinityGuard: open numa cpulist failed, path=" << cpuListPath);
        return false;
    }

    std::string cpuList;
    std::getline(cpuListFile, cpuList);
    if (cpuList.empty()) {
        BM_LOG_WARN("CpuAffinityGuard: empty numa cpulist, path=" << cpuListPath);
        return false;
    }

    try {
        return ParseCpuListImpl(cpuList, cpus);
    } catch (const std::exception &e) {
        BM_LOG_WARN("CpuAffinityGuard: parse numa cpulist failed, numaNode=" << numaNode << ", cpulist=" << cpuList
                                                                             << ", error=" << e.what());
        return false;
    }
}

bool BuildCpuMaskImpl(int32_t cpuGroupId, cpu_set_t &mask)
{
    CPU_ZERO(&mask);
    const auto numaNodes = ResolveNumaNodesFromCpuGroupImpl(cpuGroupId);
    if (numaNodes.empty()) {
        BM_LOG_WARN("CpuAffinityGuard: empty numa nodes, cpuGroupId=" << cpuGroupId);
        return false;
    }

    bool hasCpu = false;
    for (const auto numaNode : numaNodes) {
        std::vector<int32_t> cpus;
        if (!GetCpuList(numaNode, cpus)) {
            BM_LOG_WARN("CpuAffinityGuard: get numa node cpus failed, cpuGroupId=" << cpuGroupId
                                                                                   << ", numaNode=" << numaNode);
            return false;
        }
        for (const auto cpu : cpus) {
            CPU_SET(cpu, &mask);
            hasCpu = true;
        }
    }
    return hasCpu;
}

} // namespace

// 通过解析flags来判断numa亲和策略
NumaBindPolicyInfo HybmNumaUtil::GetNumaBindPolicyInfo(uint32_t flags, int32_t deviceId)
{
    NumaBindPolicyInfo info{};
    const uint32_t performance =
        NumUtil::ExtractBits(flags, HYBM_PERFORMANCE_MODE_FLAG_INDEX, HYBM_PERFORMANCE_MODE_FLAG_LEN);
    if (performance == UINT32_MAX) {
        BM_LOG_WARN("Invalid performance flag bits: " << std::bitset<UINT32_WIDTH>(flags)
                                                      << " start index:" << HYBM_PERFORMANCE_MODE_FLAG_INDEX
                                                      << " flag len:" << HYBM_PERFORMANCE_MODE_FLAG_LEN);
        info.valid = false;
        return info;
    }
    if (performance == 0U) {
        return info;
    }

    const uint32_t bindNuma = NumUtil::ExtractBits(flags, HYBM_BIND_NUMA_FLAG_INDEX, HYBM_BIND_NUMA_FLAG_LEN);
    if (bindNuma == UINT32_MAX) {
        BM_LOG_WARN("Invalid numa bind flag bits: " << std::bitset<UINT32_WIDTH>(flags)
                                                    << " start index:" << HYBM_BIND_NUMA_FLAG_INDEX
                                                    << " flag len:" << HYBM_BIND_NUMA_FLAG_LEN);
        info.valid = false;
        return info;
    }

    info.numaIndex = bindNuma;
    info.policy = bindNuma == HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG ? NumaBindPolicy::AUTO : NumaBindPolicy::MANUAL;
    if (DlAclApi::GetAscendSocType() != AscendSocType::ASCEND_950) {
        return info;
    }

    if (info.policy == NumaBindPolicy::AUTO) {
        info.cpuGroupId = ResolveCpuGroupFromDeviceImpl(deviceId);
        return info;
    }

    if (info.numaIndex > 3U) {
        BM_LOG_WARN("Invalid manual affinity numa index: " << info.numaIndex
                                                           << ", flags:" << std::bitset<UINT32_WIDTH>(flags));
        info.policy = NumaBindPolicy::OFF;
        info.valid = false;
        return info;
    }
    info.isManual = true;
    info.cpuGroupId = static_cast<int32_t>(info.numaIndex / 2U);
    return info;
}

CpuAffinityGuard::CpuAffinityGuard(int32_t cpuGroupId)
{
    if (cpuGroupId < 0) {
        BM_LOG_INFO("CpuAffinityGuard disabled, cpuGroupId=-1");
        return;
    }

    cpu_set_t newMask;
    if (!BuildCpuMaskImpl(cpuGroupId, newMask)) {
        BM_LOG_WARN("CpuAffinityGuard: failed to build CPU mask, cpuGroupId=" << cpuGroupId);
        return;
    }

    if (sched_getaffinity(0, sizeof(oldMask_), &oldMask_) != 0) {
        BM_LOG_WARN("CpuAffinityGuard: getaffinity failed, errno=" << errno << ", strerror=" << strerror(errno));
        return;
    }
    if (sched_setaffinity(0, sizeof(newMask), &newMask) != 0) {
        BM_LOG_WARN("CpuAffinityGuard: setaffinity failed, cpuGroupId=" << cpuGroupId << ", errno=" << errno
                                                                        << ", strerror=" << strerror(errno));
        return;
    }
    enabled_ = true;
    BM_LOG_INFO("CpuAffinityGuard enabled, cpuGroupId=" << cpuGroupId);
}

CpuAffinityGuard::~CpuAffinityGuard()
{
    if (!enabled_) {
        return;
    }
    sched_setaffinity(0, sizeof(oldMask_), &oldMask_);
}

bool CpuAffinityGuard::Enabled() const
{
    return enabled_;
}

} // namespace mf
} // namespace ock
