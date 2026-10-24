/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#ifndef MF_HCOMM_TRANSPORT_LAYOUT_H
#define MF_HCOMM_TRANSPORT_LAYOUT_H
#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include "hcomm_api_wrapper.h"
#include "hybm_logger.h"
namespace ock {
namespace mf {
namespace transport {
namespace device {
constexpr uint32_t RDMA_HCOMM_PRIVATE_DATA_MAGIC = 0xA5FAD001U;
// DEVICE 形态保持 master 的 v1；A5(950) host RoCE 用 v2（额外交换 flagAddr），两者互不影响。
constexpr uint16_t RDMA_HCOMM_PRIVATE_DATA_VERSION = 1U;
constexpr uint16_t RDMA_HCOMM_PRIVATE_DATA_VERSION_HOST = 2U;

// TransportMemoryKey slot 2 metadata layout (like device_urma's UrmaExportDesc):
// keys[TRANS_KEY_DEV_SLOT2 .. TRANS_KEY_DEV_SLOT3-1] accessed via HcommKeyMeta struct
// Writing: auto &meta = HcommKeyMetaAt(key); meta.addr = ...;
// Reading: const auto &meta = HcommKeyMetaAt(key); ... = meta.addr;
// A zero-capacity peer exports its existing completion word once, as control information only.
constexpr uint64_t kHcommControlKey = 0x48434F4D4354524CULL;
struct HcommKeyMeta {
    uint64_t unused{0};  // +0 (reserved)
    uint64_t addr{0};    // +1 peer DVA
    uint64_t size{0};    // +2 peer size
    uint64_t descLen{0}; // +3 HCOMM desc length
    uint64_t flagLen{0}; // +4 flag desc length
    uint64_t gvaBase{0}; // +5 GVA base
};
static_assert(sizeof(HcommKeyMeta) <= KEY_SIZE * sizeof(uint64_t), "HcommKeyMeta must fit in one slot");

inline HcommKeyMeta &HcommKeyMetaAt(TransportMemoryKey &key)
{
    return *reinterpret_cast<HcommKeyMeta *>(&key.keys[TRANS_KEY_DEV_SLOT2]);
}
inline const HcommKeyMeta &HcommKeyMetaAt(const TransportMemoryKey &key)
{
    return *reinterpret_cast<const HcommKeyMeta *>(&key.keys[TRANS_KEY_DEV_SLOT2]);
}
// Guarantee the payload is large enough for the slot-2 metadata (HcommKeyMetaAt reads at TRANS_KEY_DEV_SLOT2).
inline void EnsureHcommKeyMeta(TransportMemoryKey &key)
{
    if (key.keys.size() < TRANS_KEY_DEV_SLOT2 + KEY_SIZE) {
        key.ResizeWords(TRANS_KEY_DEV_SLOT2 + KEY_SIZE);
    }
}

// Slot 3: flag descriptor data
constexpr uint32_t HCOMM_KEY_FLAG_SLOT = TRANS_KEY_DEV_SLOT3;

// Fallback positions in other slots (backward compat with older transport layouts)
constexpr uint32_t HCOMM_KEY_LEGACY_ADDR_IDX = TRANS_KEY_DEV_SLOT1 + (KEY_SIZE - 3);
constexpr uint32_t HCOMM_KEY_LEGACY_SIZE_IDX = TRANS_KEY_DEV_SLOT1 + (KEY_SIZE - 2);
constexpr uint32_t HCOMM_KEY_HOST_ADDR_IDX = TRANS_KEY_HOST_SLOT;
constexpr uint32_t HCOMM_KEY_HOST_SIZE_IDX = TRANS_KEY_HOST_SLOT + 1;

struct RdmaHcommPrivateData {
    uint32_t magic{RDMA_HCOMM_PRIVATE_DATA_MAGIC};
    uint16_t version{RDMA_HCOMM_PRIVATE_DATA_VERSION};
    uint16_t payloadLen{0};
    CommProtocol protocol{COMM_PROTOCOL_RESERVED};
    CommAddrType addrType{COMM_ADDR_TYPE_RESERVED};
    uint8_t addrBytes[36]{};
    uint32_t devPhyId{0};
    uint32_t sdid{0};
    uint32_t listenPort{0}; // HCOMM endpoint listen port for channel creation
};
static_assert(std::is_trivially_copyable<RdmaHcommPrivateData>::value,
              "RdmaHcommPrivateData must be trivially copyable for byte serialization");

// A5(950) host RoCE 专用私有数据（v2）：在 v1 基础上增加本端 transfer flag 的注册地址。
// 对端直接拿它当 RDMA 远端地址做一次读，不走 memory key 描述符 + MemImport。
struct RdmaHcommPrivateDataHost {
    uint32_t magic{RDMA_HCOMM_PRIVATE_DATA_MAGIC};
    uint16_t version{RDMA_HCOMM_PRIVATE_DATA_VERSION_HOST};
    uint16_t payloadLen{0};
    CommProtocol protocol{COMM_PROTOCOL_RESERVED};
    CommAddrType addrType{COMM_ADDR_TYPE_RESERVED};
    uint8_t addrBytes[36]{};
    uint32_t devPhyId{0};
    uint32_t sdid{0};
    uint32_t listenPort{0};
    uint64_t flagAddr{0};
};
static_assert(std::is_trivially_copyable<RdmaHcommPrivateDataHost>::value,
              "RdmaHcommPrivateDataHost must be trivially copyable for byte serialization");

// v1/v2 归一化后的对端私有信息，供 CreatePeerResources 使用（DEVICE 形态 flagAddr=0）。
struct RdmaHcommPeerPrivateInfo {
    CommAddrType addrType{COMM_ADDR_TYPE_RESERVED};
    uint8_t addrBytes[36]{};
    uint32_t devPhyId{0};
    uint32_t sdid{0};
    uint32_t listenPort{0};
    uint64_t flagAddr{0};
};

// C d336953a exports the original EndpointDesc at the end of the actual MR descriptor.
// This is an explicit ABI dependency; never infer an endpoint from the padded key size.
static_assert(sizeof(EndpointDesc) == 160, "C MR trailer requires the 160-byte EndpointDesc ABI");
inline bool HcommRangeContains(uint64_t base, uint64_t length, uint64_t addr, uint64_t size)
{
    return base != 0 && length != 0 && length <= UINT64_MAX - base && addr >= base && addr - base <= length &&
           size <= length - (addr - base);
}

inline Result BuildHcommControlKey(const void *desc, uint32_t length, uint64_t address, uint64_t size,
                                   TransportMemoryKey &key)
{
    BM_VALIDATE_RETURN(
        desc != nullptr && length > sizeof(EndpointDesc) && length <= TRANS_KEY_DEV_SLOT2 * sizeof(uint64_t) &&
            size == sizeof(uint64_t) && HcommRangeContains(address, size, address, size),
        "Invalid HCOMM control buffer export, descriptor bytes: " << length << ", buffer bytes: " << size,
        BM_INVALID_PARAM);
    key.keys.assign(HCOMM_KEY_FLAG_SLOT, 0);
    std::copy_n(static_cast<const uint8_t *>(desc), length, key.ByteData());
    auto &meta = HcommKeyMetaAt(key);
    meta.unused = kHcommControlKey;
    meta.addr = address;
    meta.size = size;
    meta.gvaBase = address;
    meta.descLen = length;
    return BM_OK;
}

inline Result ReadHcommPrivateData(const TransportMemoryKey &key, RdmaHcommPeerPrivateInfo &peer)
{
    RdmaHcommPrivateData data{};
    BM_VALIDATE_RETURN(ReadStructFromKey(key, data) && data.magic == RDMA_HCOMM_PRIVATE_DATA_MAGIC &&
                           data.protocol == COMM_PROTOCOL_ROCE &&
                           (data.addrType == COMM_ADDR_TYPE_IP_V4 || data.addrType == COMM_ADDR_TYPE_IP_V6),
                       "Invalid HCOMM private data header, bytes: " << key.ByteSize(), BM_INVALID_PARAM);
    size_t payloadSize = sizeof(data);
    uint64_t flagAddr = 0;
    if (data.version == RDMA_HCOMM_PRIVATE_DATA_VERSION_HOST) {
        RdmaHcommPrivateDataHost host{};
        BM_VALIDATE_RETURN(ReadStructFromKey(key, host), "Truncated HCOMM v2 private data", BM_INVALID_PARAM);
        payloadSize = sizeof(host);
        flagAddr = host.flagAddr;
    } else {
        BM_VALIDATE_RETURN(data.version == RDMA_HCOMM_PRIVATE_DATA_VERSION,
                           "Unsupported HCOMM private data version: " << data.version, BM_INVALID_PARAM);
    }
    // Existing v1/v2 writers leave payloadLen zero. Also accept an explicit structure length.
    BM_VALIDATE_RETURN((data.payloadLen == 0 || data.payloadLen == payloadSize) && data.listenPort <= UINT16_MAX,
                       "Invalid HCOMM private data length or port", BM_INVALID_PARAM);
    peer.addrType = data.addrType;
    std::memcpy(peer.addrBytes, data.addrBytes, sizeof(peer.addrBytes));
    peer.devPhyId = data.devPhyId;
    peer.sdid = data.sdid;
    peer.listenPort = data.listenPort;
    peer.flagAddr = flagAddr;
    return BM_OK;
}

inline bool SameHcommPrivateData(const RdmaHcommPeerPrivateInfo &left, const RdmaHcommPeerPrivateInfo &right)
{
    return left.addrType == right.addrType && left.devPhyId == right.devPhyId && left.sdid == right.sdid &&
           left.listenPort == right.listenPort && left.flagAddr == right.flagAddr &&
           std::memcmp(left.addrBytes, right.addrBytes, sizeof(left.addrBytes)) == 0;
}

inline Result ReadHcommEndpoint(const uint8_t *desc, uint64_t length, uint64_t available,
                                const RdmaHcommPeerPrivateInfo &peer, EndpointDesc &endpoint)
{
    BM_VALIDATE_RETURN(desc != nullptr && length > sizeof(endpoint) && length <= available && length <= UINT32_MAX,
                       "Invalid HCOMM MR descriptor length: " << length << ", available bytes: " << available,
                       BM_INVALID_PARAM);
    std::memcpy(&endpoint, desc + length - sizeof(endpoint), sizeof(endpoint));
    BM_VALIDATE_RETURN(
        endpoint.protocol == COMM_PROTOCOL_ROCE &&
            (endpoint.loc.locType == ENDPOINT_LOC_TYPE_HOST || endpoint.loc.locType == ENDPOINT_LOC_TYPE_DEVICE) &&
            (endpoint.commAddr.type == COMM_ADDR_TYPE_IP_V4 || endpoint.commAddr.type == COMM_ADDR_TYPE_IP_V6),
        "Unsupported HCOMM MR endpoint protocol, location or address type", BM_INVALID_PARAM);
    const size_t addressSize = endpoint.commAddr.type == COMM_ADDR_TYPE_IP_V4 ? sizeof(in_addr) : sizeof(in6_addr);
    BM_VALIDATE_RETURN(endpoint.commAddr.type == peer.addrType &&
                           std::memcmp(endpoint.commAddr.raws, peer.addrBytes, addressSize) == 0,
                       "HCOMM MR endpoint disagrees with private data address", BM_INVALID_PARAM);
    if (endpoint.loc.locType == ENDPOINT_LOC_TYPE_DEVICE) {
        BM_VALIDATE_RETURN(endpoint.loc.device.devPhyId == peer.devPhyId && endpoint.loc.device.superDevId == peer.sdid,
                           "HCOMM MR endpoint disagrees with private data device identity", BM_INVALID_PARAM);
    }
    return BM_OK;
}

struct HcommKeyView {
    HcommKeyMeta meta{};
    EndpointDesc endpoint{};
    const uint8_t *data{nullptr};
    const uint8_t *flag{nullptr};
};

inline Result ReadHcommKey(const TransportMemoryKey &key, const RdmaHcommPeerPrivateInfo &peer, bool requireFlag,
                           HcommKeyView &view)
{
    constexpr size_t metaOffset = TRANS_KEY_DEV_SLOT2 * sizeof(uint64_t);
    constexpr size_t flagOffset = HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t);
    BM_VALIDATE_RETURN(key.ByteSize() >= flagOffset && key.WordCount() <= TRANS_KEY_MAX_WORDS,
                       "Truncated HCOMM memory key", BM_INVALID_PARAM);
    std::memcpy(&view.meta, key.ByteData() + metaOffset, sizeof(view.meta));
    const auto &meta = view.meta;
    BM_VALIDATE_RETURN(HcommRangeContains(meta.addr, meta.size, meta.addr, meta.size) &&
                           HcommRangeContains(meta.gvaBase, meta.size, meta.gvaBase, meta.size),
                       "Invalid HCOMM memory key address range", BM_INVALID_PARAM);
    view.data = key.ByteData();
    auto ret = ReadHcommEndpoint(view.data, meta.descLen, metaOffset, peer, view.endpoint);
    if (ret != BM_OK) {
        return ret;
    }
    const bool control_only = meta.unused == kHcommControlKey;
    BM_VALIDATE_RETURN(!control_only ||
                           (meta.size == sizeof(uint64_t) && meta.flagLen == 0 && meta.gvaBase == meta.addr),
                       "Invalid HCOMM control key shape, buffer bytes: " << meta.size, BM_INVALID_PARAM);
    BM_VALIDATE_RETURN(!requireFlag || meta.flagLen != 0 || control_only, "Missing HCOMM flag MR", BM_INVALID_PARAM);
    if (meta.flagLen != 0) {
        EndpointDesc flagEndpoint{};
        view.flag = key.ByteData() + flagOffset;
        ret = ReadHcommEndpoint(view.flag, meta.flagLen, key.ByteSize() - flagOffset, peer, flagEndpoint);
        if (ret != BM_OK) {
            return ret;
        }
        BM_VALIDATE_RETURN(std::memcmp(&view.endpoint, &flagEndpoint, sizeof(flagEndpoint)) == 0,
                           "HCOMM data and flag MR endpoints differ", BM_INVALID_PARAM);
    }
    return BM_OK;
}

// First deployment uses one fixed remote pool per peer. Reject expansion explicitly;
// refreshing an AICPU snapshot does not exchange a newly allocated remote pool's keys.
struct HcommPeerMemory {
    EndpointDesc remoteEndpointDesc{};
    bool hasEndpointDesc{false};
    RdmaHcommPeerPrivateInfo peerData{};
    bool hasPeerData{false};
    bool failed{false};
    uint64_t remoteFlagAddr{0};
    uint64_t remoteFlagSize{0};
    uint64_t peerDva{0};
    uint64_t peerDvaSize{0};
    uint64_t peerGva{0};
    std::vector<std::vector<uint8_t>> importedDescs{};
};

inline Result ValidateHcommPeerKeys(const std::vector<TransportMemoryKey> &keys, const HcommPeerMemory &state,
                                    bool requireFlag, HcommKeyView &selected)
{
    for (const auto &key : keys) {
        if (key.Empty()) {
            continue;
        }
        HcommKeyView view{};
        auto ret = ReadHcommKey(key, state.peerData, requireFlag, view);
        if (ret != BM_OK) {
            return ret;
        }
        const EndpointDesc *previous = selected.data != nullptr
                                           ? &selected.endpoint
                                           : (state.hasEndpointDesc ? &state.remoteEndpointDesc : nullptr);
        BM_VALIDATE_RETURN(previous == nullptr || std::memcmp(previous, &view.endpoint, sizeof(*previous)) == 0,
                           "Inconsistent HCOMM endpoint across peer MRs", BM_INVALID_PARAM);
        if (selected.data != nullptr) {
            BM_VALIDATE_RETURN(
                selected.meta.descLen == view.meta.descLen &&
                    std::memcmp(selected.data, view.data, view.meta.descLen) == 0 &&
                    selected.meta.addr == view.meta.addr && selected.meta.gvaBase == view.meta.gvaBase &&
                    selected.meta.size == view.meta.size && selected.meta.flagLen == view.meta.flagLen &&
                    (view.meta.flagLen == 0 || std::memcmp(selected.flag, view.flag, view.meta.flagLen) == 0),
                "Only one fixed remote HCOMM pool per peer is supported", BM_INVALID_PARAM);
        }
        selected = view;
    }
    if (selected.data != nullptr && state.hasEndpointDesc) {
        BM_VALIDATE_RETURN(
            !state.importedDescs.empty() && state.importedDescs[0].size() == selected.meta.descLen &&
                std::memcmp(state.importedDescs[0].data(), selected.data, selected.meta.descLen) == 0 &&
                state.peerGva == selected.meta.gvaBase && state.peerDvaSize == selected.meta.size &&
                (selected.meta.flagLen == 0
                     ? state.importedDescs.size() == 1
                     : (state.importedDescs.size() == 2 && state.importedDescs[1].size() == selected.meta.flagLen &&
                        std::memcmp(state.importedDescs[1].data(), selected.flag, selected.meta.flagLen) == 0)),
            "Remote HCOMM pool changed; remove the peer before replacing its pool", BM_INVALID_PARAM);
    }
    return BM_OK;
}

inline Result ImportHcommPeerKeys(EndpointHandle endpoint, const std::vector<TransportMemoryKey> &keys,
                                  bool requireFlag, HcommPeerMemory &state)
{
    HcommKeyView view{};
    auto ret = ValidateHcommPeerKeys(keys, state, requireFlag, view);
    if (ret != BM_OK || view.data == nullptr || state.hasEndpointDesc) {
        return ret;
    }
    std::vector<std::vector<uint8_t>> imported;
    HcommCommMem data{}, flag{};
    auto importOne = [&](const uint8_t *desc, uint32_t len, HcommCommMem &mem) {
        auto result = HcommApiWrapper::RawMemImport(endpoint, desc, len, mem);
        if (result != BM_OK) {
            BM_LOG_ERROR("HcommMemImport failed, descriptor bytes: " << len << ", ret: " << result);
            return result;
        }
        imported.emplace_back(desc, desc + len);
        BM_VALIDATE_RETURN((mem.type == COMM_MEM_TYPE_HOST || mem.type == COMM_MEM_TYPE_DEVICE) &&
                               HcommRangeContains(reinterpret_cast<uint64_t>(mem.addr), mem.size,
                                                  reinterpret_cast<uint64_t>(mem.addr), mem.size),
                           "HcommMemImport returned an invalid memory range", BM_INVALID_PARAM);
        return BM_OK;
    };
    ret = importOne(view.data, static_cast<uint32_t>(view.meta.descLen), data);
    if (ret == BM_OK && data.size != view.meta.size) {
        BM_LOG_ERROR("HcommMemImport size differs from memory key, bytes: " << data.size);
        ret = BM_INVALID_PARAM;
    }
    if (ret == BM_OK && view.meta.unused == kHcommControlKey) {
        flag = data;
    }
    if (ret == BM_OK && view.meta.flagLen != 0) {
        ret = importOne(view.flag, static_cast<uint32_t>(view.meta.flagLen), flag);
        if (ret == BM_OK && flag.size < sizeof(uint64_t)) {
            BM_LOG_ERROR("HcommMemImport flag is too small, bytes: " << flag.size);
            ret = BM_INVALID_PARAM;
        }
    }
    if (ret != BM_OK) {
        for (auto it = imported.rbegin(); it != imported.rend(); ++it) {
            auto undo = HcommApiWrapper::RawMemUnimport(endpoint, it->data(), static_cast<uint32_t>(it->size()));
            if (undo != BM_OK) {
                BM_LOG_ERROR("HcommMemUnimport rollback failed, ret: " << undo);
                state.failed = true;
                state.importedDescs.push_back(*it);
            }
        }
        return ret;
    }
    state.importedDescs = std::move(imported);
    state.peerDva = reinterpret_cast<uint64_t>(data.addr);
    state.peerDvaSize = data.size;
    state.peerGva = view.meta.gvaBase;
    state.remoteFlagAddr = reinterpret_cast<uint64_t>(flag.addr);
    state.remoteFlagSize = flag.size;
    state.remoteEndpointDesc = view.endpoint;
    state.hasEndpointDesc = true;
    return BM_OK;
}
} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
#endif
