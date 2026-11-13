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

#include "hybm_urma_common.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ock {
namespace mf {
namespace transport {
namespace urma {

std::ostream &operator<<(std::ostream &stream, UrmaProtocol protocol)
{
    switch (protocol) {
        case UrmaProtocol::RESERVED:
            return stream << "RESERVED";
        case UrmaProtocol::HCCS:
            return stream << "HCCS";
        case UrmaProtocol::ROCE:
            return stream << "ROCE";
        case UrmaProtocol::PCIE:
            return stream << "PCIE";
        case UrmaProtocol::SIO:
            return stream << "SIO";
        case UrmaProtocol::UBC_CTP:
            return stream << "UBC_CTP";
        case UrmaProtocol::UBC_TP:
            return stream << "UBC_TP";
        case UrmaProtocol::UB_MEM:
            return stream << "UB_MEM";
        case UrmaProtocol::UBOE:
            return stream << "UBOE";
        case UrmaProtocol::UB_RTP:
            return stream << "UB_RTP";
        default:
            return stream << "UNKNOWN";
    }
}

std::string UrmaEndpointSessionName(const UrmaEndpointDesc &desc)
{
    return std::to_string(desc.superPodIdx) + ":" + std::to_string(desc.serverIdx) + ":" +
           std::to_string(desc.superDevId) + ":" + std::to_string(desc.devPhyId) + ":" +
           std::to_string(desc.processId) + ":" + std::to_string(desc.instanceId);
}

std::string UrmaChannelSessionName(const UrmaEndpointDesc &local, const UrmaEndpointDesc &remote)
{
    const auto localName = UrmaEndpointSessionName(local);
    const auto remoteName = UrmaEndpointSessionName(remote);
    return "mf-" + std::min(localName, remoteName) + "-" + std::max(localName, remoteName);
}

CommProtocol ToHcommProtocol(UrmaProtocol protocol)
{
    if (protocol == UrmaProtocol::ROCE) {
        return COMM_PROTOCOL_ROCE;
    }
    if (protocol == UrmaProtocol::UBC_TP) {
        return COMM_PROTOCOL_UBC_TP;
    }
    if (protocol == UrmaProtocol::UBC_CTP) {
        return COMM_PROTOCOL_UBC_CTP;
    }
    if (protocol == UrmaProtocol::UBOE) {
        return COMM_PROTOCOL_UBOE;
    }
    if (protocol == UrmaProtocol::UB_RTP) {
        return COMM_PROTOCOL_UB_RTP;
    }
    return COMM_PROTOCOL_RESERVED;
}

bool GetRangeEnd(const UrmaCommMem &mem, uint64_t &end)
{
    if (mem.addr == 0 || mem.size == 0) {
        return false;
    }
    if (std::numeric_limits<uint64_t>::max() - mem.addr < mem.size) {
        return false;
    }
    end = mem.addr + mem.size;
    return true;
}

EndpointDesc ToHcommEndpointDesc(const UrmaEndpointDesc &desc)
{
    EndpointDesc endpoint{};
    endpoint.protocol = ToHcommProtocol(desc.protocol);
    endpoint.commAddr.type = desc.type;
    std::memcpy(endpoint.commAddr.raws, desc.raws, sizeof(endpoint.commAddr.raws));
    endpoint.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
    endpoint.loc.device.devPhyId = desc.devPhyId;
    endpoint.loc.device.superDevId = desc.superDevId;
    endpoint.loc.device.serverIdx = desc.serverIdx;
    endpoint.loc.device.superPodIdx = desc.superPodIdx;
    return endpoint;
}

HcommCommMem ToHcommMem(const UrmaCommMem &mem)
{
    HcommCommMem hcommMem{};
    hcommMem.type = COMM_MEM_TYPE_HOST;
    if (mem.type == UrmaMemoryType::DEVICE_HBM) {
        hcommMem.type = COMM_MEM_TYPE_DEVICE;
    }
    hcommMem.addr = reinterpret_cast<void *>(mem.addr);
    hcommMem.size = mem.size;
    return hcommMem;
}

} // namespace urma
} // namespace transport
} // namespace mf
} // namespace ock
