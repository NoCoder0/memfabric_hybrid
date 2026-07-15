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

#ifndef MF_HYBM_DEVICE_URMA_EID_READER_H
#define MF_HYBM_DEVICE_URMA_EID_READER_H

#include <cstdint>
#include <array>
#include <string>

#include "dl_hcomm_api.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

Result GetDeviceUrmaEid(uint32_t phyDeviceId, uint32_t rankId, std::array<uint8_t, COMM_ADDR_EID_LEN> &eidData);

Result GetDeviceUrmaIpAddr(uint32_t phyDeviceId, uint32_t rankId, CommAddrType &addrType,
                           std::array<uint8_t, URMA_ENDPOINT_RAW_LEN> &addrData);

#ifdef UT_ENABLED
// Internal seam for UT: inject mock hccn_tool path and temp /etc/hccn.conf.
// Production wrapper calls with default paths; tests inject temp paths.
Result GetDeviceUrmaIpAddrFromSources(const std::string &toolPath, const std::string &configPath, uint32_t phyDeviceId,
                                      uint32_t rankId, CommAddrType &addrType,
                                      std::array<uint8_t, URMA_ENDPOINT_RAW_LEN> &addrData);
#endif

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_DEVICE_URMA_EID_READER_H
