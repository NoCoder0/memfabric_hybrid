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

#include "device_rdma_hcomm_transport_manager.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>

#include "dl_acl_api.h"
#include "dl_hccp_api.h"
#include "hybm_logger.h"
#include "hybm_stream_manager.h"
#include "hybm_va_manager.h"
#include "urma_topo_helper.h"
#include "hybm_batch_transfer.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {

constexpr int32_t CHANNEL_STATUS_POLL_INTERVAL_MS = 1;
constexpr int32_t CHANNEL_CONNECT_TIMEOUT_MS = 30000;
constexpr const char *HYBM_DEVICE_FUNC_READ = "HybmBatchRead";
constexpr const char *HYBM_DEVICE_FUNC_WRITE = "HybmBatchWrite";

bool IsValidMem(const TransportMemoryRegion &mr)
{
    return mr.addr != 0 && mr.size != 0;
}

// Read NPU IP from EID file (devPhyId:IP format, one per line).
// This matches the old MF_DEVICE_URMA_EID_FILE format used before develop0717.
static Result GetDeviceUrmaIpAddrFromFile(uint32_t phyDeviceId, uint32_t rankId, const char *eidPath, char *ipBuf,
                                          size_t ipBufLen)
{
    std::ifstream eidFile(eidPath);
    if (!eidFile.is_open()) {
        BM_LOG_ERROR("cannot open EID file: " << eidPath);
        return BM_ERROR;
    }
    std::string line;
    while (std::getline(eidFile, line)) {
        if (line.empty()) {
            continue;
        }
        auto colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }
        auto idStr = line.substr(0, colonPos);
        char *end = nullptr;
        auto devId = static_cast<uint32_t>(std::strtoul(idStr.c_str(), &end, 10));
        if (*end != '\0') {
            continue;
        }
        if (devId != phyDeviceId) {
            continue;
        }
        std::string ipStr = line.substr(colonPos + 1);
        auto trimLeft = ipStr.find_first_not_of(" \t");
        if (trimLeft != std::string::npos) {
            ipStr = ipStr.substr(trimLeft);
        }
        auto trimRight = ipStr.find_last_not_of(" \t\r\n");
        if (trimRight != std::string::npos) {
            ipStr = ipStr.substr(0, trimRight + 1);
        }
        if (ipStr.length() >= ipBufLen) {
            return BM_ERROR;
        }
        std::memcpy(ipBuf, ipStr.c_str(), ipStr.length() + 1);
        BM_LOG_DEBUG("NPU IP from EID file: phyId=" << phyDeviceId << " ip=" << ipBuf);
        return BM_OK;
    }
    BM_LOG_ERROR("devPhyId " << phyDeviceId << " not found in EID file: " << eidPath);
    return BM_INVALID_PARAM;
}

// Query NPU device IP via hccn_tool with 910B3-compatible syntax:
//   hccn_tool -i <devId> -ip -g
// This is different from the A5 syntax used in GetDeviceUrmaIpAddr.
static Result GetDeviceIpViaHccnTool(uint32_t phyDeviceId, struct in_addr &addr)
{
    // Find hccn_tool path
    std::string toolPath;
    if (access("/usr/local/Ascend/driver/tools/hccn_tool", X_OK) == 0) {
        toolPath = "/usr/local/Ascend/driver/tools/hccn_tool";
    } else {
        toolPath = "hccn_tool";
    }

    const std::string cmd = toolPath + " -i " + std::to_string(phyDeviceId) + " -ip -g";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return BM_ERROR;
    }

    // Parse output like: "ipaddr: 10.20.0.6 netmask: 255.255.255.0"
    static constexpr size_t LINE_BUF_SIZE = 256;
    char buf[LINE_BUF_SIZE];
    while (fgets(buf, static_cast<int>(sizeof(buf)), pipe) != nullptr) {
        const std::string line(buf);
        auto pos = line.find("ipaddr:");
        if (pos == std::string::npos) {
            continue;
        }
        std::string ipStr = line.substr(pos + 7); // skip "ipaddr:"
        auto trimLeft = ipStr.find_first_not_of(" \t");
        if (trimLeft != std::string::npos) {
            ipStr = ipStr.substr(trimLeft);
        }
        auto trimRight = ipStr.find_last_not_of(" \t\r\n");
        if (trimRight != std::string::npos) {
            ipStr = ipStr.substr(0, trimRight + 1);
        }
        if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
            pclose(pipe);
            BM_LOG_DEBUG("NPU IP via hccn_tool(910B3): dev=" << phyDeviceId << " ip=" << ipStr);
            return BM_OK;
        }
    }
    pclose(pipe);
    return BM_ERROR;
}

} // namespace

DeviceRdmaHcommTransportManager::~DeviceRdmaHcommTransportManager()
{
    (void)CloseDevice();
}

// ============================================================================
// AICPU kernel loading
// ============================================================================

Result DeviceRdmaHcommTransportManager::LoadDeviceKernel()
{
    std::lock_guard<std::mutex> guard(deviceKernelMutex_);
    if (deviceFuncHandles_.batchWrite != nullptr && deviceFuncHandles_.batchRead != nullptr) {
        return BM_OK;
    }
    return LoadDeviceKernelAndValidate(HYBM_DEVICE_FUNC_READ, HYBM_DEVICE_FUNC_WRITE, deviceKernelHandle_,
                                       deviceFuncHandles_);
}

// ============================================================================
// Endpoint helpers
// ============================================================================

Result DeviceRdmaHcommTransportManager::BuildLocalEndpointDesc(EndpointDesc &desc) const
{
    desc.protocol = COMM_PROTOCOL_ROCE;
    desc.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
    desc.loc.device.devPhyId = phyDeviceId_;
    desc.loc.device.superDevId = sdid_;
    desc.loc.device.serverIdx = serverId_;
    desc.loc.device.superPodIdx = superPodId_;

    // Get NPU-side IP address for HCOMM endpoint creation.
    {
        // 1) Try GetDeviceUrmaIpAddr (A5 hccn_tool syntax: -g -ip -i <dev> -d bond<dev>)
        CommAddrType addrType = COMM_ADDR_TYPE_RESERVED;
        std::array<uint8_t, URMA_ENDPOINT_RAW_LEN> addrBytes{};
        if (GetDeviceUrmaIpAddr(phyDeviceId_, rankId_, addrType, addrBytes) == BM_OK) {
            desc.commAddr.type = addrType;
            std::memcpy(desc.commAddr.raws, addrBytes.data(), sizeof(desc.commAddr.raws));
            return BM_OK;
        }

        // 2) Try hccn_tool with 910B3-compatible syntax: hccn_tool -i <dev> -ip -g
        struct in_addr ipv4 {};
        if (GetDeviceIpViaHccnTool(phyDeviceId_, ipv4) == BM_OK) {
            desc.commAddr.type = COMM_ADDR_TYPE_IP_V4;
            desc.commAddr.addr = ipv4;
            return BM_OK;
        }

        // 3) Last resort: MF_DEVICE_URMA_EID_FILE (devPhyId:IP per line)
        const char *eidPath = std::getenv("MF_DEVICE_URMA_EID_FILE");
        if (eidPath != nullptr && eidPath[0] != '\0') {
            char ipBuf[INET_ADDRSTRLEN]{};
            if (GetDeviceUrmaIpAddrFromFile(phyDeviceId_, rankId_, eidPath, ipBuf, sizeof(ipBuf)) == BM_OK) {
                if (inet_pton(AF_INET, ipBuf, &desc.commAddr.addr) == 1) {
                    desc.commAddr.type = COMM_ADDR_TYPE_IP_V4;
                    return BM_OK;
                }
            }
        }
    }

    BM_LOG_ERROR("cannot determine NPU-side IP for phyDeviceId=" << phyDeviceId_);
    return BM_INVALID_PARAM;
}

// ============================================================================
// OpenDevice / CloseDevice
// ============================================================================

Result DeviceRdmaHcommTransportManager::OpenDevice(const TransportOptions &options)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (opened_) {
        return BM_OK;
    }
    if (options.rankCount == 0 || options.rankId >= options.rankCount) {
        BM_LOG_ERROR("invalid rankCount or rankId");
        return BM_INVALID_PARAM;
    }

    // Ensure HCOMM library is loaded
    auto loadRet = HcommApiWrapper::LoadHcomm();
    if (loadRet != BM_OK) {
        BM_LOG_ERROR("DlHcommApi::LoadLibrary failed, ret: " << loadRet);
        return loadRet;
    }

    rankId_ = options.rankId;
    rankCount_ = options.rankCount;
    localNic_ = options.nic;

    // Get local device info
    auto ret = InitLocalDeviceInfo(phyDeviceId_, sdid_, serverId_, superPodId_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("InitLocalDeviceInfo failed, rank=" << rankId_);
        return ret;
    }

    // Build local EndpointDesc and create HCOMM endpoint (always RoCE)
    EndpointDesc desc{};
    ret = BuildLocalEndpointDesc(desc);
    if (ret != BM_OK) {
        return ret;
    }
    EndpointHandle handle = nullptr;
    ret = hcommApi_.CreateEndpoint(desc, handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommEndpointCreate failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    localEndpoint_ = handle;
    localEndpointDesc_ = desc;
    BM_LOG_INFO("HcommEndpointCreate success, handle=" << handle << " protocol=ROCE addrType=" << desc.commAddr.type);

    // Load AICPU kernel
    ret = LoadDeviceKernel();
    if (ret != BM_OK) {
        BM_LOG_ERROR("LoadDeviceKernel failed, ret: " << ret);
        (void)hcommApi_.DestroyEndpoint(localEndpoint_);
        localEndpoint_ = nullptr;
        return ret;
    }

    // Initialize transfer flag buffer (for kernel notify/confirm read-back)
    ret = InitTransferFlagLocked();
    if (ret != BM_OK) {
        BM_LOG_ERROR("InitTransferFlagLocked failed, ret: " << ret);
        (void)hcommApi_.DestroyEndpoint(localEndpoint_);
        localEndpoint_ = nullptr;
        return ret;
    }

    openGeneration_ = std::make_shared<OpenGeneration>();
    openGeneration_->id = 1;
    opened_ = true;
    BM_LOG_INFO("DeviceRdmaHcommTransportManager OpenDevice success, rank: "
                << rankId_ << " rankCount: " << rankCount_ << " protocol: ROCE" << " nic: " << localNic_);
    return BM_OK;
}

void DeviceRdmaHcommTransportManager::ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers)
{
    if (buffers.dstList != nullptr) {
        (void)DlAclApi::AclrtFree(buffers.dstList);
    }
    buffers.dstList = nullptr;
    buffers.srcList = nullptr;
    buffers.lenList = nullptr;
}

// ============================================================================
// Transfer flag buffer (kernel notify/confirm)
// ============================================================================

Result DeviceRdmaHcommTransportManager::InitTransferFlagLocked()
{
    if (localFlagPtr_ != nullptr) {
        return BM_OK;
    }
    auto ret = DlAclApi::AclrtMalloc(
        &localFlagPtr_, localFlagSize_,
        static_cast<aclrtMemMallocPolicy>(ACL_MEM_TYPE_HIGH_BAND_WIDTH | ACL_MEM_MALLOC_HUGE_ONLY));
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMalloc for local flag buffer failed, ret: " << ret);
        return ret;
    }
    uint64_t flagInit = 1;
    ret = DlAclApi::AclrtMemcpy(localFlagPtr_, localFlagSize_, &flagInit, localFlagSize_, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemcpy init local flag buffer failed, ret: " << ret);
        (void)DlAclApi::AclrtFree(localFlagPtr_);
        localFlagPtr_ = nullptr;
        return ret;
    }
    HcommCommMem flagMem{};
    flagMem.type = COMM_MEM_TYPE_DEVICE;
    flagMem.addr = localFlagPtr_;
    flagMem.size = localFlagSize_;
    ret = hcommApi_.RawMemReg(localEndpoint_, "__hybm_transfer_flag__", flagMem, localFlagHandle_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HcommMemReg for local flag buffer failed, ret: " << ret);
        (void)DlAclApi::AclrtFree(localFlagPtr_);
        localFlagPtr_ = nullptr;
        return ret;
    }

    // Cache the exported flag descriptor for embedding into memory keys
    void *exportDesc = nullptr;
    uint32_t exportLen = 0;
    ret = hcommApi_.RawMemExport(localEndpoint_, localFlagHandle_, exportDesc, exportLen);
    if (ret != BM_OK || exportDesc == nullptr || exportLen == 0) {
        BM_LOG_ERROR("HcommMemExport for local flag failed, ret: " << ret);
        (void)hcommApi_.RawMemUnreg(localEndpoint_, localFlagHandle_);
        localFlagHandle_ = nullptr;
        (void)DlAclApi::AclrtFree(localFlagPtr_);
        localFlagPtr_ = nullptr;
        return ret;
    }
    // Export descriptor is owned by Hcomm — copy locally
    localFlagExportDesc_.resize(exportLen);
    std::memcpy(localFlagExportDesc_.data(), exportDesc, exportLen);
    localFlagExportLen_ = exportLen;

    BM_LOG_INFO("local flag buffer allocated and registered, addr: " << localFlagPtr_ << " size: " << localFlagSize_
                                                                     << " handle: " << localFlagHandle_
                                                                     << " exportLen: " << exportLen);
    return BM_OK;
}

void DeviceRdmaHcommTransportManager::DestroyTransferFlagLocked()
{
    if (localFlagHandle_ != nullptr) {
        (void)hcommApi_.RawMemUnreg(localEndpoint_, localFlagHandle_);
        localFlagHandle_ = nullptr;
    }
    if (localFlagPtr_ != nullptr) {
        (void)DlAclApi::AclrtFree(localFlagPtr_);
        localFlagPtr_ = nullptr;
    }
    localFlagExportDesc_.clear();
    localFlagExportLen_ = 0;
}

// ============================================================================
// CloseDevice
// ============================================================================

Result DeviceRdmaHcommTransportManager::CloseDevice()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!opened_) {
        return BM_OK;
    }
    Result finalRet = BM_OK;

    // Destroy all peer resources
    for (auto &item : remoteRanks_) {
        // Unimport peer descriptors from the HCOMM endpoint before tearing down,
        // so the endpoint table does not carry stale entries across reopen.
        for (const auto &desc : item.second.importedDescs) {
            if (desc.empty()) {
                continue;
            }
            (void)hcommApi_.RawMemUnimport(localEndpoint_, desc.data(), static_cast<uint32_t>(desc.size()));
        }
        auto ret = DestroyPeerResources(item.first);
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
    }
    remoteRanks_.clear();

    // Unregister local memory
    for (auto &item : localRegistrations_) {
        if (item.second.handle != nullptr) {
            auto ret = hcommApi_.RawMemUnreg(localEndpoint_, item.second.handle);
            if (ret != 0 && finalRet == BM_OK) {
                finalRet = BM_DL_FUNCTION_FAILED;
            }
        }
    }
    localRegistrations_.clear();

    // Clean up all CompletionContexts (notify/stream/pending)
    for (auto &ctx : registry_) {
        if (ctx == nullptr) {
            continue;
        }
        if (ctx->notifyHcommHandle != nullptr) {
            (void)hcommApi_.RawMemUnreg(localEndpoint_, ctx->notifyHcommHandle);
            ctx->notifyHcommHandle = nullptr;
        }
        for (auto &pt : ctx->pendingTransfers) {
            ReleaseDeviceTransferBuffers(pt.buffers);
        }
        ctx->pendingTransfers.clear();
        if (ctx->notify != nullptr) {
            (void)DlAclApi::AclrtDestroyNotify(ctx->notify);
            ctx->notify = nullptr;
        }
        // ctx->stream is a thread-local shared FAST stream owned by HybmStreamManager;
        // it must NOT be destroyed here (device_urma does the same).
        ctx->stream = nullptr;
    }
    registry_.clear();
    openGeneration_.reset();

    // Destroy local flag buffer
    DestroyTransferFlagLocked();

    // Destroy local endpoint
    if (localEndpoint_ != nullptr) {
        auto ret = hcommApi_.DestroyEndpoint(localEndpoint_);
        if (ret != 0 && finalRet == BM_OK) {
            finalRet = BM_DL_FUNCTION_FAILED;
        }
        localEndpoint_ = nullptr;
    }

    deviceKernelHandle_ = nullptr;
    deviceFuncHandles_ = DeviceFuncHandles{};
    opened_ = false;
    connected_ = false;
    BM_LOG_INFO("DeviceRdmaHcommTransportManager CloseDevice");
    return finalRet;
}

// ============================================================================
// Memory registration
// ============================================================================

Result DeviceRdmaHcommTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!opened_) {
        BM_LOG_ERROR("transport not opened");
        return BM_ERROR;
    }
    if (!IsValidMem(mr)) {
        BM_LOG_ERROR("invalid memory region");
        return BM_INVALID_PARAM;
    }

    auto it = localRegistrations_.find(mr.addr);
    if (it != localRegistrations_.end()) {
        it->second.refCount++;
        return BM_OK;
    }

    // Follow device_urma pattern: convert HVA to DVA for DRAM memory
    uint64_t regAddr = mr.addr;
    CommMemType memType = (mr.flags & REG_MR_FLAG_HBM) ? COMM_MEM_TYPE_DEVICE : COMM_MEM_TYPE_HOST;
    if ((mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0) {
        const uint64_t dva = HybmVaManager::GetInstance().TransformVa(mr.addr, HVM_HVA, HVM_DVA);
        if (dva != 0) {
            regAddr = dva;
            memType = COMM_MEM_TYPE_DEVICE;
            BM_LOG_INFO("DRAM addr 0x" << std::hex << mr.addr << " mapped to DVA 0x" << dva << std::dec);
        } else {
            BM_LOG_WARN("DRAM addr 0x" << std::hex << mr.addr << " has no DVA mapping, using HVA");
        }
    }

    HcommCommMem hcommMem{};
    hcommMem.type = memType;
    hcommMem.addr = reinterpret_cast<void *>(regAddr);
    hcommMem.size = mr.size;

    HcommMemHandle handle = nullptr;
    auto ret = hcommApi_.RawMemReg(localEndpoint_, std::to_string(mr.addr).c_str(), hcommMem, handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommMemReg failed, addr: 0x" << std::hex << regAddr << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    LocalMemEntry entry{};
    entry.handle = handle;
    entry.addr = regAddr;
    entry.size = mr.size;
    entry.refCount = 1;
    localRegistrations_.emplace(mr.addr, entry);

    BM_LOG_INFO("HcommMemReg success, addr: 0x" << std::hex << regAddr << " handle: " << handle);
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = localRegistrations_.find(addr);
    if (it == localRegistrations_.end()) {
        BM_LOG_WARN("addr not registered: " << std::hex << addr);
        return BM_OK;
    }
    if (it->second.refCount > 1) {
        it->second.refCount--;
        return BM_OK;
    }

    auto ret = hcommApi_.RawMemUnreg(localEndpoint_, it->second.handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommMemUnreg failed, addr: " << std::hex << addr << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    localRegistrations_.erase(it);
    return BM_OK;
}

bool DeviceRdmaHcommTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    std::lock_guard<std::mutex> guard(mutex_);
    // Range match (consistent with legacy native RDMA / URMA / host_hcom):
    // a sub-range inside a registered region counts as registered.
    for (const auto &[regAddr, entry] : localRegistrations_) {
        if (addr >= regAddr && (addr - regAddr) <= entry.size && size <= entry.size - (addr - regAddr)) {
            return true;
        }
    }
    return false;
}

Result DeviceRdmaHcommTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = localRegistrations_.find(addr);
    if (it == localRegistrations_.end()) {
        BM_LOG_ERROR("addr not registered: " << std::hex << addr);
        return BM_INVALID_PARAM;
    }

    void *memDesc = nullptr;
    uint32_t descLen = 0;
    auto ret = hcommApi_.RawMemExport(localEndpoint_, it->second.handle, memDesc, descLen);
    if (ret != 0) {
        BM_LOG_ERROR("HcommMemExport failed, addr: " << std::hex << addr << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    std::memcpy(key.keys, memDesc, (descLen <= sizeof(key.keys)) ? descLen : sizeof(key.keys));
    // Store metadata in slot 2 via HcommKeyMeta struct (device_urma-compatible pattern)
    auto &meta = HcommKeyMetaAt(key);
    meta.addr = it->second.addr;
    meta.size = it->second.size;
    meta.descLen = static_cast<uint64_t>(descLen);
    // gvaBase must be the peer's GVA base (not HVA). addr here is the HVA
    // (realSlice->vAddress_), so translate HVA->GVA; in the HBM model GVA==HVA
    // and TransformVa returns the original value.
    const uint64_t peerGva = HybmVaManager::GetInstance().TransformVa(addr, HVM_HVA, HVM_GVA);
    meta.gvaBase = (peerGva != 0) ? peerGva : addr;
    BM_LOG_DEBUG("QueryMemoryKey localFlagHandle_=" << localFlagHandle_
                                                    << " localFlagExportLen_=" << localFlagExportLen_
                                                    << " descSize=" << localFlagExportDesc_.size());
    if (localFlagHandle_ != nullptr) {
        const uint32_t flagSlot = HCOMM_KEY_FLAG_SLOT;
        const uint32_t maxFlagBytes = (sizeof(key.keys) / sizeof(key.keys[0]) - HCOMM_KEY_FLAG_SLOT) * sizeof(uint64_t);
        meta.flagLen = static_cast<uint64_t>(localFlagExportLen_);
        BM_LOG_DEBUG("QueryMemoryKey flagExportLen=" << localFlagExportLen_
                                                     << " descSize=" << localFlagExportDesc_.size()
                                                     << " maxFlagBytes=" << maxFlagBytes);
        if (localFlagExportLen_ > 0 && !localFlagExportDesc_.empty() && localFlagExportLen_ <= maxFlagBytes) {
            std::memcpy(&key.keys[flagSlot], localFlagExportDesc_.data(), localFlagExportLen_);
            BM_LOG_DEBUG("QueryMemoryKey flag data copied, len=" << localFlagExportLen_);
        } else {
            BM_LOG_DEBUG("QueryMemoryKey flag memcpy skipped: len=" << localFlagExportLen_
                                                                    << " empty=" << localFlagExportDesc_.empty()
                                                                    << " maxBytes=" << maxFlagBytes);
        }
    } else {
        meta.flagLen = 0;
        BM_LOG_DEBUG("QueryMemoryKey localFlagHandle_ is nullptr!");
    }

    BM_LOG_DEBUG("QueryMemoryKey addr=0x" << std::hex << addr << " export descLen=" << descLen << std::dec);
    return BM_OK;
}

void DeviceRdmaHcommTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    if (addr != nullptr) {
        HcommKeyMetaAt(key).addr = reinterpret_cast<uint64_t>(addr);
    }
}

// ============================================================================
// Connection prepare
// ============================================================================

Result DeviceRdmaHcommTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!opened_) {
        BM_LOG_ERROR("transport not opened");
        return BM_ERROR;
    }

    for (const auto &item : options.options) {
        const uint32_t peerRank = item.first;
        if (peerRank >= rankCount_) {
            BM_LOG_ERROR("invalid peerRank: " << peerRank);
            return BM_INVALID_PARAM;
        }
        if (peerRank == rankId_) {
            continue;
        }

        RdmaHcommPrivateData peerData{};
        std::memcpy(&peerData, item.second.privateData.key.keys, sizeof(peerData));
        if (peerData.magic != RDMA_HCOMM_PRIVATE_DATA_MAGIC || peerData.version != RDMA_HCOMM_PRIVATE_DATA_VERSION) {
            BM_LOG_ERROR("invalid private data from peer: " << peerRank);
            return BM_INVALID_PARAM;
        }

        // 如果有远端内存 key，先 import，再创建通道
        // 如果没有（对端尚未注册内存），等 UpdateRankOptions 带 memKeys 后再处理
        if (!item.second.memKeys.empty()) {
            auto ret = ImportPeerMems(peerRank, item.second.memKeys);
            if (ret != BM_OK) {
                BM_LOG_ERROR("ImportPeerMems failed for peer: " << peerRank << " ret: " << ret);
                return ret;
            }
            BM_LOG_DEBUG("Prepare has memKeys for rank=" << peerRank << ", creating channel now");
            ret = CreatePeerResources(peerRank, peerData);
            if (ret != BM_OK) {
                BM_LOG_ERROR("CreatePeerResources failed for peer: " << peerRank);
                return ret;
            }
        }
    }
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::CreatePeerResources(uint32_t peerRank, const RdmaHcommPrivateData &peerData)
{
    auto &state = remoteRanks_[peerRank];

    if (state.channel != 0 || state.thread != 0) {
        return BM_OK;
    }

    // Build remote EndpointDesc from peer data
    EndpointDesc remoteDesc{};
    remoteDesc.protocol = COMM_PROTOCOL_ROCE;
    remoteDesc.commAddr.type = peerData.addrType;
    std::memcpy(remoteDesc.commAddr.raws, peerData.addrBytes, sizeof(remoteDesc.commAddr.raws));
    remoteDesc.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
    remoteDesc.loc.device.devPhyId = peerData.devPhyId;
    remoteDesc.loc.device.superDevId = peerData.sdid;
    remoteDesc.loc.device.serverIdx = serverId_;
    remoteDesc.loc.device.superPodIdx = superPodId_;
    state.remoteEndpointDesc = remoteDesc;

    // Allocate thread
    uint32_t notifyNum = DEFAULT_NOTIFY_NUM;
    ThreadHandle threadHandle = 0;
    auto ret = HcommApiWrapper::AllocThread(COMM_ENGINE_AICPU_TS, notifyNum, threadHandle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommThreadAlloc failed for peer: " << peerRank << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    state.thread = threadHandle;

    // Build channel descriptor
    HcommChannelDesc chDesc{};
    HcommChannelDescInit(&chDesc, 1);
    chDesc.role = (rankId_ > peerRank) ? HCOMM_SOCKET_ROLE_CLIENT : HCOMM_SOCKET_ROLE_SERVER;
    chDesc.remoteEndpoint = remoteDesc;
    chDesc.notifyNum = 1;
    chDesc.exchangeAllMems = true;

    // Build channelName: consistent on both ends (sorted rank pair)
    const uint32_t lowerRank = (rankId_ < peerRank) ? rankId_ : peerRank;
    const uint32_t higherRank = (rankId_ < peerRank) ? peerRank : rankId_;
    channelNameStore_ = "hybm_" + std::to_string(lowerRank) + "_" + std::to_string(higherRank);
    chDesc.channelName = channelNameStore_.c_str();

    chDesc.roceAttr.tc = DEVICE_RDMA_HCOMM_DEFAULT_TC;
    chDesc.roceAttr.sl = DEVICE_RDMA_HCOMM_DEFAULT_SL;
    chDesc.roceAttr.queueNum = 1;

    // Set port from peer's listen port (critical for QP to enter RTS state)
    chDesc.port = static_cast<uint16_t>(peerData.listenPort);
    BM_LOG_INFO("CreatePeerResources: using peer listenPort=" << peerData.listenPort << " for peer rank=" << peerRank);

    ChannelHandle channelHandle = 0;
    ret = hcommApi_.CreateChannel(localEndpoint_, COMM_ENGINE_AICPU, chDesc, channelHandle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommChannelCreate failed for peer: " << peerRank << " ret: " << ret);
        (void)HcommApiWrapper::FreeThread(threadHandle);
        state.thread = 0;
        return BM_DL_FUNCTION_FAILED;
    }
    state.channel = channelHandle;

    // Wait for channel to connect
    int32_t status = -1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(CHANNEL_CONNECT_TIMEOUT_MS);
    while (std::chrono::steady_clock::now() < deadline) {
        auto chk = hcommApi_.GetChannelStatus(channelHandle, status);
        if (chk == 0 && status == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(CHANNEL_STATUS_POLL_INTERVAL_MS));
    }
    if (status != 0) {
        BM_LOG_ERROR("channel connect timeout for peer: " << peerRank << " status: " << status);
        (void)hcommApi_.DestroyChannel(channelHandle);
        (void)HcommApiWrapper::FreeThread(threadHandle);
        state.channel = 0;
        state.thread = 0;
        return BM_TIMEOUT;
    }

    BM_LOG_INFO("CreatePeerResources success for peer: "
                << peerRank << " thread: " << threadHandle << " channel: " << channelHandle << " role: "
                << (chDesc.role == HCOMM_SOCKET_ROLE_CLIENT ? "CLIENT" : "SERVER") << " port: " << chDesc.port);
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::DestroyPeerResources(uint32_t peerRank)
{
    auto it = remoteRanks_.find(peerRank);
    if (it == remoteRanks_.end()) {
        return BM_OK;
    }
    auto &state = it->second;

    // stream/notify/context are per-thread (CompletionContext), not per-peer.
    // They are destroyed in CloseDevice via registry_ cleanup.
    if (state.channel != 0) {
        (void)hcommApi_.DestroyChannel(state.channel);
        state.channel = 0;
    }
    if (state.thread != 0) {
        (void)HcommApiWrapper::FreeThread(state.thread);
        state.thread = 0;
    }
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::ImportPeerMems(uint32_t peerRank,
                                                       const std::vector<TransportMemoryKey> &memKeys)
{
    // 幂等：peerDva 在 import 成功后设置。Prepare（644 行）可能对同一 peer
    // 多次调用（如后续 UpdateRankOptions 带 memKeys 再入），重复 import 会让 HCOMM 返回
    // "memDesc already imported"（HCCL_E_AGAIN），这里直接跳过已导入的 peer。
    if (remoteRanks_[peerRank].peerDva != 0) {
        return BM_OK;
    }
    for (const auto &key : memKeys) {
        // Read metadata from slot 2 via HcommKeyMeta struct; fallback to other slots if empty
        const auto &meta = HcommKeyMetaAt(key);
        uint64_t peerAddr = meta.addr;
        uint64_t peerSize = meta.size;
        if (peerAddr == 0 || peerSize == 0) {
            peerAddr = key.keys[HCOMM_KEY_LEGACY_ADDR_IDX];
            peerSize = key.keys[HCOMM_KEY_LEGACY_SIZE_IDX];
            if (peerAddr == 0 || peerSize == 0) {
                peerAddr = key.keys[HCOMM_KEY_HOST_ADDR_IDX];
                peerSize = key.keys[HCOMM_KEY_HOST_SIZE_IDX];
                if (peerAddr == 0 || peerSize == 0) {
                    continue;
                }
            }
        }
        const uint64_t descLen = meta.descLen;
        const uint64_t flagLen = meta.flagLen;
        const uint64_t gvaBase = meta.gvaBase;
        BM_LOG_DEBUG("ImportPeerMems rank=" << peerRank << " peerAddr=0x" << std::hex << peerAddr << " peerSize=0x"
                                            << peerSize << " descLen=" << std::dec << descLen);

        if (peerAddr == 0 || peerSize == 0 || descLen == 0) {
            BM_LOG_DEBUG("ImportPeerMems skip rank=" << peerRank << " (no valid HCOMM descriptor in memKeys)");
            continue;
        }

        void *memDesc = reinterpret_cast<void *>(const_cast<uint64_t *>(key.keys));
        BM_LOG_DEBUG("ImportPeerMems descLen=" << descLen << " meta={addr=0x" << std::hex << meta.addr << " size=0x"
                                               << meta.size << " descLen=" << std::dec << meta.descLen << " gvaBase=0x"
                                               << std::hex << meta.gvaBase << std::dec << " flagLen=" << meta.flagLen
                                               << "}");

        HcommCommMem outMem{};
        auto ret = RawMemImportWithRetry(localEndpoint_, memDesc, static_cast<uint32_t>(descLen), peerRank, outMem);
        if (ret != 0) {
            BM_LOG_ERROR("HcommMemImport failed for peer: " << peerRank << " ret: " << ret);
            return BM_DL_FUNCTION_FAILED;
        }
        BM_LOG_DEBUG("ImportPeerMems success for rank=" << peerRank << " outMem addr=0x" << std::hex << outMem.addr
                                                        << " size=0x" << outMem.size << std::dec
                                                        << " type=" << outMem.type);

        // Store peer DVA/GVA for address translation
        auto &state = remoteRanks_[peerRank];
        state.peerDva = reinterpret_cast<uint64_t>(outMem.addr);
        state.peerDvaSize = outMem.size;
        state.peerGva = gvaBase;
        // Save the imported descriptor so RemoveRanks can Unimport it and keep the
        // HCOMM endpoint table in sync (avoid "memDesc already imported" on re-join).
        state.importedDescs.emplace_back(static_cast<const uint8_t *>(memDesc),
                                         static_cast<const uint8_t *>(memDesc) + descLen);
        BM_LOG_INFO("ImportPeerMems stored peer rank=" << peerRank << " peerDva=0x" << std::hex << state.peerDva
                                                       << " peerGva=0x" << state.peerGva << " peerSize=0x"
                                                       << state.peerDvaSize << std::dec);

        // Import flag descriptor from slot 3+ (if present)
        if (flagLen > 0 && flagLen <= (sizeof(TransportMemoryKey{}.keys) - HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t))) {
            const void *flagDesc = reinterpret_cast<const void *>(&key.keys[HCOMM_KEY_FLAG_SLOT]);
            HcommCommMem flagOutMem{};
            auto flagRet =
                RawMemImportWithRetry(localEndpoint_, flagDesc, static_cast<uint32_t>(flagLen), peerRank, flagOutMem);
            if (flagRet == 0 && flagOutMem.type != COMM_MEM_TYPE_INVALID) {
                state.remoteFlagAddr = reinterpret_cast<uint64_t>(flagOutMem.addr);
                state.remoteFlagSize = flagOutMem.size;
                state.importedDescs.emplace_back(static_cast<const uint8_t *>(flagDesc),
                                                 static_cast<const uint8_t *>(flagDesc) + flagLen);
                BM_LOG_INFO("ImportPeerMems imported flag for peer=" << peerRank << " addr=0x" << std::hex
                                                                     << state.remoteFlagAddr << std::dec
                                                                     << " size=" << state.remoteFlagSize);
            }
        }
    }
    return BM_OK;
}

int32_t DeviceRdmaHcommTransportManager::RawMemImportWithRetry(HcommEndpointHandle endpoint, const void *desc,
                                                               uint32_t descLen, uint32_t peerRank,
                                                               HcommCommMem &outMem) const
{
    constexpr int kImportMaxRetry = 10;
    constexpr int kImportRetrySleepUs = 1000;
    constexpr int32_t kHcclEAgain = 20; // HCCL_E_AGAIN：资源暂不可用（并发 import 冲突）
    for (int attempt = 0; attempt < kImportMaxRetry; ++attempt) {
        auto ret = hcommApi_.RawMemImport(endpoint, desc, descLen, outMem);
        if (ret == 0) {
            return ret;
        }
        if (ret != kHcclEAgain) {
            return ret;
        }
        // HCCL_E_AGAIN：并发 join 时多进程对同一对端 import 的瞬时冲突，重试可成功
        BM_LOG_WARN("HcommMemImport AGAIN, retry attempt=" << (attempt + 1) << "/" << kImportMaxRetry
                                                           << " peer: " << peerRank << " ret: " << ret);
        usleep(kImportRetrySleepUs);
    }
    return kHcclEAgain;
}

// ============================================================================
// Connect / Remove ranks
// ============================================================================

Result DeviceRdmaHcommTransportManager::Connect()
{
    connected_ = true;
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::AsyncConnect()
{
    return Connect();
}

Result DeviceRdmaHcommTransportManager::WaitForConnected(int64_t timeoutNs)
{
    (void)timeoutNs;
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto rankId : removedRanks) {
        auto it = remoteRanks_.find(rankId);
        if (it != remoteRanks_.end()) {
            // Unimport peer memDesc/flagDesc from the HCOMM endpoint BEFORE dropping
            // local state, otherwise a later re-import (e.g. join retry after a rank
            // reconnect) hits "memDesc already imported" (HCCL_E_AGAIN) forever.
            for (const auto &desc : it->second.importedDescs) {
                if (desc.empty()) {
                    continue;
                }
                auto ret = hcommApi_.RawMemUnimport(localEndpoint_, desc.data(), static_cast<uint32_t>(desc.size()));
                if (ret != BM_OK) {
                    BM_LOG_WARN("RemoveRanks RawMemUnimport failed for peer: " << rankId << " ret: " << ret);
                }
            }
        }
        DestroyPeerResources(rankId);
        remoteRanks_.erase(rankId);
    }
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    return Prepare(options);
}

const std::string &DeviceRdmaHcommTransportManager::GetNic() const
{
    return localNic_;
}

const TransportPrivateData DeviceRdmaHcommTransportManager::GetPrivateData() const
{
    TransportPrivateData data{};
    if (localEndpoint_ == nullptr) {
        return data;
    }

    RdmaHcommPrivateData priv{};
    priv.protocol = COMM_PROTOCOL_ROCE;
    priv.addrType = localEndpointDesc_.commAddr.type;
    std::memcpy(priv.addrBytes, localEndpointDesc_.commAddr.raws, sizeof(priv.addrBytes));
    priv.devPhyId = phyDeviceId_;
    priv.sdid = sdid_;
    // listenPort defaults to 0; channel creation works via peer endpoint, not port.

    std::memcpy(data.key.keys, &priv, sizeof(priv));
    return data;
}

// ============================================================================
// AICPU kernel launch
// ============================================================================

// ============================================================================
// GVA → DVA address conversion for HCOMM RDMA writes/reads
// HCOMM registers DVA addresses, but the framework passes GVA.
// ============================================================================

uint64_t DeviceRdmaHcommTransportManager::ConvertGvaToDva(uint64_t gva) const
{
    // localRegistrations_ is keyed by HVA (mr.addr), while callers pass GVA.
    // In HBM model GVA == HVA so the direct lookup works; in DRAM + 56-bit GVA
    // model GVA (0x1000...) differs from HVA (0x2800...), so translate GVA→HVA
    // first before looking up the registered range.
    const uint64_t hva = HybmVaManager::GetInstance().TransformVa(gva, HVM_GVA, HVM_HVA);
    const uint64_t lookupKey = (hva != 0) ? hva : gva;
    BM_LOG_DEBUG("ConvertGvaToDva gva=0x" << std::hex << gva << " -> hva=0x" << hva << std::dec);

    // Look up in localRegistrations_ (keyed by HVA, value stores DVA)
    auto it = localRegistrations_.find(lookupKey);
    if (it != localRegistrations_.end()) {
        return it->second.addr + (lookupKey - it->first);
    }
    // Fallback: linear scan for offset within a registered range
    for (const auto &[regGva, entry] : localRegistrations_) {
        if (lookupKey >= regGva && lookupKey < regGva + entry.size) {
            return entry.addr + (lookupKey - regGva);
        }
    }
    BM_LOG_WARN("ConvertGvaToDva: no mapping for GVA=0x" << std::hex << gva << std::dec
                                                         << ", using raw value (may fail if HOST DRAM)");
    return gva;
}

// Convert remote GVA to DVA using the peer's registered base.
// state.peerGva stores the peer's GVA base (from QueryMemoryKey gvaBase), and
// callers pass GVA. So remote DVA = peerDva + (rGva - peerGva). For memory that
// the VaManager maps GVA→DVA (local HBM), use that mapping first.
uint64_t DeviceRdmaHcommTransportManager::ConvertRemoteGvaToDva(const RemoteRankState &state, uint64_t rGva) const
{
    uint64_t remoteDva = rGva;

    // Local memory with an existing GVA→DVA mapping (e.g. HBM, or imported
    // memory the VaManager registered with a DVA).
    const uint64_t dvaViaMgr = HybmVaManager::GetInstance().TransformVa(rGva, HVM_GVA, HVM_DVA);
    if (dvaViaMgr != 0) {
        return dvaViaMgr;
    }

    // Remote peer memory: peerGva is the peer's GVA base (0x1000...), peerDva is
    // the imported DVA of that base. rGva is a GVA in the peer's space.
    if (state.peerDva != 0 && state.peerGva != 0) {
        const int64_t gvaOffset = static_cast<int64_t>(rGva) - static_cast<int64_t>(state.peerGva);
        if (gvaOffset >= 0 && static_cast<uint64_t>(gvaOffset) < state.peerDvaSize) {
            remoteDva = state.peerDva + static_cast<uint64_t>(gvaOffset);
        }
    }
    return remoteDva;
}

// ============================================================================
// TLS binding & per-thread CompletionContext
// ============================================================================

std::vector<DeviceRdmaHcommTransportManager::ContextBinding> &DeviceRdmaHcommTransportManager::GetTlsBindings()
{
    static thread_local std::vector<ContextBinding> bindings;
    return bindings;
}

DeviceRdmaHcommTransportManager::CompletionContext *DeviceRdmaHcommTransportManager::FindCurrentContextLocked() const
{
    auto &bindings = GetTlsBindings();
    for (auto &b : bindings) {
        if (!b.owner.expired()) {
            auto ctx = b.ctx.lock();
            if (ctx != nullptr) {
                return ctx.get();
            }
        }
    }
    return nullptr;
}

DeviceRdmaHcommTransportManager::CompletionContext *DeviceRdmaHcommTransportManager::LookupOrCreateContextLocked()
{
    auto existing = FindCurrentContextLocked();
    if (existing != nullptr) {
        return existing;
    }
    auto ctx = std::make_shared<CompletionContext>();
    auto ret = CreateAndPublishContextLocked(ctx);
    if (ret != BM_OK) {
        return nullptr;
    }
    return ctx.get();
}

Result DeviceRdmaHcommTransportManager::CreateAndPublishContextLocked(std::shared_ptr<CompletionContext> &ctx)
{
    // AICPU kernel launch requires a FAST_LAUNCH | FAST_SYNC stream (same as device_urma).
    void *stream = HybmStreamManager::GetThreadAclStream();
    if (stream == nullptr) {
        BM_LOG_ERROR("CreateAndPublishContextLocked GetThreadAclStream failed");
        return BM_DL_FUNCTION_FAILED;
    }
    ctx->stream = stream;

    auto ret = EnsureContextInitLocked(*ctx);
    if (ret != BM_OK) {
        ctx->stream = nullptr;
        return ret;
    }

    auto &bindings = GetTlsBindings();
    ContextBinding binding{};
    binding.owner = openGeneration_;
    binding.ctx = ctx;
    bindings.push_back(binding);
    {
        std::lock_guard<std::mutex> regGuard(mutex_);
        registry_.push_back(ctx);
    }

    BM_LOG_INFO("CreateAndPublishContextLocked success, notifyId=" << ctx->notifyId);
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::EnsureContextInitLocked(CompletionContext &ctx)
{
    if (ctx.initialized) {
        return BM_OK;
    }

    void *notify = nullptr;
    auto ret = DlAclApi::AclrtCreateNotify(&notify, ACL_NOTIFY_DEVICE_USE_ONLY);
    if (ret != BM_OK) {
        BM_LOG_ERROR("EnsureContextInitLocked AclrtCreateNotify failed, ret=" << ret);
        return ret;
    }
    ctx.notify = notify;

    uint32_t notifyId = 0;
    ret = DlAclApi::AclrtGetNotifyId(notify, &notifyId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("EnsureContextInitLocked AclrtGetNotifyId failed, ret=" << ret);
        (void)DlAclApi::AclrtDestroyNotify(notify);
        ctx.notify = nullptr;
        return ret;
    }
    ctx.notifyId = notifyId;
    // RtGetDevResAddress is STARS-only (A5), not available on A2.
    // notifyAddr/notifyLen stay 0; notify works via notifyId (use_notify_record=1).
    // notifyHcommHandle stays nullptr; CloseDevice skips unregistration.

    ctx.initialized = true;
    BM_LOG_INFO("EnsureContextInitLocked success, notifyId=" << notifyId);
    return BM_OK;
}

// ============================================================================
// Pending transfer management
// ============================================================================

void DeviceRdmaHcommTransportManager::ExtractRankPending(std::vector<PendingTransfer> &src, uint32_t rankId,
                                                         std::vector<PendingTransfer> &dst)
{
    auto it = src.begin();
    while (it != src.end()) {
        if (it->rankId == rankId) {
            dst.push_back(std::move(*it));
            it = src.erase(it);
        } else {
            ++it;
        }
    }
}

void DeviceRdmaHcommTransportManager::RestoreRankPending(std::vector<PendingTransfer> &src,
                                                         std::vector<PendingTransfer> &dst)
{
    for (auto &p : src) {
        dst.push_back(std::move(p));
    }
    src.clear();
}

Result DeviceRdmaHcommTransportManager::SynchronizeContextLocked(void * /* notify */, void *stream,
                                                                 std::vector<PendingTransfer> &pendingTransfers)
{
    auto syncRet = DlAclApi::AclrtSynchronizeStream(stream);
    if (syncRet != BM_OK) {
        BM_LOG_WARN("SynchronizeContextLocked AclrtSynchronizeStream failed, ret=" << syncRet);
    }
    for (auto &pt : pendingTransfers) {
        device::ReleaseDeviceTransferBuffers(pt.buffers);
    }
    pendingTransfers.clear();
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::SynchronizeRankPendingLocked(CompletionContext &ctx, RemoteRankState &state,
                                                                     uint32_t rankId, bool hasInFlight)
{
    std::vector<PendingTransfer> rankPending{};
    std::lock_guard<std::mutex> rankLock(state.rankMutex);
    ExtractRankPending(ctx.pendingTransfers, rankId, rankPending);

    // Data kernel already ensured data consistency via WRITE → RDMA READ flag (QP ordering).
    // No marker kernel needed; just drain the stream and release buffers.
    auto syncRet = SynchronizeContextLocked(ctx.notify, ctx.stream, rankPending);
    if (syncRet != BM_OK) {
        RestoreRankPending(rankPending, ctx.pendingTransfers);
    }
    return syncRet;
}

// ============================================================================
// Transfer staging
// ============================================================================

Result DeviceRdmaHcommTransportManager::StageAndLaunchTransfer(CompletionContext &ctx, RemoteRankState &state,
                                                               bool isRead, const std::vector<uint64_t> &localVec,
                                                               const std::vector<uint64_t> &remoteVec,
                                                               const std::vector<uint64_t> &sizeVec, uint32_t rankId)
{
    std::lock_guard<std::mutex> rankLock(state.rankMutex);

    DeviceTransferBuffers buffers{};
    auto ret = device::LaunchDeviceKernel(
        KernelLaunchConfig{isRead ? deviceFuncHandles_.batchRead : deviceFuncHandles_.batchWrite, state.thread,
                           state.channel, ctx.stream, isRead, static_cast<uint32_t>(localVec.size()), localVec.data(),
                           remoteVec.data(), sizeVec.data(), state.remoteFlagAddr,
                           reinterpret_cast<uint64_t>(localFlagPtr_), static_cast<uint32_t>(localFlagSize_)},
        buffers);
    if (ret != BM_OK) {
        return ret;
    }

    // Synchronize immediately after launch to keep only one kernel running on the
    // shared HCOMM thread/channel at a time. Concurrent kernels interleave on the
    // AICPU engine thread and corrupt HCOMM's thread-local BatchMode state
    // (g_threadLaunchCtx), which fails the kernel with aicpu error 0x2a. This
    // matches the synchronous model of device_urma; the flag read-back inside the
    // kernel still provides write ordering (QP ordering) on A2 where Fence is no-op.
    const auto syncRet = DlAclApi::AclrtSynchronizeStream(ctx.stream);
    device::ReleaseDeviceTransferBuffers(buffers);
    if (syncRet != BM_OK) {
        BM_LOG_ERROR("StageAndLaunchTransfer AclrtSynchronizeStream failed, rank: " << rankId << " ret: " << syncRet);
        return syncRet;
    }
    return BM_OK;
}

// ============================================================================
// Data transfer
// ============================================================================

Result DeviceRdmaHcommTransportManager::RemoteIo(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size,
                                                 bool isRead)
{
    if (size == 0) {
        return BM_OK;
    }
    auto it = remoteRanks_.find(rankId);
    if (it == remoteRanks_.end()) {
        BM_LOG_ERROR("rank not connected: " << rankId);
        return BM_NOT_CONNECTED;
    }
    auto &state = it->second;
    if (state.channel == 0 || state.thread == 0) {
        BM_LOG_ERROR("peer resources not ready for rank: " << rankId);
        return BM_NOT_INITIALIZED;
    }

    // Convert GVA to DVA for HCOMM: RDMA uses registered DVA addresses
    const uint64_t localDva = ConvertGvaToDva(lAddr);
    const uint64_t remoteDva = ConvertRemoteGvaToDva(state, rAddr);

    BM_LOG_DEBUG("RemoteIo rank=" << rankId << (isRead ? " READ" : " WRITE") << " lGVA=0x" << std::hex << lAddr
                                  << " lDVA=0x" << localDva << " rGVA=0x" << rAddr << " rDVA=0x" << remoteDva
                                  << " size=0x" << size << std::dec);

    // TLS: look up or create per-thread CompletionContext
    CompletionContext *ctx = LookupOrCreateContextLocked();
    if (ctx == nullptr) {
        BM_LOG_ERROR("RemoteIo LookupOrCreateContextLocked failed");
        return BM_MALLOC_FAILED;
    }

    return StageAndLaunchTransfer(*ctx, state, isRead, {localDva}, {remoteDva}, {size}, rankId);
}

Result DeviceRdmaHcommTransportManager::RemoteIoBatch(uint32_t rankId, const CopyDescriptor &descriptor, bool isRead)
{
    if (descriptor.counts.empty()) {
        return BM_OK;
    }
    auto it = remoteRanks_.find(rankId);
    if (it == remoteRanks_.end()) {
        BM_LOG_ERROR("rank not connected: " << rankId);
        return BM_NOT_CONNECTED;
    }
    auto &state = it->second;
    if (state.channel == 0 || state.thread == 0) {
        BM_LOG_ERROR("peer resources not ready for rank: " << rankId);
        return BM_NOT_INITIALIZED;
    }

    const auto batchSize = descriptor.counts.size();
    std::vector<uint64_t> localVec(batchSize);
    std::vector<uint64_t> remoteVec(batchSize);
    for (size_t i = 0; i < batchSize; ++i) {
        // Convert GVA to DVA for HCOMM RDMA
        const uint64_t gvaLocal = reinterpret_cast<uint64_t>(descriptor.localAddrs[i]);
        const uint64_t gvaRemote = reinterpret_cast<uint64_t>(descriptor.globalAddrs[i]);
        localVec[i] = ConvertGvaToDva(gvaLocal);
        remoteVec[i] = ConvertRemoteGvaToDva(state, gvaRemote);
    }

    CompletionContext *ctx = LookupOrCreateContextLocked();
    if (ctx == nullptr) {
        BM_LOG_ERROR("RemoteIoBatch LookupOrCreateContextLocked failed");
        return BM_MALLOC_FAILED;
    }

    return StageAndLaunchTransfer(*ctx, state, isRead, localVec, remoteVec, descriptor.counts, rankId);
}

// ============================================================================
// Data transfer public API (overrides from TransportManager)
// ============================================================================

Result DeviceRdmaHcommTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    auto ret = RemoteIo(rankId, lAddr, rAddr, size, true);
    if (ret == BM_OK) {
        ret = Synchronize(rankId);
    }
    return ret;
}

Result DeviceRdmaHcommTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    auto ret = RemoteIo(rankId, lAddr, rAddr, size, false);
    if (ret == BM_OK) {
        ret = Synchronize(rankId);
    }
    return ret;
}

Result DeviceRdmaHcommTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIo(rankId, lAddr, rAddr, size, true);
}

Result DeviceRdmaHcommTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIo(rankId, lAddr, rAddr, size, false);
}

Result DeviceRdmaHcommTransportManager::Synchronize(uint32_t rankId)
{
    auto it = remoteRanks_.find(rankId);
    if (it == remoteRanks_.end()) {
        BM_LOG_ERROR("rank not connected: " << rankId);
        return BM_NOT_CONNECTED;
    }
    auto &state = it->second;
    if (state.channel == 0) {
        BM_LOG_ERROR("peer resources not ready for rank: " << rankId);
        return BM_NOT_INITIALIZED;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    CompletionContext *currentCtx = FindCurrentContextLocked();
    if (currentCtx == nullptr) {
        // No TLS context for current thread; warn if rank has pending in other contexts
        bool otherCtxHasPending = false;
        for (const auto &ctxSp : registry_) {
            if (!ctxSp) {
                continue;
            }
            for (const auto &pt : ctxSp->pendingTransfers) {
                if (pt.rankId == rankId) {
                    otherCtxHasPending = true;
                    break;
                }
            }
        }
        if (otherCtxHasPending) {
            BM_LOG_ERROR("Synchronize: rank " << rankId << " has pending in another context");
            return BM_ERROR;
        }
        return BM_OK;
    }

    // Check if current context has pending for target rank
    bool needSync = false;
    for (const auto &pt : currentCtx->pendingTransfers) {
        if (pt.rankId == rankId) {
            needSync = true;
            break;
        }
    }
    if (needSync) {
        return SynchronizeRankPendingLocked(*currentCtx, state, rankId, false);
    }

    // No pending in current context. Check other contexts.
    for (const auto &ctxSp : registry_) {
        if (!ctxSp || ctxSp.get() == currentCtx) {
            continue;
        }
        for (const auto &pt : ctxSp->pendingTransfers) {
            if (pt.rankId == rankId) {
                BM_LOG_ERROR("Synchronize: rank " << rankId << " has pending in another context");
                return BM_ERROR;
            }
        }
    }
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    return RemoteIoBatch(rankId, descriptor, false);
}

Result DeviceRdmaHcommTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    return RemoteIoBatch(rankId, descriptor, true);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
