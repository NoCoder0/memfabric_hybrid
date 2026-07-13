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

// ---------- ParseRootInfoStream ----------

Result TopoReader::ParseRootInfoStream(std::istream &input, uint32_t phyDeviceId, uint32_t rankId, RootInfo &rootInfo)
{
    std::string buf;
    constexpr size_t kReadSize = MAX_INPUT_BYTES_PLUS_1;
    try {
        buf.resize(kReadSize);
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("TopoReader: OOM phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    input.read(buf.data(), static_cast<std::streamsize>(kReadSize));
    const auto got = static_cast<size_t>(input.gcount());
    if (got > MAX_INPUT_BYTES) {
        BM_LOG_ERROR("TopoReader: input exceeds 1MiB, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    if (input.bad()) {
        BM_LOG_ERROR("TopoReader: stream read failed (badbit), phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_FILE_NOT_ACCESS;
    }
    if (got == 0) {
        BM_LOG_ERROR("TopoReader: empty stream, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    const char *data = buf.data();
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    Result ret = ParseRootInfoEid(data, data + got, phyDeviceId, rankId, eid);
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
    return ParseRootInfoFile(ROOTINFO_PATH, phyDeviceId, rankId, rootInfo);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
