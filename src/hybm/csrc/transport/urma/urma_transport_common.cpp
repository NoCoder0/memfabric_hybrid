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

#include <cstring>

#include "hybm_logger.h"
#include "urma_transport_common.h"

namespace ock {
namespace mf {
namespace transport {
namespace urma {

namespace {
bool IsValidProtocol(UrmaProtocol protocol)
{
    return protocol == UrmaProtocol::ROCE || protocol == UrmaProtocol::UBC_CTP || protocol == UrmaProtocol::UBC_TP ||
           protocol == UrmaProtocol::UBOE;
}

bool IsValidAddressType(CommAddrType type)
{
    return type == COMM_ADDR_TYPE_EID || type == COMM_ADDR_TYPE_IP_V4 || type == COMM_ADDR_TYPE_IP_V6;
}

bool IsValidLocationType(EndpointLocType type)
{
    return type == ENDPOINT_LOC_TYPE_DEVICE || type == ENDPOINT_LOC_TYPE_HOST;
}

bool IsValidEndpoint(const UrmaEndpointDesc &endpoint)
{
    return IsValidProtocol(endpoint.protocol) && IsValidAddressType(endpoint.type) &&
           IsValidLocationType(endpoint.loc.locType);
}
} // namespace

Result SerializeUrmaPrivateData(const UrmaEndpointDesc &endpoint, TransportPrivateData &privateData)
{
    if (!IsValidEndpoint(endpoint)) {
        BM_LOG_ERROR("urma SerializeUrmaPrivateData invalid endpoint, protocol: "
                     << static_cast<int32_t>(endpoint.protocol) << ", addrType: " << endpoint.type
                     << ", locType: " << endpoint.loc.locType);
        return BM_INVALID_PARAM;
    }

    privateData = {};
    UrmaPrivateDataDesc header{};
    header.payloadLen = static_cast<uint16_t>(sizeof(UrmaEndpointDesc));
    auto *raw = reinterpret_cast<uint8_t *>(privateData.key.keys);
    std::memcpy(raw, &header, sizeof(header));
    std::memcpy(raw + sizeof(header), &endpoint, sizeof(endpoint));
    return BM_OK;
}

Result ParseUrmaPrivateData(const TransportPrivateData &privateData, UrmaEndpointDesc &endpoint)
{
    const auto *raw = reinterpret_cast<const uint8_t *>(privateData.key.keys);
    UrmaPrivateDataDesc header{};
    std::memcpy(&header, raw, sizeof(header));
    if (header.magic != URMA_PRIVATE_DATA_MAGIC || header.version != URMA_PRIVATE_DATA_VERSION ||
        header.payloadLen != sizeof(UrmaEndpointDesc) ||
        sizeof(header) + header.payloadLen > sizeof(privateData.key.keys)) {
        BM_LOG_ERROR("urma ParseUrmaPrivateData invalid header, magic: 0x"
                     << std::hex << header.magic << std::dec << ", version: " << header.version
                     << ", payloadLen: " << header.payloadLen << ", capacity: " << sizeof(privateData.key.keys));
        return BM_INVALID_PARAM;
    }

    UrmaEndpointDesc parsed{};
    std::memcpy(&parsed, raw + sizeof(header), sizeof(parsed));
    if (!IsValidEndpoint(parsed)) {
        BM_LOG_ERROR("urma ParseUrmaPrivateData invalid endpoint, protocol: " << static_cast<int32_t>(parsed.protocol)
                                                                              << ", addrType: " << parsed.type
                                                                              << ", locType: " << parsed.loc.locType);
        return BM_INVALID_PARAM;
    }
    endpoint = parsed;
    return BM_OK;
}

} // namespace urma
} // namespace transport
} // namespace mf
} // namespace ock
