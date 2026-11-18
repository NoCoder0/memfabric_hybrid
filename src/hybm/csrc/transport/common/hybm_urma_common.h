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

#ifndef MF_HYBM_URMA_COMMON_H
#define MF_HYBM_URMA_COMMON_H

#include <cstdint>
#include <ostream>
#include <string>
#include <type_traits>
#include "vendor/ascend/dl_hcomm_api.h"
#include "hybm_transport_common.h"

namespace ock {
namespace mf {
namespace transport {
namespace urma {

constexpr uint32_t URMA_EXPORT_DESC_MAGIC = 0xA5FAB001U;
constexpr uint16_t URMA_EXPORT_DESC_VERSION = 1U;
constexpr uint32_t DEVICE_URMA_MAX_EXPORT_KEY_LENGTH = KEY_SIZE * 6;
constexpr uint32_t DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS = 2;
constexpr uint32_t DEVICE_URMA_EXPORT_KEY_INFO = 1256;
constexpr uint32_t DEVICE_URMA_EXPORT_KEY_DATA_BYTES =
    (DEVICE_URMA_MAX_EXPORT_KEY_LENGTH - DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS + DEVICE_URMA_EXPORT_KEY_INFO) *
    sizeof(uint64_t);

using UrmaMemTag = uint64_t;

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
    UB_RTP = 9,
};

enum UrmaMemoryType : uint16_t {
    HOST_DRAM = 0,
    DEVICE_HBM = 1,
    INVALID_BUTT = 2,
};

struct UrmaEndpointDesc {
    uint32_t devPhyId{0};
    int32_t localId{0};
    int32_t remoteId{-1};
    uint32_t superDevId{0};
    uint32_t serverIdx{0};
    uint32_t superPodIdx{0};
    UrmaProtocol protocol{UrmaProtocol::RESERVED};
    CommAddrType type{COMM_ADDR_TYPE_RESERVED};
    uint8_t raws[URMA_ENDPOINT_RAW_LEN]{};
    uint32_t processId{0};
    uint64_t instanceId{0};
};

// Host endpoints use a separate wire version; the existing v2 device layout stays unchanged.
constexpr uint16_t URMA_HOST_PRIVATE_DATA_VERSION = 3U;
struct UrmaHostEndpointDesc {
    UrmaEndpointDesc endpoint{};
    uint32_t hostId{0};
};

struct UrmaCommMem {
    uint64_t addr{0};
    uint64_t size{0};
    UrmaMemoryType type{UrmaMemoryType::INVALID_BUTT};
};

struct UrmaExportDesc {
    uint32_t magic{URMA_EXPORT_DESC_MAGIC};
    uint16_t version{URMA_EXPORT_DESC_VERSION};
    uint16_t headerSize{0};
    int32_t remoteId{-1};
    UrmaMemoryType memoryType{UrmaMemoryType::INVALID_BUTT};
    UrmaMemTag memTag{0};
    uint64_t addr{0};
    uint64_t size{0};
    uint32_t hcommDescLen{0};
    uint32_t deviceMarkerDescLen{0};
};

// Must NOT conflict with URMA_EXPORT_DESC_MAGIC (0xA5FAB001).
constexpr uint32_t URMA_PRIVATE_DATA_MAGIC = 0xA5FAC003U;
constexpr uint16_t URMA_PRIVATE_DATA_VERSION = 2U;

struct UrmaPrivateDataDesc {
    uint32_t magic{URMA_PRIVATE_DATA_MAGIC};
    uint16_t version{URMA_PRIVATE_DATA_VERSION};
    uint16_t payloadLen{0};
};

std::ostream &operator<<(std::ostream &stream, UrmaProtocol protocol);
std::string UrmaEndpointSessionName(const UrmaEndpointDesc &desc);
std::string UrmaChannelSessionName(const UrmaEndpointDesc &local, const UrmaEndpointDesc &remote);
CommProtocol ToHcommProtocol(UrmaProtocol protocol);
EndpointDesc ToHcommEndpointDesc(const UrmaEndpointDesc &desc);
HcommCommMem ToHcommMem(const UrmaCommMem &mem);
bool GetRangeEnd(const UrmaCommMem &mem, uint64_t &end);

static_assert(std::is_trivially_copyable<UrmaEndpointDesc>::value, "endpoint must be binary serializable");
static_assert(std::is_trivially_copyable<UrmaHostEndpointDesc>::value, "host endpoint must be binary serializable");
static_assert(std::is_trivially_copyable<UrmaExportDesc>::value, "export descriptor must be binary serializable");

} // namespace urma
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_URMA_COMMON_H
