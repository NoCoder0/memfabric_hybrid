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
#include "genetty_verbs_device_wrapper.h"

#include <arpa/inet.h>

#include "under_api/acl/genetty_dl_acl_api.h"
#include "under_api/hccp/genetty_dl_hccp_api.h"
#include "under_api/tsd/genetty_dl_tsd_api.h"

namespace ock {
namespace openDCC {
namespace verbs {
namespace {
constexpr uint32_t TSD_RDMA_RANK_SIZE = 2; /* rankSize greater than one starts the HCCP service */
constexpr int HCCP_NETWORK_OFFLINE = 1;
constexpr int HCCP_HDC_TYPE_RDMA = 6;
constexpr uint32_t HCCP_NOTIFY = 1;
} // namespace

DeviceRdmaContext *DeviceRdmaContext::PENDING_CLEANUP = nullptr;
std::mutex DeviceRdmaContext::PENDING_CLEANUP_MUTEX;

DeviceRdmaContext::DeviceRdmaContext() : cleanupContext_(new(std::nothrow) DeviceRdmaContext(PendingCleanupTag{})) {}

Result DeviceRdmaContext::CreateContext(uint32_t userDeviceId, const std::string &deviceIp,
                                        DeviceRdmaContextPtr &out) noexcept
{
    GEN_LOG_DEBUG("enter, userDeviceId(" << userDeviceId << "), ip(" << deviceIp << ")");

    /* step1: initialize APIs and finish pending cleanup */
    auto result = InitializeApis();
    if (result != GEN_OK) {
        return result;
    }
    result = RetryPendingCleanup();
    if (result != GEN_OK) {
        return result;
    }

    /* step2: create context and reserve its cleanup owner */
    auto context = mf::MakeRef<DeviceRdmaContext>();
    GEN_ASSERT_MSG_RETURN(context != nullptr && context->cleanupContext_ != nullptr,
                          "Create DeviceRdmaContext failed, probably out of memory, userDeviceId("
                              << userDeviceId << "), ip(" << deviceIp << ")",
                          GEN_MALLOC_FAILED);

    /* step3: initialize context; its destructor retains any unfinished cleanup */
    result = context->Initialize(userDeviceId, deviceIp);
    if (result != GEN_OK) {
        return result;
    }

    out = context;
    GEN_LOG_DEBUG("Created device context, userDeviceId(" << userDeviceId << "), ip(" << context->Ip() << ")");
    return GEN_OK;
}

Result DeviceRdmaContext::InitializeApis() noexcept
{
    auto result = AclApi::Load();
    if (result != GEN_OK) {
        return result;
    }
    result = TsdApi::Load();
    if (result != GEN_OK) {
        return result;
    }
    result = HccpApi::Load();
    if (result != GEN_OK) {
        return result;
    }
    return AclApi::Init();
}

Result DeviceRdmaContext::Initialize(uint32_t userDeviceId, const std::string &deviceIp) noexcept
{
    deviceIp_ = deviceIp;
    auto result = SelectDevice(userDeviceId);
    if (result != GEN_OK) {
        return result;
    }
    result = OpenTsd();
    if (result != GEN_OK) {
        return result;
    }
    result = InitRa();
    if (result != GEN_OK) {
        return result;
    }
    if (deviceIp_.empty()) {
        result = QueryDeviceIp();
        if (result != GEN_OK) {
            return result;
        }
    }
    return OpenRdmaDevice();
}

void DeviceRdmaContext::Destroy(std::map<MemoryInfo, DeviceVerbsMR> &mrs) noexcept
{
    /* transfer ownership without allocating during destruction */
    pendingMrs_.swap(mrs);
    Destroy();
}

Result DeviceRdmaContext::CreateQueue(const GenettyOptions &options, uint32_t cqDepth, uint64_t id,
                                      DeviceVerbsQueuePtr &out) noexcept
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    GEN_ASSERT_MSG_RETURN(!closing_ && rdmaHandle_ != nullptr, "Device context is closed", GEN_NOT_INITIALIZED);
    auto queue = mf::MakeRef<DeviceVerbsQueue>();
    GEN_ASSERT_MSG_RETURN(queue != nullptr, "Allocate Device QP failed", GEN_MALLOC_FAILED);
    /* Retain the owner before acquiring hardware resources, including partial initialization. */
    queues_[queue.Get()] = queue;
    auto result = queue->Initialize(rdmaHandle_, userDeviceId_, logicDeviceId_, options, cqDepth, id);
    if (result != GEN_OK) {
        if (queue->Destroy() == GEN_OK) {
            queues_.erase(queue.Get());
        }
        return result;
    }
    out = queue;
    return GEN_OK;
}

Result DeviceRdmaContext::DestroyQueue(const DeviceVerbsQueuePtr &queue) noexcept
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    GEN_ASSERT_MSG_RETURN(queue != nullptr, "Destroy Device QP failed, queue is null", GEN_INVALID_PARAM);
    auto result = queue->Destroy();
    if (result == GEN_OK) {
        queues_.erase(queue.Get());
    }
    return result;
}

bool DeviceRdmaContext::HasPendingQueueCleanup() noexcept
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    for (const auto &entry : queues_) {
        if (entry.second->CleanupPending()) {
            return true;
        }
    }
    return false;
}

Result DeviceRdmaContext::DestroyAllQueues() noexcept
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    Result result = GEN_OK;
    auto iter = queues_.begin();
    while (iter != queues_.end()) {
        auto cleanup = iter->second->Destroy();
        if (cleanup != GEN_OK) {
            result = cleanup;
            ++iter;
        } else {
            iter = queues_.erase(iter);
        }
    }
    return result;
}

Result DeviceRdmaContext::UnRegisterAllMemory() noexcept
{
    if (pendingMrs_.empty()) {
        return GEN_OK;
    }
    GEN_ASSERT_MSG_RETURN(rdmaHandle_ != nullptr,
                          "Unregister all memory failed, as RDMA device handle is null, mr count(" << pendingMrs_.size()
                                                                                                   << ")",
                          GEN_DEVICE_ERROR);

    Result result = GEN_OK;
    auto iter = pendingMrs_.begin();
    while (iter != pendingMrs_.end()) {
        const auto &mr = iter->second;
        if (UNLIKELY(mr.handle == nullptr)) {
            GEN_LOG_ERROR("Unregister memory failed, as mr is null, address(" << mr.address << "), size(" << mr.size
                                                                              << ")");
            result = GEN_ERROR;
            ++iter;
            continue;
        }

        auto cleanupResult = HccpApi::RaDeregisterMr(rdmaHandle_, mr.handle);
        if (cleanupResult != 0) {
            GEN_LOG_ERROR("Unregister memory failed, result(" << cleanupResult << "), address(" << mr.address
                                                              << "), size(" << mr.size << ")");
            result = GEN_ERROR;
            ++iter;
            continue;
        }
        iter = pendingMrs_.erase(iter);
    }
    return result;
}

Result DeviceRdmaContext::TryDestroy() noexcept
{
    /* step1: release QPs before their memory regions and Rdev */
    auto result = DestroyAllQueues();
    if (result != GEN_OK) {
        return result;
    }
    result = UnRegisterAllMemory();
    if (result != GEN_OK) {
        return result;
    }

    /* step2: close Rdev; an error after the call does not make its old handle reusable */
    result = CloseRdmaDevice();
    if (rdmaHandle_ != nullptr) {
        return result;
    }

    /* step3: deinitialize RA before closing TSD */
    auto cleanupResult = DeinitRa();
    if (cleanupResult != GEN_OK) {
        return cleanupResult;
    }
    cleanupResult = CloseTsd();
    return cleanupResult != GEN_OK ? cleanupResult : result;
}

void DeviceRdmaContext::KeepPendingCleanup() noexcept
{
    GEN_ASSERT_MSG_NO_RETURN(cleanupContext_ != nullptr,
                             "Cleanup owner is null, userDeviceId(" << userDeviceId_ << ")");
    auto pending = cleanupContext_.release();
    pending->rdmaHandle_ = rdmaHandle_;
    pending->raInitialized_ = raInitialized_;
    pending->tsdOpened_ = tsdOpened_;
    pending->userDeviceId_ = userDeviceId_;
    pending->logicDeviceId_ = logicDeviceId_;
    pending->physicalDeviceId_ = physicalDeviceId_;
    pending->deviceIp_.swap(deviceIp_);
    pending->pendingMrs_.swap(pendingMrs_);
    pending->queues_.swap(queues_);
    pending->closing_ = true;

    std::lock_guard<std::mutex> lock(PENDING_CLEANUP_MUTEX);
    pending->pendingNext_ = PENDING_CLEANUP;
    PENDING_CLEANUP = pending;
    rdmaHandle_ = nullptr;
    raInitialized_ = false;
    tsdOpened_ = false;
    GEN_LOG_WARN("Retained pending device cleanup, userDeviceId(" << userDeviceId_ << ")");
}

Result DeviceRdmaContext::RetryPendingCleanup() noexcept
{
    std::lock_guard<std::mutex> lock(PENDING_CLEANUP_MUTEX);
    Result result = GEN_OK;
    auto entry = &PENDING_CLEANUP;
    while (*entry != nullptr) {
        auto context = *entry;
        auto cleanupResult = context->TryDestroy();
        if (cleanupResult != GEN_OK) {
            result = cleanupResult;
        }
        if (!context->queues_.empty() || !context->pendingMrs_.empty() || context->rdmaHandle_ != nullptr ||
            context->raInitialized_ || context->tsdOpened_) {
            entry = &context->pendingNext_;
            continue;
        }
        *entry = context->pendingNext_;
        delete context;
    }
    return result;
}

Result DeviceRdmaContext::SelectDevice(uint32_t userDeviceId) noexcept
{
    GEN_ASSERT_MSG_RETURN(userDeviceId <= static_cast<uint32_t>(INT32_MAX),
                          "Invalid user device ID(" << userDeviceId << ")", GEN_INVALID_PARAM);

    auto result = AclApi::AclrtSetDevice(static_cast<int32_t>(userDeviceId));
    GEN_ASSERT_MSG_RETURN(result == 0,
                          "Set device failed, result(" << result << "), userDeviceId(" << userDeviceId << ")",
                          GEN_DEVICE_ERROR);

    return QueryDeviceIds();
}

Result DeviceRdmaContext::QueryDeviceIds() noexcept
{
    int32_t userDeviceId = -1;
    int32_t logicDeviceId = -1;
    int32_t physicalDeviceId = -1;

    auto result = AclApi::AclrtGetDevice(&userDeviceId);
    GEN_ASSERT_MSG_RETURN(result == 0 && userDeviceId >= 0,
                          "Get user device ID failed, result(" << result << "), deviceId(" << userDeviceId << ")",
                          GEN_DEVICE_ERROR);

    result = AclApi::AclrtGetLogicDevIdByUserDevId(userDeviceId, &logicDeviceId);
    GEN_ASSERT_MSG_RETURN(result == 0 && logicDeviceId >= 0,
                          "Get logical device ID failed, result(" << result << "), userDeviceId(" << userDeviceId
                                                                  << "), logicDeviceId(" << logicDeviceId << ")",
                          GEN_DEVICE_ERROR);

    result = AclApi::AclrtGetPhyDevIdByUserDevId(userDeviceId, &physicalDeviceId);
    GEN_ASSERT_MSG_RETURN(result == 0 && physicalDeviceId >= 0,
                          "Get physical device ID failed, result(" << result << "), userDeviceId(" << userDeviceId
                                                                   << "), physicalDeviceId(" << physicalDeviceId << ")",
                          GEN_DEVICE_ERROR);

    userDeviceId_ = static_cast<uint32_t>(userDeviceId);
    logicDeviceId_ = static_cast<uint32_t>(logicDeviceId);
    physicalDeviceId_ = static_cast<uint32_t>(physicalDeviceId);
    return GEN_OK;
}

Result DeviceRdmaContext::QueryDeviceIp() noexcept
{
    /* step1: get the interface count */
    HccpRaGetIfAttr config{};
    config.phyId = physicalDeviceId_;
    config.nicPosition = HCCP_NETWORK_OFFLINE;
    config.isAll = true;
    uint32_t count = 0;
    auto result = HccpApi::RaGetIfNum(config, count);
    GEN_ASSERT_MSG_RETURN(result == 0 && count != 0,
                          "Get interface count failed, result(" << result << "), phyId(" << physicalDeviceId_
                                                                << "), count(" << count << ")",
                          GEN_DEVICE_ERROR);

    /* step2: get interface addresses */
    std::vector<HccpInterfaceInfo> infos(count);
    result = HccpApi::RaGetIfAddrs(config, infos.data(), count);
    GEN_ASSERT_MSG_RETURN(result == 0 && count <= infos.size(),
                          "Get interface addresses failed, result(" << result << "), phyId(" << physicalDeviceId_
                                                                    << "), count(" << count << "), capacity("
                                                                    << infos.size() << ")",
                          GEN_DEVICE_ERROR);

    /* step3: select the device IPv4 address, as in MemFabric */
    auto ifName = std::string("eth").append(std::to_string(physicalDeviceId_));
    for (uint32_t i = 0; i < count; ++i) {
        const auto &info = infos[i];
        if (info.family != AF_INET || strncmp(info.ifname, ifName.c_str(), sizeof(info.ifname)) != 0 ||
            info.ifaddr.ip.addr.s_addr == INADDR_ANY) {
            continue;
        }
        char address[INET_ADDRSTRLEN]{};
        GEN_ASSERT_MSG_RETURN(inet_ntop(AF_INET, &info.ifaddr.ip.addr, address, sizeof(address)) != nullptr,
                              "Convert device IP failed, phyId(" << physicalDeviceId_ << "), errno(" << errno << ")",
                              GEN_DEVICE_ERROR);
        deviceIp_ = address;
        GEN_LOG_DEBUG("Get device IP success, phyId(" << physicalDeviceId_ << "), ip(" << deviceIp_ << ")");
        return GEN_OK;
    }

    GEN_LOG_ERROR("Get device IP failed, as no IPv4 address found, phyId(" << physicalDeviceId_ << "), interface("
                                                                           << ifName << ")");
    return GEN_NOT_FOUND;
}

Result DeviceRdmaContext::OpenTsd() noexcept
{
    if (tsdOpened_) {
        return GEN_OK;
    }
    GEN_ASSERT_MSG_RETURN(userDeviceId_ != UINT32_MAX, "User device ID is not initialized", GEN_NOT_INITIALIZED);

    /* TSD uses the user device id in this initialization path. */
    auto result = TsdApi::TsdOpen(userDeviceId_, TSD_RDMA_RANK_SIZE);
    GEN_ASSERT_MSG_RETURN(result == 0,
                          "Open TSD failed, result(" << result << "), userDeviceId(" << userDeviceId_ << "), rankSize("
                                                     << TSD_RDMA_RANK_SIZE << ")",
                          GEN_DEVICE_ERROR);

    tsdOpened_ = true;
    return GEN_OK;
}

Result DeviceRdmaContext::CloseTsd() noexcept
{
    if (!tsdOpened_) {
        return GEN_OK;
    }

    auto result = TsdApi::TsdClose(userDeviceId_);
    GEN_ASSERT_MSG_RETURN(result == 0,
                          "Close TSD failed, result(" << result << "), userDeviceId(" << userDeviceId_ << ")",
                          GEN_DEVICE_ERROR);

    tsdOpened_ = false;
    return GEN_OK;
}

Result DeviceRdmaContext::InitRa() noexcept
{
    if (raInitialized_) {
        return GEN_OK;
    }
    GEN_ASSERT_MSG_RETURN(physicalDeviceId_ != UINT32_MAX, "Physical device ID is not initialized",
                          GEN_NOT_INITIALIZED);

    HccpRaInitConfig config{};
    config.phyId = physicalDeviceId_;
    config.nicPosition = HCCP_NETWORK_OFFLINE;
    config.hdcType = HCCP_HDC_TYPE_RDMA;
    config.enableHdcAsync = false;

    auto result = HccpApi::RaInit(config);
    GEN_ASSERT_MSG_RETURN(result == 0,
                          "Initialize RA failed, result(" << result << "), phyId(" << physicalDeviceId_ << ")",
                          GEN_DEVICE_ERROR);

    raInitialized_ = true;
    return GEN_OK;
}

Result DeviceRdmaContext::DeinitRa() noexcept
{
    if (!raInitialized_) {
        return GEN_OK;
    }

    HccpRaInitConfig config{};
    config.phyId = physicalDeviceId_;
    config.nicPosition = HCCP_NETWORK_OFFLINE;
    config.hdcType = HCCP_HDC_TYPE_RDMA;
    config.enableHdcAsync = false;

    auto result = HccpApi::RaDeinit(config);
    GEN_ASSERT_MSG_RETURN(result == 0,
                          "Deinitialize RA failed, result(" << result << "), phyId(" << physicalDeviceId_ << ")",
                          GEN_DEVICE_ERROR);

    raInitialized_ = false;
    return GEN_OK;
}

Result DeviceRdmaContext::OpenRdmaDevice() noexcept
{
    if (rdmaHandle_ != nullptr) {
        return GEN_OK;
    }
    GEN_ASSERT_MSG_RETURN(physicalDeviceId_ != UINT32_MAX, "Physical device ID is not initialized",
                          GEN_NOT_INITIALIZED);

    HccpRdev rdev{};
    rdev.phyId = physicalDeviceId_;
    rdev.family = AF_INET;
    GEN_ASSERT_MSG_RETURN(inet_pton(AF_INET, deviceIp_.c_str(), &rdev.localIp.addr) == 1,
                          "Invalid device RDMA IPv4 address(" << deviceIp_ << ")", GEN_INVALID_PARAM);

    HccpRdevInitInfo info{};
    info.mode = HCCP_NETWORK_OFFLINE;
    info.notifyType = HCCP_NOTIFY;
    info.enabled2mbLite = true;
    /* Poll CQ explicitly; prevent the background thread from consuming completions. */
    info.disabledLiteThread = true;

    void *rdmaHandle = nullptr;
    auto result = HccpApi::RaRdevInitV2(info, rdev, rdmaHandle);
    GEN_ASSERT_MSG_RETURN(result == 0 && rdmaHandle != nullptr,
                          "Open RDMA device failed, result(" << result << "), phyId(" << physicalDeviceId_ << "), ip("
                                                             << deviceIp_ << "), rdmaHandle(" << rdmaHandle << ")",
                          GEN_DEVICE_ERROR);

    rdmaHandle_ = rdmaHandle;
    return GEN_OK;
}

Result DeviceRdmaContext::CloseRdmaDevice() noexcept
{
    if (rdmaHandle_ == nullptr) {
        return GEN_OK;
    }

    auto handle = rdmaHandle_;
    auto result = HccpApi::RaRdevDeinit(rdmaHandle_, HCCP_NOTIFY);
    GEN_ASSERT_MSG_RETURN(result == 0,
                          "Close RDMA device failed, result(" << result << "), phyId(" << physicalDeviceId_ << "), ip("
                                                              << deviceIp_ << "), rdmaHandle(" << handle << ")",
                          GEN_DEVICE_ERROR);

    return GEN_OK;
}
} // namespace verbs
} // namespace openDCC
} // namespace ock
