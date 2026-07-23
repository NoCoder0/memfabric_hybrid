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

#ifndef MF_HYBRID_URMA_TRANSPORT_COMMON_H
#define MF_HYBRID_URMA_TRANSPORT_COMMON_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "dl_hcomm_api.h"
#include "hybm_transport_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace urma {

constexpr uint32_t URMA_PRIVATE_DATA_MAGIC = 0xA5FAC003U;
constexpr uint16_t URMA_PRIVATE_DATA_VERSION = 2U;

enum UrmaProtocol {
    RESERVED = -1,
    HCCS = 0,
    ROCE = 1,
    PCIE = 2,
    SIO = 3,
    UBC_CTP = 4,
    UBC_TP = 5,
    UB_MEM = 6,
    UBOE = 7,
};

struct UrmaEndpointDesc {
    UrmaProtocol protocol{UrmaProtocol::RESERVED};
    CommAddrType type{COMM_ADDR_TYPE_RESERVED};
    uint8_t raws[URMA_ENDPOINT_RAW_LEN]{};
    EndpointLoc loc{};
};

struct UrmaPrivateDataDesc {
    uint32_t magic{URMA_PRIVATE_DATA_MAGIC};
    uint16_t version{URMA_PRIVATE_DATA_VERSION};
    uint16_t payloadLen{0};
};

static_assert(std::is_trivially_copyable<UrmaEndpointDesc>::value,
              "UrmaEndpointDesc must be trivially copyable for serialization");
static_assert(std::is_trivially_copyable<UrmaPrivateDataDesc>::value,
              "UrmaPrivateDataDesc must be trivially copyable for serialization");
static_assert(sizeof(EndpointLoc) == 64U, "HCOMM EndpointLoc ABI size changed");
static_assert(sizeof(UrmaEndpointDesc) == 108U, "UrmaEndpointDesc wire ABI size changed");
static_assert(sizeof(UrmaPrivateDataDesc) == 8U, "UrmaPrivateDataDesc wire ABI size changed");
static_assert(sizeof(UrmaPrivateDataDesc) + sizeof(UrmaEndpointDesc) <= sizeof(TransportPrivateData{}.key.keys),
              "UrmaEndpointDesc cannot fit into TransportPrivateData.key");

Result SerializeUrmaPrivateData(const UrmaEndpointDesc &endpoint, TransportPrivateData &privateData);
Result ParseUrmaPrivateData(const TransportPrivateData &privateData, UrmaEndpointDesc &endpoint);

} // namespace urma
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_URMA_TRANSPORT_COMMON_H
