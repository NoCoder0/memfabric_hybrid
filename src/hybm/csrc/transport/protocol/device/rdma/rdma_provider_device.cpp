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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "device_chip_info.h"
#include "dl_acl_api.h"
#include "dl_hccp_api.h"
#include "dl_hccl_api.h"
#include "dl_rt_api.h"
#include "hybm_gva.h"
#include "hybm_logger.h"
#include "hybm_networks_common.h"
#include "hybm_ptracer.h"
#include "hybm_va_manager.h"
#include "mf_env_util.h"
#include "mf_scope_guard.h"
#include "provider_control_io.h"
#include "rdma_provider_completion.h"
#include "rdma_provider_device.h"
#include "rdma_provider_link.h"
#include "device_rdma_transport_manager.h"
#include "dl_hal_api.h"
#include "joinable_ranks_qp_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {

constexpr uint32_t PROVIDER_MR_ACCESS = 7;
constexpr uint32_t QP_MODE = 4;
constexpr int HDC_TYPE_RDMA = 6;
constexpr int HDC_TYPE_RDMA_V2 = 18;

// The runtime APIs act on the current device. RdmaProviderDeviceContext shares the
// service between users of that device; different devices must each open it.
bool OpenNetService18()
{
    std::string ext = "--hdcType=18";
    RtProcExtParam param{ext.c_str(), ext.size()};
    RtNetServiceOpenArgs args{&param, 1};
    auto ret = DlRtApi::RtOpenNetService(&args);
    if (ret != 0) {
        BM_LOG_WARN("rtOpenNetService failed, hdcType=18 ret=" << ret);
        return false;
    }
    return true;
}

bool CloseNetService18()
{
    auto ret = DlRtApi::RtCloseNetService();
    if (ret != 0) {
        BM_LOG_ERROR("rtCloseNetService failed, ret=" << ret);
    }
    return ret == BM_OK;
}

void FillTypicalQp(const QpAttr &attr, TypicalQp &qp, const QosAttr &qos)
{
    qp = TypicalQp{};
    qp.qpn = attr.qpn;
    qp.psn = attr.psn;
    qp.gidIdx = attr.gidIdx;
    std::memcpy(qp.gid, attr.gid, HCCP_GID_RAW_LEN);
    qp.tc = qos.tc;
    qp.sl = qos.sl;
    qp.retryCnt = DEFAULT_RETRY_CNT;
    qp.retryTime = DEFAULT_RETRY_TIME;
}
} // namespace

bool RdmaProviderLinkReady(const RdmaProviderDevice &device, void *qp, int fd)
{
    // Nothing may arrive on an established control link; a pending frame or a closed socket both mean the
    // link is unusable. SocketIdle() states exactly that, so the shared probe is used instead of a local one.
    if (!provider_control::SocketIdle(fd)) {
        return false;
    }
    if (DlAclApi::AclrtSetDevice(static_cast<int32_t>(device.userId)) != BM_OK) {
        return false;
    }
    int status = -1;
    const auto ret = DlHccpApi::RaGetQpStatus(qp, status);
    return ret == BM_OK && status == 1;
}

RdmaProviderDeviceContext::~RdmaProviderDeviceContext()
{
    if (!CloseCompletionResources()) {
        return;
    }
    auto bindRet = DlAclApi::AclrtSetDevice(static_cast<int32_t>(userId));
    const bool deviceBound = bindRet == BM_OK;
    if (!deviceBound) {
        BM_LOG_ERROR("provider AclrtSetDevice failed during cleanup, deviceId=" << userId << " ret=" << bindRet);
    }
    // Do not run current-device runtime APIs against another live card if the switch failed.
    if (stream != nullptr && deviceBound) {
        auto ret = DlRtApi::RtStreamDestroy(stream);
        if (ret != 0) {
            BM_LOG_ERROR("provider rtStreamDestroy failed, deviceId=" << userId << " ret=" << ret);
        }
    }
    if (ownsRdev) {
        auto ret = DlHccpApi::RaRdevDeinit(rdmaHandle, 1);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider RaRdevDeinit failed, phyId=" << phyId << " ret=" << ret);
        }
    }
    if (raInitialized) {
        auto ret = DlHccpApi::RaDeinit(init);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider RaDeinit failed, phyId=" << phyId << " ret=" << ret);
        }
    }
    if (netServiceOwned && deviceBound) {
        CloseNetService18();
    }
}

bool RdmaProviderDeviceContext::OpenStream()
{
    auto ret = DlRtApi::RtStreamCreate(&stream, 0);
    if (ret != 0) {
        BM_LOG_ERROR("provider rtStreamCreate failed, deviceId=" << userId << " ret=" << ret);
    }
    return ret == 0;
}

bool RdmaProviderDeviceContext::CloseCompletionResources()
{
    if (!completions.empty()) {
        BM_LOG_ERROR("provider has pending QP completion resources, deviceId=" << userId);
        return false;
    }
    if (notifySource != nullptr && !notifySource->Close()) {
        return false;
    }
    notifySource.reset();
    return true;
}

namespace {
std::mutex contextsMutex;
std::unordered_map<uint32_t, std::shared_ptr<RdmaProviderDeviceContext>> contexts;
} // namespace

RdmaProviderDevice::~RdmaProviderDevice()
{
    Close();
}

void RdmaProviderDevice::Close()
{
    if (cleanupBlocked_) {
        return;
    }
    UnregisterMemory();
    if (mrHandle != nullptr || hostAddr != nullptr) {
        cleanupBlocked_ = true;
        return;
    }
    std::lock_guard<std::mutex> lock(contextsMutex);
    if (context != nullptr) {
        auto found = contexts.find(userId);
        if (found != contexts.end() && found->second == context && context.use_count() == 2) {
            if (!context->CloseCompletionResources()) {
                cleanupBlocked_ = true;
                return;
            }
            contexts.erase(found);
        }
        // Actual destruction is inside the same lock as acquire/create.
        context.reset();
    }
    rdmaHandle = nullptr;
}

bool RdmaProviderDevice::Open(uint32_t userDeviceId, uint32_t rankCount)
{
    Close();
    if (context != nullptr) {
        BM_LOG_ERROR("provider open blocked by pending cleanup, deviceId=" << userId);
        return false;
    }
    std::lock_guard<std::mutex> lock(contextsMutex);
    auto found = contexts.find(userDeviceId);
    auto selected = found == contexts.end() ? nullptr : found->second;
    if (selected == nullptr) {
        selected = std::make_shared<RdmaProviderDeviceContext>();
        if (!selected->Open(userDeviceId, rankCount)) {
            return false;
        }
        contexts[userDeviceId] = selected;
    }
    if (selected->completionCleanupBlocked.load()) {
        BM_LOG_ERROR("provider open blocked by shared completion cleanup, deviceId=" << userDeviceId);
        return false;
    }
    context = std::move(selected);
    userId = context->userId;
    logicId = context->logicId;
    phyId = context->phyId;
    rdmaHandle = context->rdmaHandle;
    return true;
}

bool RdmaProviderDeviceContext::Open(uint32_t userDeviceId, uint32_t rankCount)
{
    if (DlRtApi::LoadLibrary() != BM_OK) {
        BM_LOG_ERROR("provider runtime library could not be loaded, deviceId=" << userDeviceId);
        return false;
    }
    userId = userDeviceId;
    if (DlAclApi::AclrtSetDevice(static_cast<int32_t>(userDeviceId)) != BM_OK) {
        BM_LOG_ERROR("provider aclrtSetDevice failed, deviceId=" << userDeviceId);
        return false;
    }
    int32_t logic = -1;
    if (DlAclApi::RtGetLogicDevIdByUserDevId(static_cast<int32_t>(userDeviceId), &logic) != BM_OK || logic < 0) {
        BM_LOG_ERROR("provider get logic id failed, deviceId=" << userDeviceId);
        return false;
    }
    logicId = static_cast<uint32_t>(logic);
    int32_t phy = logic;
    (void)DlAclApi::AclrtGetPhyDevIdByLogicDevId(static_cast<int32_t>(userDeviceId), &phy);
    phyId = static_cast<uint32_t>(phy);

    if (DlHccpApi::RaRdevGetHandle(phyId, rdmaHandle) == BM_OK && rdmaHandle != nullptr) {
        return OpenStream();
    }

    uint32_t count = 0;
    HccpRaGetIfAttr ifAttr{};
    ifAttr.phyId = phyId;
    ifAttr.nicPosition = NETWORK_OFFLINE;
    ifAttr.isAll = true;
    auto ret = DlHccpApi::RaGetIfNum(ifAttr, count);
    if (ret != BM_OK || count == 0) {
        int hdcType = HDC_TYPE_RDMA_V2;
        netServiceOwned = OpenNetService18();
        if (!netServiceOwned) {
            if (DlHccpApi::TsdOpen(logicId, rankCount) != 0) {
                BM_LOG_ERROR("provider TsdOpen failed, logicId=" << logicId);
                return false;
            }
            hdcType = HDC_TYPE_RDMA;
        }
        init = {};
        init.phyId = phyId;
        init.nicPosition = NETWORK_OFFLINE;
        init.hdcType = hdcType;
        ret = DlHccpApi::RaInit(init);
        if (ret != BM_OK && hdcType == HDC_TYPE_RDMA_V2) {
            BM_LOG_WARN("provider RaInit failed, phyId=" << phyId << " hdcType=18 ret=" << ret);
            // The runtime service fixes the HDC type. Release 18 before opening the legacy service.
            if (!CloseNetService18()) {
                return false;
            }
            netServiceOwned = false;
            if (DlHccpApi::TsdOpen(logicId, rankCount) != BM_OK) {
                BM_LOG_ERROR("provider TsdOpen before hdcType=6 retry failed, logicId=" << logicId);
                return false;
            }
            init.hdcType = HDC_TYPE_RDMA;
            ret = DlHccpApi::RaInit(init);
        }
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider RaInit failed, phyId=" << phyId << " ret=" << ret);
            return false;
        }
        raInitialized = true;
        count = 0;
        ret = DlHccpApi::RaGetIfNum(ifAttr, count);
        if (ret != BM_OK || count == 0) {
            BM_LOG_ERROR("provider get if num failed, phyId=" << phyId << " ret=" << ret << " count=" << count);
            return false;
        }
    }
    std::vector<HccpInterfaceInfo> infos(count);
    if (DlHccpApi::RaGetIfAddrs(ifAttr, infos.data(), count) != 0) {
        BM_LOG_ERROR("provider get if addrs failed, phyId=" << phyId);
        return false;
    }
    const std::string ifName = "eth" + std::to_string(phyId);
    for (auto &info : infos) {
        if (info.family == AF_INET && ifName == info.ifname) {
            deviceIp = info.ifaddr.ip.addr.s_addr;
            break;
        }
    }
    if (deviceIp == 0) {
        BM_LOG_ERROR("provider device ip not found, phyId=" << phyId);
        return false;
    }

    if (!raInitialized) {
        in_addr ip{deviceIp};
        if (!RdmaTransportManager::GetRdmaHandleAfterInitHccl(phyId, ip, rdmaHandle) || rdmaHandle == nullptr) {
            BM_LOG_ERROR("provider HCCL bootstrap did not provide an RDMA handle, phyId=" << phyId);
            return false;
        }
        // HCCL owns this handle and its RA context; only our stream is released on Close.
        return OpenStream();
    }

    HccpRdevInitInfo rdevInfo{};
    rdevInfo.mode = NETWORK_OFFLINE;
    rdevInfo.notifyType = 1;
    rdevInfo.enabled2mbLite = true;
    // The lite thread consumes CQEs; transfers wait on their private notifications.
    rdevInfo.disabledLiteThread = false;
    HccpRdev rdev{};
    rdev.phyId = phyId;
    rdev.family = AF_INET;
    rdev.localIp.addr.s_addr = deviceIp;
    ret = DlHccpApi::RaRdevInitV2(rdevInfo, rdev, rdmaHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider RaRdevInitV2 failed, phyId=" << phyId << " ret=" << ret);
        return false;
    }
    ownsRdev = true;
    BM_LOG_INFO("provider device opened, user=" << userId << " logic=" << logicId << " phy=" << phyId
                                                << " ip=" << inet_ntoa(*reinterpret_cast<in_addr *>(&deviceIp))
                                                << " handle=" << rdmaHandle);
    return OpenStream();
}

bool RdmaProviderDevice::RegisterMr(void *addr, uint64_t size, bool isHbm, HccpMrInfo &info, void *&mrHandle)
{
    if (addr == nullptr || size == 0) {
        BM_LOG_ERROR("provider invalid memory registration, addr=" << addr << " size=" << size);
        return false;
    }
    if (context == nullptr || rdmaHandle == nullptr) {
        BM_LOG_ERROR("provider MR registration requires an open device, deviceId=" << userId);
        return false;
    }
    if (DlAclApi::AclrtSetDevice(static_cast<int32_t>(userId)) != BM_OK) {
        BM_LOG_ERROR("provider set device for MR failed, deviceId=" << userId);
        return false;
    }
    void *deviceAddr = addr;
    if (!isHbm) {
        if (DlHalApi::HalHostRegister(addr, size, HOST_MEM_MAP_DEV, logicId, &deviceAddr) != BM_OK) {
            BM_LOG_ERROR("provider HalHostRegister failed, addr=" << addr << " size=" << size);
            return false;
        }
    }
    info = HccpMrInfo{};
    info.addr = deviceAddr;
    info.size = size;
    info.access = PROVIDER_MR_ACCESS;
    mrHandle = nullptr;
    auto ret = DlHccpApi::RaRegisterMR(rdmaHandle, &info, mrHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider RaRegisterMR failed, addr=" << deviceAddr << " size=" << size << " ret=" << ret);
        if (!isHbm) {
            auto unmapRet = DlHalApi::HalHostUnregisterEx(addr, logicId, HOST_MEM_MAP_DEV);
            if (unmapRet != BM_OK) {
                BM_LOG_ERROR("provider MR rollback HalHostUnregisterEx failed, logicId=" << logicId
                                                                                         << " ret=" << unmapRet);
                cleanupBlocked_ = true;
            }
        }
        return false;
    }
    BM_LOG_INFO("MR registered, phy=" << phyId << " hbm=" << isHbm << " addr=" << deviceAddr << " size=" << size
                                      << " lkey=" << info.lkey << " rkey=" << info.rkey);
    return true;
}

bool ReleaseRegisteredMr(RdmaProviderDevice &device, void *&handle, void *hostAddress, bool isHbm)
{
    auto ret = DlAclApi::AclrtSetDevice(static_cast<int32_t>(device.userId));
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider set device for MR release failed, deviceId=" << device.userId << " ret=" << ret);
        return false;
    }
    if (handle != nullptr) {
        ret = DlHccpApi::RaDeregisterMR(device.rdmaHandle, handle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider RaDeregisterMR failed, phyId=" << device.phyId << " ret=" << ret);
            return false;
        }
        handle = nullptr;
    }
    if (!isHbm && hostAddress != nullptr) {
        ret = DlHalApi::HalHostUnregisterEx(hostAddress, device.logicId, HOST_MEM_MAP_DEV);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider HalHostUnregisterEx failed, logicId=" << device.logicId << " ret=" << ret);
            return false;
        }
    }
    return true;
}

bool RdmaProviderDevice::RegisterMemory(void *hostAddr, uint64_t size)
{
    if (mrHandle != nullptr || this->hostAddr != nullptr) {
        BM_LOG_ERROR("provider memory is still registered or pending cleanup, deviceId=" << userId);
        return false;
    }
    if (!RegisterMr(hostAddr, size, false, mrInfo, mrHandle)) {
        return false;
    }
    this->hostAddr = hostAddr;
    dvaBase = mrInfo.addr;
    mrSize = size;
    return true;
}

void RdmaProviderDevice::UnregisterMemory()
{
    if ((mrHandle != nullptr || hostAddr != nullptr) && !ReleaseRegisteredMr(*this, mrHandle, hostAddr, false)) {
        return;
    }
    dvaBase = nullptr;
    hostAddr = nullptr;
    mrSize = 0;
}

bool RdmaProviderDevice::CreateQp(void *&qpHandle, TypicalQp &localQp)
{
    std::lock_guard<std::mutex> guard(qpMutex);
    qpHandle = nullptr;
    if (cleanupBlocked_) {
        BM_LOG_ERROR("provider QP creation blocked by pending cleanup, deviceId=" << userId);
        return false;
    }
    std::unique_lock<std::mutex> contextLock;
    if (context != nullptr) {
        contextLock = std::unique_lock<std::mutex>(context->transferMutex);
        if (context->completionCleanupBlocked.load()) {
            BM_LOG_ERROR("provider QP creation blocked by shared completion cleanup, deviceId=" << userId);
            return false;
        }
    }
    const auto ret = DlAclApi::AclrtSetDevice(static_cast<int32_t>(userId));
    if (rdmaHandle == nullptr || ret != BM_OK) {
        BM_LOG_ERROR("provider QP creation requires a bound device, deviceId=" << userId << " ret=" << ret);
        return false;
    }
    const auto qos = GetDeviceRdmaQos();
    if (DlHccpApi::RaQpCreate(rdmaHandle, 0, QP_MODE, qpHandle) != BM_OK || qpHandle == nullptr) {
        BM_LOG_ERROR("provider RaQpCreate failed, phy=" << phyId);
        return false;
    }
    QpAttr attr{};
    auto attrRet = DlHccpApi::RaGetQpAttr(qpHandle, attr);
    if (attrRet != BM_OK) {
        BM_LOG_ERROR("provider RaGetQpAttr failed, phyId=" << phyId << " ret=" << attrRet);
        if (contextLock.owns_lock()) {
            contextLock.unlock();
        }
        DestroyQp(qpHandle);
        qpHandle = nullptr;
        return false;
    }
    FillTypicalQp(attr, localQp, qos);
    return true;
}

bool RdmaProviderDevice::ModifyQp(void *qpHandle, TypicalQp &localQp, TypicalQp &remoteQp)
{
    if (qpHandle == nullptr) {
        BM_LOG_ERROR("provider QP modification requires a QP, phyId=" << phyId);
        return false;
    }
    auto ret = DlHccpApi::RaTypicalQpModify(qpHandle, localQp, remoteQp);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider RaTypicalQpModify failed, phy=" << phyId << " ret=" << ret);
        return false;
    }
    return true;
}

bool RdmaProviderDevice::UsesNotifyCompletion() const
{
    return context != nullptr;
}

bool RdmaProviderDevice::GetNotifySource(uint64_t &addr, uint32_t &rkey)
{
    if (context == nullptr) {
        BM_LOG_ERROR("provider notification source requires an open device, deviceId=" << userId);
        return false;
    }
    std::lock_guard<std::mutex> lock(context->transferMutex);
    if (context->notifySource == nullptr) {
        context->notifySource = std::make_unique<RdmaProviderSignal>(userId, rdmaHandle);
    }
    if (!context->notifySource->Init()) {
        return false;
    }
    addr = context->notifySource->GetAddress();
    rkey = context->notifySource->GetRkey();
    return true;
}

bool RdmaProviderDevice::SubmitTransfer(void *qpHandle, uint64_t localDva, uint32_t lkey, uint64_t remoteAddr,
                                        uint32_t rkey, uint64_t size, bool write)
{
    if (qpHandle == nullptr || context == nullptr || size > std::numeric_limits<uint32_t>::max()) {
        BM_LOG_ERROR("provider invalid transfer, phyId=" << phyId << " size=" << size);
        return false;
    }
    std::lock_guard<std::mutex> lock(context->transferMutex);
    if (DlAclApi::AclrtSetDevice(static_cast<int32_t>(userId)) != BM_OK) {
        BM_LOG_ERROR("provider set device for transfer failed, deviceId=" << userId);
        return false;
    }
    RdmaProviderCompletion *completion = nullptr;
    auto found = context->completions.find(qpHandle);
    if (found == context->completions.end()) {
        const bool nativeStream = context->ownsRdev && (DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_910B ||
                                                        DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_910C);
        auto entry = std::make_unique<RdmaProviderCompletion>(userId, logicId, rdmaHandle, nativeStream);
        completion = entry.get();
        context->completions.emplace(qpHandle, std::move(entry));
        if (!completion->Init()) {
            return false;
        }
    } else {
        completion = found->second.get();
    }
    sg_list sg{};
    sg.addr = localDva;
    sg.len = static_cast<uint32_t>(size);
    sg.lkey = lkey;
    send_wr wr{};
    wr.buf_list = &sg;
    wr.buf_num = 1;
    wr.dst_addr = remoteAddr;
    wr.rkey = rkey;
    wr.op = write ? 0U : 4U;
    wr.send_flag = RA_SEND_SIGNALED;
    send_wr_rsp rsp{};
    auto sendRet = DlHccpApi::RaTypicalSendWr(qpHandle, wr, rsp);
    if (sendRet != BM_OK) {
        BM_LOG_ERROR("provider RaTypicalSendWr failed, phyId=" << phyId << " write=" << write << " ret=" << sendRet);
        return false;
    }
    if (completion->UsesNativeStream()) {
        return completion->SubmitNativeDoorbell(rsp);
    }
    auto ret = DlRtApi::RtRDMADBSend(rsp.db.db_index, rsp.db.db_info, context->stream);
    if (ret != 0) {
        BM_LOG_ERROR("provider rtRDMADBSend failed, ret=" << ret);
        return false;
    }
    return true;
}

bool RdmaProviderDevice::WaitTransfer(void *qpHandle, uint64_t notifySourceAddr, uint32_t notifySourceRkey)
{
    if (qpHandle == nullptr || context == nullptr) {
        BM_LOG_ERROR("provider completion requires an established transfer, phyId=" << phyId);
        return false;
    }
    if (notifySourceAddr == 0 || notifySourceRkey == 0) {
        BM_LOG_ERROR("provider notify completion source is missing, phyId=" << phyId);
        return false;
    }
    RdmaProviderCompletion *completion = nullptr;
    {
        std::lock_guard<std::mutex> lock(context->transferMutex);
        if (DlAclApi::AclrtSetDevice(static_cast<int32_t>(userId)) != BM_OK) {
            BM_LOG_ERROR("provider set device for completion failed, deviceId=" << userId);
            return false;
        }
        auto found = context->completions.find(qpHandle);
        if (found == context->completions.end() || found->second == nullptr) {
            BM_LOG_ERROR("provider completion not submitted for qp, phyId=" << phyId);
            return false;
        }
        completion = found->second.get();
        if (!completion->UsesNativeStream()) {
            // 数据 WR 的 doorbell 走运行时 stream，先把它落盘再发 fence 读。
            auto ret = DlRtApi::RtStreamSynchronize(context->stream);
            if (ret != 0) {
                BM_LOG_ERROR("provider rtStreamSynchronize failed, phyId=" << phyId << " ret=" << ret);
                return false;
            }
        }
        if (!completion->SubmitFenceRead(qpHandle, notifySourceAddr, notifySourceRkey)) {
            return false;
        }
    }
    // notify 等待在锁外进行：不同 QP 的等待互不阻塞。
    return completion->WaitFenceNotify();
}

bool RdmaProviderDevice::TryDestroyQp(void *&qpHandle)
{
    if (qpHandle == nullptr) {
        return true;
    }
    std::unique_lock<std::mutex> contextLock;
    if (context != nullptr) {
        contextLock = std::unique_lock<std::mutex>(context->transferMutex);
    }
    auto retiredQp = qpHandle;
    auto ret = DlHccpApi::RaQpDestroy(qpHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider RaQpDestroy failed, phyId=" << phyId << " qp=" << qpHandle << " ret=" << ret);
        return false;
    }
    qpHandle = nullptr;
    if (context != nullptr) {
        auto found = context->completions.find(retiredQp);
        if (found != context->completions.end()) {
            if (!found->second->Close()) {
                context->completionCleanupBlocked.store(true);
                cleanupBlocked_ = true;
                return false;
            }
            context->completions.erase(found);
        }
    }
    return true;
}

void RdmaProviderDevice::DestroyQp(void *qpHandle)
{
    if (!TryDestroyQp(qpHandle)) {
        cleanupBlocked_ = true;
    }
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
