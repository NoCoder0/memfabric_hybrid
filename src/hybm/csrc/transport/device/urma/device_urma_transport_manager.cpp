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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cassert>

#include "dl_acl_api.h"
#include "dl_hcomm_api.h"
#include "hybm_batch_transfer.h"
#include "hybm_logger.h"
#include "hybm_stream_manager.h"
#include "device_urma_eid_reader.h"
#include "hybm_va_manager.h"
#include "device_urma_transport_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {
constexpr uint32_t HCOMM_NORMAL_NOTIFY_NUM = 0;
constexpr const char *HYBM_DEVICE_FUNC_READ = "HybmBatchRead";
constexpr const char *HYBM_DEVICE_FUNC_WRITE = "HybmBatchWrite";
constexpr uint32_t HYBM_DEVICE_KERNEL_BLOCK_DIM = 1U;
constexpr uint32_t ACL_NOTIFY_FLAG_DEVICE_ONLY = 0x00000001U; // 使能该bit表示创建的Notify仅在Device上调用。
constexpr uint32_t ACL_MEM_TYPE_HIGH_BAND_WIDTH =
    0x1000U; // 系统内部会默认采取ACL_MEM_MALLOC_HUGE_FIRST，优先申请大页。
constexpr uint32_t ACL_MEM_MALLOC_HUGE_ONLY =
    1; // 申请大页内存，内存申请粒度为2M，不足2M的倍数，向上2M对齐。 表示仅申请大页
constexpr uint16_t HYBM_DEVICE_KERNEL_TIMEOUT_S = 60U;
constexpr uint32_t HYBM_NOTIFY_DEFAULT_WAIT_TIME_S = 27U * 68U;
static_assert(std::is_trivially_copyable<UrmaExportDesc>::value, "UrmaExportDesc must be binary serializable");
static_assert(std::is_trivially_copyable<UrmaEndpointDesc>::value,
              "UrmaEndpointDesc must be trivially copyable for memcpy serialization");

// Must NOT conflict with URMA_EXPORT_DESC_MAGIC (0xA5FAB001).
constexpr uint32_t URMA_PRIVATE_DATA_MAGIC = 0xA5FAC003U;
constexpr uint16_t URMA_PRIVATE_DATA_VERSION = 1U;

struct UrmaPrivateDataDesc {
    uint32_t magic{URMA_PRIVATE_DATA_MAGIC};
    uint16_t version{URMA_PRIVATE_DATA_VERSION};
    uint16_t payloadLen{0};
};

static_assert(sizeof(UrmaPrivateDataDesc) + sizeof(UrmaEndpointDesc) <= sizeof(TransportPrivateData{}.key.keys),
              "UrmaEndpointDesc cannot fit into TransportPrivateData.key");

UrmaMemoryType ToUrmaMemoryType(uint32_t flags)
{
    if (flags & REG_MR_FLAG_HBM) {
        return UrmaMemoryType::DEVICE_HBM;
    }
    return UrmaMemoryType::HOST_DRAM;
}

UrmaCommMem ToUrmaMem(const TransportMemoryRegion &mr)
{
    return UrmaCommMem{mr.addr, mr.size, ToUrmaMemoryType(mr.flags)};
}

bool IsSupportedMemoryFlags(uint32_t flags)
{
    const bool hasDram = (flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0;
    const bool hasHbm = (flags & REG_MR_FLAG_HBM) != 0;
    return !(hasDram && hasHbm);
}

UrmaProtocol ToUrmaProtocol(CommProtocol protocol)
{
    if (protocol == COMM_PROTOCOL_ROCE) {
        return UrmaProtocol::ROCE;
    }
    if (protocol == COMM_PROTOCOL_UBC_TP) {
        return UrmaProtocol::UBC_TP;
    }
    if (protocol == COMM_PROTOCOL_UBC_CTP) {
        return UrmaProtocol::UBC_CTP;
    }
    if (protocol == COMM_PROTOCOL_UBOE) {
        return UrmaProtocol::UBOE;
    }
    return UrmaProtocol::RESERVED;
}

bool ToUrmaEndpointDesc(const EndpointDesc &hcommDesc, UrmaEndpointDesc &urmaDesc)
{
    if (hcommDesc.commAddr.type != COMM_ADDR_TYPE_EID && hcommDesc.commAddr.type != COMM_ADDR_TYPE_IP_V6 &&
        hcommDesc.commAddr.type != COMM_ADDR_TYPE_IP_V4) {
        BM_LOG_ERROR("device_urma topo endpoint must use EID/IP address, addr type: " << hcommDesc.commAddr.type);
        return false;
    }

    auto protocol = ToUrmaProtocol(hcommDesc.protocol);
    if (protocol == UrmaProtocol::RESERVED) {
        BM_LOG_ERROR("device_urma unsupported topo endpoint protocol: " << hcommDesc.protocol);
        return false;
    }

    UrmaEndpointDesc desc{};
    desc.protocol = protocol;
    desc.devPhyId = hcommDesc.loc.device.devPhyId;
    desc.superDevId = hcommDesc.loc.device.superDevId;
    desc.serverIdx = hcommDesc.loc.device.serverIdx;
    desc.superPodIdx = hcommDesc.loc.device.superPodIdx;
    desc.type = hcommDesc.commAddr.type;
    std::memcpy(desc.raws, hcommDesc.commAddr.raws, sizeof(desc.raws));
    urmaDesc = desc;
    return true;
}

bool ContainsAddressRange(uint64_t outerAddr, uint64_t outerSize, uint64_t innerAddr, uint64_t innerSize)
{
    uint64_t outerEnd = 0;
    uint64_t innerEnd = 0;
    const UrmaCommMem outer{outerAddr, outerSize, UrmaMemoryType::HOST_DRAM};
    const UrmaCommMem inner{innerAddr, innerSize, UrmaMemoryType::HOST_DRAM};
    return GetRangeEnd(outer, outerEnd) && GetRangeEnd(inner, innerEnd) && outerAddr <= innerAddr &&
           outerEnd >= innerEnd;
}

// Returns BM_INVALID_PARAM on magic/version/payloadLen/capacity mismatch.
Result ParsePrivateDataToEndpointDesc(const TransportPrivateData &privateData, UrmaEndpointDesc &outDesc)
{
    constexpr size_t headerSize = sizeof(UrmaPrivateDataDesc);
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(privateData.key.keys);

    UrmaPrivateDataDesc header{};
    std::memcpy(&header, raw, headerSize);
    if (header.magic != URMA_PRIVATE_DATA_MAGIC) {
        BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc invalid magic: 0x"
                     << std::hex << header.magic << " expected: 0x" << URMA_PRIVATE_DATA_MAGIC);
        return BM_INVALID_PARAM;
    }
    if (header.version != URMA_PRIVATE_DATA_VERSION) {
        BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc unsupported version: "
                     << header.version << " expected: " << URMA_PRIVATE_DATA_VERSION);
        return BM_INVALID_PARAM;
    }
    if (header.payloadLen != sizeof(UrmaEndpointDesc)) {
        BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc payloadLen mismatch: "
                     << header.payloadLen << " expected: " << sizeof(UrmaEndpointDesc));
        return BM_INVALID_PARAM;
    }
    if (headerSize + header.payloadLen > sizeof(privateData.key.keys)) {
        BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc payload exceeds key capacity, payloadLen: "
                     << header.payloadLen);
        return BM_INVALID_PARAM;
    }
    std::memcpy(&outDesc, raw + headerSize, sizeof(UrmaEndpointDesc));
    return BM_OK;
}

UrmaProtocol GetEndpointProtocolFromOptions(uint32_t protocol)
{
    if (protocol & HYBM_DOP_TYPE_DEVICE_UBOE) {
        return UrmaProtocol::UBOE;
    }
    if (protocol & HYBM_DOP_TYPE_DEVICE_URMA) {
        return UrmaProtocol::UBC_CTP;
    }
    return UrmaProtocol::RESERVED;
}

} // namespace

DeviceUrmaTransportManager::~DeviceUrmaTransportManager()
{
    (void)CloseDevice();
}

Result DeviceUrmaTransportManager::InitLocalDeviceInfoLocked(const TransportOptions &options)
{
    int32_t userId = -1;
    auto ret = DlAclApi::AclrtGetDevice(&userId);
    BM_ASSERT_LOG_AND_RETURN(ret == 0 && userId >= 0,
                             "AclrtGetDevice() return=" << ret << ", output deviceId=" << userId,
                             BM_DL_FUNCTION_FAILED);

    options_ = options;
    rankId_ = options.rankId;
    rankCount_ = options.rankCount;
    userDeviceId_ = static_cast<uint32_t>(userId);
    int32_t phyId = 0;
    // 实测需要使用userDeviceId
    ret = DlAclApi::AclrtGetPhyDevIdByLogicDevId(userId, &phyId);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "aclrtGetPhyDevIdByLogicDevId() return=" << ret << ", userDeviceId=" << userId,
                             BM_DL_FUNCTION_FAILED);
    BM_LOG_INFO("aclrtGetPhyDevIdByLogicDevId: userId=" << userId << ", phyId=" << phyId);
    phyDeviceId_ = static_cast<uint32_t>(phyId);

    // Get device location info
    int64_t infoValue = 0;
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(userId), 0, INFO_TYPE_SDID, &infoValue);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "RtGetDeviceInfo(INFO_TYPE_SDID) return=" << ret, BM_DL_FUNCTION_FAILED);
    sdid_ = static_cast<uint32_t>(infoValue);

    infoValue = 0;
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(userId), 0, INFO_TYPE_SERVER_ID, &infoValue);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "RtGetDeviceInfo(INFO_TYPE_SERVER_ID) return=" << ret, BM_DL_FUNCTION_FAILED);
    serverId_ = static_cast<uint32_t>(infoValue);

    infoValue = 0;
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(userId), 0, INFO_TYPE_SUPER_POD_ID, &infoValue);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "RtGetDeviceInfo(INFO_TYPE_SUPER_POD_ID) return=" << ret, BM_DL_FUNCTION_FAILED);
    superPodId_ = static_cast<uint32_t>(infoValue);
    BM_LOG_INFO("local device info: userId=" << userId << ", phyId=" << phyId << " sdid=" << sdid_
                                             << ", server_id=" << serverId_ << ", superpod id=" << superPodId_);

    return BM_OK;
}

Result DeviceUrmaTransportManager::InitDeviceKernelNotifyLocked()
{
    void *notify = nullptr;
    auto ret = DlAclApi::AclrtCreateNotify(&notify, ACL_NOTIFY_FLAG_DEVICE_ONLY);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma AclrtCreateNotify failed, ret: " << ret);
        return ret;
    }
    notify_ = notify;

    uint32_t notifyId = 0;
    ret = DlAclApi::AclrtGetNotifyId(notify, &notifyId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma AclrtGetNotifyId failed, ret: " << ret);
        (void)DlAclApi::AclrtDestroyNotify(notify_);
        notify_ = nullptr;
        return ret;
    }
    notifyId_ = notifyId;

    uint64_t devAddr = 0;
    uint32_t devLen = 0;
    rtDevResInfo resInfo{};
    resInfo.dieId = 0U;
    resInfo.procType = RT_PROCESS_HCCP;
    resInfo.resType = RT_RES_TYPE_STARS_NOTIFY_RECORD;
    resInfo.resId = notifyId_;
    resInfo.flag = 0U;
    rtDevResAddrInfo addrInfo{};
    addrInfo.resAddress = &devAddr;
    addrInfo.len = &devLen;
    ret = DlRtApi::RtGetDevResAddress(&resInfo, &addrInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma RtGetDevResAddress failed for notify, ret: " << ret << " notifyId: " << notifyId_);
        (void)DlAclApi::AclrtDestroyNotify(notify_);
        notify_ = nullptr;
        notifyId_ = 0;
        return ret;
    }
    notifyAddr_ = devAddr;
    notifyLen_ = devLen;
    BM_LOG_INFO("device_urma notify record addr: " << VaToStr(notifyAddr_) << " len: " << notifyLen_
                                                   << " notifyId: " << notifyId_);
    {
        const UrmaCommMem notifyMem{notifyAddr_, notifyLen_, UrmaMemoryType::DEVICE_HBM};
        HcommMemHandle notifyHandle = nullptr;
        ret = manager_.HcommMemReg(localEndpoint_, notifyAddr_, notifyMem, &notifyHandle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma HcommMemReg for notify addr failed, ret: " << ret);
            (void)DlAclApi::AclrtDestroyNotify(notify_);
            notify_ = nullptr;
            notifyId_ = 0;
            notifyAddr_ = 0;
            notifyLen_ = 0;
            return ret;
        }
        notifyHcommHandle_ = notifyHandle;
    }
    BM_LOG_INFO("device_urma notify addr registered with Hcomm, handle: " << notifyHcommHandle_);
    return BM_OK;
}

Result DeviceUrmaTransportManager::InitDeviceTransferFlagLocked()
{
    // Allocate a local flag buffer on device, initialise to 1, and register with Hcomm.
    // It is exported as an Hcomm flag descriptor in the TransportMemoryKey payload,
    // so remote peers can import it and use the resulting address as remote_flag_addr.
    // NOT inserted into localRegistrations_ — not exported/imported as a regular MR.
    void *flagPtr = nullptr;
    auto ret = DlAclApi::AclrtMalloc(
        &flagPtr, sizeof(int64_t),
        static_cast<aclrtMemMallocPolicy>(ACL_MEM_TYPE_HIGH_BAND_WIDTH | ACL_MEM_MALLOC_HUGE_ONLY));
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma AclrtMalloc for local flag buffer failed, ret: " << ret);
        return ret;
    }
    int64_t flagInit = 1;
    ret = DlAclApi::AclrtMemcpy(flagPtr, sizeof(int64_t), &flagInit, sizeof(int64_t), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma AclrtMemcpy init local flag buffer failed, ret: " << ret);
        (void)DlAclApi::AclrtFree(flagPtr);
        return ret;
    }
    const UrmaCommMem flagMem{reinterpret_cast<uint64_t>(flagPtr), sizeof(int64_t), UrmaMemoryType::DEVICE_HBM};
    HcommMemHandle flagHandle = nullptr;
    ret = manager_.HcommMemReg(localEndpoint_, 1, flagMem, &flagHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma HcommMemReg for local flag buffer failed, ret: " << ret);
        (void)DlAclApi::AclrtFree(flagPtr);
        return ret;
    }
    devTransFlagPtr_ = flagPtr;
    devTransFlagSize_ = sizeof(int64_t);
    devTransFlagHcommHandle_ = flagHandle;
    BM_LOG_INFO("device_urma local flag buffer allocated and registered, addr: "
                << VaToStr(devTransFlagPtr_) << " size: " << devTransFlagSize_
                << " handle: " << devTransFlagHcommHandle_);
    return BM_OK;
}

void DeviceUrmaTransportManager::RollbackOpenDeviceLocked()
{
    if (devTransFlagHcommHandle_ != nullptr) {
        auto ret = manager_.HcommMemUnreg(localEndpoint_, devTransFlagHcommHandle_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma RollbackOpenDevice HcommMemUnreg devTransFlag failed, ret: " << ret);
        }
        devTransFlagHcommHandle_ = nullptr;
    }
    if (devTransFlagPtr_ != nullptr) {
        (void)DlAclApi::AclrtFree(devTransFlagPtr_);
        devTransFlagPtr_ = nullptr;
    }
    if (notifyHcommHandle_ != nullptr) {
        auto ret = manager_.HcommMemUnreg(localEndpoint_, notifyHcommHandle_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma RollbackOpenDevice HcommMemUnreg notify failed, ret: " << ret);
        }
        notifyHcommHandle_ = nullptr;
    }
    if (notify_ != nullptr) {
        (void)DlAclApi::AclrtDestroyNotify(notify_);
        notify_ = nullptr;
    }
    notifyId_ = 0;
    notifyAddr_ = 0;
    notifyLen_ = 0;
    if (localEndpoint_ != nullptr) {
        (void)HcomUrmaDestroyEndpoint(localEndpoint_->hcommEndpoint);
        localEndpoint_.reset();
    }
    localEndpointDesc_ = UrmaEndpointDesc{};
}

Result DeviceUrmaTransportManager::OpenDevice(const TransportOptions &options)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (opened_) {
        return BM_OK;
    }
    if (options.rankCount == 0 || options.rankId >= options.rankCount) {
        BM_LOG_ERROR("device_urma OpenDevice: invalid rankCount or rankId");
        return BM_INVALID_PARAM;
    }
    if (DlAclApi::GetAscendSocType() != AscendSocType::ASCEND_950) {
        BM_LOG_ERROR("device_urma is only supported on Ascend950 soc, rank: " << options.rankId);
        return BM_NOT_SUPPORTED;
    }

    auto ret = InitLocalDeviceInfoLocked(options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma InitLocalDeviceInfoLocked failed, rank=" << options.rankId << " ret: " << ret);
        return ret;
    }
    // Determine endpoint protocol from data_op_type (DEVICE_URMA -> UBC_CTP, DEVICE_UBOE -> UBOE)
    const auto protocol = GetEndpointProtocolFromOptions(options.protocol);
    if (protocol == UrmaProtocol::RESERVED) {
        BM_LOG_ERROR("device_urma OpenDevice unsupported protocol bits: " << options.protocol
                                                                          << ", rankId=" << rankId_);
        return BM_INVALID_PARAM;
    }
    BM_LOG_INFO("device_urma OpenDevice protocol=" << static_cast<int>(protocol) << ", rankId=" << rankId_
                                                    << ", phyDeviceId=" << phyDeviceId_);

    // Build UrmaEndpointDesc and create local endpoint
    UrmaEndpointDesc localDesc{};
    localDesc.protocol = protocol;
    localDesc.devPhyId = phyDeviceId_;
    localDesc.superDevId = sdid_;
    localDesc.serverIdx = serverId_;
    localDesc.superPodIdx = superPodId_;

    if (protocol == UrmaProtocol::UBC_CTP) {
        std::array<uint8_t, COMM_ADDR_EID_LEN> eidData{};
        const auto retEid = GetDeviceUrmaEid(phyDeviceId_, rankId_, eidData);
        if (retEid != 0) {
            return retEid;
        }
        localDesc.type = COMM_ADDR_TYPE_IP_V6;
        std::memcpy(localDesc.raws, eidData.data(), COMM_ADDR_EID_LEN);
    } else if (protocol == UrmaProtocol::UBOE) {
        CommAddrType addrType = COMM_ADDR_TYPE_RESERVED;
        std::array<uint8_t, sizeof(localDesc.raws)> addrData{};
        const auto retIp = GetDeviceUrmaIpAddr(phyDeviceId_, rankId_, addrType, addrData);
        if (retIp != 0) {
            return retIp;
        }
        localDesc.type = addrType;
        const size_t copyLen = (addrType == COMM_ADDR_TYPE_IP_V4) ? sizeof(struct in_addr) : sizeof(struct in6_addr);
        std::memcpy(localDesc.raws, addrData.data(), copyLen);
    } else {
        BM_LOG_ERROR("device_urma unexpected protocol: " << static_cast<int>(protocol));
        return BM_INVALID_PARAM;
    }
    auto endpoint = manager_.CreateEndpoint(localDesc);
    if (endpoint == nullptr) {
        BM_LOG_ERROR("device_urma CreateEndpoint failed, rankId=" << rankId_);
        return BM_MALLOC_FAILED;
    }
    localEndpoint_ = endpoint;
    localEndpointDesc_ = localDesc;

    ret = InitDeviceKernelNotifyLocked();
    if (ret != BM_OK) {
        RollbackOpenDeviceLocked();
        return ret;
    }

    ret = InitDeviceTransferFlagLocked();
    if (ret != BM_OK) {
        RollbackOpenDeviceLocked();
        return ret;
    }

    // preload kernel
    ret = EnsureDeviceKernelLoadedLocked();
    if (ret != BM_OK) {
        RollbackOpenDeviceLocked();
        return ret;
    }

    opened_ = true;
    BM_LOG_INFO("device_urma OpenDevice success, rank: " << rankId_ << " rankCount: " << rankCount_
                                                         << " devPhyId: " << phyDeviceId_);
    return BM_OK;
}

Result DeviceUrmaTransportManager::EnsureDeviceKernelLoadedLocked()
{
    if (deviceKernelLoaded_) {
        return BM_OK;
    }

    auto ret = LoadDeviceKernelAndGetHandles(HYBM_DEVICE_FUNC_READ, HYBM_DEVICE_FUNC_WRITE, deviceKernelHandle_,
                                             deviceFuncHandles_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LoadDeviceKernelAndGetHandles failed, ret: " << ret);
        return ret;
    }
    if (deviceFuncHandles_.batchRead == nullptr || deviceFuncHandles_.batchWrite == nullptr) {
        BM_LOG_ERROR("device_urma invalid device kernel function handles, read: "
                     << deviceFuncHandles_.batchRead << " write: " << deviceFuncHandles_.batchWrite);
        return BM_DL_FUNCTION_FAILED;
    }
    deviceKernelLoaded_ = true;
    return BM_OK;
}

Result DeviceUrmaTransportManager::CloseDevice()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!opened_) {
        return BM_OK;
    }
    Result finalRet = BM_OK;
    // Phase 1: per-rank cleanup — destroy channels/threads, unimport remote memories,
    //          unregister peer handles from each per-rank endpoint, then destroy that endpoint.
    for (auto &rankItem : remoteRanks_) {
        const uint32_t peerRank = rankItem.first;
        auto &state = rankItem.second;
        const auto ret = CleanupPeerRankState(state, peerRank);
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
    }

    // Phase 2: unregister global handle for each local registration (peer handles already removed above)
    const auto retLocalReg = CleanupLocalRegistrationsLocked();
    if (retLocalReg != BM_OK && finalRet == BM_OK) {
        finalRet = retLocalReg;
    }

    // Phase 3+4: unregister handles from localEndpoint_
    auto unregHandle = [&](const char *label, HcommMemHandle &handle) {
        if (handle == nullptr)
            return;
        const auto ret = manager_.HcommMemUnreg(localEndpoint_, handle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma CloseDevice HcommMemUnreg " << label << " failed, ret: " << ret);
            if (finalRet == BM_OK)
                finalRet = ret;
        }
        handle = nullptr;
    };
    unregHandle("devTransFlag", devTransFlagHcommHandle_);
    unregHandle("notify", notifyHcommHandle_);

    // Phase 5: discard remote rank state (per-rank endpoints already destroyed in phase 1)
    remoteRanks_.clear();

    // Phase 6: destroy class-level local endpoint
    if (localEndpoint_ != nullptr) {
        (void)HcomUrmaDestroyEndpoint(localEndpoint_->hcommEndpoint);
        localEndpoint_.reset();
    }
    localEndpointDesc_ = UrmaEndpointDesc{};

    // Phase 7: destroy notify
    if (notify_ != nullptr) {
        (void)DlAclApi::AclrtDestroyNotify(notify_);
        notify_ = nullptr;
        notifyId_ = 0;
    }

    // Phase 8: free dev trans flag buffer
    if (devTransFlagPtr_ != nullptr) {
        (void)DlAclApi::AclrtFree(devTransFlagPtr_);
        devTransFlagPtr_ = nullptr;
    }

    deviceKernelLoaded_ = false;
    opened_ = false;
    return finalRet;
}

Result DeviceUrmaTransportManager::DestroyRankChannelsAndThread(RemoteRankState &state)
{
    Result localResult = BM_OK;
    state.pendingOps = 0;
    if (state.channel != 0) {
        auto hcommChan = state.channel;
        const auto ret = DlHcommApi::HcommChannelDestroy(&hcommChan, 1);
        if (ret != 0 && localResult == BM_OK) {
            BM_LOG_ERROR("device_urma HcommChannelDestroy failed, channel: " << state.channel << " ret: " << ret);
            localResult = BM_DL_FUNCTION_FAILED;
        }
        state.channel = 0;
        state.channelDesc = {};
    }
    if (state.thread != 0) {
        auto hcommThread = state.thread;
        const auto ret = DlHcommApi::HcommThreadFree(&hcommThread, 1);
        if (ret != BM_OK && localResult == BM_OK) {
            BM_LOG_ERROR("device_urma HcommThreadFree failed, thread: " << state.thread << " ret: " << ret);
            localResult = ret;
        }
        state.thread = 0;
    }
    return localResult;
}

Result DeviceUrmaTransportManager::UnimportPeerImportsAndFlag(RemoteRankState &state, uint32_t peerRank)
{
    Result localResult = BM_OK;
    for (auto &remote : state.imports) {
        if (!remote.descBytes.empty()) {
            const auto ret = manager_.HcommMemUnimport(localEndpoint_, remote.descBytes.data(),
                                                       static_cast<uint32_t>(remote.descBytes.size()));
            if (ret != BM_OK) {
                BM_LOG_ERROR("device_urma UnimportPeerImportsAndFlag HcommMemUnimport failed, "
                             << "peerRank: " << peerRank << " descBytes.size: " << remote.descBytes.size()
                             << " ret: " << ret);
                if (localResult == BM_OK)
                    localResult = ret;
            }
        }
    }
    if (!state.remoteFlagDescBytes.empty()) {
        const auto ret = DlHcommApi::HcommMemUnimport(localEndpoint_->hcommEndpoint, state.remoteFlagDescBytes.data(),
                                                      static_cast<uint32_t>(state.remoteFlagDescBytes.size()));
        if (ret != 0) {
            BM_LOG_ERROR("device_urma UnimportPeerImportsAndFlag HcommMemUnimport for flag desc failed, "
                         << "peerRank: " << peerRank << " ret: " << ret);
            if (localResult == BM_OK)
                localResult = BM_DL_FUNCTION_FAILED;
        }
        state.remoteFlagDescBytes.clear();
    }
    state.imports.clear();
    state.remoteFlagAddr = 0;
    state.remoteFlagSize = 0;
    return localResult;
}

Result DeviceUrmaTransportManager::UnregisterPeerHandlesAndDestroyEndpoint(RemoteRankState &state, uint32_t peerRank)
{
    Result localResult = BM_OK;
    if (state.localEndpoint == nullptr) {
        return BM_OK;
    }
    for (auto &item : localRegistrations_) {
        auto handleIt = item.second.peerHandles.find(peerRank);
        if (handleIt == item.second.peerHandles.end()) {
            continue;
        }
        const auto ret = manager_.HcommMemUnreg(state.localEndpoint, handleIt->second);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma UnregisterPeerHandlesAndDestroyEndpoint HcommMemUnreg peer "
                         << "handle failed, rank: " << peerRank << " addr: " << std::hex << item.first
                         << " ret: " << ret);
            if (localResult == BM_OK) {
                localResult = ret;
            }
        }
        item.second.peerHandles.erase(handleIt);
    }
    (void)HcomUrmaDestroyEndpoint(state.localEndpoint->hcommEndpoint);
    state.localEndpoint.reset();
    return localResult;
}

Result DeviceUrmaTransportManager::CleanupPeerRankState(RemoteRankState &state, uint32_t peerRank)
{
    Result localResult = BM_OK;

    const auto retChan = DestroyRankChannelsAndThread(state);
    if (retChan != BM_OK && localResult == BM_OK) {
        localResult = retChan;
    }
    const auto retImports = UnimportPeerImportsAndFlag(state, peerRank);
    if (retImports != BM_OK && localResult == BM_OK) {
        localResult = retImports;
    }
    const auto retHelper = UnregisterPeerHandlesAndDestroyEndpoint(state, peerRank);
    if (retHelper != BM_OK && localResult == BM_OK) {
        localResult = retHelper;
    }
    return localResult;
}

Result DeviceUrmaTransportManager::CleanupLocalRegistrationsLocked()
{
    Result localResult = BM_OK;
    for (auto &item : localRegistrations_) {
        const auto ret = manager_.HcommMemUnreg(localEndpoint_, item.second.handle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma CleanupLocalRegistrationsLocked HcommMemUnreg global handle failed, "
                         << "addr: " << VaToStr(item.first) << " ret: " << ret);
            if (localResult == BM_OK)
                localResult = ret;
        }
    }
    localRegistrations_.clear();
    return localResult;
}

Result DeviceUrmaTransportManager::FindLocalRegistrationLocked(uint64_t addr, uint64_t size,
                                                               LocalRegistration *registration) const
{
    if (addr == 0) {
        BM_LOG_ERROR("device_urma FindLocalRegistrationLocked: addr is 0");
        return BM_INVALID_PARAM;
    }
    if (size == 0) {
        return BM_OK;
    }
    // Validate that (addr, size) forms a valid non-wrapping address range.
    if (!ContainsAddressRange(addr, size, addr, size)) {
        BM_LOG_ERROR("device_urma FindLocalRegistrationLocked: address range check failed, addr=" << VaToStr(addr)
                                                                                                  << " size=" << size);
        return BM_INVALID_PARAM;
    }
    for (const auto &item : localRegistrations_) {
        if (ContainsAddressRange(item.second.mr.addr, item.second.mr.size, addr, size)) {
            if (registration != nullptr) {
                *registration = item.second;
            }
            return BM_OK;
        }
    }
    return BM_INVALID_PARAM;
}

Result DeviceUrmaTransportManager::FindRemoteRegistrationLocked(uint32_t rankId, uint64_t addr, uint64_t size,
                                                                RemoteRegistration *registration) const
{
    if (rankId >= rankCount_) {
        return BM_INVALID_PARAM;
    }
    if (size == 0) {
        return BM_OK;
    }
    const auto rankIt = remoteRanks_.find(rankId);
    if (rankIt == remoteRanks_.end()) {
        return BM_NOT_CONNECTED;
    }
    // Validate that (addr, size) forms a valid non-wrapping address range.
    if (!ContainsAddressRange(addr, size, addr, size)) {
        BM_LOG_ERROR("device_urma FindRemoteRegistrationLocked: address range check failed, addr=0x"
                     << std::hex << addr << " size=0x" << size << std::dec);
        return BM_INVALID_PARAM;
    }
    for (const auto &remote : rankIt->second.imports) {
        if (ContainsAddressRange(remote.addr, remote.size, addr, size)) {
            if (registration != nullptr) {
                *registration = remote;
            }
            return BM_OK;
        }
    }
    return BM_INVALID_PARAM;
}

Result DeviceUrmaTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::lock_guard<std::mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    BM_VALIDATE_RETURN(mr.addr != 0 && mr.size != 0, "device_urma RegisterMemoryRegion: mr.addr or mr.size is 0",
                       BM_INVALID_PARAM);
    if (!IsSupportedMemoryFlags(mr.flags)) {
        BM_LOG_ERROR("device_urma RegisterMemoryRegion: unsupported memory flags: " << mr.flags);
        return BM_INVALID_PARAM;
    }
    UrmaCommMem mem = ToUrmaMem(mr);
    if (!IsValidMem(mem)) {
        BM_LOG_ERROR("device_urma RegisterMemoryRegion: invalid memory");
        return BM_INVALID_PARAM;
    }

    // localEndpoint_ must be created by OpenDevice before any memory registration
    if (localEndpoint_ == nullptr) {
        BM_LOG_ERROR("device_urma localEndpoint_ is null, cannot register memory");
        return BM_NOT_INITIALIZED;
    }

    auto item = localRegistrations_.find(mr.addr);
    if (item != localRegistrations_.end()) {
        if (item->second.mr.size != mr.size) {
            BM_LOG_ERROR("device_urma register memory conflict, addr: " << std::hex << mr.addr);
            return BM_ERROR;
        }
        item->second.refCount++;
        return BM_OK;
    }

    LocalRegistration registration{};
    registration.mr = mr;
    registration.memTag = mr.addr;
    registration.refCount = 1;

    if ((mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0) {
        const uint64_t dva = HybmVaManager::GetInstance().TransformVa(mr.addr, HVM_HVA, HVM_DVA);
        if (dva != 0) {
            registration.deviceVa = dva;
            mem.addr = dva;
        } else {
            BM_LOG_WARN("device_urma RegisterMemoryRegion: DRAM addr " << VaToStr(mr.addr)
                                                                       << " has no DVA mapping, using HVA");
        }
    }

    HcommMemHandle hcommHandle = nullptr;
    auto ret = manager_.HcommMemReg(localEndpoint_, registration.memTag, mem, &hcommHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma HcommMemReg failed, addr: " << VaToStr(mr.addr) << " size: " << mr.size
                                                              << " ret: " << ret);
        return ret;
    }
    registration.handle = hcommHandle;

    try {
        localRegistrations_.emplace(mr.addr, registration);
    } catch (...) {
        (void)manager_.HcommMemUnreg(localEndpoint_, hcommHandle);
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    std::lock_guard<std::mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    auto item = localRegistrations_.find(addr);
    if (item == localRegistrations_.end()) {
        BM_LOG_WARN("device_urma unregister skipped for unknown addr: " << std::hex << addr);
        return BM_OK;
    }
    if (item->second.refCount > 1) {
        item->second.refCount--;
        return BM_OK;
    }

    Result finalRet = BM_OK;
    auto retUnreg = manager_.HcommMemUnreg(localEndpoint_, item->second.handle);
    if (retUnreg != BM_OK) {
        BM_LOG_ERROR("device_urma HcommMemUnreg failed for global handle, addr: " << std::hex << addr
                                                                                  << " ret: " << retUnreg);
        if (finalRet == BM_OK) {
            finalRet = retUnreg;
        }
    }
    // Unregister peer handles for this local registration
    for (auto &peerEntry : item->second.peerHandles) {
        auto retPeer = manager_.HcommMemUnreg(localEndpoint_, peerEntry.second);
        if (retPeer != BM_OK && finalRet == BM_OK) {
            BM_LOG_ERROR("device_urma HcommMemUnreg failed for peer handle, addr: "
                         << std::hex << addr << " peerRank: " << peerEntry.first << " ret: " << retPeer);
            finalRet = retPeer;
        }
    }
    item->second.peerHandles.clear();
    localRegistrations_.erase(item);
    return finalRet;
}

bool DeviceUrmaTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    std::lock_guard<std::mutex> guard(mutex_);
    return FindLocalRegistrationLocked(addr, size, nullptr) == BM_OK;
}

Result DeviceUrmaTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    std::lock_guard<std::mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    LocalRegistration registration{};
    auto ret = FindLocalRegistrationLocked(addr, 1, &registration);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma QueryMemoryKey addr is not registered: " << std::hex << addr);
        return ret;
    }

    if (registration.handle == INVALID_MEM_HANDLE) {
        BM_LOG_ERROR("device_urma QueryMemoryKey addr has no HCOMM handle: " << std::hex << addr);
        return BM_ERROR;
    }

    // Guard: devTransFlagHcommHandle_ must be valid for flag export
    if (devTransFlagHcommHandle_ == nullptr) {
        BM_LOG_ERROR("device_urma QueryMemoryKey devTransFlagHcommHandle_ is null, cannot export flag");
        return BM_ERROR;
    }

    // Export primary memory descriptor via HcommTransportManager (caches UrmaExportDesc + hcommDesc)
    const uint8_t *memDesc = nullptr;
    uint32_t memDescLen = 0;
    ret = manager_.HcommMemExport(localEndpoint_, registration.handle, &memDesc, &memDescLen);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma QueryMemoryKey HcommMemExport failed for addr: " << std::hex << addr
                                                                                   << " ret: " << ret);
        return ret;
    }

    // Parse exported desc to recover hcommDesc pointer and len
    UrmaExportDesc exportDesc{};
    const uint8_t *hcommDesc = nullptr;
    uint32_t hcommDescLen = 0;
    if (!DeserializeExportDesc(memDesc, memDescLen, exportDesc, &hcommDesc, &hcommDescLen)) {
        BM_LOG_ERROR("device_urma QueryMemoryKey DeserializeExportDesc failed");
        return BM_ERROR;
    }

    // Export flag descriptor via raw HCOMM API
    void *flagDescRaw = nullptr;
    uint32_t flagDescLenRaw = 0;
    ret = DlHcommApi::HcommMemExport(localEndpoint_->hcommEndpoint, devTransFlagHcommHandle_, &flagDescRaw,
                                     &flagDescLenRaw);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma QueryMemoryKey flag HcommMemExport failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    if (flagDescRaw == nullptr || flagDescLenRaw == 0) {
        BM_LOG_ERROR("device_urma QueryMemoryKey flag export returned null or zero length");
        return BM_ERROR;
    }
    const uint8_t *flagDesc = static_cast<const uint8_t *>(flagDescRaw);
    const uint32_t flagDescLen = flagDescLenRaw;

    // Serialize into key payload
    key.keys[0] = URMA_EXPORT_DESC_MAGIC;
    uint64_t gva = HybmVaManager::GetInstance().TransformVa(registration.mr.addr, HVM_HVA, HVM_GVA);
    key.keys[1] = (gva != 0) ? gva : registration.mr.addr; // 要导出gva

    UrmaExportDesc keyExportDesc = exportDesc;
    keyExportDesc.devTransFlagDescLen = flagDescLen;

    uint8_t *payload = reinterpret_cast<uint8_t *>(&key.keys[DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS]);
    constexpr uint32_t exportHeaderSize = sizeof(UrmaExportDesc);
    std::memcpy(payload, &keyExportDesc, exportHeaderSize);
    std::memcpy(payload + exportHeaderSize, hcommDesc, hcommDescLen);
    std::memcpy(payload + exportHeaderSize + hcommDescLen, flagDesc, flagDescLen);

    return BM_OK;
}

void DeviceUrmaTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    if (addr != nullptr) {
        key.keys[1] = reinterpret_cast<uint64_t>(addr);
    }
}

Result DeviceUrmaTransportManager::ImportRemoteMemKeysLocked(uint32_t peerRank, RemoteRankState &state,
                                                             const std::vector<TransportMemoryKey> &memKeys)
{
    if (memKeys.empty()) {
        return BM_OK;
    }
    if (localEndpoint_ == nullptr) {
        BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked localEndpoint_ is null, peer: " << peerRank);
        return BM_NOT_INITIALIZED;
    }

    // Collect newly imported registrations in a local vector;
    // commit to state.imports only after all keys succeed.
    std::vector<RemoteRegistration> newImports;
    bool flagImportedInThisCall = false;
    auto rollbackNewImports = [&]() {
        if (flagImportedInThisCall && !state.remoteFlagDescBytes.empty()) {
            (void)DlHcommApi::HcommMemUnimport(localEndpoint_->hcommEndpoint, state.remoteFlagDescBytes.data(),
                                               static_cast<uint32_t>(state.remoteFlagDescBytes.size()));
            state.remoteFlagDescBytes.clear();
            state.remoteFlagAddr = 0;
            state.remoteFlagSize = 0;
            flagImportedInThisCall = false;
        }
        for (const auto &ni : newImports) {
            if (!ni.descBytes.empty()) {
                (void)manager_.HcommMemUnimport(localEndpoint_, ni.descBytes.data(),
                                                static_cast<uint32_t>(ni.descBytes.size()));
            }
        }
        newImports.clear();
    };

    for (const auto &key : memKeys) {
        // --- 1. Validate top-level magic ---
        if (key.keys[0] != URMA_EXPORT_DESC_MAGIC) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked invalid key magic 0x"
                         << std::hex << key.keys[0] << ", peer: " << peerRank << ", expected: 0x"
                         << URMA_EXPORT_DESC_MAGIC);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        const uint64_t remoteAddr = key.keys[1];
        if (remoteAddr == 0) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked zero addr(" << remoteAddr << ") in key, peer: "
                                                                            << peerRank);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        // --- 2. Parse UrmaExportDesc header from payload after header slots ---
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(&key.keys[DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS]);
        UrmaExportDesc exportDesc{};
        std::memcpy(&exportDesc, raw, sizeof(UrmaExportDesc));

        if (exportDesc.magic != URMA_EXPORT_DESC_MAGIC || exportDesc.version != URMA_EXPORT_DESC_VERSION) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked invalid UrmaExportDesc magic/version in key, peer: "
                         << peerRank);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }
        if (exportDesc.headerSize != sizeof(UrmaExportDesc)) {
            BM_LOG_ERROR(
                "device_urma ImportRemoteMemKeysLocked UrmaExportDesc headerSize mismatch, peer: " << peerRank);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }
        if (exportDesc.hcommDescLen == 0) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked hcommDescLen is 0, peer: " << peerRank);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }
        if (exportDesc.size == 0) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked exportDesc.size is 0, peer: " << peerRank);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        const uint64_t remoteSize = exportDesc.size;
        const uint64_t memTag = exportDesc.memTag;
        const uint32_t memDescLen = sizeof(UrmaExportDesc) + exportDesc.hcommDescLen;

        // Validate total payload fits within the key data area
        if (memDescLen + exportDesc.devTransFlagDescLen > DEVICE_URMA_EXPORT_KEY_DATA_BYTES) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked total payload exceeds key capacity, "
                         << "peer: " << peerRank << " memDescLen: " << memDescLen
                         << " flagDescLen: " << exportDesc.devTransFlagDescLen);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        // --- 3. Idempotency: skip if already imported (by memTag) ---
        auto it = std::find_if(state.imports.begin(), state.imports.end(),
                               [memTag](const auto &r) { return r.memTag == memTag; });
        if (it != state.imports.end()) {
            BM_LOG_DEBUG("device_urma ImportRemoteMemKeysLocked skip duplicate memTag: " << memTag
                                                                                         << ", peer: " << peerRank);
            continue;
        }

        // --- 4. Protocol compatibility check before import ---
        if (state.remoteEndpointDesc.protocol != localEndpoint_->desc.protocol) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked protocol mismatch, peer: "
                         << peerRank << " remote protocol: " << state.remoteEndpointDesc.protocol
                         << " local protocol: " << localEndpoint_->desc.protocol);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        // --- 5. HcommMemImport using global localEndpoint_ ---
        UrmaCommMem view{};
        auto ret = manager_.HcommMemImport(localEndpoint_, raw, memDescLen, &view);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked HcommMemImport failed, memTag: "
                         << memTag << " peer: " << peerRank << " ret: " << ret);
            rollbackNewImports();
            return ret;
        }

        // --- 6. Build RemoteRegistration ---
        RemoteRegistration remote{};
        remote.addr = remoteAddr;
        remote.size = remoteSize;
        remote.memTag = memTag;
        remote.descBytes.assign(raw, raw + memDescLen);
        remote.view = view;
        newImports.emplace_back(std::move(remote));

        // --- 7. Import flag desc from UrmaExportDesc (if present and not yet imported for this peer) ---
        if (exportDesc.devTransFlagDescLen > 0 && !flagImportedInThisCall && state.remoteFlagAddr == 0) {
            const uint8_t *flagRaw = raw + sizeof(UrmaExportDesc) + exportDesc.hcommDescLen;
            HcommCommMem flagOutMem{};
            const auto flagRet = DlHcommApi::HcommMemImport(localEndpoint_->hcommEndpoint, flagRaw,
                                                            exportDesc.devTransFlagDescLen, &flagOutMem);
            if (flagRet != 0) {
                BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked HcommMemImport for flag failed, peer: "
                             << peerRank << " ret: " << flagRet);
                rollbackNewImports();
                return BM_DL_FUNCTION_FAILED;
            }
            if (flagOutMem.type == COMM_MEM_TYPE_INVALID) {
                BM_LOG_ERROR(
                    "device_urma ImportRemoteMemKeysLocked flag import returned invalid type, peer: " << peerRank);
                rollbackNewImports();
                return BM_INVALID_PARAM;
            }
            state.remoteFlagAddr = reinterpret_cast<uint64_t>(flagOutMem.addr);
            state.remoteFlagSize = flagOutMem.size;
            state.remoteFlagDescBytes.assign(flagRaw, flagRaw + exportDesc.devTransFlagDescLen);
            flagImportedInThisCall = true;
            BM_LOG_INFO("device_urma ImportRemoteMemKeysLocked imported flag, peer: "
                        << peerRank << " flagAddr: " << VaToStr(state.remoteFlagAddr)
                        << " flagSize: " << state.remoteFlagSize << " descLen: " << exportDesc.devTransFlagDescLen);
        }

        BM_LOG_INFO("device_urma ImportRemoteMemKeysLocked imported mem, peer: "
                    << peerRank << " memTag: " << memTag << " addr: " << VaToStr(remoteAddr) << " size: " << remoteSize
                    << " view: " << VaToStr(view.addr));
    }

    state.imports.insert(state.imports.end(), newImports.begin(), newImports.end());
    return BM_OK;
}

Result DeviceUrmaTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    std::lock_guard<std::mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    BM_VALIDATE_RETURN(localEndpoint_ != nullptr,
                       "evice_urma Prepare failed: localEndpoint_ is null (OpenDevice may not have completed)",
                       BM_NOT_INITIALIZED);
    for (const auto &item : options.options) {
        const uint32_t peerRank = item.first;
        if (peerRank >= rankCount_) {
            BM_LOG_ERROR("device_urma Prepare invalid peerRank: " << peerRank << " rankCount: " << rankCount_);
            return BM_INVALID_PARAM;
        }
        if (peerRank == rankId_) {
            BM_LOG_WARN("device_urma Prepare skipping self rank: " << peerRank);
            continue;
        }
        auto &state = remoteRanks_[peerRank];

        // 1. Parse peer UrmaEndpointDesc from privateData
        UrmaEndpointDesc peerDesc{};
        auto ret = ParsePrivateDataToEndpointDesc(item.second.privateData, peerDesc);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma Prepare failed to parse peer endpoint desc, peer: " << peerRank);
            return ret;
        }

        bool resourcesCreatedInThisCall = false;

        // 2. Idempotency: if thread and channel already exist, compare with stored remoteEndpointDesc.
        //    If unchanged → skip resource creation; if changed → reject (no hot-replace).
        //    Either way, fall through to mem import — do NOT continue.
        if (state.channel != 0 && state.thread != 0) {
            if (std::memcmp(&state.remoteEndpointDesc, &peerDesc, sizeof(UrmaEndpointDesc)) != 0) {
                BM_LOG_ERROR(
                    "device_urma Prepare peer endpoint desc changed but hot-replace not supported, peer: " << peerRank);
                return BM_INVALID_PARAM;
            }
            BM_LOG_INFO("device_urma Prepare reusing existing channel/thread for peer: " << peerRank);
            // resourcesCreatedInThisCall remains false — do not destroy channel/thread on import failure
        } else {
            // 3. Convert peer UrmaEndpointDesc → HCOMM EndpointDesc for HcommChannelDesc.remoteEndpoint
            EndpointDesc hcommRemoteEndpoint = ToHcommEndpointDesc(peerDesc);

            // 4. Build a single HcommChannelDesc for this peer
            HcommChannelDesc channelDesc{};
            HcommChannelDescInit(&channelDesc, 1); // not in DlHcommApi namespace, direct C function
            channelDesc.role = (rankId_ > peerRank) ? HCOMM_SOCKET_ROLE_CLIENT : HCOMM_SOCKET_ROLE_SERVER;
            channelDesc.remoteEndpoint = hcommRemoteEndpoint;
            channelDesc.notifyNum = HCOMM_NORMAL_NOTIFY_NUM;
            channelDesc.exchangeAllMems = true; // 填true, 不用管memHandles了, remoteEndpoint要填对
            if (localEndpoint_->desc.protocol == UrmaProtocol::UBOE) {
                // CRITICAL: HcommChannelDescInit sets union to 0xFF garbage values.
                // Must zero the entire union before setting ubAttr to avoid:
                // - queueNum=0xFFFFFFFF (4B QPs → OOM)
                // - retryCnt=0xFFFFFFFF (impossible retries → timeout)
                // - tc/sl=0xFF (invalid QoS → init failure)
                std::memset(channelDesc.raws, 0, sizeof(channelDesc.raws));
                // sqDepth合法范围[16,256]且需为2的幂，0会被CheckUbAttr拒绝；128对齐hcomm MS模式默认值
                channelDesc.ubAttr.sqDepth = 128;
            }

            // 5. Allocate one thread per peer (use temporary variable for safe rollback)
            HcommThreadHandle threadHandle = 0;
            ret = DlHcommApi::HcommThreadAlloc(COMM_ENGINE_AICPU_TS, 1, &HCOMM_NORMAL_NOTIFY_NUM, &threadHandle);
            if (ret != BM_OK) {
                BM_LOG_ERROR("device_urma Prepare HcommThreadAlloc failed, peer: "
                             << peerRank << " engine: " << COMM_ENGINE_AICPU_TS << " ret: " << ret);
                return ret;
            }
            if (threadHandle == 0) {
                BM_LOG_ERROR("device_urma Prepare HcommThreadAlloc returned invalid thread, peer: " << peerRank);
                return BM_DL_FUNCTION_FAILED;
            }

            // 6. Create one channel per peer (temporary variable for safe rollback)
            HcommChannelHandle channelHandle = 0;
            ret = DlHcommApi::HcommChannelCreate(localEndpoint_->hcommEndpoint, COMM_ENGINE_AICPU, &channelDesc, 1,
                                                 &channelHandle);
            if (ret != 0) {
                BM_LOG_ERROR("device_urma Prepare HcommChannelCreate failed, peer: " << peerRank << " ret: " << ret);
                auto rollbackThread = threadHandle;
                // No stream sync needed: thread was just allocated, no RemoteIO launched during Prepare.
                (void)DlHcommApi::HcommThreadFree(&rollbackThread, 1);
                return BM_DL_FUNCTION_FAILED;
            }
            if (channelHandle == 0) {
                BM_LOG_ERROR("device_urma Prepare HcommChannelCreate returned invalid channel, peer: " << peerRank);
                auto rollbackThread = threadHandle;
                // No stream sync needed: thread was just allocated, no RemoteIO launched during Prepare.
                (void)DlHcommApi::HcommThreadFree(&rollbackThread, 1);
                return BM_DL_FUNCTION_FAILED;
            }

            // 7. Persist new resources to state
            state.remoteEndpointDesc = peerDesc;
            state.hasEndpointDesc = true;
            state.thread = threadHandle;
            state.channelDesc = channelDesc;
            state.channel = channelHandle;
            resourcesCreatedInThisCall = true;

            BM_LOG_INFO("device_urma Prepare created channel/thread, peer: " << peerRank << " thread: " << threadHandle
                                                                             << " channel: " << channelHandle);
        }

        // 8. Import remote memory keys (always, even if channel/thread were reused)
        ret = ImportRemoteMemKeysLocked(peerRank, state, item.second.memKeys);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma Prepare ImportRemoteMemKeysLocked failed, peer: " << peerRank);
            if (resourcesCreatedInThisCall) {
                // Rollback the channel/thread that were just created in this call
                BM_LOG_WARN("device_urma Prepare rolling back newly created channel/thread for peer: " << peerRank);
                if (state.channel != 0) {
                    auto chanRollback = state.channel;
                    // No stream sync needed: channel was just created, no RemoteIO launched during Prepare.
                    (void)DlHcommApi::HcommChannelDestroy(&chanRollback, 1);
                    state.channel = 0;
                }
                if (state.thread != 0) {
                    auto threadRollback = state.thread;
                    (void)DlHcommApi::HcommThreadFree(&threadRollback, 1);
                    state.thread = 0;
                }
                state.channelDesc = {};
                state.remoteEndpointDesc = UrmaEndpointDesc{};
                state.hasEndpointDesc = false;
            }
            // If resourcesCreatedInThisCall is false, ImportRemoteMemKeysLocked already
            // rolled back its own partial imports; we leave pre-existing channel/thread intact.
            return ret;
        }

        BM_LOG_INFO("device_urma Prepare success, peer: " << peerRank << " thread: " << state.thread << " channel: "
                                                          << state.channel << " imports: " << state.imports.size());
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::RemoveRankLocked(uint32_t rankId)
{
    auto rankIt = remoteRanks_.find(rankId);
    if (rankIt == remoteRanks_.end()) {
        return BM_OK;
    }
    auto &state = rankIt->second;
    Result finalRet = DestroyRankChannelsAndThread(state);

    const auto retImports = UnimportPeerImportsAndFlag(state, rankId);
    if (retImports != BM_OK && finalRet == BM_OK) {
        finalRet = retImports;
    }
    const auto retHelper = UnregisterPeerHandlesAndDestroyEndpoint(state, rankId);
    if (retHelper != BM_OK && finalRet == BM_OK) {
        finalRet = retHelper;
    }
    remoteRanks_.erase(rankIt);
    return finalRet;
}

Result DeviceUrmaTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    std::lock_guard<std::mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    Result finalRet = BM_OK;
    for (auto rankId : removedRanks) {
        auto ret = RemoveRankLocked(rankId);
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
    }
    return finalRet;
}

Result DeviceUrmaTransportManager::Connect()
{
    connected_ = true;
    return BM_OK;
}

Result DeviceUrmaTransportManager::AsyncConnect()
{
    return BM_OK;
}

Result DeviceUrmaTransportManager::WaitForConnected(int64_t timeoutNs)
{
    (void)timeoutNs;
    return BM_OK;
}

Result DeviceUrmaTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    bool needFallback = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
        if (localEndpoint_ == nullptr) {
            BM_LOG_ERROR("device_urma UpdateRankOptions failed: localEndpoint_ is null");
            return BM_NOT_INITIALIZED;
        }
        for (const auto &item : options.options) {
            const uint32_t peerRank = item.first;
            if (peerRank >= rankCount_) {
                BM_LOG_ERROR("device_urma UpdateRankOptions invalid peerRank: " << peerRank
                                                                                << " rankCount: " << rankCount_);
                return BM_INVALID_PARAM;
            }
            if (peerRank == rankId_) {
                BM_LOG_WARN("device_urma UpdateRankOptions skipping self rank: " << peerRank);
                continue;
            }
            auto stateIt = remoteRanks_.find(peerRank);
            if (stateIt == remoteRanks_.end()) {
                BM_LOG_WARN("device_urma UpdateRankOptions peer rank " << peerRank
                                                                       << " not prepared yet, fallback to Prepare");
                needFallback = true;
                break;
            }
            auto &state = stateIt->second;
            if (state.channel == 0 || state.thread == 0) {
                BM_LOG_WARN("device_urma UpdateRankOptions peer rank "
                            << peerRank << " has no channel/thread, fallback to Prepare");
                needFallback = true;
                break;
            }
        }
    } // mutex_ released

    if (needFallback) {
        // Fallback to Prepare (without holding mutex_) — it will create resources and import memKeys.
        BM_LOG_INFO("device_urma UpdateRankOptions falling back to Prepare for new dynamic ranks");
        return Prepare(options);
    }

    // Only import remote memory keys without re-creating resources.
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto &item : options.options) {
        const uint32_t peerRank = item.first;
        if (peerRank >= rankCount_ || peerRank == rankId_) {
            continue; // already validated/skipped above
        }
        auto stateIt = remoteRanks_.find(peerRank);
        if (stateIt == remoteRanks_.end()) {
            continue;
        }
        auto &state = stateIt->second;
        auto ret = ImportRemoteMemKeysLocked(peerRank, state, item.second.memKeys);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma UpdateRankOptions ImportRemoteMemKeysLocked failed, peer: " << peerRank);
            return ret;
        }
        BM_LOG_INFO("device_urma UpdateRankOptions success, peer: " << peerRank);
    }
    return BM_OK;
}

const std::string &DeviceUrmaTransportManager::GetNic() const
{
    static const std::string emptyNic;
    return emptyNic;
}

const TransportPrivateData DeviceUrmaTransportManager::GetPrivateData() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    TransportPrivateData data{};
    if (localEndpoint_ == nullptr) {
        BM_LOG_WARN("device_urma GetPrivateData called before localEndpoint_ is ready, returning empty");
        return data; // empty data, peer's ParsePrivateDataToEndpointDesc will reject with magic mismatch
    }
    UrmaPrivateDataDesc header{};
    header.payloadLen = static_cast<uint16_t>(sizeof(UrmaEndpointDesc));
    uint8_t *raw = reinterpret_cast<uint8_t *>(data.key.keys);
    std::memcpy(raw, &header, sizeof(UrmaPrivateDataDesc));
    std::memcpy(raw + sizeof(UrmaPrivateDataDesc), &localEndpointDesc_, sizeof(UrmaEndpointDesc));
    return data;
}

Result DeviceUrmaTransportManager::RemoteIo(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write)
{
    if (size == 0) {
        return BM_OK;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    RemoteRegistration remote{};
    auto ret = FindRemoteRegistrationLocked(rankId, rAddr, size, &remote);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma remote address is not prepared, rank: " << rankId << " addr: " << std::hex << rAddr);
        return ret;
    }
    auto &state = remoteRanks_[rankId];
    if (state.channel == 0) {
        return BM_NOT_CONNECTED;
    }
    const auto translatedRemoteAddr = remote.view.addr + (rAddr - remote.addr);
    BM_LOG_DEBUG("device_urma lAddr:" << VaToStr(lAddr) << ", rAddr=" << VaToStr(rAddr) << ", translated rAddr="
                                      << VaToStr(translatedRemoteAddr) << ", size=" << size << ", write: " << write);
    const auto channel = state.channel;
    // Route single-element transfer through LaunchDeviceKernelBatch to unify notify/flag logic.
    ret = LaunchDeviceKernelBatch(state.thread, !write, channel, std::vector<uint64_t>{lAddr},
                                  std::vector<uint64_t>{translatedRemoteAddr}, std::vector<uint64_t>{size},
                                  state.remoteFlagAddr);
    if (ret != BM_OK) {
        return ret;
    }
    state.pendingOps++;
    return BM_OK;
}

Result DeviceUrmaTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    auto ret = ReadRemoteAsync(rankId, lAddr, rAddr, size);
    if (ret != BM_OK) {
        return ret;
    }
    return Synchronize(rankId);
}

Result DeviceUrmaTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    auto ret = WriteRemoteAsync(rankId, lAddr, rAddr, size);
    if (ret != BM_OK) {
        return ret;
    }
    return Synchronize(rankId);
}

Result DeviceUrmaTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIo(rankId, lAddr, rAddr, size, false);
}

Result DeviceUrmaTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIo(rankId, lAddr, rAddr, size, true);
}

Result DeviceUrmaTransportManager::RemoteIoBatch(uint32_t rankId, const CopyDescriptor &descriptor, bool write)
{
    const auto &localAddrs = descriptor.localAddrs;
    const auto &globalAddrs = descriptor.globalAddrs;
    const auto &counts = descriptor.counts;
    const auto batchSize = counts.size();

    if (batchSize == 0) {
        return BM_OK;
    }
    if (localAddrs.size() != batchSize || globalAddrs.size() != batchSize) {
        BM_LOG_ERROR("device_urma RemoteIoBatch: descriptor vector sizes mismatch, local="
                     << localAddrs.size() << " global=" << globalAddrs.size() << " counts=" << batchSize);
        return BM_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    auto rankIt = remoteRanks_.find(rankId);
    if (rankIt == remoteRanks_.end()) {
        BM_LOG_ERROR("device_urma RemoteIoBatch rank not found: " << rankId);
        return BM_NOT_CONNECTED;
    }
    auto &state = rankIt->second;
    if (state.channel == 0) {
        BM_LOG_ERROR("device_urma RemoteIoBatch no channel for rank: " << rankId);
        return BM_NOT_CONNECTED;
    }

    std::vector<uint64_t> localVec;
    std::vector<uint64_t> remoteVec;
    std::vector<uint64_t> sizeVec;
    Result ret = BM_OK;
    localVec.reserve(batchSize);
    remoteVec.reserve(batchSize);
    sizeVec.reserve(batchSize);

    for (uint32_t i = 0; i < batchSize; ++i) {
        const uint64_t lAddr = reinterpret_cast<uint64_t>(localAddrs[i]);
        const uint64_t rAddr = reinterpret_cast<uint64_t>(globalAddrs[i]);
        const uint64_t size = counts[i];
        if (size == 0) {
            continue;
        }

        RemoteRegistration remote{};
        ret = FindRemoteRegistrationLocked(rankId, rAddr, size, &remote);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma RemoteIoBatch remote address not prepared, rank: "
                         << rankId << " addr: 0x" << std::hex << rAddr << " index: " << i);
            return ret;
        }
        const auto translatedRemoteAddr = remote.view.addr + (rAddr - remote.addr);
        localVec.push_back(lAddr);
        remoteVec.push_back(translatedRemoteAddr);
        sizeVec.push_back(size);
    }

    if (localVec.empty()) {
        return BM_OK;
    }

    const auto channel = state.channel;
    ret = LaunchDeviceKernelBatch(state.thread, !write, channel, localVec, remoteVec, sizeVec, state.remoteFlagAddr);
    if (ret != BM_OK) {
        return ret;
    }
    state.pendingOps++;
    return BM_OK;
}

Result DeviceUrmaTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    return RemoteIoBatch(rankId, descriptor, true);
}

Result DeviceUrmaTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    return RemoteIoBatch(rankId, descriptor, false);
}

aclrtFuncHandle DeviceUrmaTransportManager::GetDeviceKernelFunc(bool isRead) const
{
    return isRead ? deviceFuncHandles_.batchRead : deviceFuncHandles_.batchWrite;
}

void DeviceUrmaTransportManager::ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers)
{
    // Only dstList is the allocation base; srcList and lenList are internal offsets.
    if (buffers.dstList != nullptr) {
        (void)DlAclApi::AclrtFree(buffers.dstList);
    }
    buffers.dstList = nullptr;
    buffers.srcList = nullptr;
    buffers.lenList = nullptr;
}

Result DeviceUrmaTransportManager::AddBatchPendingDeviceBuffers(DeviceTransferBuffers &buffers)
{
    std::lock_guard<std::mutex> guard(pendingBatchMutex_);
    try {
        pendingBatchBuffers_.push_back(buffers);
    } catch (...) {
        BM_LOG_ERROR("device_urma save batch pending device buffers failed");
        return BM_ERROR;
    }
    buffers = DeviceTransferBuffers{};
    return BM_OK;
}

void DeviceUrmaTransportManager::FreeBatchPendingDeviceBuffers()
{
    std::lock_guard<std::mutex> guard(pendingBatchMutex_);
    for (auto &buffers : pendingBatchBuffers_) {
        ReleaseDeviceTransferBuffers(buffers);
    }
    pendingBatchBuffers_.clear();
}

Result DeviceUrmaTransportManager::LaunchDeviceKernelBatch(HcommThreadHandle thread, bool isRead,
                                                           HcommChannelHandle channel,
                                                           const std::vector<uint64_t> &localAddrs,
                                                           const std::vector<uint64_t> &remoteAddrs,
                                                           const std::vector<uint64_t> &sizes, uint64_t remoteFlagAddr)
{
    const auto batchSize = localAddrs.size();
    if (batchSize == 0 || batchSize != remoteAddrs.size() || batchSize != sizes.size()) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch: invalid batchSize=" << batchSize);
        return BM_INVALID_PARAM;
    }
    if (channel == 0) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch: channel is 0, not connected");
        return BM_NOT_CONNECTED;
    }
    if (thread == 0) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch: thread is 0, not connected");
        return BM_NOT_CONNECTED;
    }

    std::lock_guard<std::mutex> guard(deviceKernelMutex_);

    DeviceTransferBuffers buffers{};
    const auto ptrBytes = batchSize * sizeof(void *);
    const auto lenBytes = batchSize * sizeof(uint64_t);
    const auto totalBytes = ptrBytes * 2UL + lenBytes;

    // Single allocation: layout = [dst pointers][src pointers][uint64 lengths].
    // dstList is the base; srcList and lenList are internal offsets.
    auto ret = DlAclApi::AclrtMalloc(&buffers.dstList, totalBytes, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch alloc batch buffers failed, ret: " << ret);
        return ret;
    }
    buffers.srcList = static_cast<uint8_t *>(buffers.dstList) + ptrBytes;
    buffers.lenList = static_cast<uint8_t *>(buffers.dstList) + ptrBytes * 2UL;

    // Build single contiguous host buffer, then one H2D copy
    std::vector<uint8_t> hostBuf(totalBytes);
    auto *dstBase = reinterpret_cast<void **>(hostBuf.data());
    auto *srcBase = reinterpret_cast<void **>(hostBuf.data() + ptrBytes);
    auto *lenBase = reinterpret_cast<uint64_t *>(hostBuf.data() + ptrBytes * 2UL);
    for (size_t i = 0; i < batchSize; ++i) {
        dstBase[i] = reinterpret_cast<void *>(isRead ? localAddrs[i] : remoteAddrs[i]);
        srcBase[i] = reinterpret_cast<void *>(isRead ? remoteAddrs[i] : localAddrs[i]);
        lenBase[i] = sizes[i];
    }

    ret = DlAclApi::AclrtMemcpy(buffers.dstList, totalBytes, hostBuf.data(), totalBytes, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch copy batch buffers failed, ret: " << ret);
        ReleaseDeviceTransferBuffers(buffers);
        return ret;
    }
    HybmOneSideOpParam args{};
    args.thread = thread;
    args.channel = channel;
    args.list_num = static_cast<uint32_t>(batchSize);
    args.dst_buf_addr_list = static_cast<void **>(buffers.dstList);
    args.src_buf_addr_list = static_cast<void **>(buffers.srcList);
    args.len_list = static_cast<uint64_t *>(buffers.lenList);
    args.remote_flag_addr = remoteFlagAddr;
    args.local_flag_addr = notifyAddr_;
    args.flag_size = static_cast<uint32_t>(notifyLen_);

    aclrtArgsHandle argsHandle = nullptr;
    auto funcHandle = GetDeviceKernelFunc(isRead);
    ret = DlAclApi::AclrtKernelArgsInit(funcHandle, &argsHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch AclrtKernelArgsInit failed, ret: " << ret);
        ReleaseDeviceTransferBuffers(buffers);
        return ret;
    }
    aclrtParamHandle paramHandle = nullptr;
    ret = DlAclApi::AclrtKernelArgsAppend(argsHandle, &args, sizeof(args), &paramHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch AclrtKernelArgsAppend failed, ret: " << ret);
        ReleaseDeviceTransferBuffers(buffers);
        return ret;
    }
    ret = DlAclApi::AclrtKernelArgsFinalize(argsHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch AclrtKernelArgsFinalize failed, ret: " << ret);
        ReleaseDeviceTransferBuffers(buffers);
        return ret;
    }
    void *stream = HybmStreamManager::GetThreadAclStream();
    if (stream == nullptr) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch GetThreadAclStream failed");
        ReleaseDeviceTransferBuffers(buffers);
        return BM_DL_FUNCTION_FAILED;
    }

    aclrtLaunchKernelAttr attr{};
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = HYBM_NOTIFY_DEFAULT_WAIT_TIME_S;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;

    ret = DlAclApi::AclrtLaunchKernelWithConfig(funcHandle, HYBM_DEVICE_KERNEL_BLOCK_DIM, stream, &cfg, argsHandle,
                                                nullptr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch AclrtLaunchKernelWithConfig failed, kernel: "
                     << (isRead ? HYBM_DEVICE_FUNC_READ : HYBM_DEVICE_FUNC_WRITE) << " ret: " << ret);
        ReleaseDeviceTransferBuffers(buffers);
        return ret;
    }

    // Batch pending buffers are tracked separately and freed (not recycled) after sync.
    ret = AddBatchPendingDeviceBuffers(buffers);
    if (ret != BM_OK) {
        ReleaseDeviceTransferBuffers(buffers);
        return ret;
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::Synchronize(uint32_t rankId)
{
    std::lock_guard<std::mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    auto rankIt = remoteRanks_.find(rankId);
    if (rankIt == remoteRanks_.end()) {
        return BM_NOT_CONNECTED;
    }
    if (rankIt->second.pendingOps == 0) {
        return BM_OK;
    }
    if (rankIt->second.channel == 0) {
        return BM_NOT_CONNECTED;
    }
    auto ret = SynchronizeDeviceKernelStream();
    if (ret != BM_OK) {
        return ret;
    }
    rankIt->second.pendingOps = 0;
    return BM_OK;
}

Result DeviceUrmaTransportManager::SynchronizeDeviceKernelStream()
{
    {
        std::lock_guard<std::mutex> guard(pendingBatchMutex_);
        if (pendingBatchBuffers_.empty()) {
            return BM_OK;
        }
    }

    void *stream = HybmStreamManager::GetThreadAclStream();
    if (stream == nullptr) {
        BM_LOG_ERROR("device_urma GetThreadAclStream failed while synchronizing device kernel");
        FreeBatchPendingDeviceBuffers();
        return BM_DL_FUNCTION_FAILED;
    }

    Result ret = BM_OK;
    ret = DlAclApi::AclrtWaitAndResetNotify(notify_, stream, HYBM_DEVICE_KERNEL_TIMEOUT_S);
    if (ret != BM_OK) {
        BM_LOG_WARN("device_urma AclrtWaitAndResetNotify failed, ret: " << ret
                                                                        << ", fallback to AclrtSynchronizeStream");
    }

    auto syncRet = DlAclApi::AclrtSynchronizeStream(stream);
    if (ret != 0 || syncRet != 0) {
        BM_LOG_ERROR("device_urma AclrtSynchronizeStream failed, notifyRet: " << ret << " syncRet: " << syncRet);
        FreeBatchPendingDeviceBuffers();
        return ret != 0 ? ret : syncRet;
    }

    FreeBatchPendingDeviceBuffers();
    return BM_OK;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
