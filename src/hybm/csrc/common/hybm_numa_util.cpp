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

#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_define.h"
#include "hybm_logger.h"
#include "mf_num_util.h"

namespace ock {
namespace mf {

namespace {

constexpr const char *CPU_SYSFS_ROOT = "/sys/devices/system/cpu";
constexpr const char *NODE_SYSFS_ROOT = "/sys/devices/system/node";
// 输入：path 为 sysfs 文件路径，value 用于接收文件第一行内容。
// 输出：成功读取到非空内容时返回 true，否则返回 false。
// 功能：统一读取只包含单个值的 sysfs 文件。
bool ReadFirstLineImpl(const std::string &path, std::string &value)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        BM_LOG_WARN("Open sysfs file failed, path=" << path);
        return false;
    }

    std::getline(file, value);
    if (value.empty()) {
        BM_LOG_WARN("Read empty sysfs value, path=" << path);
        return false;
    }
    return true;
}

// 输入：cpuList 为 Linux cpulist 字符串，例如 "0-3,8,10-12"。
// 输出：cpus 接收排序并去重后的 CPU ID，解析成功返回 true。
// 功能：将 Linux cpulist 字符串展开为明确的 CPU ID 列表。
bool ParseCpuListImpl(const std::string &cpuList, std::vector<int32_t> &cpus)
{
    cpus.clear();

    try {
        std::stringstream stream(cpuList);
        std::string token;
        while (std::getline(stream, token, ',')) {
            if (token.empty()) {
                continue;
            }

            const size_t dashPos = token.find('-');
            const int32_t firstCpu = std::stoi(dashPos == std::string::npos ? token : token.substr(0, dashPos));
            const int32_t lastCpu = dashPos == std::string::npos ? firstCpu : std::stoi(token.substr(dashPos + 1));
            if (firstCpu < 0 || lastCpu < firstCpu) {
                BM_LOG_WARN("Invalid CPU range, token=" << token << ", cpulist=" << cpuList);
                return false;
            }

            for (int64_t cpu = firstCpu; cpu <= static_cast<int64_t>(lastCpu); ++cpu) {
                cpus.emplace_back(static_cast<int32_t>(cpu));
            }
        }
    } catch (const std::exception &e) {
        BM_LOG_WARN("Parse CPU list failed, cpulist=" << cpuList << ", error=" << e.what());
        cpus.clear();
        return false;
    }

    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    return !cpus.empty();
}

// 输入：path 为保存 Linux cpulist 的 sysfs 文件路径。
// 输出：cpus 接收解析后的 CPU ID，读取并解析成功返回 true。
// 功能：将 sysfs 文件读取和 cpulist 解析封装为一个通用操作。
bool ReadCpuListFileImpl(const std::string &path, std::vector<int32_t> &cpus)
{
    std::string cpuList;
    return ReadFirstLineImpl(path, cpuList) && ParseCpuListImpl(cpuList, cpus);
}

// 输入：cpu 为逻辑 CPU ID。
// 输出：socketId 接收 physical_package_id，读取成功返回 true。
// 功能：查询指定逻辑 CPU 所属的物理 CPU socket。
bool ReadSocketIdImpl(int32_t cpu, int32_t &socketId)
{
    const std::string path =
        std::string(CPU_SYSFS_ROOT) + "/cpu" + std::to_string(cpu) + "/topology/physical_package_id";
    std::string value;
    if (!ReadFirstLineImpl(path, value)) {
        return false;
    }

    try {
        socketId = std::stoi(value);
        return socketId >= 0;
    } catch (const std::exception &e) {
        BM_LOG_WARN("Parse socket id failed, cpu=" << cpu << ", value=" << value << ", error=" << e.what());
        return false;
    }
}

// 输入：numaIndex 为用户在 MANUAL 策略下指定的 NUMA node。
// 输出：cpus 接收该 NUMA node 包含的 CPU ID，获取成功返回 true。
// 功能：从 NUMA sysfs cpulist 获取 MANUAL 策略使用的初始 CPU 列表。
bool GetNumaCpuListImpl(uint32_t numaIndex, std::vector<int32_t> &cpus)
{
    const std::string path = std::string(NODE_SYSFS_ROOT) + "/node" + std::to_string(numaIndex) + "/cpulist";
    if (!ReadCpuListFileImpl(path, cpus)) {
        BM_LOG_WARN("Get NUMA CPU list failed, numaIndex=" << numaIndex << ", path=" << path);
        return false;
    }
    return true;
}

// 输入：deviceId 为 AUTO 策略需要查询的 Ascend 设备 ID。
// 输出：cpus 接收 DCMI 返回的设备亲和 CPU ID，获取成功返回 true。
// 功能：动态获取 AUTO 策略使用的初始 CPU 列表，避免硬编码设备拓扑。
bool GetDeviceAffinityCpuListImpl(int32_t deviceId, std::vector<int32_t> &cpus)
{
    std::string cpuList;
    const int32_t ret = DlHalApi::DcmiGetAffinityCpuInfo(deviceId, cpuList);
    if (ret != 0) {
        return false;
    }

    if (!ParseCpuListImpl(cpuList, cpus)) {
        BM_LOG_WARN("Parse device affinity CPU list failed, deviceId=" << deviceId << ", cpulist=" << cpuList);
        return false;
    }
    return true;
}

// 输入：seedCpus 为从 DCMI 或 NUMA node 获取的初始 CPU 列表。
// 输出：socketCpus 接收这些 CPU 所属物理 socket 中的全部 online CPU。
// 功能：将初始 CPU 列表扩展为亲和绑定所需的完整 socket CPU 列表。
bool ExpandToSocketCpuListImpl(const std::vector<int32_t> &seedCpus, std::vector<int32_t> &socketCpus)
{
    std::set<int32_t> socketIds;
    for (const int32_t cpu : seedCpus) {
        int32_t socketId = -1;
        if (!ReadSocketIdImpl(cpu, socketId)) {
            BM_LOG_WARN("Resolve socket from seed CPU failed, cpu=" << cpu);
            return false;
        }
        socketIds.insert(socketId);
    }

    std::vector<int32_t> onlineCpus;
    const std::string onlinePath = std::string(CPU_SYSFS_ROOT) + "/online";
    if (!ReadCpuListFileImpl(onlinePath, onlineCpus)) {
        BM_LOG_WARN("Get online CPU list failed, path=" << onlinePath);
        return false;
    }

    socketCpus.clear();
    for (const int32_t cpu : onlineCpus) {
        int32_t socketId = -1;
        if (!ReadSocketIdImpl(cpu, socketId)) {
            return false;
        }
        if (socketIds.count(socketId) != 0U) {
            socketCpus.emplace_back(cpu);
        }
    }

    return !socketCpus.empty();
}

// 输入：policy 指定 AUTO 或 MANUAL，numaIndex 和 deviceId 提供对应策略的查询参数。
// 输出：socketCpus 接收最终解析出的完整 socket CPU 列表。
// 功能：统一 AUTO 和 MANUAL 的拓扑解析流程，简化上层调用逻辑。
bool ResolveSocketCpuListImpl(NumaBindPolicy policy, uint32_t numaIndex, int32_t deviceId,
                              std::vector<int32_t> &socketCpus, std::vector<int32_t> &numaCpus)
{
    numaCpus.clear();
    const bool resolved = policy == NumaBindPolicy::AUTO ? GetDeviceAffinityCpuListImpl(deviceId, numaCpus)
                                                         : GetNumaCpuListImpl(numaIndex, numaCpus);
    if (!resolved) {
        return false;
    }

    return ExpandToSocketCpuListImpl(numaCpus, socketCpus);
}

// 输入：cpus 为目标 socket CPU 列表，allowedMask 为当前线程允许使用的 CPU 集合。
// 输出：mask 接收两者交集，没有可用 CPU 时返回 false。
// 功能：构造符合 cgroup、cpuset 和 taskset 限制的安全 CPU 亲和掩码。
bool BuildCpuMaskImpl(const std::vector<int32_t> &cpus, const cpu_set_t &allowedMask, cpu_set_t &mask)
{
    CPU_ZERO(&mask);
    bool hasCpu = false;

    for (const int32_t cpu : cpus) {
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            BM_LOG_WARN("CPU id exceeds cpu_set_t capacity, cpu=" << cpu << ", CPU_SETSIZE=" << CPU_SETSIZE);
            return false;
        }

        // 只保留当前线程在 cgroup、cpuset 或 taskset 限制下允许使用的 CPU。
        if (CPU_ISSET(cpu, &allowedMask)) {
            CPU_SET(cpu, &mask);
            hasCpu = true;
        }
    }

    if (!hasCpu) {
        BM_LOG_WARN("Resolved socket CPU list has no intersection with current allowed CPU mask");
    }
    return hasCpu;
}

// 输入：cpus 为已排序的 CPU ID 列表。
// 输出：返回 Linux cpulist 格式字符串，例如 "0-3,8,10-12"。
// 功能：将 CPU ID 列表压缩为可读的 cpulist 格式，用于日志输出。
std::string FormatCpuListImpl(const std::vector<int32_t> &cpus)
{
    if (cpus.empty()) {
        return "";
    }

    std::ostringstream oss;
    int32_t rangeStart = cpus[0];
    int32_t rangeEnd = rangeStart;

    for (size_t i = 1; i < cpus.size(); ++i) {
        if (cpus[i] == rangeEnd + 1) {
            rangeEnd = cpus[i];
        } else {
            if (rangeStart == rangeEnd) {
                oss << rangeStart << ",";
            } else {
                oss << rangeStart << "-" << rangeEnd << ",";
            }
            rangeStart = cpus[i];
            rangeEnd = rangeStart;
        }
    }

    if (rangeStart == rangeEnd) {
        oss << rangeStart;
    } else {
        oss << rangeStart << "-" << rangeEnd;
    }

    return oss.str();
}

} // namespace

// 输入：flags 包含性能模式和 NUMA 策略位，deviceId 供 AUTO 策略查询使用。
// 输出：返回解析后的策略信息以及最终 socket CPU 列表。
// 功能：解析 flags、动态解析系统拓扑，并向调用方返回统一结果。
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

    // 非 A5 仅保留 NUMA 内存分配策略，不解析 CPU/socket 亲和信息。
    if (DlAclApi::GetAscendSocType() != AscendSocType::ASCEND_950) {
        BM_LOG_DEBUG("Skip NUMA CPU affinity resolution on non-A5 SOC");
        return info;
    }

    if (!ResolveSocketCpuListImpl(info.policy, info.numaIndex, deviceId, info.socketCpus, info.numaCpus)) {
        BM_LOG_WARN("Resolve socket CPU list failed, policy=" << static_cast<int32_t>(info.policy) << ", deviceId="
                                                              << deviceId << ", numaIndex=" << info.numaIndex);
        info.policy = NumaBindPolicy::OFF;
        info.valid = false;
        info.socketCpus.clear();
        info.numaCpus.clear();
    }

    return info;
}

// 输入：policyInfo 包含策略模式、NUMA CPU 列表和 socket CPU 列表。
// 输出：无直接返回值，可通过 Enabled() 判断亲和绑定是否成功。
// 功能：保存当前线程原有亲和性，并临时将调用线程绑定到目标 CPU。
CpuAffinityGuard::CpuAffinityGuard(const NumaBindPolicyInfo &policyInfo)
{
    const auto &socketCpus = policyInfo.socketCpus;
    if (socketCpus.empty()) {
        BM_LOG_INFO("CpuAffinityGuard disabled, empty socket CPU list");
        return;
    }

    if (sched_getaffinity(0, sizeof(oldMask_), &oldMask_) != 0) {
        BM_LOG_WARN("CpuAffinityGuard: getaffinity failed, errno=" << errno << ", strerror=" << strerror(errno));
        return;
    }

    cpu_set_t newMask;
    if (!BuildCpuMaskImpl(socketCpus, oldMask_, newMask)) {
        BM_LOG_WARN("CpuAffinityGuard: build CPU mask failed");
        return;
    }

    if (sched_setaffinity(0, sizeof(newMask), &newMask) != 0) {
        BM_LOG_WARN("CpuAffinityGuard: setaffinity failed, errno=" << errno << ", strerror=" << strerror(errno));
        return;
    }

    enabled_ = true;
    BM_LOG_INFO("CpuAffinityGuard enabled, policy="
                << static_cast<int32_t>(policyInfo.policy) << ", numaIndex=" << policyInfo.numaIndex
                << ", numaCpus=" << FormatCpuListImpl(policyInfo.numaCpus)
                << ", socketCpus=" << FormatCpuListImpl(socketCpus) << ", cpuCount=" << CPU_COUNT(&newMask));
}

// 功能：Guard 离开作用域时恢复调用线程原有的 CPU 亲和性。
CpuAffinityGuard::~CpuAffinityGuard()
{
    if (!enabled_) {
        return;
    }

    if (sched_setaffinity(0, sizeof(oldMask_), &oldMask_) != 0) {
        BM_LOG_WARN("CpuAffinityGuard: restore affinity failed, errno=" << errno << ", strerror=" << strerror(errno));
    }
}

// 输出：Guard 成功修改 CPU 亲和性时返回 true，否则返回 false。
// 功能：供调用方判断临时 CPU 亲和绑定当前是否生效。
bool CpuAffinityGuard::Enabled() const
{
    return enabled_;
}

} // namespace mf
} // namespace ock
