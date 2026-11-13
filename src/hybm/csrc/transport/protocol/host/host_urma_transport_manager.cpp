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

#include <atomic>
#include <chrono>
#include <unistd.h>
#include "urma_topo_manager.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

#include "dl_acl_api.h"
#include "dl_hcomm_api.h"
#include "hybm_def.h"
#include "hybm_logger.h"
#include "hybm_urma_eid_query.h"
#include "hybm_va_manager.h"
#include "host_urma_transport_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace host {

namespace {

constexpr uint64_t HOST_TRANSFER_FLAG_SIZE = sizeof(uint64_t);

Result SerializeUrmaPrivateData(const UrmaEndpointDesc &desc, uint32_t hostId, TransportPrivateData &data)
{
    UrmaPrivateDataDesc header{};
    header.version = URMA_HOST_PRIVATE_DATA_VERSION;
    header.payloadLen = sizeof(UrmaHostEndpointDesc);
    const UrmaHostEndpointDesc host{desc, hostId};
    data.key.ResizeBytes(sizeof(header) + sizeof(host));
    std::memcpy(data.key.ByteData(), &header, sizeof(header));
    std::memcpy(data.key.ByteData() + sizeof(header), &host, sizeof(host));
    return BM_OK;
}

Result ParseUrmaPrivateData(const TransportPrivateData &privateData, UrmaEndpointDesc &outDesc)
{
    UrmaPrivateDataDesc header{};
    if (privateData.key.ByteSize() < sizeof(header)) {
        BM_LOG_ERROR("host_urma endpoint header is truncated");
        return BM_INVALID_PARAM;
    }
    std::memcpy(&header, privateData.key.ByteData(), sizeof(header));
    if (header.magic != URMA_PRIVATE_DATA_MAGIC || header.version != URMA_PRIVATE_DATA_VERSION ||
        header.payloadLen == 0 || header.payloadLen % sizeof(UrmaEndpointDesc) != 0 ||
        sizeof(header) + header.payloadLen > privateData.key.ByteSize()) {
        BM_LOG_ERROR("host_urma invalid device endpoint, version: " << header.version
                                                                    << " bytes: " << header.payloadLen);
        return BM_INVALID_PARAM;
    }
    int32_t localId = -1;
    UrmaTopoManager::GetInstance().GetLocalId(localId);
    for (size_t offset = 0; offset < header.payloadLen; offset += sizeof(UrmaEndpointDesc)) {
        UrmaEndpointDesc peer{};
        std::memcpy(&peer, privateData.key.ByteData() + sizeof(header) + offset, sizeof(peer));
        if (peer.remoteId == -1 || peer.remoteId == localId) {
            outDesc = peer;
            return BM_OK;
        }
    }
    BM_LOG_ERROR("host_urma no device endpoint matches localId: " << localId);
    return BM_INVALID_PARAM;
}

struct ParsedRemoteMemKey {
    uint64_t remoteAddr{0};
    UrmaExportDesc exportDesc{};
};

bool IsAllHex(const std::string &s)
{
    for (auto ch : s) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

Result ParseRemoteMemKey(const TransportMemoryKey &memKey, uint32_t peerRank, ParsedRemoteMemKey &parsed)
{
    constexpr size_t keyHeaderBytes = DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS * sizeof(uint64_t);
    if (memKey.ByteSize() < keyHeaderBytes + sizeof(UrmaExportDesc)) {
        BM_LOG_ERROR("Truncated remote memory key, peerRank: " << peerRank << " bytes: " << memKey.ByteSize());
        return BM_INVALID_PARAM;
    }
    if (memKey.keys[0] != URMA_EXPORT_DESC_MAGIC || memKey.keys[1] == 0) {
        BM_LOG_ERROR("Invalid remote memory key, peerRank: " << peerRank << " magic: 0x" << std::hex << memKey.keys[0]
                                                             << " keyAddr: 0x" << memKey.keys[1]);
        return BM_INVALID_PARAM;
    }
    parsed.remoteAddr = memKey.keys[1];
    const auto *payload = memKey.ByteData() + keyHeaderBytes;
    std::memcpy(&parsed.exportDesc, payload, sizeof(UrmaExportDesc));
    const auto &desc = parsed.exportDesc;
    const bool validType =
        desc.memoryType == UrmaMemoryType::HOST_DRAM || desc.memoryType == UrmaMemoryType::DEVICE_HBM;
    uint64_t exportEnd = 0;
    const UrmaCommMem exportMem{desc.addr, desc.size, desc.memoryType};
    const uint64_t totalDescLen = sizeof(UrmaExportDesc) + static_cast<uint64_t>(desc.hcommDescLen) +
                                  static_cast<uint64_t>(desc.deviceMarkerDescLen);
    if (desc.magic != URMA_EXPORT_DESC_MAGIC || desc.version != URMA_EXPORT_DESC_VERSION ||
        desc.headerSize != sizeof(UrmaExportDesc) || !validType || desc.addr == 0 || desc.size == 0 ||
        desc.hcommDescLen == 0 || !GetRangeEnd(exportMem, exportEnd) ||
        totalDescLen > DEVICE_URMA_EXPORT_KEY_DATA_BYTES || totalDescLen > memKey.ByteSize() - keyHeaderBytes) {
        BM_LOG_ERROR("Invalid remote export descriptor, peerRank: "
                     << peerRank << " keyAddr: 0x" << std::hex << parsed.remoteAddr << " descAddr: 0x" << desc.addr
                     << std::dec << " size: " << desc.size << " memoryType: " << desc.memoryType
                     << " hcommDescLen: " << desc.hcommDescLen << " flagDescLen: " << desc.deviceMarkerDescLen);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result ParseHostUrmaEid(uint32_t phyDeviceId, uint32_t rankId, std::array<uint8_t, COMM_ADDR_EID_LEN> &eid)
{
    std::string hostEid;
    auto ret = QueryHostUrmaEid(phyDeviceId, hostEid);
    if (ret != BM_OK) {
        BM_LOG_ERROR("host_urma QueryHostUrmaEid failed, phyDeviceId=" << phyDeviceId << ", rankId=" << rankId
                                                                       << ", ret=" << ret);
        return ret;
    }
    const std::string &val = hostEid;
    if (val.length() != COMM_ADDR_EID_LEN * 2 || !IsAllHex(val)) {
        BM_LOG_ERROR("Invalid hostEid from QueryHostUrmaEid='" << val << "' (expect " << (COMM_ADDR_EID_LEN * 2)
                                                               << " hex chars), phyDeviceId=" << phyDeviceId
                                                               << ", rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; i++) {
        auto byteStr = val.substr(i * 2, 2);
        eid[i] = static_cast<uint8_t>(std::strtoul(byteStr.c_str(), nullptr, 16) & 0xFF);
    }
    return BM_OK;
}

} // namespace

HostUrmaTransportManager::~HostUrmaTransportManager()
{
    (void)CloseDevice();
}

Result HostUrmaTransportManager::OpenDevice(const TransportOptions &options)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
        BM_LOG_INFO("HostUrmaTransportManager already opened, rankId: " << options.rankId);
        return BM_OK;
    }
    rankId_ = options.rankId;
    options_ = options;
    auto ret = ResolvePhysicalDeviceIdLocked();
    if (ret != BM_OK) {
        return ret;
    }
    ret = BuildLocalHostEndpointDescLocked(localEndpointDesc_);
    if (ret != BM_OK) {
        return ret;
    }
    auto hcommEndpoint = ToHcommEndpointDesc(localEndpointDesc_);
    hcommEndpoint.loc = {};
    hcommEndpoint.loc.locType = ENDPOINT_LOC_TYPE_HOST;
    hcommEndpoint.loc.host.id = rankId_;
    ret = hcommApi_.CreateEndpoint(hcommEndpoint, localEndpoint_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to create Host URMA endpoint, rankId: " << rankId_ << " ret: " << ret);
        return ret;
    }
    ret = InitHostTransferFlagLocked();
    if (ret != BM_OK) {
        (void)hcommApi_.DestroyEndpoint(localEndpoint_);
        localEndpoint_ = nullptr;
        return ret;
    }
    opened_ = true;
    BM_LOG_INFO("HostUrmaTransportManager opened, rankId: " << rankId_);
    return BM_OK;
}

Result HostUrmaTransportManager::ResolvePhysicalDeviceIdLocked()
{
    int32_t userId = -1;
    auto ret = DlAclApi::AclrtGetDevice(&userId);
    if (ret != BM_OK || userId < 0) {
        BM_LOG_ERROR("host_urma AclrtGetDevice failed, ret=" << ret << ", deviceId=" << userId
                                                             << ", rankId=" << rankId_);
        return BM_DL_FUNCTION_FAILED;
    }
    int32_t phyId = 0;
    ret = DlAclApi::AclrtGetPhyDevIdByLogicDevId(userId, &phyId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("host_urma aclrtGetPhyDevIdByLogicDevId failed, ret=" << ret << ", userId=" << userId
                                                                           << ", rankId=" << rankId_);
        return BM_DL_FUNCTION_FAILED;
    }
    phyDeviceId_ = static_cast<uint32_t>(phyId);
    BM_LOG_INFO("host_urma resolved physical device, userId=" << userId << ", phyDeviceId=" << phyDeviceId_
                                                              << ", rankId=" << rankId_);
    return BM_OK;
}

Result HostUrmaTransportManager::BuildLocalHostEndpointDescLocked(UrmaEndpointDesc &endpoint) const
{
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    auto ret = ParseHostUrmaEid(phyDeviceId_, rankId_, eid);
    if (ret != BM_OK) {
        return ret;
    }
    endpoint = {};
    endpoint.protocol = UrmaProtocol::UBC_CTP;
    endpoint.type = COMM_ADDR_TYPE_EID;
    std::memcpy(endpoint.raws, eid.data(), COMM_ADDR_EID_LEN);
    endpoint.devPhyId = phyDeviceId_;
    endpoint.processId = static_cast<uint32_t>(getpid());
    static std::atomic<uint64_t> nextHostInstance{1};
    endpoint.instanceId = nextHostInstance.fetch_add(1, std::memory_order_relaxed);
    // Separate host and device session names; HCOMM ignores device location fields for HOST endpoints.
    endpoint.superDevId = UINT32_MAX;
    return BM_OK;
}

Result HostUrmaTransportManager::InitHostTransferFlagLocked()
{
    hostTransFlagSize_ = HOST_TRANSFER_FLAG_SIZE;
    hostTransFlagPtr_ = std::malloc(hostTransFlagSize_);
    if (hostTransFlagPtr_ == nullptr) {
        BM_LOG_ERROR("Failed to allocate Host transfer flag, size: " << hostTransFlagSize_ << " rankId: " << rankId_);
        return BM_MALLOC_FAILED;
    }
    *static_cast<uint64_t *>(hostTransFlagPtr_) = 1;
    UrmaCommMem flagMem{};
    flagMem.addr = reinterpret_cast<uint64_t>(hostTransFlagPtr_);
    flagMem.size = hostTransFlagSize_;
    flagMem.type = UrmaMemoryType::HOST_DRAM;
    auto hcommFlagMem = ToHcommMem(flagMem);
    auto ret = hcommApi_.RegisterMemory(localEndpoint_, reinterpret_cast<UrmaMemTag>(hostTransFlagPtr_), hcommFlagMem,
                                        hostTransFlagHcommHandle_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to register Host transfer flag, rankId: " << rankId_ << " ret: " << ret);
        std::free(hostTransFlagPtr_);
        hostTransFlagPtr_ = nullptr;
        return ret;
    }
    BM_LOG_DEBUG("Host transfer flag registered, rankId: " << rankId_ << " addr: " << std::hex << flagMem.addr);
    return BM_OK;
}

Result HostUrmaTransportManager::CloseDevice()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        return BM_OK;
    }
    Result finalRet = BM_OK;
    for (auto &[peerRank, state] : remoteRanks_) {
        auto ret = DestroyRemoteChannelLocked(peerRank, state);
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
    }
    remoteRanks_.clear();

    for (auto &[addr, reg] : localRegistrations_) {
        if (reg.handle != nullptr) {
            (void)hcommApi_.UnregisterMemory(localEndpoint_, reg.handle);
        }
    }
    localRegistrations_.clear();

    if (hostTransFlagHcommHandle_ != nullptr) {
        (void)hcommApi_.UnregisterMemory(localEndpoint_, hostTransFlagHcommHandle_);
        hostTransFlagHcommHandle_ = nullptr;
    }
    if (hostTransFlagPtr_ != nullptr) {
        std::free(hostTransFlagPtr_);
        hostTransFlagPtr_ = nullptr;
    }
    hostTransFlagSize_ = 0;

    if (localEndpoint_ != nullptr) {
        (void)hcommApi_.DestroyEndpoint(localEndpoint_);
        localEndpoint_ = nullptr;
    }
    opened_ = false;
    BM_LOG_INFO("HostUrmaTransportManager closed, rankId: " << rankId_);
    return finalRet;
}

Result HostUrmaTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        BM_LOG_ERROR("HostUrmaTransportManager not opened, rankId: " << rankId_);
        return BM_NOT_INITIALIZED;
    }
    if (mr.addr == 0 || mr.size == 0) {
        BM_LOG_ERROR("Invalid memory region, addr: " << std::hex << mr.addr << " size: " << std::dec << mr.size);
        return BM_INVALID_PARAM;
    }
    if ((mr.flags & REG_MR_FLAG_DRAM) == 0U || (mr.flags & REG_MR_FLAG_HBM) != 0U) {
        BM_LOG_ERROR("Host URMA only supports DRAM memory, rankId: " << rankId_ << " addr: " << std::hex << mr.addr
                                                                     << " size: " << std::dec << mr.size << " flags: 0x"
                                                                     << std::hex << mr.flags);
        return BM_INVALID_PARAM;
    }
    auto it = localRegistrations_.find(mr.addr);
    if (it != localRegistrations_.end()) {
        if (it->second.mr.size == mr.size) {
            it->second.refCount++;
            BM_LOG_DEBUG("Memory region already registered, refCount: " << it->second.refCount << " addr: " << std::hex
                                                                        << mr.addr);
            return BM_OK;
        }
        BM_LOG_ERROR("Overlapping but different registration, addr: " << std::hex << mr.addr << " newSize: " << std::dec
                                                                      << mr.size
                                                                      << " existingSize: " << it->second.mr.size);
        return BM_INVALID_PARAM;
    }
    UrmaMemTag memTag = static_cast<UrmaMemTag>(mr.addr);
    UrmaCommMem commMem{};
    commMem.addr = mr.addr;
    commMem.size = mr.size;
    commMem.type = UrmaMemoryType::HOST_DRAM;
    auto hcommMem = ToHcommMem(commMem);
    HcommMemHandleType handle = nullptr;
    auto ret = hcommApi_.RegisterMemory(localEndpoint_, memTag, hcommMem, handle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to register memory region, addr: " << std::hex << mr.addr << " size: " << std::dec
                                                                << mr.size << " ret: " << ret);
        return ret;
    }
    uint64_t exportedGva = 0;
    auto gvaRet = ResolveExportedGvaLocked(mr, exportedGva);
    if (gvaRet != BM_OK) {
        BM_LOG_WARN("GVA resolution failed for mr addr: " << std::hex << mr.addr
                                                          << " this mr is local-only, exportedGva=0");
    }
    LocalRegistration reg{};
    reg.mr = mr;
    reg.handle = handle;
    reg.memTag = memTag;
    reg.exportedGva = exportedGva;
    reg.refCount = 1;
    localRegistrations_.emplace(mr.addr, reg);
    BM_LOG_DEBUG("Memory region registered, addr: " << std::hex << mr.addr << " size: " << std::dec << mr.size
                                                    << " exportedGva: " << std::hex << exportedGva);
    return BM_OK;
}

Result HostUrmaTransportManager::ResolveExportedGvaLocked(const TransportMemoryRegion &mr, uint64_t &exportedGva) const
{
    exportedGva = 0;
    auto result = HybmVaManager::GetInstance().FindAllocByVa(mr.addr, HVM_GVA);
    if (!result.second) {
        return BM_OK;
    }
    const auto &vaInfo = result.first;
    if (vaInfo.base.size < mr.size || vaInfo.base.va[HVM_GVA] > mr.addr ||
        vaInfo.base.va[HVM_GVA] + vaInfo.base.size < mr.addr + mr.size) {
        return BM_OK;
    }
    if (vaInfo.base.memType != HYBM_MEM_TYPE_HOST) {
        return BM_OK;
    }
    exportedGva = mr.addr;
    return BM_OK;
}

Result HostUrmaTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = localRegistrations_.find(addr);
    if (it == localRegistrations_.end()) {
        BM_LOG_ERROR("Memory region not found, addr: " << std::hex << addr);
        return BM_INVALID_PARAM;
    }
    it->second.refCount--;
    if (it->second.refCount > 0) {
        return BM_OK;
    }
    auto ret = hcommApi_.UnregisterMemory(localEndpoint_, it->second.handle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to unregister memory region, addr: " << std::hex << addr << " ret: " << ret);
        return ret;
    }
    localRegistrations_.erase(it);
    return BM_OK;
}

bool HostUrmaTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = localRegistrations_.find(addr);
    if (it == localRegistrations_.end()) {
        return false;
    }
    return it->second.mr.size >= size;
}

Result HostUrmaTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        BM_LOG_ERROR("HostUrmaTransportManager not opened");
        return BM_NOT_INITIALIZED;
    }
    auto it = localRegistrations_.find(addr);
    if (it == localRegistrations_.end()) {
        BM_LOG_ERROR("Address not registered, addr: " << std::hex << addr);
        return BM_INVALID_PARAM;
    }
    if (it->second.exportedGva == 0) {
        BM_LOG_ERROR("Address has no exported GVA, addr: " << std::hex << addr);
        return BM_INVALID_PARAM;
    }
    if (hostTransFlagHcommHandle_ == nullptr) {
        BM_LOG_ERROR("Host transfer flag not registered");
        return BM_ERROR;
    }

    // Export raw HCOMM memory descriptor via cached ExportMemory.
    const uint8_t *hcommDesc = nullptr;
    uint32_t hcommDescLen = 0;
    auto ret = hcommApi_.ExportMemory(localEndpoint_, it->second.handle, hcommDesc, hcommDescLen);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to export memory region, addr: " << std::hex << addr << " ret: " << ret);
        return ret;
    }

    // Construct UrmaExportDesc directly from registration metadata (no DeserializeExportDesc).
    UrmaExportDesc exportDesc{};
    exportDesc.headerSize = sizeof(UrmaExportDesc);
    exportDesc.memoryType = UrmaMemoryType::HOST_DRAM;
    exportDesc.memTag = it->second.memTag;
    exportDesc.addr = it->second.exportedGva;
    exportDesc.size = it->second.mr.size;
    exportDesc.hcommDescLen = hcommDescLen;

    // Export flag descriptor via raw HCOMM API (flag is cached but exported raw for one-shot use).
    void *flagDesc = nullptr;
    uint32_t flagDescLen = 0;
    ret = DlHcommApi::HcommMemExport(localEndpoint_, hostTransFlagHcommHandle_, &flagDesc, &flagDescLen);
    if (ret != 0 || flagDesc == nullptr || flagDescLen == 0) {
        BM_LOG_ERROR("Failed to export Host transfer flag, rankId: " << rankId_ << " ret: " << ret);
        return BM_ERROR;
    }

    exportDesc.deviceMarkerDescLen = flagDescLen;
    constexpr uint32_t exportHdrSize = sizeof(UrmaExportDesc);
    if (hcommDescLen > DEVICE_URMA_EXPORT_KEY_DATA_BYTES - exportHdrSize ||
        flagDescLen > DEVICE_URMA_EXPORT_KEY_DATA_BYTES - exportHdrSize - hcommDescLen) {
        BM_LOG_ERROR("Export descriptors too large for key slots, hcommDescLen: "
                     << hcommDescLen << " flagDescLen: " << flagDescLen
                     << " capacity: " << DEVICE_URMA_EXPORT_KEY_DATA_BYTES);
        return BM_ERROR;
    }

    // Build key layout: [keys[0]=magic, keys[1]=gva, keys[2..]=UrmaExportDesc+hcommDesc+flagDesc]
    key.ResizeWords(DEVICE_URMA_MAX_EXPORT_KEY_LENGTH);
    key.keys[0] = URMA_EXPORT_DESC_MAGIC;
    key.keys[1] = it->second.exportedGva;
    uint8_t *payload = reinterpret_cast<uint8_t *>(&key.keys[DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS]);
    std::memcpy(payload, &exportDesc, exportHdrSize);
    std::memcpy(payload + exportHdrSize, hcommDesc, hcommDescLen);
    std::memcpy(payload + exportHdrSize + hcommDescLen, flagDesc, flagDescLen);
    return BM_OK;
}

void HostUrmaTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    if (key.keys.size() <= DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS) {
        BM_LOG_ERROR("host_urma UpdateMemoryKey key too small, words: " << key.keys.size());
        return;
    }
    key.keys[1] = reinterpret_cast<uint64_t>(addr);
}

Result HostUrmaTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        BM_LOG_ERROR("HostUrmaTransportManager not opened");
        return BM_NOT_INITIALIZED;
    }
    for (const auto &[peerRank, peerInfo] : options.options) {
        UrmaEndpointDesc peerEndpoint{};
        auto ret = ParseUrmaPrivateData(peerInfo.privateData, peerEndpoint);
        if (ret != BM_OK) {
            return ret;
        }
        auto &state = remoteRanks_[peerRank];
        if (state.channel == 0) {
            state.endpointDesc = peerEndpoint;
            ret = PreparePeerLocked(peerRank, state);
            if (ret != BM_OK) {
                remoteRanks_.erase(peerRank);
                return ret;
            }
        } else if (std::memcmp(&state.endpointDesc, &peerEndpoint, sizeof(peerEndpoint)) != 0) {
            BM_LOG_ERROR("host_urma cannot replace a prepared endpoint, peerRank: " << peerRank);
            return BM_NOT_SUPPORTED;
        }
        ret = ValidateDevicePeerMemoryKeysLocked(peerRank, peerInfo.memKeys);
        if (ret != BM_OK) {
            return ret;
        }
    }
    return BM_OK;
}

Result HostUrmaTransportManager::PreparePeerLocked(uint32_t peerRank, RemoteRankState &state)
{
    EndpointDesc hcommRemoteEndpoint = ToHcommEndpointDesc(state.endpointDesc);
    HcommChannelDesc channelDesc{};
    auto ret = HcommChannelDescInit(&channelDesc, 1);
    if (ret != 0) {
        BM_LOG_ERROR("Failed to init channel desc for peer " << peerRank << " ret: " << ret);
        return BM_ERROR;
    }
    channelDesc.remoteEndpoint = hcommRemoteEndpoint;
    state.channelName = UrmaChannelSessionName(localEndpointDesc_, state.endpointDesc);
    channelDesc.channelName = state.channelName.c_str();
    channelDesc.exchangeAllMems = true;
    channelDesc.port = 0;
    channelDesc.role = (rankId_ > peerRank) ? HCOMM_SOCKET_ROLE_CLIENT : HCOMM_SOCKET_ROLE_SERVER;
    ChannelHandle channel = 0;
    ret = HcommApiWrapper::CreateChannel(localEndpoint_, COMM_ENGINE_CPU, channelDesc, channel);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to create channel with peer " << peerRank << " ret: " << ret);
        return ret;
    }
    state.channelDesc = channelDesc;
    state.channel = channel;
    BM_LOG_INFO("Host URMA channel created with peer "
                << peerRank << " channel: " << channel
                << " role: " << (channelDesc.role == HCOMM_SOCKET_ROLE_CLIENT ? "CLIENT" : "SERVER"));
    return BM_OK;
}

Result
HostUrmaTransportManager::ValidateDevicePeerMemoryKeysLocked(uint32_t peerRank,
                                                             const std::vector<TransportMemoryKey> &memKeys) const
{
    for (const auto &memKey : memKeys) {
        ParsedRemoteMemKey parsed{};
        const auto ret = ParseRemoteMemKey(memKey, peerRank, parsed);
        if (ret != BM_OK) {
            return ret;
        }
        const auto &desc = parsed.exportDesc;
        if (desc.memoryType != UrmaMemoryType::DEVICE_HBM) {
            BM_LOG_ERROR("Device peer exported unsupported memory type, rankId: "
                         << rankId_ << " peerRank: " << peerRank << " keyAddr: 0x" << std::hex << parsed.remoteAddr
                         << " descAddr: 0x" << desc.addr << std::dec << " memoryType: " << desc.memoryType);
            return BM_INVALID_PARAM;
        }
        BM_LOG_INFO("Host URMA skips Device HBM key, rankId: "
                    << rankId_ << " peerRank: " << peerRank << " gva: 0x" << std::hex << parsed.remoteAddr
                    << " hbmAddr: 0x" << desc.addr << std::dec << " size: " << desc.size << " memTag: " << desc.memTag);
    }
    return BM_OK;
}

Result HostUrmaTransportManager::DestroyRemoteChannelLocked(uint32_t peerRank, RemoteRankState &state)
{
    if (state.channel == 0) {
        return BM_OK;
    }
    const auto channel = state.channel;
    auto ret = HcommApiWrapper::DestroyChannel(state.channel);
    state.channel = 0; // ensure cleared even on failure
    if (ret != BM_OK) {
        BM_LOG_ERROR("DestroyChannel failed, rankId: " << rankId_ << " peerRank: " << peerRank
                                                       << " channel: " << channel << " ret: " << ret);
        return BM_ERROR;
    }
    return BM_OK;
}

Result HostUrmaTransportManager::RemoveRankLocked(uint32_t peerRank)
{
    auto it = remoteRanks_.find(peerRank);
    if (it == remoteRanks_.end()) {
        return BM_OK;
    }
    auto ret = DestroyRemoteChannelLocked(peerRank, it->second);
    remoteRanks_.erase(it);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to clean remote rank, rankId: " << rankId_ << " peerRank: " << peerRank
                                                             << " ret: " << ret);
    }
    return ret;
}

Result HostUrmaTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        BM_LOG_ERROR("HostUrmaTransportManager not opened, rankId: " << rankId_);
        return BM_NOT_INITIALIZED;
    }
    Result finalRet = BM_OK;
    for (auto peerRank : removedRanks) {
        auto ret = RemoveRankLocked(peerRank);
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
    }
    return finalRet;
}

Result HostUrmaTransportManager::ConnectRank(uint32_t rankId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = remoteRanks_.find(rankId);
    if (!opened_ || it == remoteRanks_.end() || it->second.channel == 0) {
        BM_LOG_ERROR("host_urma peer not prepared, rankId: " << rankId_ << " peerRank: " << rankId);
        return BM_NOT_CONNECTED;
    }
    const auto timeout =
        std::chrono::seconds(MfEnvUtil::GetOptionalUintOrDefault(env::MF_QP_READY_CHECK_TIMEOUT_BASE, 100));
    return HcommApiWrapper::WaitForChannelsReady({{it->second.channel, rankId}}, timeout, rankId_);
}

Result HostUrmaTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    bool needFallback = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &[peerRank, peerInfo] : options.options) {
            auto it = remoteRanks_.find(peerRank);
            if (it == remoteRanks_.end()) {
                BM_LOG_WARN("UpdateRankOptions: peer " << peerRank << " not prepared yet, fallback to Prepare");
                needFallback = true;
                break;
            }
            if (peerInfo.privateData.key.ByteSize() != 0) {
                UrmaEndpointDesc newDesc{};
                auto ret = ParseUrmaPrivateData(peerInfo.privateData, newDesc);
                if (ret != BM_OK) {
                    return ret;
                }
                if (std::memcmp(&it->second.endpointDesc, &newDesc, sizeof(UrmaEndpointDesc)) != 0) {
                    BM_LOG_ERROR("UpdateRankOptions: endpoint changed for peer " << peerRank);
                    return BM_NOT_SUPPORTED;
                }
            }
            if (!peerInfo.memKeys.empty()) {
                needFallback = true;
            }
        }
    }
    return needFallback ? Prepare(options) : BM_OK;
}

const std::string &HostUrmaTransportManager::GetNic() const
{
    return options_.nic;
}

const TransportPrivateData HostUrmaTransportManager::GetPrivateData() const
{
    TransportPrivateData data{};
    if (!opened_) {
        BM_LOG_ERROR("HostUrmaTransportManager not opened, returning empty private data");
        return data;
    }
    auto ret = SerializeUrmaPrivateData(localEndpointDesc_, rankId_, data);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to serialize private data, ret: " << ret);
    }
    return data;
}

Result HostUrmaTransportManager::ReadRemote(uint32_t rankId, uint64_t, uint64_t, uint64_t)
{
    BM_LOG_ERROR("AICPU host endpoint does not initiate IO, peerRank: " << rankId);
    return BM_NOT_SUPPORTED;
}

Result HostUrmaTransportManager::WriteRemote(uint32_t rankId, uint64_t, uint64_t, uint64_t)
{
    BM_LOG_ERROR("AICPU host endpoint does not initiate IO, peerRank: " << rankId);
    return BM_NOT_SUPPORTED;
}

Result HostUrmaTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t, uint64_t, uint64_t)
{
    BM_LOG_ERROR("AICPU host endpoint does not initiate IO, peerRank: " << rankId);
    return BM_NOT_SUPPORTED;
}

Result HostUrmaTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t, uint64_t, uint64_t)
{
    BM_LOG_ERROR("AICPU host endpoint does not initiate IO, peerRank: " << rankId);
    return BM_NOT_SUPPORTED;
}

Result HostUrmaTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &)
{
    BM_LOG_ERROR("AICPU host endpoint does not initiate batch IO, peerRank: " << rankId);
    return BM_NOT_SUPPORTED;
}

Result HostUrmaTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &)
{
    BM_LOG_ERROR("AICPU host endpoint does not initiate batch IO, peerRank: " << rankId);
    return BM_NOT_SUPPORTED;
}

Result HostUrmaTransportManager::Synchronize(uint32_t rankId)
{
    (void)rankId;
    return BM_OK;
}

} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock
