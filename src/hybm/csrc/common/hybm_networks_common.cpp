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
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>
#include "hybm_functions.h"
#include "hybm_logger.h"
#include "hybm_networks_common.h"

namespace ock {
namespace mf {
namespace {
std::string GetDefaultRouteNetwork()
{
    std::string routeFileName{"/proc/net/route"};
    std::ifstream input(routeFileName);
    if (!input.is_open()) {
        BM_LOG_ERROR("open route file failed: " << strerror(errno));
        return "";
    }

    std::string ifname;
    uint32_t destination;
    uint32_t temp;
    uint32_t mask;
    std::string line;
    std::getline(input, line); // skip header line
    while (std::getline(input, line)) {
        std::stringstream ss{line};
        ss >> ifname >> std::hex; // Iface
        ss >> destination;        // Destination
        ss >> temp;               // Gateway
        ss >> temp;               // Flags
        ss >> temp;               // RefCnt
        ss >> temp;               // Use
        ss >> temp;               // Metric
        ss >> mask;               // Mask
        if (destination == 0U && mask == 0U) {
            BM_LOG_INFO("default route network : " << ifname);
            return ifname;
        }
    }
    return "";
}

bool ParseMachineId(const std::string &content, MachineId &machineId)
{
    constexpr size_t machineIdHexLen = 32U;
    constexpr size_t machineIdHalfHexLen = 16U;

    std::string hexStr;
    hexStr.reserve(machineIdHexLen);
    for (char ch : content) {
        if (std::isxdigit(static_cast<unsigned char>(ch)) != 0) {
            hexStr.push_back(ch);
            if (hexStr.size() == machineIdHexLen) {
                break;
            }
        } else if (ch != '-') {
            break;
        }
    }

    if (hexStr.size() < machineIdHexLen) {
        return false;
    }

    try {
        auto major = std::stoull(hexStr.substr(0U, machineIdHalfHexLen), nullptr, 16);
        auto minor = std::stoull(hexStr.substr(machineIdHalfHexLen, machineIdHalfHexLen), nullptr, 16);
        machineId = MachineId(major, minor);
    } catch (...) {
        return false;
    }
    return true;
}

bool ReadMachineIdFromFile(const char *path, MachineId &machineId)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        BM_LOG_DEBUG("open machine id file failed: " << path << " : " << SafeStrError(errno));
        return false;
    }

    std::string content;
    std::getline(file, content);
    if (!ParseMachineId(content, machineId)) {
        BM_LOG_DEBUG("invalid machine id from " << path);
        return false;
    }
    return true;
}
} // namespace
std::vector<uint32_t> NetworkGetIpAddresses() noexcept
{
    std::vector<uint32_t> addresses;
    struct ifaddrs *ifa;
    struct ifaddrs *p;
    if (getifaddrs(&ifa) < 0) {
        BM_LOG_ERROR("getifaddrs() failed: " << errno << " : " << SafeStrError(errno));
        return addresses;
    }

    uint32_t routeIp = 0;
    auto routeName = GetDefaultRouteNetwork();
    for (p = ifa; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr) {
            continue;
        }

        if (p->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((p->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        if ((p->ifa_flags & IFF_UP) == 0 || (p->ifa_flags & IFF_RUNNING) == 0) {
            continue;
        }

        std::string ifname{p->ifa_name};
        auto sin = reinterpret_cast<struct sockaddr_in *>(p->ifa_addr);
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
        if (routeName == ifname) {
            routeIp = ntohl(sin->sin_addr.s_addr);
            BM_LOG_INFO("find route ip address: " << p->ifa_name << " -> " << ip_str);
        } else {
            addresses.emplace_back(ntohl(sin->sin_addr.s_addr));
            BM_LOG_INFO("find ip address: " << p->ifa_name << " -> " << ip_str);
        }
    }

    freeifaddrs(ifa);
    std::sort(addresses.begin(), addresses.end(), std::less<uint32_t>());
    if (routeIp != 0) {
        addresses.insert(addresses.begin(), routeIp);
    }
    return addresses;
}

bool ReadMachineId(MachineId &machineId) noexcept
{
    static std::mutex machineIdMutex;
    static bool machineIdCached = false;
    static MachineId machineIdCache{};

    std::lock_guard<std::mutex> lock(machineIdMutex);
    if (!machineIdCached) {
        constexpr const char *machineIdPath = "/etc/machine-id";
        constexpr const char *bootIdPath = "/proc/sys/kernel/random/boot_id";

        if (!ReadMachineIdFromFile(machineIdPath, machineIdCache)) {
            BM_LOG_WARN("read machine id from " << machineIdPath << " failed, fallback to boot id: " << bootIdPath);
            if (!ReadMachineIdFromFile(bootIdPath, machineIdCache)) {
                BM_LOG_ERROR("failed to read machine identity from " << machineIdPath << " and " << bootIdPath);
            }
        }
        machineIdCached = true;
    }

    machineId = machineIdCache;
    return machineIdCache.major != 0UL || machineIdCache.minor != 0UL;
}
} // namespace mf
} // namespace ock
