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

#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <array>
#include <arpa/inet.h>

#include "hybm_logger.h"
#include "dl_hcomm_api.h"
#include "device_urma_eid_reader.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {

Result GetDeviceUrmaAddrValue(uint32_t phyDeviceId, uint32_t rankId, const char *missingValueDesc,
                              std::string &addrValue, std::string &eidFilePath)
{
    const char *eidFilePathEnv = std::getenv("MF_DEVICE_URMA_EID_FILE");
    if (eidFilePathEnv == nullptr || eidFilePathEnv[0] == '\0') {
        BM_LOG_ERROR("device_urma env MF_DEVICE_URMA_EID_FILE not set, rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    eidFilePath = eidFilePathEnv;
    std::ifstream eidFile(eidFilePath);
    if (!eidFile.is_open()) {
        BM_LOG_ERROR("device_urma cannot open EID file: " << eidFilePath << ", rankId=" << rankId);
        return BM_ERROR;
    }
    std::unordered_map<uint32_t, std::string> valueMap;
    std::string line;
    while (std::getline(eidFile, line)) {
        if (line.empty()) {
            continue;
        }
        const auto colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            BM_LOG_ERROR("device_urma invalid EID file format (missing colon), line: " << line
                        << ", file: " << eidFilePath);
            return BM_INVALID_PARAM;
        }
        std::string idStr = line.substr(0, colonPos);
        auto trimLeft = idStr.find_first_not_of(" \t");
        auto trimRight = idStr.find_last_not_of(" \t");
        if (trimLeft == std::string::npos) {
            BM_LOG_ERROR("device_urma invalid EID file format (empty devPhyId), line: " << line
                        << ", file: " << eidFilePath);
            return BM_INVALID_PARAM;
        }
        idStr = idStr.substr(trimLeft, trimRight - trimLeft + 1);
        char *end = nullptr;
        const auto devId = static_cast<uint32_t>(std::strtoul(idStr.c_str(), &end, 10));
        if (*end != '\0') {
            BM_LOG_ERROR("device_urma invalid EID file format (non-numeric devPhyId), line: " << line << ", file: "
                        << eidFilePath);
            return BM_INVALID_PARAM;
        }
        std::string valueStr = line.substr(colonPos + 1);
        trimLeft = valueStr.find_first_not_of(" \t");
        if (trimLeft == std::string::npos) {
            BM_LOG_ERROR("device_urma invalid EID file format (" << missingValueDesc << "), line: " << line
                        << ", file: " << eidFilePath);
            return BM_INVALID_PARAM;
        }
        valueStr = valueStr.substr(trimLeft);
        trimRight = valueStr.find_last_not_of(" \t\r\n");
        if (trimRight != std::string::npos) {
            valueStr = valueStr.substr(0, trimRight + 1);
        }
        valueMap[devId] = valueStr;
    }
    const auto it = valueMap.find(phyDeviceId);
    if (it == valueMap.end()) {
        BM_LOG_ERROR("device_urma devPhyId " << phyDeviceId << " not found in EID file: " << eidFilePath
                    << ", rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    addrValue = it->second;
    return BM_OK;
}

} // anonymous namespace

Result GetDeviceUrmaEid(uint32_t phyDeviceId, uint32_t rankId, std::array<uint8_t, COMM_ADDR_EID_LEN> &eidData)
{
    std::string hexStr;
    std::string eidFilePath;
    Result ret = GetDeviceUrmaAddrValue(phyDeviceId, rankId, "missing hex EID", hexStr, eidFilePath);
    if (ret != BM_OK) {
        return ret;
    }
    if (hexStr.length() != COMM_ADDR_EID_LEN * 2U) {
        BM_LOG_ERROR("device_urma invalid EID hex length: " << hexStr.length() << " (expected "
                                                             << (COMM_ADDR_EID_LEN * 2U) << "), file: " << eidFilePath);
        return BM_INVALID_PARAM;
    }
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        auto byteStr = hexStr.substr(i * 2, 2);
        char *endp = nullptr;
        auto val = std::strtoul(byteStr.c_str(), &endp, 16);
        if (*endp != '\0') {
            BM_LOG_ERROR("device_urma invalid EID hex character at byte " << i << ", file: " << eidFilePath);
            return BM_INVALID_PARAM;
        }
        eid[i] = static_cast<uint8_t>(val & 0xFF);
    }
    eidData = eid;
    return BM_OK;
}

Result GetDeviceUrmaIpAddr(uint32_t phyDeviceId, uint32_t rankId, CommAddrType &addrType,
                           std::array<uint8_t, URMA_ENDPOINT_RAW_LEN> &addrData)
{
    std::string ipStr;
    std::string eidFilePath;
    Result ret = GetDeviceUrmaAddrValue(phyDeviceId, rankId, "missing IP address", ipStr, eidFilePath);
    if (ret != BM_OK) {
        return ret;
    }
    struct in_addr ipv4Addr;
    if (inet_pton(AF_INET, ipStr.c_str(), &ipv4Addr) == 1) {
        addrType = COMM_ADDR_TYPE_IP_V4;
        addrData.fill(0);
        std::memcpy(addrData.data(), &ipv4Addr, sizeof(ipv4Addr));
        BM_LOG_DEBUG("device_urma GetDeviceUrmaIpAddr ipv4, devPhyId=" << phyDeviceId
                     << " rankId=" << rankId << " ip=" << ipStr);
        return BM_OK;
    }
    struct in6_addr ipv6Addr;
    if (inet_pton(AF_INET6, ipStr.c_str(), &ipv6Addr) == 1) {
        addrType = COMM_ADDR_TYPE_IP_V6;
        addrData.fill(0);
        std::memcpy(addrData.data(), &ipv6Addr, sizeof(ipv6Addr));
        BM_LOG_DEBUG("device_urma GetDeviceUrmaIpAddr ipv6, devPhyId=" << phyDeviceId
                     << " rankId=" << rankId << " ip=" << ipStr);
        return BM_OK;
    }
    // Try to parse as 32 hex digits (IPv6 without colon separators)
    if (ipStr.length() == 32U) {
        std::array<uint8_t, 16U> raw{};
        bool valid = true;
        for (size_t i = 0; i < 16U; ++i) {
            auto byteStr = ipStr.substr(i * 2, 2);
            char *endp = nullptr;
            auto val = std::strtoul(byteStr.c_str(), &endp, 16U);
            if (*endp != '\0') {
                valid = false;
                break;
            }
            raw[i] = static_cast<uint8_t>(val & 0xFF);
        }
        if (valid) {
            addrType = COMM_ADDR_TYPE_IP_V6;
            addrData.fill(0);
            std::memcpy(addrData.data(), raw.data(), raw.size());
            BM_LOG_DEBUG("device_urma GetDeviceUrmaIpAddr ipv6-hex, devPhyId=" << phyDeviceId
                         << " rankId=" << rankId << " ip=" << ipStr);
            return BM_OK;
        }
    }
    BM_LOG_ERROR("device_urma invalid IP address format: '" << ipStr << "' for devPhyId "
                  << phyDeviceId << ", file: " << eidFilePath);
    return BM_INVALID_PARAM;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
