/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <array>
#include <cstdint>
#include <fstream>
#include <string>

#include "hybm_logger.h"
#include "hybm_types.h"
#include "rootinfo_parser.h"
#include "topo_reader.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {

Result ReadRootInfoStream(std::istream &input, uint32_t phyDeviceId, uint32_t rankId, std::string &buf)
{
    try {
        buf.resize(MAX_INPUT_BYTES_PLUS_1);
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("TopoReader: OOM reading rootinfo, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    input.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const size_t got = static_cast<size_t>(input.gcount());
    if (got > MAX_INPUT_BYTES) {
        BM_LOG_ERROR("TopoReader: rootinfo exceeds 1MiB, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    if (input.bad()) {
        BM_LOG_ERROR("TopoReader: rootinfo read failed, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_FILE_NOT_ACCESS;
    }
    if (got == 0) {
        BM_LOG_ERROR("TopoReader: empty rootinfo, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    buf.resize(got);
    return BM_OK;
}

Result ReadTopoFile(const std::string &path, std::string &buf)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        BM_LOG_ERROR("TopoReader: cannot open topo file=" << path);
        return BM_FILE_NOT_ACCESS;
    }
    try {
        buf.resize(MAX_INPUT_BYTES_PLUS_1);
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("TopoReader: OOM topo file=" << path);
        return BM_INVALID_PARAM;
    }
    input.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const size_t got = static_cast<size_t>(input.gcount());
    if (got > MAX_INPUT_BYTES) {
        BM_LOG_ERROR("TopoReader: topology file exceeds 1MiB, path=" << path << " size=" << got);
        return BM_INVALID_PARAM;
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        BM_LOG_ERROR("TopoReader: topology read failed, path=" << path);
        return BM_FILE_NOT_ACCESS;
    }
    buf.resize(got);
    if (buf.empty()) {
        BM_LOG_ERROR("TopoReader: empty topo file, path=" << path);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result ResolveExpectedPortCount(const std::string &topoPath, const std::string &topoBuf, size_t &expectedPortCount)
{
    if (topoBuf.find("Atlas 850") != std::string::npos) {
        expectedPortCount = 8U;
        return BM_OK;
    }
    if (topoBuf.find("Atlas 950 SuperPoD") != std::string::npos) {
        expectedPortCount = 6U;
        return BM_OK;
    }
    BM_LOG_ERROR("TopoReader: hardware token not found in topology, topo_path=" << topoPath);
    return BM_INVALID_PARAM;
}

Result ExtractTopoFilePath(const std::string &rootBuf, uint32_t phyDeviceId, uint32_t rankId, std::string &topoPath)
{
    constexpr const char *key = "\"topo_file_path\"";
    constexpr size_t keyLen = sizeof("\"topo_file_path\"") - 1U;
    const size_t keyPos = rootBuf.find(key);
    if (keyPos == std::string::npos) {
        BM_LOG_ERROR("TopoReader: topo_file_path missing, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    size_t valuePos = keyPos + keyLen;
    while (valuePos < rootBuf.size() && (rootBuf[valuePos] == ' ' || rootBuf[valuePos] == '\t' ||
                                         rootBuf[valuePos] == '\n' || rootBuf[valuePos] == '\r')) {
        ++valuePos;
    }
    if (valuePos >= rootBuf.size() || rootBuf[valuePos] != ':') {
        BM_LOG_ERROR("TopoReader: invalid topo_file_path separator, phyDeviceId=" << phyDeviceId
                                                                                  << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    ++valuePos;
    while (valuePos < rootBuf.size() && (rootBuf[valuePos] == ' ' || rootBuf[valuePos] == '\t' ||
                                         rootBuf[valuePos] == '\n' || rootBuf[valuePos] == '\r')) {
        ++valuePos;
    }
    if (valuePos >= rootBuf.size() || rootBuf[valuePos] != '"') {
        BM_LOG_ERROR("TopoReader: topo_file_path is not a string, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    const size_t valueEnd = rootBuf.find('"', valuePos + 1U);
    if (valueEnd == std::string::npos || valueEnd == valuePos + 1U) {
        BM_LOG_ERROR("TopoReader: invalid topo_file_path value, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    topoPath.assign(rootBuf, valuePos + 1U, valueEnd - valuePos - 1U);
    return BM_OK;
}

Result ParseRootInfoBuffer(const std::string &rootBuf, uint32_t phyDeviceId, uint32_t rankId, RootInfo &rootInfo)
{
    std::string topoPath;
    Result ret = ExtractTopoFilePath(rootBuf, phyDeviceId, rankId, topoPath);
    if (ret != BM_OK) {
        return ret;
    }
    std::string topoBuf;
    ret = ReadTopoFile(topoPath, topoBuf);
    if (ret != BM_OK) {
        return ret;
    }
    size_t expectedPortCount = 0;
    ret = ResolveExpectedPortCount(topoPath, topoBuf, expectedPortCount);
    if (ret != BM_OK) {
        return ret;
    }
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    ret =
        ParseRootInfoEid(rootBuf.data(), rootBuf.data() + rootBuf.size(), phyDeviceId, rankId, eid, expectedPortCount);
    if (ret != BM_OK) {
        return ret;
    }
    rootInfo.eid = eid;
    return BM_OK;
}

} // anonymous namespace

// ---------- ParseRootInfoStream ----------

Result TopoReader::ParseRootInfoStream(std::istream &input, uint32_t phyDeviceId, uint32_t rankId, RootInfo &rootInfo)
{
    std::string buf;
    Result ret = ReadRootInfoStream(input, phyDeviceId, rankId, buf);
    if (ret != BM_OK) {
        return ret;
    }
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    ret = ParseRootInfoEid(buf.data(), buf.data() + buf.size(), phyDeviceId, rankId, eid);
    if (ret != BM_OK) {
        return ret;
    }
    rootInfo.eid = eid;
    return BM_OK;
}

// ---------- ParseRootInfoFile ----------

Result TopoReader::ParseRootInfoFile(const std::string &path, uint32_t phyDeviceId, uint32_t rankId, RootInfo &rootInfo)
{
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        BM_LOG_ERROR("TopoReader: cannot open file=" << path << " phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_FILE_NOT_ACCESS;
    }
    return ParseRootInfoStream(ifs, phyDeviceId, rankId, rootInfo);
}

// ---------- ParseRootInfo ----------

Result TopoReader::ParseRootInfo(uint32_t phyDeviceId, uint32_t rankId, RootInfo &rootInfo)
{
    return ParseRootInfo(std::string(ROOTINFO_PATH), phyDeviceId, rankId, rootInfo);
}

Result TopoReader::ParseRootInfo(const std::string &rootInfoPath, uint32_t phyDeviceId, uint32_t rankId,
                                 RootInfo &rootInfo)
{
    std::ifstream input(rootInfoPath);
    if (!input.is_open()) {
        BM_LOG_ERROR("TopoReader: cannot open rootinfo file=" << rootInfoPath << " phyDeviceId=" << phyDeviceId
                                                              << " rankId=" << rankId);
        return BM_FILE_NOT_ACCESS;
    }
    std::string buf;
    Result ret = ReadRootInfoStream(input, phyDeviceId, rankId, buf);
    if (ret != BM_OK) {
        return ret;
    }
    return ParseRootInfoBuffer(buf, phyDeviceId, rankId, rootInfo);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
