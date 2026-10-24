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

#include <vector>
#include <string>
#include <cstring>

#include "hybm_def.h"
#include "hybm_logger.h"
#include "host_hcom_transport_manager.h"
#include "host_rdma_hcomm_transport_manager.h"
#include "device_rdma_transport_manager.h"
#include "device_rdma_aicpu_transport_manager.h"
#include "hybm_gva_version.h"
#include "device_urma_transport_manager.h"
#include "device_rdma_hcomm_transport_manager.h"
#include "dl_acl_api.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "mf_str_util.h"

using namespace ock::mf;
using namespace ock::mf::transport;

namespace {
const char NIC_DELIMITER = ';';
const std::string HOST_TRANSPORT_TYPE = "host#";
const std::string DEVICE_TRANSPORT_TYPE = "device#";
const uint32_t HOST_PROTOCOL = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
const uint32_t DEVICE_PROTOCOL = HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA;
} // namespace

Result ComposeTransportManager::OpenHostTransport(const TransportOptions &options)
{
    if (hostTransportManager_ != nullptr) {
        BM_LOG_ERROR("Failed to open host transport is opened");
        return BM_ERROR;
    }
    // Per-entity host transport instance: the per-rank channel/key state of a singleton cannot serve
    // several store groups of the same process at the same time (the multi-connection replay case).
    // Host HCOMM uses the explicitly configured server port; each server entity needs a distinct port.
    const bool useHcomm = (options.protocol & HYBM_DOP_TYPE_HOST_RDMA) != 0 &&
                          MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_HOST_RDMA_USE_HCOMM, 0u) != 0u;
    if (useHcomm) {
        hostTransportManager_ = std::make_shared<host::HostRdmaHcommTransportManager>();
    } else {
        hostTransportManager_ = std::make_shared<host::HcomTransportManager>();
    }
    return hostTransportManager_->OpenDevice(options);
}

namespace {
// HCOMM 要求 CANN >= 9.1（对应 ACL 版本 >= 1.17，AclrtGetVersion 返回 ACL 版本号）
constexpr int32_t HCOMM_REQUIRED_MAJOR = 1;
constexpr int32_t HCOMM_REQUIRED_MINOR = 17;
} // namespace

Result ComposeTransportManager::OpenDeviceTransport(const TransportOptions &options)
{
    if (deviceTransportManager_ != nullptr) {
        BM_LOG_ERROR("Failed to open device transport is opened");
        return BM_ERROR;
    }
    if (options.protocol & HYBM_DOP_TYPE_DEVICE_URMA) {
        deviceTransportManager_ = std::make_shared<device::DeviceUrmaTransportManager>();
    } else if (options.protocol & HYBM_DOP_TYPE_DEVICE_RDMA) {
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
            deviceTransportManager_ = std::make_shared<device::DeviceRdmaHcommTransportManager>();
        } else {
            // AICPU 直发开关（默认关闭）：打开后用 AI QP + AICPU kernel 提交 WQE，host 仅敲 doorbell。
            // 仅适用于非 HYBM_TYPE_AI_CORE_INITIATE 场景（AI_CORE_INITIATE 用 FixedRanksQpManager，走基类）。
            static const bool aicpuEnabled = MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_RDMA_USE_AICPU, 0u) != 0u;
            const bool useAicpu = aicpuEnabled && (options.initialType != HYBM_TYPE_AI_CORE_INITIATE);
            if (useAicpu) {
                BM_LOG_INFO("using AICPU device RDMA transport, rankId: " << options.rankId);
                deviceTransportManager_ = std::make_shared<device::RdmaAicpuTransportManager>();
            } else {
                BM_LOG_INFO("using native device RDMA transport");
                deviceTransportManager_ = std::make_shared<device::RdmaTransportManager>();
            }
        }
    } else {
        BM_LOG_ERROR("Failed to open device transport with unsupported protocol: " << options.protocol
                                                                                   << ", rankId: " << options.rankId);
        return BM_INVALID_PARAM;
    }
    return deviceTransportManager_->OpenDevice(options);
}

bool ComposeTransportManager::IsDeviceUrma() const
{
    return (options_.protocol & HYBM_DOP_TYPE_DEVICE_URMA) != 0;
}

Result ComposeTransportManager::OpenDevice(const TransportOptions &options)
{
    const bool hostHcomm = (options.protocol & HYBM_DOP_TYPE_HOST_RDMA) &&
                           MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_HOST_RDMA_USE_HCOMM, 0u) != 0u;
    const bool deviceHcomm = (options.protocol & HYBM_DOP_TYPE_DEVICE_RDMA) &&
                             MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_RDMA_USE_HCOMM, 0u) != 0u;
    BM_VALIDATE_RETURN(!(hostHcomm || deviceHcomm) || (options.protocol == HYBM_DOP_TYPE_HOST_RDMA ||
                                                       options.protocol == HYBM_DOP_TYPE_DEVICE_RDMA),
                       "HCOMM requires one network backend per entity", BM_INVALID_PARAM);
    options_ = options;
    Result ret = BM_ERROR;
    if (options_.protocol & HOST_PROTOCOL) {
        ret = OpenHostTransport(options);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to open device transport nic: " << options.nic);
            CloseDevice();
            return BM_ERROR;
        }
    }

    if (options_.protocol & DEVICE_PROTOCOL) {
        ret = OpenDeviceTransport(options);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to open device transport, nic: " << options.nic << ", protocol: " << options.protocol
                                                                  << ", rankId: " << options.rankId
                                                                  << ", ret: " << ret);
            CloseDevice();
            return BM_ERROR;
        }
    }

    std::stringstream ss;
    if ((options_.protocol & HOST_PROTOCOL)) {
        if (hostTransportManager_ == nullptr) {
            BM_LOG_ERROR("Failed to open host transport nic: " << options.nic);
            CloseDevice();
            return BM_ERROR;
        }
        ss << HOST_TRANSPORT_TYPE << hostTransportManager_->GetNic() << NIC_DELIMITER;
    }
    if (options_.protocol & DEVICE_PROTOCOL) {
        if (deviceTransportManager_ == nullptr) {
            BM_LOG_ERROR("Failed to open device transport nic: " << options.nic);
            CloseDevice();
            return BM_ERROR;
        }
        ss << DEVICE_TRANSPORT_TYPE << deviceTransportManager_->GetNic() << NIC_DELIMITER;
    }
    nicInfo_ = ss.str();
    BM_LOG_TRACE("Success to open device rankId:" << options_.rankId << " protocol:" << options_.protocol
                                                  << " nic:" << nicInfo_);
    return BM_OK;
}

Result ComposeTransportManager::CloseDevice()
{
    Result finalRet = BM_OK;
    if (deviceTransportManager_) {
        auto ret = deviceTransportManager_->CloseDevice();
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
        deviceTransportManager_.reset();
    }

    if (hostTransportManager_) {
        auto ret = hostTransportManager_->CloseDevice();
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
        hostTransportManager_.reset();
    }

    return finalRet;
}

Result ComposeTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    bool deviceRegistered = false;
    if (deviceTransportManager_) {
        Result ret = deviceTransportManager_->RegisterMemoryRegion(mr);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to register memory region " << mr);
            return ret;
        }
        deviceRegistered = true;
    }
    if (hostTransportManager_) {
        Result ret = hostTransportManager_->RegisterMemoryRegion(mr);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to register memory region " << mr);
            if (deviceRegistered && deviceTransportManager_) {
                (void)deviceTransportManager_->UnregisterMemoryRegion(mr.addr);
            }
            return ret;
        }
    }
    ComposeMemoryRegion cmr{mr.addr, mr.size, TT_COMPOSE};
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
    if (deviceTransportManager_) {
        Result ret = deviceTransportManager_->UnregisterMemoryRegion(addr);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to unregister mr addr, ret: " << ret);
            return ret;
        }
    }
    if (hostTransportManager_) {
        Result ret = hostTransportManager_->UnregisterMemoryRegion(addr);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to unregister mr addr, ret: " << ret);
            return ret;
        }
    }
    mrs_.erase(pos);
    return BM_OK;
}

bool ComposeTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    if (deviceTransportManager_) {
        auto ret = deviceTransportManager_->QueryHasRegistered(addr, size);
        if (ret) {
            return ret;
        }
    }
    if (hostTransportManager_) {
        auto ret = hostTransportManager_->QueryHasRegistered(addr, size);
        if (ret) {
            return ret;
        }
    }
    return false;
}

Result ComposeTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    // device and host payloads are wrapped into a length-prefixed envelope; each keeps its own variable length.
    if (deviceTransportManager_) {
        TransportMemoryKey deviceKey{};
        auto ret = deviceTransportManager_->QueryMemoryKey(addr, deviceKey);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to query device transport memKey addr:" << std::hex << addr);
            return ret;
        }
        WriteDeviceMemoryKey(deviceKey, key);
    }

    if (hostTransportManager_) {
        TransportMemoryKey hostKey{};
        auto ret = hostTransportManager_->QueryMemoryKey(addr, hostKey);
        if (ret != BM_OK) {
            BM_LOG_WARN("Unable to query host transport memKey addr:" << std::hex << addr);
            if (hostTransportManager_->IsHcomm()) {
                return ret;
            }
            // HCOM 无法处理HBM池，这里直接返回，兼容即开启HBM池又要走HCOM通信的场景
        }
        WriteHostMemoryKey(hostKey, key);
    }

    return BM_OK;
}

Result ComposeTransportManager::QueryControlKey(TransportMemoryKey &key)
{
    key = {};
    for (const auto &manager : {deviceTransportManager_, hostTransportManager_}) {
        if (manager == nullptr || !manager->IsHcomm()) {
            continue;
        }
        TransportMemoryKey control_key{};
        auto ret = manager->QueryControlKey(control_key);
        BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "Failed to export HCOMM control key, rank: " << options_.rankId
                                                                                           << ", ret: " << ret);
        if (manager == deviceTransportManager_) {
            WriteDeviceMemoryKey(control_key, key);
        } else {
            WriteHostMemoryKey(control_key, key);
        }
    }
    return BM_OK;
}

void ComposeTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    // Split first, then update each payload, then rebuild the envelope: writing one payload resets the envelope.
    TransportMemoryKey deviceKey{};
    TransportMemoryKey hostKey{};
    if ((hostTransportManager_ && hostTransportManager_->IsHcomm()) ||
        (deviceTransportManager_ && deviceTransportManager_->IsHcomm())) {
        // Preserve the remote envelope slot. A single HCOMM backend can relocate either payload format.
        ReadDeviceMemoryKey(key, deviceKey);
        ReadHostMemoryKey(key, hostKey);
        if (!deviceKey.Empty()) {
            const auto &manager = deviceTransportManager_ ? deviceTransportManager_ : hostTransportManager_;
            manager->UpdateMemoryKey(deviceKey, addr);
        }
        if (!hostKey.Empty()) {
            const auto &manager = hostTransportManager_ ? hostTransportManager_ : deviceTransportManager_;
            manager->UpdateMemoryKey(hostKey, addr);
        }
        WriteDeviceMemoryKey(deviceKey, key);
        WriteHostMemoryKey(hostKey, key);
        return;
    }
    if (deviceTransportManager_) {
        ReadDeviceMemoryKey(key, deviceKey);
    }
    if (hostTransportManager_) {
        ReadHostMemoryKey(key, hostKey);
    }
    if (deviceTransportManager_) {
        deviceTransportManager_->UpdateMemoryKey(deviceKey, addr);
    }
    if (hostTransportManager_) {
        hostTransportManager_->UpdateMemoryKey(hostKey, addr);
    }
    WriteDeviceMemoryKey(deviceKey, key);
    WriteHostMemoryKey(hostKey, key);
}

namespace {
Result RouteHcommPrepareOptions(const HybmTransPrepareOptions &input, bool localHost, HybmEntityTagInfo &tagManager,
                                uint32_t localRank, HybmTransPrepareOptions &output)
{
    for (const auto &item : input.options) {
        const auto opType = tagManager.GetRank2RankOpType(item.first, localRank);
        if ((opType & (localHost ? HOST_PROTOCOL : DEVICE_PROTOCOL)) == 0) {
            continue;
        }
        const auto &peer = item.second;
        TransportRankPrepareInfo info{};
        info.privateData = peer.privateData;
        info.role = peer.role;
        // Empty private data on an update is allowed by the backend only after it has cached the initial data.
        // A cross-envelope update still needs the header to establish HCOMM compatibility at this layer.
        device::RdmaHcommPeerPrivateInfo privateInfo{};
        if (!peer.privateData.key.Empty()) {
            auto ret = device::ReadHcommPrivateData(peer.privateData.key, privateInfo);
            if (ret != BM_OK) {
                return ret;
            }
        }
        bool haveLocation = false;
        bool remoteHost = localHost;
        for (const auto &key : peer.memKeys) {
            BM_VALIDATE_RETURN(key.WordCount() >= TRANS_KEY_ENVELOPE_WORDS,
                               "Truncated HCOMM compose envelope, rank: " << item.first, BM_INVALID_PARAM);
            const auto available = key.WordCount() - TRANS_KEY_ENVELOPE_WORDS;
            BM_VALIDATE_RETURN(key.keys[0] <= available && key.keys[1] <= available - key.keys[0] &&
                                   key.keys[0] + key.keys[1] == available,
                               "Invalid HCOMM compose payload lengths, rank: " << item.first, BM_INVALID_PARAM);
            TransportMemoryKey hostKey{}, deviceKey{};
            ReadHostMemoryKey(key, hostKey);
            ReadDeviceMemoryKey(key, deviceKey);
            BM_VALIDATE_RETURN(hostKey.Empty() || deviceKey.Empty(),
                               "Mixed HCOMM compose payloads are unsupported, rank: " << item.first, BM_INVALID_PARAM);
            if (hostKey.Empty() && deviceKey.Empty()) {
                continue;
            }
            BM_VALIDATE_RETURN(!peer.privateData.key.Empty(),
                               "HCOMM MR routing requires private data, rank: " << item.first, BM_INVALID_PARAM);
            const bool fromHost = !hostKey.Empty();
            const auto &candidate = fromHost ? hostKey : deviceKey;
            // Same-backend Device traffic keeps its original opaque MR descriptor format.
            if (fromHost || localHost) {
                device::HcommKeyView view{};
                auto ret = device::ReadHcommKey(candidate, privateInfo, false, view);
                if (ret != BM_OK) {
                    return ret;
                }
                const auto expected = fromHost ? ENDPOINT_LOC_TYPE_HOST : ENDPOINT_LOC_TYPE_DEVICE;
                BM_VALIDATE_RETURN(view.endpoint.loc.locType == expected,
                                   "Invalid cross-type HCOMM payload, rank: " << item.first, BM_INVALID_PARAM);
            }
            BM_VALIDATE_RETURN(!haveLocation || remoteHost == fromHost,
                               "HCOMM peer payload location changed, rank: " << item.first, BM_INVALID_PARAM);
            haveLocation = true;
            remoteHost = fromHost;
            info.peerProtocol = fromHost ? HYBM_DOP_TYPE_HOST_RDMA : HYBM_DOP_TYPE_DEVICE_RDMA;
            info.memKeys.push_back(candidate);
        }
        const auto nics = StrUtil::Split(peer.nic, NIC_DELIMITER);
        for (const auto &nic : nics) {
            const auto &prefix = remoteHost ? HOST_TRANSPORT_TYPE : DEVICE_TRANSPORT_TYPE;
            if (StrUtil::StartWith(nic, prefix)) {
                info.nic = nic.substr(prefix.length());
            }
        }
        BM_VALIDATE_RETURN(!haveLocation || !info.nic.empty(),
                           "HCOMM peer NIC prefix disagrees with its payload, rank: " << item.first, BM_INVALID_PARAM);
        // With no MR, preserve private data and defer endpoint/channel creation to a later update.
        output.options.emplace(item.first, std::move(info));
    }
    return BM_OK;
}
} // namespace

Result ComposeTransportManager::GetHostPrepareOptions(const HybmTransPrepareOptions &param,
                                                      HybmTransPrepareOptions &hostOptions)
{
    if (hostTransportManager_ && hostTransportManager_->IsHcomm()) {
        return RouteHcommPrepareOptions(param, true, *tagManager_, options_.rankId, hostOptions);
    }
    auto options = param.options;
    for (const auto &item : options) {
        auto rankId = item.first;
        uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
        if (!(opType & HOST_PROTOCOL)) {
            BM_LOG_DEBUG("remote rank:" << rankId << " to local rank:" << options_.rankId << " use protocol:" << opType
                                        << " skip host connect");
            continue;
        }
        TransportRankPrepareInfo info{};
        std::vector<std::string> nicVec = StrUtil::Split(item.second.nic, NIC_DELIMITER);
        for (const auto &nic : nicVec) {
            if (StrUtil::StartWith(nic, HOST_TRANSPORT_TYPE)) {
                info.nic = nic.substr(HOST_TRANSPORT_TYPE.length());
            }
        }

        for (auto &key : item.second.memKeys) {
            TransportMemoryKey tmp{};
            ReadHostMemoryKey(key, tmp);
            info.memKeys.emplace_back(tmp);
        }
        hostOptions.options.emplace(rankId, info);
    }
    return BM_OK;
}

Result ComposeTransportManager::GetDevicePrepareOptions(const HybmTransPrepareOptions &param,
                                                        HybmTransPrepareOptions &deviceOptions)
{
    if (deviceTransportManager_ && deviceTransportManager_->IsHcomm()) {
        return RouteHcommPrepareOptions(param, false, *tagManager_, options_.rankId, deviceOptions);
    }
    auto options = param.options;
    for (const auto &item : options) {
        auto rankId = item.first;
        uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
        if (!(opType & DEVICE_PROTOCOL)) {
            BM_LOG_DEBUG("remote rank:" << rankId << " to local rank:" << options_.rankId << " use protocol:" << opType
                                        << " skip device connect");
            continue;
        }
        TransportRankPrepareInfo info{};
        std::vector<std::string> nicVec = StrUtil::Split(item.second.nic, NIC_DELIMITER);
        for (const auto &nic : nicVec) {
            if (StrUtil::StartWith(nic, DEVICE_TRANSPORT_TYPE)) {
                info.nic = nic.substr(DEVICE_TRANSPORT_TYPE.length());
            }
        }

        for (auto &key : item.second.memKeys) {
            TransportMemoryKey tmp{};
            ReadDeviceMemoryKey(key, tmp);
            info.memKeys.emplace_back(tmp);
        }
        info.privateData = item.second.privateData;
        deviceOptions.options.emplace(rankId, info);
    }
    return BM_OK;
}

Result ComposeTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    Result ret = BM_OK;
    if (hostTransportManager_) {
        HybmTransPrepareOptions hostOptions{};
        ret = GetHostPrepareOptions(options, hostOptions);
        if (ret != BM_OK) {
            return ret;
        }
        ret = hostTransportManager_->Prepare(hostOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to prepare host ret: " << ret);
            return ret;
        }
    }
    if (deviceTransportManager_) {
        HybmTransPrepareOptions deviceOptions{};
        ret = GetDevicePrepareOptions(options, deviceOptions);
        if (ret != BM_OK) {
            return ret;
        }
        ret = deviceTransportManager_->Prepare(deviceOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to prepare host ret: " << ret);
            return ret;
        }
    }
    return BM_OK;
}

Result ComposeTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    BM_LOG_INFO("compose RemoveRanks called, ranks: "
                << removedRanks.size() << " host: " << (hostTransportManager_ != nullptr ? "non-null" : "nullptr")
                << " device: " << (deviceTransportManager_ != nullptr ? "non-null" : "nullptr"));
    Result lastResult = BM_OK;
    if (hostTransportManager_) {
        auto ret = hostTransportManager_->RemoveRanks(removedRanks);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed for host transport manager remove ranks ret: " << ret);
            lastResult = ret;
        }
    }

    if (deviceTransportManager_) {
        auto ret = deviceTransportManager_->RemoveRanks(removedRanks);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed for device transport manager remove ranks ret: " << ret);
            lastResult = ret;
        }
    }

    return lastResult;
}

Result ComposeTransportManager::ConnectRank(uint32_t rankId)
{
    Result ret = BM_OK;
    if (hostTransportManager_) {
        ret = hostTransportManager_->ConnectRank(rankId);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to connect host rank " << rankId << " ret: " << ret);
            return ret;
        }
    }
    if (deviceTransportManager_) {
        ret = deviceTransportManager_->ConnectRank(rankId);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to connect device rank " << rankId << " ret: " << ret);
            return ret;
        }
    }
    return BM_OK;
}

const std::string &ComposeTransportManager::GetNic() const
{
    return nicInfo_;
}

Result ComposeTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
    // 传输顺序 device_rdma -> host_rdma
    if ((opType & DEVICE_PROTOCOL) && deviceTransportManager_ != nullptr) {
        auto ret = deviceTransportManager_->ReadRemote(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemote by device transport ret:" << ret);
    }

    if ((opType & HOST_PROTOCOL) && hostTransportManager_ != nullptr) {
        auto ret = hostTransportManager_->ReadRemote(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemote by host transport, ret: " << ret);
    }

    BM_LOG_ERROR("Failed to ReadRemote, rankId: " << rankId << std::hex << " lAddr: 0x" << lAddr << " rAddr: 0x"
                                                  << rAddr << std::dec << " size: " << size);
    return BM_ERROR;
}

Result ComposeTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
    if ((opType & DEVICE_PROTOCOL) && deviceTransportManager_ != nullptr) {
        auto ret = deviceTransportManager_->WriteRemote(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to WriteRemote by device transport, ret: " << ret);
    }

    if ((opType & HOST_PROTOCOL) && hostTransportManager_ != nullptr) {
        auto ret = hostTransportManager_->WriteRemote(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to WriteRemote by host transport, ret: " << ret);
    }
    BM_LOG_ERROR("Failed to WriteRemote, rankId: " << rankId << std::hex << " lAddr: 0x" << lAddr << " rAddr: 0x"
                                                   << rAddr << std::dec << " size: " << size);
    return BM_ERROR;
}

Result ComposeTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
    if ((opType & DEVICE_PROTOCOL) && deviceTransportManager_ != nullptr) {
        auto ret = deviceTransportManager_->ReadRemoteAsync(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemoteAsync by device transport ret:" << ret);
    }

    if ((opType & HOST_PROTOCOL) && hostTransportManager_ != nullptr) {
        auto ret = hostTransportManager_->ReadRemoteAsync(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemoteAsync by host transport ret:" << ret << " remote rankId:" << rankId
                                                                        << " lAddr:" << std::hex << lAddr
                                                                        << " rAddr:" << rAddr << " size:" << size);
    }

    BM_LOG_ERROR("Failed to ReadRemote.");
    return BM_ERROR;
}

Result ComposeTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
    if ((opType & DEVICE_PROTOCOL) && deviceTransportManager_ != nullptr) {
        auto ret = deviceTransportManager_->ReadRemoteBatchAsync(rankId, descriptor);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemoteBatchAsync by device transport ret:" << ret << ", remote rankId:" << rankId);
    }

    if ((opType & HOST_PROTOCOL) && hostTransportManager_ != nullptr) {
        auto ret = hostTransportManager_->ReadRemoteBatchAsync(rankId, descriptor);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemoteBatchAsync by host transport ret:" << ret);
    }

    BM_LOG_ERROR("Failed to ReadRemote.");
    return BM_ERROR;
}

Result ComposeTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
    if ((opType & DEVICE_PROTOCOL) && deviceTransportManager_ != nullptr) {
        auto ret = deviceTransportManager_->WriteRemoteAsync(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to ReadRemoteAsync by device transport ret:" << ret);
    }

    if ((opType & HOST_PROTOCOL) && hostTransportManager_ != nullptr) {
        auto ret = hostTransportManager_->WriteRemoteAsync(rankId, lAddr, rAddr, size);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to WriteRemoteAsync by host transport ret:" << ret);
    }

    BM_LOG_ERROR("Failed to WriteRemote.");
    return BM_ERROR;
}
Result ComposeTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
    if ((opType & DEVICE_PROTOCOL) && deviceTransportManager_ != nullptr) {
        auto ret = deviceTransportManager_->WriteRemoteBatchAsync(rankId, descriptor);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to WriteRemoteBatchAsync by device transport ret:" << ret);
    }

    if ((opType & HOST_PROTOCOL) && hostTransportManager_ != nullptr) {
        auto ret = hostTransportManager_->WriteRemoteBatchAsync(rankId, descriptor);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to WriteRemoteBatchAsync by host transport ret:" << ret);
    }

    BM_LOG_ERROR("Failed to WriteRemote.");
    return BM_ERROR;
}

Result ComposeTransportManager::TransferRemoteBatchAsync(
    const hybm_batch_copy_params &params, hybm_data_copy_direction direction, const RankGroupMap &groupMap,
    std::vector<uint32_t> &localIndices, RankGroupMap &unregisteredGroups, std::set<uint32_t> &batchRanks)
{
    batchRanks.clear();
    if (deviceTransportManager_ == nullptr) {
        BM_LOG_ERROR("TransferRemoteBatchAsync raw batch device transport is null, batchSize: " << params.batchSize);
        return BM_ERROR;
    }
    return deviceTransportManager_->TransferRemoteBatchAsync(params, direction, groupMap, localIndices,
                                                             unregisteredGroups, batchRanks);
}

Result ComposeTransportManager::Synchronize(uint32_t rankId)
{
    uint32_t opType = tagManager_->GetRank2RankOpType(rankId, options_.rankId);
    if ((opType & DEVICE_PROTOCOL) && deviceTransportManager_ != nullptr) {
        auto ret = deviceTransportManager_->Synchronize(rankId);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to Synchronize by device transport, ret: " << ret << " rankId: " << rankId);
    }

    if ((opType & HOST_PROTOCOL) && hostTransportManager_ != nullptr) {
        auto ret = hostTransportManager_->Synchronize(rankId);
        if (ret == BM_OK) {
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to Synchronize by host transport, ret: " << ret << " rankId: " << rankId);
    }

    BM_LOG_ERROR("Failed to Synchronize, rankId: " << rankId);
    return BM_ERROR;
}

Result ComposeTransportManager::SynchronizeRanks(const std::set<uint32_t> &rankIds)
{
    if (rankIds.empty()) {
        return BM_OK;
    }
    if (deviceTransportManager_ == nullptr) {
        BM_LOG_ERROR("SynchronizeRanks raw batch device transport is null, rankNum: " << rankIds.size());
        return BM_ERROR;
    }

    auto ret = deviceTransportManager_->SynchronizeRanks(rankIds);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to SynchronizeRanks by device transport, ret: " << ret << ", rankNum: " << rankIds.size());
    }
    return ret;
}

Result ComposeTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    Result ret = BM_OK;
    if (hostTransportManager_) {
        HybmTransPrepareOptions hostOptions{};
        ret = GetHostPrepareOptions(options, hostOptions);
        if (ret != BM_OK) {
            return ret;
        }
        BM_LOG_DEBUG("Try to update host transport rank options: " << hostOptions);
        ret = hostTransportManager_->UpdateRankOptions(hostOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to prepare host ret: " << ret);
        }
    }
    if (deviceTransportManager_) {
        HybmTransPrepareOptions deviceOptions{};
        ret = GetDevicePrepareOptions(options, deviceOptions);
        if (ret != BM_OK) {
            return ret;
        }
        BM_LOG_DEBUG("Try to update device transport rank options: " << deviceOptions);
        ret = deviceTransportManager_->UpdateRankOptions(deviceOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to prepare device ret: " << ret);
        }
    }
    return ret;
}

const TransportPrivateData ComposeTransportManager::GetPrivateData() const
{
    if (hostTransportManager_) {
        return hostTransportManager_->GetPrivateData();
    }
    if (deviceTransportManager_) {
        return deviceTransportManager_->GetPrivateData();
    }
    return TransportPrivateData{};
}

const void *ComposeTransportManager::GetQpInfo() const
{
    if (deviceTransportManager_ != nullptr) {
        return deviceTransportManager_->GetQpInfo();
    }
    return nullptr;
}
