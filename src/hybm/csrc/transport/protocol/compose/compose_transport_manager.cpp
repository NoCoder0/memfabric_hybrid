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
#include "compose_transport_manager.h"

#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <cstring>

#include "hybm_def.h"
#include "hybm_logger.h"
#include "host_hcom_transport_manager.h"
#include "device_rdma_transport_manager.h"
#include "device_rdma_aicpu_transport_manager.h"
#include "hybm_gva_version.h"
#include "device_urma_transport_manager.h"
#include "device_rdma_hcomm_transport_manager.h"
#include "dl_acl_api.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "mf_str_util.h"
#include "hybm_mem_segment.h"

using namespace ock::mf;
using namespace ock::mf::transport;

namespace {
const char NIC_DELIMITER = ';';
const std::string HOST_TRANSPORT_TYPE = "host#";
const std::string DEVICE_TRANSPORT_TYPE = "device#";
const uint32_t HOST_PROTOCOL = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
const uint32_t DEVICE_PROTOCOL = HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA;
const uint32_t DEVICE_RDMA_PROTOCOL = HYBM_DOP_TYPE_DEVICE_RDMA;
const uint32_t DEVICE_URMA_PROTOCOL = HYBM_DOP_TYPE_DEVICE_URMA;
} // namespace

std::shared_ptr<TransportManager> ComposeTransportManager::FindTransport(uint32_t opTypes) const
{
    for (const auto &[protocol, manager] : transports_) {
        if ((protocol & opTypes) != 0U) {
            return manager;
        }
    }
    return nullptr;
}

uint32_t ComposeTransportManager::FindTransportProtocol(uint32_t opTypes) const
{
    for (const auto &[protocol, manager] : transports_) {
        if ((protocol & opTypes) != 0U) {
            return protocol;
        }
    }
    return 0U;
}

std::vector<std::pair<uint32_t, std::shared_ptr<TransportManager>>>
ComposeTransportManager::GetRankTransports(uint32_t rankId) const
{
    auto protocols = GetConnectedProtocols(rankId);
    if (protocols.empty()) {
        protocols = BuildRankProtocols(rankId);
    }

    std::vector<std::pair<uint32_t, std::shared_ptr<TransportManager>>> rankTransports;
    rankTransports.reserve(protocols.size());
    for (const auto protocol : protocols) {
        auto it = transports_.find(protocol);
        if (it != transports_.end()) {
            rankTransports.emplace_back(protocol, it->second);
        }
    }
    return rankTransports;
}

Result ComposeTransportManager::OpenHostTransport(const TransportOptions &options)
{
    if (FindTransport(HOST_PROTOCOL) != nullptr) {
        BM_LOG_ERROR("Failed to open host transport is opened");
        return BM_ERROR;
    }
    // Per-entity host transport instance: the per-rank channel/key state of a singleton cannot serve
    // several store groups of the same process at the same time (the multi-connection replay case), and
    // independent instances get naturally disjoint bind ports via basePort + rankId.
    auto transportManager = std::make_shared<host::HcomTransportManager>();
    transports_[HOST_PROTOCOL] = transportManager;
    const Result ret = transportManager->OpenDevice(options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to open host transport, nic: " << options.nic << ", rankId: " << options.rankId
                                                            << ", ret: " << ret);
    }
    return ret;
}

namespace {
// HCOMM 要求 CANN >= 9.1（对应 ACL 版本 >= 1.17，AclrtGetVersion 返回 ACL 版本号）
constexpr int32_t HCOMM_REQUIRED_MAJOR = 1;
constexpr int32_t HCOMM_REQUIRED_MINOR = 17;
} // namespace

Result ComposeTransportManager::OpenDeviceTransport(const TransportOptions &options)
{
    if ((options.protocol & DEVICE_URMA_PROTOCOL) != 0U) {
        auto ret = OpenTransport(options, DEVICE_URMA_PROTOCOL);
        if (ret != BM_OK) {
            return ret;
        }
    }
    if ((options.protocol & DEVICE_RDMA_PROTOCOL) != 0U) {
        return OpenTransport(options, DEVICE_RDMA_PROTOCOL);
    }
    if ((options.protocol & DEVICE_PROTOCOL) == 0U) {
        BM_LOG_ERROR("Failed to open device transport with unsupported protocol: " << options.protocol
                                                                                   << ", rankId: " << options.rankId);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result ComposeTransportManager::OpenTransport(const TransportOptions &options, uint32_t opTypes)
{
    if (FindTransport(opTypes) != nullptr) {
        BM_LOG_ERROR("Failed to open transport is opened, opTypes: " << opTypes);
        return BM_ERROR;
    }

    std::shared_ptr<TransportManager> transportManager;
    if (opTypes == DEVICE_URMA_PROTOCOL) {
        transportManager = std::make_shared<device::DeviceUrmaTransportManager>();
    } else if (opTypes == DEVICE_RDMA_PROTOCOL) {
        // HCOMM 开关（默认关闭）：关闭时无论 CANN 版本一律走 native RDMA；
        // 打开后恢复 CANN 版本判断（>= 9.1 走 HCOMM，否则 native）。
        // HCOMM 代码保留，待 HCOMM 支持动态 MR 更新后置 1 即可启用。
        // 注：A5(950) 的 HCOMM device RDMA 是主机侧 RoCE 形态（HOST 端点 + 主机侧 Nbi 提交），
        // 与 910B/910_93 的 DEVICE 形态实现不同，由 DeviceRdmaHcommTransportManager 内部按 SoC 分派。
        static const bool hcommEnabled = MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_RDMA_USE_HCOMM, 0u) != 0u;
        const bool useHcomm = hcommEnabled && DlAclApi::IsCannGE(HCOMM_REQUIRED_MAJOR, HCOMM_REQUIRED_MINOR);
        BM_LOG_INFO("useHcomm=" << (useHcomm ? "true" : "false") << " (hcommEnabled=" << hcommEnabled
                                << ", ACL >= " << HCOMM_REQUIRED_MAJOR << "." << HCOMM_REQUIRED_MINOR
                                << " i.e. CANN >= 9.1 required)");
        if (useHcomm) {
            transportManager = std::make_shared<device::DeviceRdmaHcommTransportManager>();
        } else {
            // AICPU 直发开关（默认打开）：打开后用 AI QP + AICPU kernel 提交 WQE，host 仅敲 doorbell。
            // 仅适用于非 HYBM_TYPE_AI_CORE_INITIATE 场景（AI_CORE_INITIATE 用 FixedRanksQpManager，走基类）。
            static const bool aicpuEnabled = MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_RDMA_USE_AICPU, 1U) != 0U;
            const bool useAicpu = aicpuEnabled && (options.initialType != HYBM_TYPE_AI_CORE_INITIATE);
            if (useAicpu) {
                BM_LOG_INFO("using AICPU device RDMA transport, rankId: " << options.rankId);
                transportManager = std::make_shared<device::RdmaAicpuTransportManager>();
            } else {
                BM_LOG_INFO("using native device RDMA transport");
                transportManager = std::make_shared<device::RdmaTransportManager>();
            }
        }
    } else {
        BM_LOG_ERROR("Failed to open transport with unsupported protocol: " << opTypes
                                                                            << ", rankId: " << options.rankId);
        return BM_INVALID_PARAM;
    }
    transports_[opTypes] = transportManager;
    const Result ret = transportManager->OpenDevice(options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to open transport, nic: " << options.nic << ", protocol: " << opTypes
                                                       << ", rankId: " << options.rankId << ", ret: " << ret);
    }
    return ret;
}

bool ComposeTransportManager::IsDeviceUrma() const
{
    return (options_.protocol & HYBM_DOP_TYPE_DEVICE_URMA) != 0;
}

Result ComposeTransportManager::OpenDevice(const TransportOptions &options)
{
    options_ = options;
    Result ret = BM_ERROR;
    if ((options_.protocol & DEVICE_PROTOCOL) != 0U) {
        ret = OpenDeviceTransport(options);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to open device transport, nic: " << options.nic << ", protocol: " << options.protocol
                                                                  << ", rankId: " << options.rankId
                                                                  << ", ret: " << ret);
            CloseDevice();
            return BM_ERROR;
        }
    }

    if ((options_.protocol & HOST_PROTOCOL) != 0U) {
        ret = OpenHostTransport(options);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to open device transport nic: " << options.nic);
            CloseDevice();
            return BM_ERROR;
        }
    }

    std::stringstream ss;
    if ((options_.protocol & HOST_PROTOCOL) != 0U) {
        auto hostTransportManager = FindTransport(HOST_PROTOCOL);
        if (hostTransportManager == nullptr) {
            BM_LOG_ERROR("Failed to open host transport nic: " << options.nic);
            CloseDevice();
            return BM_ERROR;
        }
        ss << HOST_TRANSPORT_TYPE << hostTransportManager->GetNic() << NIC_DELIMITER;
    }
    if ((options_.protocol & DEVICE_PROTOCOL) != 0U) {
        bool foundDevice = false;
        for (const auto &[protocol, manager] : transports_) {
            if ((protocol & DEVICE_PROTOCOL) == 0U) {
                continue;
            }
            foundDevice = true;
            ss << DEVICE_TRANSPORT_TYPE << manager->GetNic() << NIC_DELIMITER;
        }
        if (!foundDevice) {
            BM_LOG_ERROR("Failed to open device transport nic: " << options.nic);
            CloseDevice();
            return BM_ERROR;
        }
    }
    nicInfo_ = ss.str();
    BM_LOG_TRACE("Success to open device rankId:" << options_.rankId << " protocol:" << options_.protocol
                                                  << " nic:" << nicInfo_);
    return BM_OK;
}

Result ComposeTransportManager::CloseDevice()
{
    Result finalRet = BM_OK;
    for (const auto &[protocol, manager] : transports_) {
        auto ret = manager->CloseDevice();
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
    }

    transports_.clear();
    {
        std::lock_guard<std::mutex> lock(connectedProtocolsMutex_);
        connectedProtocols_.clear();
    }
    return finalRet;
}

Result ComposeTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::vector<uint32_t> registeredProtocols;
    for (const auto &[protocol, manager] : transports_) {
        if ((mr.flags & REG_MR_FLAG_HOST_ONLY) != 0U && (protocol & HOST_PROTOCOL) == 0U) {
            continue;
        }
        Result ret = manager->RegisterMemoryRegion(mr);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to register memory region, protocol: " << protocol << ", mr: " << mr
                                                                        << ", ret: " << ret);
            for (auto it = registeredProtocols.rbegin(); it != registeredProtocols.rend(); ++it) {
                auto rollbackRet = transports_.at(*it)->UnregisterMemoryRegion(mr.addr);
                if (rollbackRet != BM_OK) {
                    BM_LOG_ERROR("Failed to rollback memory region, protocol: " << *it << ", mr: " << mr
                                                                                << ", ret: " << rollbackRet);
                }
            }
            return ret;
        }
        registeredProtocols.push_back(protocol);
    }
    ComposeMemoryRegion cmr{mr.addr, mr.size, TT_COMPOSE, mr.flags};
    std::unique_lock<std::mutex> uniqueLock{mrsMutex_};
    mrs_.emplace(mr.addr, cmr);
    return BM_OK;
}

Result ComposeTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    std::unique_lock<std::mutex> uniqueLock{mrsMutex_};
    auto pos = mrs_.find(addr);
    if (pos == mrs_.end()) {
        uniqueLock.unlock();
        BM_LOG_ERROR("input address not register!");
        return BM_INVALID_PARAM;
    }
    for (const auto &[protocol, manager] : transports_) {
        if ((mrs_.at(addr).flags & REG_MR_FLAG_HOST_ONLY) != 0U && (protocol & HOST_PROTOCOL) == 0U) {
            continue;
        }
        Result ret = manager->UnregisterMemoryRegion(addr);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to unregister mr addr, protocol: " << protocol << ", ret: " << ret);
            return ret;
        }
    }
    mrs_.erase(pos);
    return BM_OK;
}

bool ComposeTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    for (const auto &[protocol, manager] : transports_) {
        auto ret = manager->QueryHasRegistered(addr, size);
        if (ret) {
            return ret;
        }
    }
    return false;
}

Result ComposeTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    // device and host payloads are wrapped into a length-prefixed envelope; each keeps its own variable length.
    auto deviceTransportManager = FindTransport(DEVICE_PROTOCOL);
    if (deviceTransportManager != nullptr) {
        TransportMemoryKey deviceKey{};
        auto ret = deviceTransportManager->QueryMemoryKey(addr, deviceKey);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to query device transport memKey addr:" << std::hex << addr);
            return ret;
        }
        WriteDeviceMemoryKey(deviceKey, key);
    }

    auto hostTransportManager = FindTransport(HOST_PROTOCOL);
    if (hostTransportManager != nullptr) {
        TransportMemoryKey hostKey{};
        auto ret = hostTransportManager->QueryMemoryKey(addr, hostKey);
        if (ret != BM_OK) {
            BM_LOG_WARN("Unable to query host transport memKey addr:" << std::hex << addr);
            // HCOM 无法处理HBM池，这里直接返回，兼容即开启HBM池又要走HCOM通信的场景
        }
        WriteHostMemoryKey(hostKey, key);
    }

    return BM_OK;
}

void ComposeTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    // Split first, then update each payload, then rebuild the envelope: writing one payload resets the envelope.
    TransportMemoryKey deviceKey{};
    TransportMemoryKey hostKey{};
    auto deviceTransportManager = FindTransport(DEVICE_PROTOCOL);
    auto hostTransportManager = FindTransport(HOST_PROTOCOL);
    if (deviceTransportManager != nullptr) {
        ReadDeviceMemoryKey(key, deviceKey);
    }
    if (hostTransportManager != nullptr) {
        ReadHostMemoryKey(key, hostKey);
    }
    if (deviceTransportManager != nullptr) {
        deviceTransportManager->UpdateMemoryKey(deviceKey, addr);
    }
    if (hostTransportManager != nullptr) {
        hostTransportManager->UpdateMemoryKey(hostKey, addr);
    }
    WriteDeviceMemoryKey(deviceKey, key);
    WriteHostMemoryKey(hostKey, key);
}

void ComposeTransportManager::GetTransportPrepareOptions(const HybmTransPrepareOptions &param, uint32_t protocolMask,
                                                         HybmTransPrepareOptions &transportOptions)
{
    auto options = param.options;
    const bool isHostProtocol = (protocolMask & HOST_PROTOCOL) != 0U;
    const char *nicPrefix = isHostProtocol ? HOST_TRANSPORT_TYPE.c_str() : DEVICE_TRANSPORT_TYPE.c_str();
    for (const auto &item : options) {
        auto rankId = item.first;
        uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
        if ((opType & protocolMask) == 0U) {
            BM_LOG_DEBUG("remote rank:" << rankId << " to local rank:" << options_.rankId << " use protocol:" << opType
                                        << " skip protocol:" << protocolMask);
            continue;
        }
        TransportRankPrepareInfo info{};
        std::vector<std::string> nicVec = StrUtil::Split(item.second.nic, NIC_DELIMITER);
        for (const auto &nic : nicVec) {
            if (StrUtil::StartWith(nic, nicPrefix)) {
                info.nic = nic.substr(strlen(nicPrefix));
            }
        }

        for (auto &key : item.second.memKeys) {
            TransportMemoryKey tmp{};
            if (isHostProtocol) {
                ReadHostMemoryKey(key, tmp);
            } else {
                ReadDeviceMemoryKey(key, tmp);
            }
            info.memKeys.emplace_back(tmp);
        }
        info.privateData = item.second.privateData;
        transportOptions.options.emplace(rankId, info);
    }
}

Result ComposeTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    Result ret = BM_OK;
    {
        std::lock_guard<std::shared_mutex> lock(rankPrepareInfosMutex_);
        for (const auto &item : options.options) {
            rankPrepareInfos_[item.first] = item.second;
        }
    }
    for (const auto &item : options.options) {
        std::lock_guard<std::mutex> lock(connectedProtocolsMutex_);
        connectedProtocols_.erase(item.first);
    }
    for (const auto &[protocol, manager] : transports_) {
        HybmTransPrepareOptions transportOptions{};
        GetTransportPrepareOptions(options, protocol, transportOptions);
        ret = manager->Prepare(transportOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to prepare transport, protocol: " << protocol << ", ret: " << ret);
            return ret;
        }
    }
    return BM_OK;
}

Result ComposeTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    BM_LOG_INFO("compose RemoveRanks called, ranks: " << removedRanks.size()
                                                      << " transportCount: " << transports_.size());
    {
        std::lock_guard<std::shared_mutex> lock(rankPrepareInfosMutex_);
        for (auto rankId : removedRanks) {
            rankPrepareInfos_.erase(rankId);
        }
    }
    for (auto rankId : removedRanks) {
        std::lock_guard<std::mutex> lock(connectedProtocolsMutex_);
        connectedProtocols_.erase(rankId);
    }
    Result lastResult = BM_OK;
    for (const auto &[protocol, manager] : transports_) {
        auto ret = manager->RemoveRanks(removedRanks);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to remove ranks, protocol: " << protocol << ", ret: " << ret);
            lastResult = ret;
        }
    }

    return lastResult;
}

Result ComposeTransportManager::ConnectRank(uint32_t rankId)
{
    const auto protocols = BuildRankProtocols(rankId);
    std::vector<uint32_t> connected;
    Result lastResult = BM_OK;
    for (const auto protocol : protocols) {
        auto it = transports_.find(protocol);
        if (it == transports_.end()) {
            BM_LOG_ERROR("No transport found for protocol, rankId: " << rankId << ", protocol: " << protocol);
            lastResult = BM_INVALID_PARAM;
            continue;
        }
        auto ret = it->second->ConnectRank(rankId);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to connect rank, rankId: " << rankId << ", protocol: " << protocol
                                                            << ", ret: " << ret);
            lastResult = ret;
            continue;
        }
        connected.push_back(protocol);
    }
    const bool hasConnected = !connected.empty();
    {
        std::lock_guard<std::mutex> lock(connectedProtocolsMutex_);
        if (!connected.empty()) {
            connectedProtocols_[rankId] = std::move(connected);
        } else {
            connectedProtocols_.erase(rankId);
        }
    }
    if (!hasConnected) {
        BM_LOG_ERROR("No transport connected, rankId:" << rankId << ", protocols size:" << protocols.size()
                                                       << ", ret:" << lastResult);
        return lastResult == BM_OK ? BM_ERROR : lastResult;
    }
    return BM_OK;
}

std::vector<uint32_t> ComposeTransportManager::GetConnectedProtocols(uint32_t rankId) const
{
    std::lock_guard<std::mutex> lock(connectedProtocolsMutex_);
    auto it = connectedProtocols_.find(rankId);
    return it != connectedProtocols_.end() ? it->second : std::vector<uint32_t>{};
}

std::shared_ptr<TransportManager> ComposeTransportManager::GetTransport(uint32_t opTypes) const
{
    return FindTransport(opTypes);
}

std::vector<uint32_t> ComposeTransportManager::BuildRankProtocols(uint32_t rankId) const
{
    std::vector<uint32_t> protocols;
    const auto layers = tagManager_->GetRank2RankLayers(rankId, options_.rankId);
    if (!layers.empty()) {
        auto opTypes = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
        opTypes = FilterUnreachableLayers(rankId, opTypes);
        for (const auto layer : layers) {
            const auto protocol = FindTransportProtocol(static_cast<uint32_t>(layer));
            if (protocol != 0U && (opTypes & static_cast<uint32_t>(layer)) != 0U &&
                std::find(protocols.begin(), protocols.end(), protocol) == protocols.end()) {
                protocols.emplace_back(protocol);
            }
        }
        return protocols;
    }

    static constexpr std::array<hybm_data_op_type, 7> kPrioritedOpTypes = {
        HYBM_DOP_TYPE_SDMA,      HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_DEVICE_URMA, HYBM_DOP_TYPE_HOST_RDMA,
        HYBM_DOP_TYPE_HOST_URMA, HYBM_DOP_TYPE_HOST_TCP,    HYBM_DOP_TYPE_HOST_SHM};
    const auto opTypes = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
    for (auto opType : kPrioritedOpTypes) {
        if ((opTypes & static_cast<uint32_t>(opType)) == 0U) {
            continue;
        }
        const auto protocol = FindTransportProtocol(static_cast<uint32_t>(opType));
        if (protocol != 0U && std::find(protocols.begin(), protocols.end(), protocol) == protocols.end()) {
            protocols.emplace_back(protocol);
        }
    }
    return protocols;
}

uint32_t ComposeTransportManager::FilterUnreachableLayers(uint32_t rankId, uint32_t opTypes) const
{
    transport::TransportRankPrepareInfo physical;
    {
        std::shared_lock<std::shared_mutex> lock(rankPrepareInfosMutex_);
        auto it = rankPrepareInfos_.find(rankId);
        if (it == rankPrepareInfos_.end()) {
            return opTypes;
        }
        physical = it->second;
    }
    if ((opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA)) != 0U &&
        !MemSegment::CanSdmaReaches(physical.superPodId, physical.serverId,
                                    static_cast<uint32_t>(physical.devicePhyId))) {
        opTypes &= ~static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA);
    }
    if ((opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_AIV_SDMA)) != 0U &&
        !MemSegment::CanSdmaReaches(physical.superPodId, physical.serverId,
                                    static_cast<uint32_t>(physical.devicePhyId))) {
        opTypes &= ~static_cast<uint32_t>(HYBM_DOP_TYPE_AIV_SDMA);
    }
    if ((opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_MTE)) != 0U &&
        !MemSegment::CanSdmaReaches(physical.superPodId, physical.serverId,
                                    static_cast<uint32_t>(physical.devicePhyId))) {
        opTypes &= ~static_cast<uint32_t>(HYBM_DOP_TYPE_MTE);
    }
    if ((opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_SHM)) != 0U &&
        !MemSegment::CanLocalHostReaches(physical.superPodId, physical.serverId,
                                         static_cast<uint32_t>(physical.devicePhyId))) {
        opTypes &= ~static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_SHM);
    }
    return opTypes;
}

const std::string &ComposeTransportManager::GetNic() const
{
    return nicInfo_;
}

Result ComposeTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    for (const auto &[protocol, manager] : GetRankTransports(rankId)) {
        auto ret = manager->ReadRemote(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemote by transport, rankId: " << rankId << ", ret: " << ret);
    }

    BM_LOG_ERROR("Failed to ReadRemote, rankId: " << rankId << std::hex << " lAddr: 0x" << lAddr << " rAddr: 0x"
                                                  << rAddr << std::dec << " size: " << size);
    return BM_ERROR;
}

Result ComposeTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    for (const auto &[protocol, manager] : GetRankTransports(rankId)) {
        auto ret = manager->WriteRemote(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to WriteRemote by transport, rankId: " << rankId << ", ret: " << ret);
    }
    BM_LOG_ERROR("Failed to WriteRemote, rankId: " << rankId << std::hex << " lAddr: 0x" << lAddr << " rAddr: 0x"
                                                   << rAddr << std::dec << " size: " << size);
    return BM_ERROR;
}

Result ComposeTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    for (const auto &[protocol, manager] : GetRankTransports(rankId)) {
        auto ret = manager->ReadRemoteAsync(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemoteAsync by transport, rankId: " << rankId << ", ret: " << ret << ", lAddr: 0x"
                                                                        << std::hex << lAddr << ", rAddr: 0x" << rAddr
                                                                        << std::dec << ", size: " << size);
    }

    BM_LOG_ERROR("Failed to ReadRemote.");
    return BM_ERROR;
}

Result ComposeTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    for (const auto &[protocol, manager] : GetRankTransports(rankId)) {
        auto ret = manager->ReadRemoteBatchAsync(rankId, descriptor);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemoteBatchAsync by transport, rankId: " << rankId << ", ret: " << ret);
    }

    BM_LOG_ERROR("Failed to ReadRemote.");
    return BM_ERROR;
}

Result ComposeTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    for (const auto &[protocol, manager] : GetRankTransports(rankId)) {
        auto ret = manager->WriteRemoteAsync(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to WriteRemoteAsync by transport, rankId: " << rankId << ", ret: " << ret);
    }

    BM_LOG_ERROR("Failed to WriteRemote.");
    return BM_ERROR;
}
Result ComposeTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    for (const auto &[protocol, manager] : GetRankTransports(rankId)) {
        auto ret = manager->WriteRemoteBatchAsync(rankId, descriptor);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to WriteRemoteBatchAsync by transport, rankId: " << rankId << ", ret: " << ret);
    }

    BM_LOG_ERROR("Failed to WriteRemote.");
    return BM_ERROR;
}

Result ComposeTransportManager::TransferRemoteBatchAsync(
    const hybm_batch_copy_params &params, hybm_data_copy_direction direction, const RankGroupMap &groupMap,
    std::vector<uint32_t> &localIndices, RankGroupMap &unregisteredGroups, std::set<uint32_t> &batchRanks)
{
    batchRanks.clear();
    localIndices.clear();
    unregisteredGroups.clear();
    std::map<uint32_t, std::pair<std::shared_ptr<TransportManager>, RankGroupMap>> groupedTransfers;
    for (const auto &[p2pInfo, indices] : groupMap) {
        uint32_t remoteRank = 0U;
        if (p2pInfo.second == options_.rankId) {
            remoteRank = p2pInfo.first;
        } else if (p2pInfo.first == options_.rankId) {
            remoteRank = p2pInfo.second;
        } else {
            BM_LOG_ERROR("Transfer group excludes local rank, localRank: "
                         << options_.rankId << ", srcRank: " << p2pInfo.first << ", destRank: " << p2pInfo.second
                         << ", batchSize: " << params.batchSize);
            return BM_INVALID_PARAM;
        }

        const auto rankTransports = GetRankTransports(remoteRank);
        auto rankTransport = std::find_if(rankTransports.begin(), rankTransports.end(), [](const auto &transport) {
            return (transport.first & DEVICE_PROTOCOL) != 0U;
        });
        if (rankTransport == rankTransports.end()) {
            BM_LOG_ERROR("No device transport for transfer rank, localRank: "
                         << options_.rankId << ", remoteRank: " << remoteRank << ", batchSize: " << params.batchSize);
            return BM_INVALID_PARAM;
        }
        auto &groupedTransfer = groupedTransfers[rankTransport->first];
        groupedTransfer.first = rankTransport->second;
        groupedTransfer.second[p2pInfo] = indices;
    }

    for (const auto &[protocol, groupedTransfer] : groupedTransfers) {
        std::vector<uint32_t> localResult;
        RankGroupMap unregisteredResult;
        std::set<uint32_t> batchRanksResult;
        auto ret = groupedTransfer.first->TransferRemoteBatchAsync(params, direction, groupedTransfer.second,
                                                                   localResult, unregisteredResult, batchRanksResult);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to transfer remote batch by transport, localRank: "
                         << options_.rankId << ", protocol: " << protocol << ", batchSize: " << params.batchSize
                         << ", ret: " << ret);
            return ret;
        }

        localIndices.insert(localIndices.end(), localResult.begin(), localResult.end());
        for (const auto &[p2pInfo, indices] : unregisteredResult) {
            auto &mergedIndices = unregisteredGroups[p2pInfo];
            mergedIndices.insert(mergedIndices.end(), indices.begin(), indices.end());
        }
        batchRanks.insert(batchRanksResult.begin(), batchRanksResult.end());
    }

    return BM_OK;
}

Result ComposeTransportManager::Synchronize(uint32_t rankId)
{
    for (const auto &[protocol, manager] : GetRankTransports(rankId)) {
        auto ret = manager->Synchronize(rankId);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to Synchronize by transport, rankId: " << rankId << ", ret: " << ret);
    }

    BM_LOG_ERROR("Failed to Synchronize, rankId: " << rankId);
    return BM_ERROR;
}

Result ComposeTransportManager::SynchronizeRanks(const std::set<uint32_t> &rankIds)
{
    if (rankIds.empty()) {
        return BM_OK;
    }
    std::map<uint32_t, std::pair<std::shared_ptr<TransportManager>, std::set<uint32_t>>> groupedRanks;
    for (const auto rankId : rankIds) {
        const auto rankTransports = GetRankTransports(rankId);
        auto rankTransport = std::find_if(rankTransports.begin(), rankTransports.end(), [](const auto &transport) {
            return (transport.first & DEVICE_PROTOCOL) != 0U;
        });
        if (rankTransport == rankTransports.end()) {
            BM_LOG_ERROR("No device transport for synchronize rank, localRank: "
                         << options_.rankId << ", remoteRank: " << rankId << ", rankNum: " << rankIds.size());
            return BM_INVALID_PARAM;
        }
        auto &groupedRank = groupedRanks[rankTransport->first];
        groupedRank.first = rankTransport->second;
        groupedRank.second.insert(rankId);
    }

    for (const auto &[protocol, groupedRank] : groupedRanks) {
        auto ret = groupedRank.first->SynchronizeRanks(groupedRank.second);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to SynchronizeRanks by transport, localRank: "
                         << options_.rankId << ", protocol: " << protocol << ", rankNum: " << groupedRank.second.size()
                         << ", ret: " << ret);
            return ret;
        }
    }

    return BM_OK;
}

Result ComposeTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    Result lastResult = BM_OK;
    {
        std::lock_guard<std::shared_mutex> lock(rankPrepareInfosMutex_);
        for (const auto &item : options.options) {
            rankPrepareInfos_[item.first] = item.second;
        }
    }
    for (const auto &[protocol, manager] : transports_) {
        HybmTransPrepareOptions transportOptions{};
        GetTransportPrepareOptions(options, protocol, transportOptions);
        BM_LOG_DEBUG("Try to update transport rank options: " << transportOptions);
        auto ret = manager->UpdateRankOptions(transportOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to update rank options, protocol: " << protocol << ", ret: " << ret);
            lastResult = ret;
        }
    }
    return lastResult;
}

// 返回的指针仅在下次 Prepare/UpdateRankOptions/RemoveRanks 调用前有效，
// 调用方不得跨这些调用持有该指针（底层 map 可能 rehash/erase 导致悬垂）。
const TransportRankPrepareInfo *ComposeTransportManager::GetRankPrepareInfo(uint32_t rankId) const
{
    std::shared_lock<std::shared_mutex> lock(rankPrepareInfosMutex_);
    auto it = rankPrepareInfos_.find(rankId);
    return it != rankPrepareInfos_.end() ? &it->second : nullptr;
}

const TransportPrivateData ComposeTransportManager::GetPrivateData() const
{
    auto hostTransportManager = FindTransport(HOST_PROTOCOL);
    if (hostTransportManager != nullptr) {
        return hostTransportManager->GetPrivateData();
    }
    auto deviceTransportManager = FindTransport(DEVICE_PROTOCOL);
    if (deviceTransportManager != nullptr) {
        return deviceTransportManager->GetPrivateData();
    }
    return TransportPrivateData{};
}

const void *ComposeTransportManager::GetQpInfo() const
{
    auto deviceTransportManager = FindTransport(DEVICE_PROTOCOL);
    if (deviceTransportManager != nullptr) {
        return deviceTransportManager->GetQpInfo();
    }
    return nullptr;
}
