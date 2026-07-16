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

#ifndef MF_HYBM_TOPO_READER_H
#define MF_HYBM_TOPO_READER_H

#include <array>
#include <cstdint>
#include <istream>
#include <string>

#include "dl_hcomm_api.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

struct RootInfo {
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
};

class TopoReader {
public:
    static constexpr const char *ROOTINFO_PATH = "/etc/hccl_rootinfo.json";

    static Result ParseRootInfo(uint32_t phyDeviceId, uint32_t rankId, RootInfo &rootInfo);

    static Result ParseRootInfo(const std::string &rootInfoPath, uint32_t phyDeviceId, uint32_t rankId,
                                RootInfo &rootInfo);

    static Result ParseRootInfoFile(const std::string &path, uint32_t phyDeviceId, uint32_t rankId, RootInfo &rootInfo);

    static Result ParseRootInfoStream(std::istream &input, uint32_t phyDeviceId, uint32_t rankId, RootInfo &rootInfo);
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_TOPO_READER_H
