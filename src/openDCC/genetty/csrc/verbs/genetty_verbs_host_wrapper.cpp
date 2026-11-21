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
#include "genetty_verbs_host_wrapper.h"

#include <arpa/inet.h>
#include <poll.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ock {
namespace openDCC {
namespace verbs {
namespace {
std::string FormatGid(const uint8_t *gid)
{
    char buf[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, gid, buf, sizeof(buf)) == nullptr) {
        return "invalid";
    }

    return buf;
}

std::string MakeDeviceKey(const VerbsDevicePtr &device)
{
    return device->DeviceName() + ":" + std::to_string(device->PortIndex());
}
} // namespace

/**
 * Functions for verbs device
 */
std::map<std::string, VerbsDevicePtr> VerbsDevice::ALL_DEVICES;
struct ibv_device **VerbsDevice::DEVICE_LIST = nullptr;
bool VerbsDevice::INITED = false;
std::mutex VerbsDevice::MUTEX;

Result VerbsDevice::Init() noexcept
{
    std::lock_guard<std::mutex> lock(MUTEX);
    if (INITED) {
        GEN_LOG_DEBUG("Already initialized");
        return GEN_OK;
    }

    auto result = InitInner();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize rdma devices failed, result(" << result << ")");
        return result;
    }

    INITED = true;
    GEN_LOG_DEBUG("Initialization finished, device count(" << ALL_DEVICES.size() << ")");
    return GEN_OK;
}

Result VerbsDevice::InitInner() noexcept
{
    auto result = VerbsApi::Load();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Load libibverbs failed, result(" << result << ")");
        return result;
    }

    int numDevices = 0;
    DEVICE_LIST = VerbsApi::ibv_get_device_list(&numDevices);
    if (DEVICE_LIST == nullptr) {
        GEN_LOG_ERROR("[IBV_API] ibv_get_device_list() failed, errno(" << errno << ")");
        return GEN_DEVICE_ERROR;
    }

    if (numDevices <= 0) {
        GEN_LOG_ERROR("[IBV_API] No rdma device found in the system, num devices(" << numDevices << ")");
        VerbsApi::ibv_free_device_list(DEVICE_LIST);
        DEVICE_LIST = nullptr;
        return GEN_DEVICE_ERROR;
    }

    GEN_LOG_DEBUG("Get device list successfully, there are " << numDevices << " RDMA devices");
    result = LoadDevices(DEVICE_LIST, numDevices);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Load rdma devices failed, device num(" << numDevices << "), result(" << result << ")");
        VerbsApi::ibv_free_device_list(DEVICE_LIST);
        DEVICE_LIST = nullptr;
        return result;
    }

    return GEN_OK;
}

Result VerbsDevice::LoadDevices(struct ibv_device **deviceList, int numDevices) noexcept
{
    uint32_t loaded = 0;
    for (int i = 0; i < numDevices; i++) {
        auto rawDevice = deviceList[i];
        if (UNLIKELY(rawDevice == nullptr)) {
            GEN_LOG_DEBUG("Invalid device in device list, index(" << i << ")");
            continue;
        }

        auto context = VerbsApi::ibv_open_device(rawDevice);
        if (context == nullptr) {
            GEN_LOG_WARN("[IBV_API] ibv_open_device(" << rawDevice->name << ") failed, errno(" << errno << ")");
            continue;
        }

        struct ibv_device_attr devAttr{};
        if (VerbsApi::ibv_query_device(context, &devAttr) != 0) {
            GEN_LOG_WARN("[IBV_API] ibv_query_device(" << rawDevice->name << ") failed, errno(" << errno << ")");
            VerbsApi::ibv_close_device(context);
            continue;
        }

        auto result = LoadPorts(context, rawDevice, static_cast<uint16_t>(i), devAttr);
        VerbsApi::ibv_close_device(context);
        if (result == GEN_OK) {
            loaded++;
        }
    }

    if (loaded == 0) {
        GEN_LOG_ERROR("No usable rdma port found, device num(" << numDevices << ")");
        return GEN_DEVICE_ERROR;
    }

    return GEN_OK;
}

Result VerbsDevice::LoadPorts(struct ibv_context *context, struct ibv_device *rawDevice, uint16_t devIndex,
                              const struct ibv_device_attr &devAttr) noexcept
{
    /* loop all port existed, create one device of each port */
    uint32_t loaded = 0;
    for (uint8_t port = 1; port <= devAttr.phys_port_cnt; port++) {
        struct ibv_port_attr portAttr{};
        auto compatAttr = reinterpret_cast<struct _compat_ibv_port_attr *>(&portAttr);
        if (VerbsApi::ibv_query_port(context, port, compatAttr) != 0) {
            GEN_LOG_WARN("[IBV_API] ibv_query_port(" << rawDevice->name << ", " << static_cast<uint32_t>(port)
                                                     << ") failed, errno(" << errno << ")");
            continue;
        }

        /* skip in-active device */
        if (portAttr.state != IBV_PORT_ACTIVE) {
            GEN_LOG_DEBUG("Skip inactive port " << rawDevice->name << ":" << static_cast<uint32_t>(port) << ", state("
                                                << PortState2Str(portAttr.state) << ")");
            continue;
        }

        std::string sysPath = std::string("/sys/class/infiniband/") + rawDevice->name;
        auto device = mf::MakeRef<VerbsDevice>(std::string(rawDevice->name), sysPath, port, devIndex, portAttr, devAttr,
                                               rawDevice);
        if (UNLIKELY(device == nullptr)) {
            GEN_LOG_ERROR("Create VerbsDevice object failed, probably out of memory, device(" << rawDevice->name
                                                                                              << ")");
            continue;
        }

        if (LoadGidList(context, *(device.Get())) != GEN_OK) {
            GEN_LOG_WARN("No gid loaded for device " << sysPath << ":" << static_cast<uint32_t>(port));
            continue;
        }

        /* add to map */
        std::string key = MakeDeviceKey(device);
        ALL_DEVICES[key] = device;
        loaded++;

        GEN_LOG_DEBUG("Add device(" << key << "), " << device->ToString());
    }

    if (loaded == 0) {
        GEN_LOG_ERROR("No usable port found on device(" << rawDevice->name << ")");
        return GEN_DEVICE_ERROR;
    }

    return GEN_OK;
}

Result VerbsDevice::LoadGidList(struct ibv_context *context, VerbsDevice &device) noexcept
{
    const auto &portAttr = device.PortAttributes();
    for (int32_t i = 0; i < portAttr.gid_tbl_len; i++) {
        union ibv_gid gid{};
        if (VerbsApi::ibv_query_gid(context, static_cast<uint8_t>(device.PortIndex()), i, &gid) != 0) {
            GEN_LOG_DEBUG("[IBV_API] ibv_query_gid(index(" << i << ")) failed, errno(" << errno << ")");
            continue;
        }

        VerbsGidInfo info;
        info.index = static_cast<uint16_t>(i);
        info.ibvGid = gid;
        info.version = ReadRoCEVersion(device.SysPath(), device.PortIndex(), info.index);
        info.ip = GidToIp(gid);
        device.AddGid(info);
    }

    if (device.GidList().empty()) {
        GEN_LOG_ERROR("No valid gid found, device(" << device.DeviceName() << "), port(" << device.PortIndex() << ")");
        return GEN_DEVICE_ERROR;
    }

    /* prefer a ip based roce v2 gid as the default one */
    const VerbsGidInfo *selected = &device.GidList().front();
    for (const auto &gid : device.GidList()) {
        if (gid.version == RoCE_V2 && !gid.ip.empty()) {
            selected = &gid;
            break;
        }
    }

    if (selected->ip.empty()) {
        /* fallback to any gid which carries an ip address */
        for (const auto &gid : device.GidList()) {
            if (!gid.ip.empty()) {
                selected = &gid;
                break;
            }
        }
    }

    device.gidIndex_ = selected->index;
    device.ibvGid_ = selected->ibvGid;
    device.version_ = selected->version;
    return GEN_OK;
}

RoCEVersion VerbsDevice::ReadRoCEVersion(const std::string &sysPath, uint16_t portIndex, uint16_t gidIndex) noexcept
{
    std::string path = sysPath + "/ports/" + std::to_string(portIndex) + "/gid_attrs/types/" + std::to_string(gidIndex);
    std::ifstream file(path);
    if (!file.is_open()) {
        GEN_LOG_DEBUG("Open gid type file failed, path(" << path << ")");
        return RoCE_UNKNOWN_VERSION;
    }

    std::string content;
    std::getline(file, content);
    if (content.find("RoCE v2") != std::string::npos) {
        return RoCE_V2;
    }

    if (content.find("RoCE v1.5") != std::string::npos) {
        return RoCE_V1_5;
    }

    if (content.find("RoCE v1") != std::string::npos) {
        return RoCE_V1;
    }

    GEN_LOG_DEBUG("Unknown gid type(" << content << "), path(" << path << ")");
    return RoCE_UNKNOWN_VERSION;
}

VerbsDevice::VerbsDevice(const std::string &name, const std::string &sysPath, uint16_t portIndex, uint16_t devIndex,
                         const struct ibv_port_attr &portAttr, const struct ibv_device_attr &devAttr,
                         struct ibv_device *rawDevice) noexcept
    : deviceName_(name), sysPath_(sysPath), portIndex_(portIndex), deviceId_(devIndex), portAttributes_(portAttr),
      devAttributes_(devAttr), rawDevice_(rawDevice)
{
    auto speed = SpeedToMb(portAttr.active_speed);
    auto factor = WidthToFactor(portAttr.active_width);
    uint32_t bandwidth = speed * factor / 1000; /* convert Mb/s to Gb/s */
    bandWidth_ = static_cast<uint8_t>(std::min<uint32_t>(bandwidth, UINT8_MAX));
}

uint32_t VerbsDevice::SpeedToMb(uint8_t activeSpeed) noexcept
{
    switch (activeSpeed) {
        case 1:
            return 2500; /* SDR */
        case 2:
            return 5000; /* DDR */
        case 4:
            return 10000; /* QDR */
        case 8:
            return 10000; /* FDR10 */
        case 16:
            return 14000; /* FDR */
        case 32:
            return 25000; /* EDR */
        case 64:
            return 50000; /* HDR */
        case 128:
            return 100000; /* NDR */
        default:
            break;
    }

    /* IB_SPEED_XDR(256) and beyond cannot be carried by 'active_speed' which is uint8_t in the uverbs ABI,
     * they are reported through 'active_speed_ex' which is not available on all kernels, so it is skipped here. */
    return 0;
}

std::string VerbsDevice::ToString(bool whole, const std::string &prefix, const std::string &seperator) const noexcept
{
    constexpr int width = 20;
    std::ostringstream oss;
    oss << prefix << std::left << std::setw(width) << "device name: " << deviceName_ << seperator;
    oss << prefix << std::left << std::setw(width) << "sys path: " << sysPath_ << seperator;
    oss << prefix << std::left << std::setw(width) << "port index: " << portIndex_ << seperator;
    oss << prefix << std::left << std::setw(width) << "state: " << PortState2Str(portAttributes_.state) << seperator;
    oss << prefix << std::left << std::setw(width) << "active mtu: " << PortMTU2Str(portAttributes_.active_mtu)
        << seperator;
    oss << prefix << std::left << std::setw(width) << "bandwidth: " << static_cast<uint32_t>(bandWidth_) << "Gb"
        << seperator;
    oss << prefix << std::left << std::setw(width) << "roce version: " << RoCEVersion2Str(version_) << seperator;
    oss << prefix << std::left << std::setw(width) << "gid index: " << gidIndex_ << seperator;
    oss << prefix << std::left << std::setw(width) << "gid: " << FormatGid(ibvGid_.raw) << seperator;

    if (!whole) {
        return oss.str();
    }

    oss << prefix << std::left << std::setw(width) << "max qp: " << devAttributes_.max_qp << seperator;
    oss << prefix << std::left << std::setw(width) << "max qp wr: " << devAttributes_.max_qp_wr << seperator;
    oss << prefix << std::left << std::setw(width) << "max cqe: " << devAttributes_.max_cqe << seperator;
    oss << prefix << std::left << std::setw(width) << "max mr: " << devAttributes_.max_mr << seperator;
    oss << prefix << std::left << std::setw(width) << "max pd: " << devAttributes_.max_pd << seperator;
    oss << prefix << std::left << std::setw(width) << "max sge: " << devAttributes_.max_sge << seperator;

    for (const auto &gid : gidList_) {
        oss << prefix << std::left << std::setw(width) << " - gid " << gid.index << ": " << FormatGid(gid.ibvGid.raw)
            << " (" << RoCEVersion2Str(gid.version) << ", ip: " << (gid.ip.empty() ? "none" : gid.ip) << ")"
            << seperator;
    }

    return oss.str();
}

/**
 * Functions for verbs context
 */
std::map<std::string, VerbsContextPtr> VerbsContext::ALL_CONTEXTS;
std::mutex VerbsContext::ALL_CONTEXTS_MUTEX;
std::atomic<uint32_t> VerbsContext::AUTO_INCREASE_CONTEXT_ID(0);

Result VerbsContext::CreateContext(const std::string &devName, VerbsContextPtr &out) noexcept
{
    GEN_LOG_DEBUG("enter dev(" << devName << ")");
    if (UNLIKELY(devName.empty())) {
        GEN_LOG_ERROR("Create verbs context failed, as param 'devName' is empty");
        return GEN_INVALID_PARAM;
    }

    VerbsDevicePtr device;
    auto result = VerbsDevice::FindDevice(devName, device);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Create verbs context failed, as device(" << devName << ") not found, result(" << result << ")");
        return result;
    }

    auto key = MakeDeviceKey(device);
    if (LookupContext(key, out) == GEN_OK) {
        return GEN_OK;
    }

    auto rawContext = VerbsApi::ibv_open_device(device->RawDevice());
    if (rawContext == nullptr) {
        GEN_LOG_ERROR("[IBV_API] ibv_open_device(" << key << ") failed, errno(" << errno << ")");
        return GEN_DEVICE_ERROR;
    }

    auto rawPd = VerbsApi::ibv_alloc_pd(rawContext);
    if (rawPd == nullptr) {
        GEN_LOG_ERROR("[IBV_API] ibv_alloc_pd(" << key << ") failed, errno(" << errno << ")");
        VerbsApi::ibv_close_device(rawContext);
        return GEN_DEVICE_ERROR;
    }

    auto context = mf::MakeRef<VerbsContext>(rawContext, rawPd, device);
    if (UNLIKELY(context == nullptr)) {
        GEN_LOG_ERROR("Create VerbsContext object failed, probably out of memory, device(" << key << ")");
        VerbsApi::ibv_dealloc_pd(rawPd);
        VerbsApi::ibv_close_device(rawContext);
        return GEN_MALLOC_FAILED;
    }

    context->contextId_ = AUTO_INCREASE_CONTEXT_ID.fetch_add(1);
    {
        std::lock_guard<std::mutex> guard(ALL_CONTEXTS_MUTEX);
        ALL_CONTEXTS[key] = context;
    }

    out = context;
    GEN_LOG_DEBUG("created context(" << context->contextId_ << ") with device(" << key << ")");
    return GEN_OK;
}

Result VerbsContext::CreateCq(uint32_t cqDepth, GenettyCQPollingType pollingType, VerbsCQPtr &out) noexcept
{
    GEN_LOG_DEBUG("enter, depth(" << cqDepth << "), polling type(" << static_cast<uint32_t>(pollingType) << ")");
    if (UNLIKELY(rawContext_ == nullptr)) {
        GEN_LOG_ERROR("Create cq failed, as context(" << contextId_ << ") is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(cqDepth == 0)) {
        GEN_LOG_ERROR("Create cq failed, as depth is 0, context(" << contextId_ << ")");
        return GEN_INVALID_PARAM;
    }

    struct ibv_comp_channel *channel = nullptr;
    if (pollingType == GenettyCQPollingType::GCQ_TYPE_EVENT) {
        channel = VerbsApi::ibv_create_comp_channel(rawContext_);
        if (channel == nullptr) {
            GEN_LOG_ERROR("[IBV_API] ibv_create_comp_channel failed, context(" << contextId_ << "), errno(" << errno
                                                                               << ")");
            return GEN_ERROR;
        }
    }

    auto rawCq = VerbsApi::ibv_create_cq(rawContext_, static_cast<int>(cqDepth), nullptr, channel, 0);
    if (rawCq == nullptr) {
        GEN_LOG_ERROR("[IBV_API] ibv_create_cq failed, depth(" << cqDepth << "), context(" << contextId_ << "), errno("
                                                               << errno << ")");
        if (channel != nullptr) {
            VerbsApi::ibv_destroy_comp_channel(channel);
        }
        return GEN_ERROR;
    }

    auto cq = mf::MakeRef<VerbsCQ>(rawCq, channel, pollingType, cqDepth, this);
    if (UNLIKELY(cq == nullptr)) {
        GEN_LOG_ERROR("Create VerbsCQ object failed, probably out of memory)");
        VerbsApi::ibv_destroy_cq(rawCq);
        if (channel != nullptr) {
            VerbsApi::ibv_destroy_comp_channel(channel);
        }
        return GEN_MALLOC_FAILED;
    }

    if (pollingType == GenettyCQPollingType::GCQ_TYPE_EVENT) {
        auto result = cq->Rearm(false);
        if (result != GEN_OK) {
            GEN_LOG_ERROR("Rearm cq failed after created, depth(" << cqDepth << "), context(" << contextId_ << ")");
            return result;
        }
    }

    out = cq;
    GEN_LOG_DEBUG("cq created, depth(" << cqDepth << ")");
    return GEN_OK;
}

Result VerbsContext::CreateQp(uint32_t sendDepth, uint32_t recvDepth, const VerbsCQPtr &sendCq,
                              const VerbsCQPtr &recvCq, VerbsQPPtr &out) noexcept
{
    if (UNLIKELY(rawPd_ == nullptr)) {
        GEN_LOG_ERROR("Create qp failed, as pd of context(" << contextId_ << ") is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(sendCq == nullptr || sendCq->Handle() == nullptr || recvCq == nullptr ||
                 recvCq->Handle() == nullptr)) {
        GEN_LOG_ERROR("Create qp failed, as send cq or recv cq is null, context(" << contextId_ << ")");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(sendDepth == 0 || recvDepth == 0)) {
        GEN_LOG_ERROR("Create qp failed, as send depth(" << sendDepth << ") or recv depth(" << recvDepth << ") is 0");
        return GEN_INVALID_PARAM;
    }

    struct ibv_qp_init_attr attr{};
    attr.send_cq = sendCq->Handle();
    attr.recv_cq = recvCq->Handle();
    attr.qp_type = IBV_QPT_RC;
    attr.sq_sig_all = 0;
    attr.cap.max_send_wr = sendDepth;
    attr.cap.max_recv_wr = recvDepth;
    attr.cap.max_send_sge = maxSGECount_;
    attr.cap.max_recv_sge = maxSGECount_;
    attr.cap.max_inline_data = maxInlineData_;

    auto rawQp = VerbsApi::ibv_create_qp(rawPd_, &attr);
    if (rawQp == nullptr) {
        GEN_LOG_ERROR("[IBV_API] ibv_create_qp failed, send depth(" << sendDepth << "), recv depth(" << recvDepth
                                                                    << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    auto qp = mf::MakeRef<VerbsQP>(rawQp, this, sendCq, recvCq, sendDepth, recvDepth);
    if (UNLIKELY(qp == nullptr)) {
        GEN_LOG_ERROR("Create VerbsQP object failed, probably out of memory, qp num(" << rawQp->qp_num << ")");
        VerbsApi::ibv_destroy_qp(rawQp);
        return GEN_MALLOC_FAILED;
    }

    out = qp;
    GEN_LOG_DEBUG("qp created, qp num(" << rawQp->qp_num << ")");
    return GEN_OK;
}

Result VerbsContext::RegisterMr(uint64_t address, uint64_t size, int access, VerbsMRPtr &out) noexcept
{
    if (UNLIKELY(rawPd_ == nullptr)) {
        GEN_LOG_ERROR("Register mr failed, as pd of context(" << contextId_ << ") is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(address == 0 || size == 0)) {
        GEN_LOG_ERROR("Register mr failed, invalid address(" << std::hex << address << std::dec << "), size(" << size
                                                             << ")");
        return GEN_INVALID_PARAM;
    }

    auto rawMr = VerbsApi::ibv_reg_mr(rawPd_, reinterpret_cast<void *>(address), size, access);
    if (rawMr == nullptr) {
        GEN_LOG_ERROR("[IBV_API] ibv_reg_mr failed, address(0x" << std::hex << address << std::dec << "), size(" << size
                                                                << "), access(" << access << "), errno(" << errno
                                                                << ")");
        return GEN_ERROR;
    }

    return NewMr(rawMr, address, size, out);
}

Result VerbsContext::RegisterMrIova(uint64_t address, uint64_t size, uint64_t iova, unsigned int access,
                                    VerbsMRPtr &out) noexcept
{
    if (UNLIKELY(rawPd_ == nullptr)) {
        GEN_LOG_ERROR("Register mr with iova failed, as pd of context(" << contextId_ << ") is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(address == 0 || size == 0)) {
        GEN_LOG_ERROR("Register mr with iova failed, invalid address(0x" << std::hex << address << std::dec
                                                                         << "), size(" << size << ")");
        return GEN_INVALID_PARAM;
    }

    auto rawMr = VerbsApi::ibv_reg_mr_iova2(rawPd_, reinterpret_cast<void *>(address), size, iova, access);
    if (rawMr == nullptr) {
        GEN_LOG_ERROR("[IBV_API] ibv_reg_mr_iova2 failed, address(0x" << std::hex << address << std::dec << "), iova(0x"
                                                                      << iova << "), size(" << size << "), errno("
                                                                      << errno << ")");
        return GEN_ERROR;
    }

    return NewMr(rawMr, address, size, out);
}

/**
 * Functions for verbs cq
 */
void VerbsCQ::Destroy() noexcept
{
    if (rawCq_ != nullptr) {
        if (LIKELY(VerbsApi::IsLoaded())) {
            if (compEvents_ > 0) {
                VerbsApi::ibv_ack_cq_events(rawCq_, compEvents_);
                compEvents_ = 0;
            }

            auto result = VerbsApi::ibv_destroy_cq(rawCq_);
            if (result != 0) {
                GEN_LOG_WARN("[IBV_API] ibv_destroy_cq failed, cq(" << static_cast<const void *>(rawCq_) << "), errno("
                                                                    << errno << ")");
            }
        }
        rawCq_ = nullptr;
    }

    if (rawChannel_ != nullptr) {
        if (LIKELY(VerbsApi::IsLoaded())) {
            auto result = VerbsApi::ibv_destroy_comp_channel(rawChannel_);
            if (result != 0) {
                GEN_LOG_WARN("[IBV_API] ibv_destroy_comp_channel failed, fd(" << rawChannel_->fd << "), errno(" << errno
                                                                              << ")");
            }
        }
        rawChannel_ = nullptr;
    }
}

Result VerbsCQ::EventWait(int timeoutMs) noexcept
{
    if (UNLIKELY(rawCq_ == nullptr || rawChannel_ == nullptr)) {
        GEN_LOG_ERROR("Wait cq event failed, as cq or comp channel is null");
        return GEN_ERROR;
    }

    struct pollfd pfd{};
    pfd.fd = rawChannel_->fd;
    pfd.events = POLLIN;

    int ret = 0;
    do {
        ret = poll(&pfd, 1, timeoutMs);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        GEN_LOG_ERROR("poll() on comp channel failed, fd(" << pfd.fd << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    if (ret == 0) {
        GEN_LOG_DEBUG("Wait cq event timeout, fd(" << pfd.fd << "), timeout(" << timeoutMs << ")");
        return GEN_ERROR;
    }

    if ((pfd.revents & POLLIN) == 0) {
        GEN_LOG_ERROR("Unexpected poll event on comp channel, fd(" << pfd.fd << "), revents(" << pfd.revents << ")");
        return GEN_ERROR;
    }

    struct ibv_cq *eventCq = nullptr;
    void *eventContext = nullptr;
    if (VerbsApi::ibv_get_cq_event(rawChannel_, &eventCq, &eventContext) != 0) {
        GEN_LOG_ERROR("[IBV_API] ibv_get_cq_event failed, fd(" << pfd.fd << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    compEvents_++;
    return GEN_OK;
}

/**
 * Functions for verbs qp
 */
Result VerbsQP::Init(uint8_t portNum) noexcept
{
    if (UNLIKELY(rawQp_ == nullptr)) {
        GEN_LOG_ERROR("Init qp failed, as qp is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(state_ != IBV_QPS_RESET)) {
        GEN_LOG_ERROR("Init qp failed, as qp(" << rawQp_->qp_num << ") is not in reset state, state(" << state_ << ")");
        return GEN_ERROR;
    }

    if (UNLIKELY(portNum == 0)) {
        GEN_LOG_ERROR("Init qp failed, as port num is 0, qp(" << rawQp_->qp_num << ")");
        return GEN_INVALID_PARAM;
    }

    auto result = ModifyToInit(portNum);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Init qp(" << rawQp_->qp_num << ") failed, port(" << static_cast<uint32_t>(portNum) << ")");
        return result;
    }

    portNum_ = portNum;
    state_ = IBV_QPS_INIT;
    GEN_LOG_DEBUG("qp(" << rawQp_->qp_num << ") moved to init state, port(" << static_cast<uint32_t>(portNum) << ")");
    return GEN_OK;
}

Result VerbsQP::Import(const VerbsQpInfo &remote, uint8_t trafficClass, uint8_t serviceLevel) noexcept
{
    GEN_LOG_DEBUG("enter, local qp(" << QpNumber() << "), remote(" << remote << ")");
    if (UNLIKELY(rawQp_ == nullptr)) {
        GEN_LOG_ERROR("Import qp failed, as qp is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(state_ != IBV_QPS_INIT)) {
        GEN_LOG_ERROR("Import qp failed, as qp(" << rawQp_->qp_num << ") is not in init state, state(" << state_
                                                 << ")");
        return GEN_ERROR;
    }

    if (UNLIKELY(remote.qpNumber == 0)) {
        GEN_LOG_ERROR("Import qp failed, as remote qp number is 0, local qp(" << rawQp_->qp_num << ")");
        return GEN_INVALID_PARAM;
    }

    trafficClass_ = trafficClass;
    serviceLevel_ = serviceLevel;
    auto result = ModifyToRtr(remote);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Import qp failed when modify to rtr, local qp(" << rawQp_->qp_num << "), remote qp("
                                                                       << remote.qpNumber << ")");
        return result;
    }
    state_ = IBV_QPS_RTR;

    result = ModifyToRts();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Import qp failed when modify to rts, local qp(" << rawQp_->qp_num << "), remote qp("
                                                                       << remote.qpNumber << ")");
        return result;
    }

    state_ = IBV_QPS_RTS;
    GEN_LOG_DEBUG("qp(" << rawQp_->qp_num << ") Imported, remote qp(" << remote.qpNumber << ")");
    return GEN_OK;
}

Result VerbsQP::ModifyToInit(uint8_t portNum) noexcept
{
    struct ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = portNum;
    attr.qp_access_flags =
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;

    const int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (VerbsApi::ibv_modify_qp(rawQp_, &attr, mask) != 0) {
        GEN_LOG_ERROR("[IBV_API] ibv_modify_qp to init failed, qp("
                      << QpNumber() << "), port(" << static_cast<uint32_t>(portNum) << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    return GEN_OK;
}

Result VerbsQP::ModifyToRtr(const VerbsQpInfo &remote) noexcept
{
    struct ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = static_cast<ibv_mtu>(remote.mtu);
    attr.dest_qp_num = remote.qpNumber;
    attr.rq_psn = remote.psn;
    attr.max_dest_rd_atomic = maxRdAtomic_;
    attr.min_rnr_timer = VERBS_DEFAULT_MIN_RNR_TIMER;
    attr.ah_attr.dlid = remote.lid;
    attr.ah_attr.sl = serviceLevel_;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = portNum_;
    attr.ah_attr.is_global = 0;

    bool hasGid = false;
    for (auto value : remote.gid) {
        if (value != 0) {
            hasGid = true;
            break;
        }
    }

    if (hasGid) {
        memcpy(attr.ah_attr.grh.dgid.raw, remote.gid, sizeof(attr.ah_attr.grh.dgid.raw));
        attr.ah_attr.grh.sgid_index = static_cast<uint8_t>(context_->Device()->GidIndex());
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 64;
        attr.ah_attr.grh.traffic_class = trafficClass_;
        attr.ah_attr.is_global = 1;
    }

    const int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                     IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (VerbsApi::ibv_modify_qp(rawQp_, &attr, mask) != 0) {
        GEN_LOG_ERROR("[IBV_API] ibv_modify_qp to rtr failed, qp(" << QpNumber() << "), remote qp(" << remote.qpNumber
                                                                   << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    return GEN_OK;
}

Result VerbsQP::ModifyToRts() noexcept
{
    struct ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = VERBS_DEFAULT_TIMEOUT;
    attr.retry_cnt = VERBS_DEFAULT_RETRY_CNT;
    attr.rnr_retry = VERBS_DEFAULT_RNR_RETRY;
    attr.sq_psn = psn_;
    attr.max_rd_atomic = maxRdAtomic_;

    const int mask =
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (VerbsApi::ibv_modify_qp(rawQp_, &attr, mask) != 0) {
        GEN_LOG_ERROR("[IBV_API] ibv_modify_qp to rts failed, qp(" << QpNumber() << "), errno(" << errno << ")");
        return GEN_ERROR;
    }

    return GEN_OK;
}

/**
 * Functions for verbs qp info
 */
std::ostream &operator<<(std::ostream &os, const VerbsQpInfo &info)
{
    os << "qp info [qp num: " << info.qpNumber << ", lid: " << info.lid
       << ", port: " << static_cast<uint32_t>(info.portNum) << ", gid: " << FormatGid(info.gid) << "]";
    return os;
}
} // namespace verbs
} // namespace openDCC
} // namespace ock
