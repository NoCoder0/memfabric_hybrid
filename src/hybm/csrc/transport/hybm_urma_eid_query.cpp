/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of the License at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "hybm_urma_eid_query.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include "hybm_logger.h"

namespace ock {
namespace mf {
namespace {

constexpr uint32_t DCMI_DSMI_MAIN_COMMAND_UB = 62U;
constexpr uint32_t DCMI_DSMI_URMA_NAME_SUB_COMMAND = 3U;
constexpr const char *DCMI_LIBRARY_NAME = "libdcmi.so";
constexpr const char *DSMI_LIBRARY_NAME = "libdrvdsmi_host.so";
constexpr const char *UBCORE_SYSFS_ROOT = "/sys/class/ubcore";
constexpr const char *UBURMA_SYSFS_ROOT = "/sys/class/uburma";
constexpr size_t MAX_UDMA_NAME = 32U;
constexpr uint32_t MAX_HOST_EIDS = 1024U;
constexpr size_t EID_HEX_LENGTH = 32U;
constexpr size_t EID_BYTE_LENGTH = 16U;
constexpr int32_t HEX_BYTE_WIDTH = 2;
constexpr uint8_t HEX_ALPHA_OFFSET = 10U;
constexpr size_t IPV4_MAPPED_PREFIX_OFFSET = 10U;
constexpr size_t IPV4_MAPPED_PREFIX_LENGTH = 2U;
constexpr size_t IPV4_MAPPED_ADDRESS_OFFSET = IPV4_MAPPED_PREFIX_OFFSET + IPV4_MAPPED_PREFIX_LENGTH;
constexpr unsigned char IPV4_MAPPED_PREFIX_VALUE = 0xFFU;

struct EidByteInfo {
    bool isPg = false;
    uint8_t port = 0;
};

struct UrmaEidQueryResult {
    uint32_t logicalDeviceId = 0;
    std::string udma;
    std::string hostEid;
};

bool Fail(uint32_t phyDeviceId, int32_t logicalDeviceId, Result &error, const char *stage, Result code,
          int32_t nativeCode, const std::string &detail)
{
    error = code;
    BM_LOG_ERROR("Query URMA EID failed, stage=" << stage << ", phyDeviceId=" << phyDeviceId
                                                 << ", logicalDeviceId=" << logicalDeviceId
                                                 << ", nativeCode=" << nativeCode << ", detail=" << detail);
    return false;
}

void Info(uint32_t phyDeviceId, int32_t logicalDeviceId, const char *stage, const std::string &detail)
{
    BM_LOG_DEBUG("Query URMA EID, stage=" << stage << ", phyDeviceId=" << phyDeviceId
                                          << ", logicalDeviceId=" << logicalDeviceId << ", detail=" << detail);
}

std::string FormatBytes(const unsigned char *bytes, size_t size)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t index = 0; index < size; ++index) {
        stream << std::setw(HEX_BYTE_WIDTH) << static_cast<unsigned int>(bytes[index]);
    }
    return stream.str();
}

bool NormalizeHexEid(const std::string &input, std::string &output)
{
    output.clear();
    output.reserve(EID_HEX_LENGTH);
    for (const char character : input) {
        if (character == ':') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(character))) {
            output.clear();
            return false;
        }
        output.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    if (output.size() != EID_HEX_LENGTH || output == std::string(EID_HEX_LENGTH, '0')) {
        output.clear();
        return false;
    }
    return true;
}

bool ParseIpv4Value(const std::string &input, uint32_t &value)
{
    if (input.empty() || input.front() == '-') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(input.c_str(), &end, 0);
    if (errno != 0 || end == input.c_str() || *end != '\0' || parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool NormalizeEid(const std::string &input, std::string &output)
{
    if (NormalizeHexEid(input, output)) {
        return true;
    }
    unsigned char raw[EID_BYTE_LENGTH] = {};
    if (inet_pton(AF_INET6, input.c_str(), raw) == 1) {
        output = FormatBytes(raw, sizeof(raw));
        return output != std::string(EID_HEX_LENGTH, '0');
    }
    uint32_t ipv4 = 0;
    if (inet_pton(AF_INET, input.c_str(), &ipv4) != 1 && !ParseIpv4Value(input, ipv4)) {
        output.clear();
        return false;
    }
    std::fill_n(raw + IPV4_MAPPED_PREFIX_OFFSET, IPV4_MAPPED_PREFIX_LENGTH, IPV4_MAPPED_PREFIX_VALUE);
    const uint32_t networkIpv4 = input.find('.') == std::string::npos ? htonl(ipv4) : ipv4;
    std::memcpy(raw + IPV4_MAPPED_ADDRESS_OFFSET, &networkIpv4, sizeof(networkIpv4));
    output = FormatBytes(raw, sizeof(raw));
    return true;
}

uint8_t HexValue(char character)
{
    if (character >= '0' && character <= '9') {
        return static_cast<uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<uint8_t>(character - 'a' + HEX_ALPHA_OFFSET);
    }
    return static_cast<uint8_t>(character - 'A' + HEX_ALPHA_OFFSET);
}

bool ParseEidByte(const std::string &eid, EidByteInfo &info)
{
    std::string normalized;
    if (!NormalizeEid(eid, normalized)) {
        return false;
    }
    const uint8_t byte6 = static_cast<uint8_t>((HexValue(normalized[10]) << 4U) | HexValue(normalized[11]));
    info.port = static_cast<uint8_t>(byte6 & 0x3FU);
    info.isPg = info.port == 0x3FU;
    return true;
}

std::string DynamicLoaderError()
{
    const char *message = dlerror();
    return message == nullptr ? "unknown dynamic-loader error" : message;
}

class DcmiApi {
public:
    using InitFn = int32_t (*)();
    using GetLogicalFn = int32_t (*)(uint32_t, uint32_t *);

    ~DcmiApi()
    {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    bool Load(uint32_t phyDeviceId, Result &error)
    {
        const std::string path = DCMI_LIBRARY_NAME;
        handle_ = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (handle_ == nullptr) {
            return Fail(phyDeviceId, -1, error, "dcmi_load", BM_DL_FUNCTION_FAILED, 0,
                        "dlopen " + path + " failed: " + DynamicLoaderError());
        }
        init_ = LoadSymbol<InitFn>("dcmiv2_init");
        getLogical_ = LoadSymbol<GetLogicalFn>("dcmiv2_get_dev_id_by_chip_phy_id");
        if (getLogical_ == nullptr) {
            getLogical_ = LoadSymbol<GetLogicalFn>("dcmiv2_get_dev_id_from_chip_phyid");
        }
        if (init_ == nullptr || getLogical_ == nullptr) {
            return Fail(phyDeviceId, -1, error, "dcmi_symbol", BM_DL_FUNCTION_FAILED, 0,
                        "required DCMI symbol is missing from " + path);
        }
        Info(phyDeviceId, -1, "dcmi_load", "loaded " + path);
        return true;
    }

    bool Initialize(uint32_t phyDeviceId, Result &error) const
    {
        int32_t ret = -1;
        for (uint32_t attempt = 0; attempt < 10U; ++attempt) {
            ret = init_();
            if (ret == 0) {
                return true;
            }
            if (attempt + 1U < 10U) {
                sleep(1);
            }
        }
        return Fail(phyDeviceId, -1, error, "dcmi_init", BM_DL_FUNCTION_FAILED, ret,
                    "dcmiv2_init failed after 10 attempts");
    }

    bool MapPhysicalToLogical(uint32_t phyDeviceId, uint32_t &logicalDeviceId, Result &error) const
    {
        const int32_t ret = getLogical_(phyDeviceId, &logicalDeviceId);
        if (ret != 0) {
            return Fail(phyDeviceId, -1, error, "physical_to_logical", BM_ERROR, ret,
                        "DCMI physical-to-logical mapping failed");
        }
        return true;
    }

private:
    template<typename Function>
    Function LoadSymbol(const char *name) const
    {
        return reinterpret_cast<Function>(dlsym(handle_, name));
    }

    void *handle_ = nullptr;
    InitFn init_ = nullptr;
    GetLogicalFn getLogical_ = nullptr;
};

class DsmiApi {
public:
    using GetDeviceInfoFn = int (*)(uint32_t, uint32_t, uint32_t, void *, uint32_t *);

    ~DsmiApi()
    {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    bool Load(uint32_t phyDeviceId, int32_t logicalDeviceId, Result &error)
    {
        const std::string path = DSMI_LIBRARY_NAME;
        handle_ = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (handle_ == nullptr) {
            return Fail(phyDeviceId, logicalDeviceId, error, "dsmi_load", BM_DL_FUNCTION_FAILED, 0,
                        "dlopen " + path + " failed: " + DynamicLoaderError());
        }
        getDeviceInfo_ = reinterpret_cast<GetDeviceInfoFn>(dlsym(handle_, "dsmi_get_device_info"));
        if (getDeviceInfo_ == nullptr) {
            return Fail(phyDeviceId, logicalDeviceId, error, "dsmi_symbol", BM_DL_FUNCTION_FAILED, 0,
                        "dsmi_get_device_info is missing from " + path);
        }
        Info(phyDeviceId, logicalDeviceId, "dsmi_load", "loaded " + path);
        return true;
    }

    bool GetUdmaName(uint32_t phyDeviceId, uint32_t logicalDeviceId, std::string &name, Result &error) const
    {
        char buffer[MAX_UDMA_NAME] = {};
        uint32_t bufferSize = static_cast<uint32_t>(sizeof(buffer));
        const int ret = getDeviceInfo_(logicalDeviceId, DCMI_DSMI_MAIN_COMMAND_UB, DCMI_DSMI_URMA_NAME_SUB_COMMAND,
                                       buffer, &bufferSize);
        if (ret != 0) {
            return Fail(phyDeviceId, static_cast<int32_t>(logicalDeviceId), error, "dsmi_udma", BM_ERROR, ret,
                        "dsmi_get_device_info(UB, URMA_DEV_NAME) failed");
        }
        const size_t nameLength = ::strnlen(buffer, sizeof(buffer));
        if (nameLength == sizeof(buffer)) {
            return Fail(phyDeviceId, static_cast<int32_t>(logicalDeviceId), error, "dsmi_udma", BM_ERROR, 0,
                        "DSMI returned a non-terminated UDMA name");
        }
        name.assign(buffer, nameLength);
        if (name.empty()) {
            return Fail(phyDeviceId, static_cast<int32_t>(logicalDeviceId), error, "dsmi_udma", BM_ERROR, 0,
                        "DSMI returned an empty UDMA name");
        }
        return true;
    }

private:
    void *handle_ = nullptr;
    GetDeviceInfoFn getDeviceInfo_ = nullptr;
};

std::string Trim(const std::string &value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1U]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool ReadLine(const std::string &path, std::string &value)
{
    std::ifstream input(path);
    if (!input.is_open() || !std::getline(input, value)) {
        return false;
    }
    value = Trim(value);
    return true;
}

bool ParseUint32(const std::string &text, uint32_t &value)
{
    if (text.empty() || text.front() == '-') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
    if (errno != 0 || end == text.c_str() || *end != '\0' || parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

std::string JoinPath(const std::string &parent, const std::string &child)
{
    if (!parent.empty() && parent.back() == '/') {
        return parent + child;
    }
    return parent + "/" + child;
}

bool IsSafeDeviceName(const std::string &name)
{
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (const char character : name) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' && character != '-' &&
            character != '.') {
            return false;
        }
    }
    return true;
}

void AppendUniquePath(const std::string &path, std::vector<std::string> &paths)
{
    for (const std::string &existing : paths) {
        if (existing == path) {
            return;
        }
    }
    paths.push_back(path);
}

void FindMatchingSysfsEntries(const std::string &root, const std::string &udma, std::vector<std::string> &matches)
{
    const std::string directPath = JoinPath(root, udma);
    std::string directName;
    if (access(directPath.c_str(), F_OK) == 0 &&
        (!ReadLine(JoinPath(directPath, "ubdev"), directName) || directName == udma)) {
        AppendUniquePath(directPath, matches);
    }
    DIR *directory = opendir(root.c_str());
    if (directory == nullptr) {
        return;
    }
    for (dirent *entry = readdir(directory); entry != nullptr; entry = readdir(directory)) {
        const std::string entryName(entry->d_name);
        if (!IsSafeDeviceName(entryName)) {
            continue;
        }
        const std::string entryPath = JoinPath(root, entryName);
        std::string ubdev;
        if (ReadLine(JoinPath(entryPath, "ubdev"), ubdev) && ubdev == udma) {
            AppendUniquePath(entryPath, matches);
        }
    }
    closedir(directory);
}

bool ResolveSysfsDevicePath(uint32_t phyDeviceId, int32_t logicalDeviceId, const std::string &udma,
                            std::string &devicePath, Result &error)
{
    if (!IsSafeDeviceName(udma)) {
        return Fail(phyDeviceId, logicalDeviceId, error, "host_eid_sysfs", BM_ERROR, 0,
                    "DSMI returned an unsafe UDMA name=" + udma);
    }
    const std::vector<std::string> roots = {UBCORE_SYSFS_ROOT, UBURMA_SYSFS_ROOT};
    for (const std::string &root : roots) {
        std::vector<std::string> matches;
        FindMatchingSysfsEntries(root, udma, matches);
        if (matches.size() == 1U) {
            devicePath = matches.front();
            return true;
        }
        if (matches.size() > 1U) {
            return Fail(phyDeviceId, logicalDeviceId, error, "host_eid_sysfs", BM_ERROR, 0,
                        "multiple UDMA sysfs devices match " + udma + " under " + root +
                            ", candidate_count=" + std::to_string(matches.size()));
        }
    }
    return Fail(phyDeviceId, logicalDeviceId, error, "host_eid_sysfs", BM_ERROR, 0,
                "UDMA sysfs device not found for " + udma);
}

bool ReadIndexedEid(const std::string &devicePath, uint32_t index, std::string &value)
{
    const std::string currentPath = JoinPath(devicePath, "eids/eid" + std::to_string(index));
    if (ReadLine(currentPath, value)) {
        return true;
    }
    const std::string legacyPath = JoinPath(devicePath, "eid" + std::to_string(index) + "/eid");
    return ReadLine(legacyPath, value);
}

bool ReadHostEids(uint32_t phyDeviceId, int32_t logicalDeviceId, const std::string &udma,
                  std::vector<std::string> &eids, Result &error)
{
    std::string devicePath;
    if (!ResolveSysfsDevicePath(phyDeviceId, logicalDeviceId, udma, devicePath, error)) {
        return false;
    }
    std::string countText;
    uint32_t maxEidCount = 0;
    const std::string countPath = JoinPath(devicePath, "max_eid_cnt");
    if (!ReadLine(countPath, countText) || !ParseUint32(countText, maxEidCount) || maxEidCount == 0U ||
        maxEidCount > MAX_HOST_EIDS) {
        return Fail(phyDeviceId, logicalDeviceId, error, "host_eid_sysfs", BM_ERROR, 0,
                    "invalid max_eid_cnt at " + countPath + " value=" + countText);
    }
    eids.clear();
    for (uint32_t index = 0; index < maxEidCount; ++index) {
        std::string value;
        std::string eid;
        if (!ReadIndexedEid(devicePath, index, value)) {
            continue;
        }
        if (!NormalizeEid(value, eid)) {
            continue;
        }
        eids.push_back(std::move(eid));
    }
    if (eids.empty()) {
        return Fail(phyDeviceId, logicalDeviceId, error, "host_eid_sysfs", BM_ERROR, 0,
                    "no valid EIDs found under " + devicePath);
    }
    Info(phyDeviceId, logicalDeviceId, "host_eid_sysfs",
         "read UDMA=" + udma + " valid_eid_count=" + std::to_string(eids.size()));
    return true;
}

bool FindHostEid(uint32_t phyDeviceId, int32_t logicalDeviceId, const std::vector<std::string> &eids,
                 const std::string &udma, std::string &hostEid, Result &error)
{
    std::set<std::string> candidates;
    for (const std::string &item : eids) {
        EidByteInfo info;
        if (ParseEidByte(item, info) && info.isPg) {
            candidates.insert(item);
        }
    }
    if (candidates.empty()) {
        return Fail(phyDeviceId, logicalDeviceId, error, "host_eid_select", BM_ERROR, 0,
                    "no CPU PG EID found for UDMA " + udma);
    }
    if (candidates.size() > 1U) {
        return Fail(phyDeviceId, logicalDeviceId, error, "host_eid_select", BM_ERROR, 0,
                    "multiple CPU PG EIDs found for UDMA " + udma +
                        ", candidate_count=" + std::to_string(candidates.size()));
    }
    hostEid = *candidates.begin();
    return true;
}

bool Discover(uint32_t phyDeviceId, UrmaEidQueryResult &result, Result &error)
{
    DcmiApi dcmi;
    if (!dcmi.Load(phyDeviceId, error) || !dcmi.Initialize(phyDeviceId, error)) {
        return false;
    }
    if (!dcmi.MapPhysicalToLogical(phyDeviceId, result.logicalDeviceId, error)) {
        return false;
    }
    DsmiApi dsmi;
    if (!dsmi.Load(phyDeviceId, static_cast<int32_t>(result.logicalDeviceId), error) ||
        !dsmi.GetUdmaName(phyDeviceId, result.logicalDeviceId, result.udma, error)) {
        return false;
    }
    std::vector<std::string> hostEids;
    if (!ReadHostEids(phyDeviceId, static_cast<int32_t>(result.logicalDeviceId), result.udma, hostEids, error)) {
        return false;
    }
    return FindHostEid(phyDeviceId, static_cast<int32_t>(result.logicalDeviceId), hostEids, result.udma, result.hostEid,
                       error);
}

std::mutex &QueryMutex()
{
    static std::mutex mutex;
    return mutex;
}

Result QueryHostUrmaEidImpl(uint32_t phyDeviceId, std::string &hostEid)
{
    std::lock_guard<std::mutex> lock(QueryMutex());
    UrmaEidQueryResult localResult;
    Result error = 0;
    if (!Discover(phyDeviceId, localResult, error)) {
        return error;
    }
    hostEid = std::move(localResult.hostEid);
    return BM_OK;
}

} // namespace

Result QueryHostUrmaEid(uint32_t phyDeviceId, std::string &hostEid)
{
    return QueryHostUrmaEidImpl(phyDeviceId, hostEid);
}

} // namespace mf
} // namespace ock
