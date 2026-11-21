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
#ifndef MF_OPENDCC_GENETTY_VERBS_WRAPPER_H
#define MF_OPENDCC_GENETTY_VERBS_WRAPPER_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/genetty_common_includes.h"
#include "genetty_types.h"
#include "genetty_verbs_types.h"
#include "under_api/verbs/genetty_dl_verbs_api.h"

namespace ock {
namespace openDCC {
namespace verbs {
class VerbsDevice;
class VerbsContext;
class VerbsQP;
class VerbsCQ;
class VerbsMR;
using VerbsDevicePtr = Ref<VerbsDevice>;
using VerbsContextPtr = Ref<VerbsContext>;
using VerbsQPPtr = Ref<VerbsQP>;
using VerbsCQPtr = Ref<VerbsCQ>;
using VerbsMRPtr = Ref<VerbsMR>;

constexpr uint32_t IBV_SYS_PATH_LEN = 512;
constexpr uint32_t VERBS_DEFAULT_CQ_DEPTH = 4096;
constexpr uint32_t VERBS_DEFAULT_SEND_QUEUE_DEPTH = 128;
constexpr uint32_t VERBS_DEFAULT_RECV_QUEUE_DEPTH = 128;
constexpr uint8_t VERBS_DEFAULT_RNR_RETRY = 6;      /* rnr retry count */
constexpr uint8_t VERBS_DEFAULT_TIMEOUT = 14;       /* local ack timeout, 4.096us * 2^14 */
constexpr uint8_t VERBS_DEFAULT_RETRY_CNT = 6;      /* retry count of the transport error */
constexpr uint8_t VERBS_DEFAULT_MIN_RNR_TIMER = 12; /* min rnr timer of the remote side, 0.01ms * 2^12 */
constexpr uint8_t VERBS_MAX_RD_ATOMIC = 16;         /* max outstanding rdma read/atomic */

/* prefix of a ipv4 mapped ipv6 address, that is ::ffff:a.b.c.d */
constexpr uint8_t IPV4_MAPPED_PREFIX[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
constexpr uint8_t IPV4_MAPPED_OFFSET = 12;

enum RoCEVersion : uint8_t {
    RoCE_V1 = 0,
    RoCE_V1_5,
    RoCE_V2,

    RoCE_UNKNOWN_VERSION,
};

/* one gid entry of a device port */
struct VerbsGidInfo {
    uint16_t index = 0;                         /* index in the gid table */
    union ibv_gid ibvGid{};                     /* raw gid */
    RoCEVersion version = RoCE_UNKNOWN_VERSION; /* roce version of this gid */
    std::string ip;                             /* ip carried by this gid, empty if not an ip based gid */
};

/* information exchanged between two peers to establish a rc connection */
struct VerbsQpInfo {
    uint32_t psn = 0;
    uint32_t mtu = 0;
    uint32_t qpNumber = 0;
    uint16_t lid = 0;
    uint8_t portNum = 1;
    uint8_t gid[ROCE_GID_LEN] = {};
};

std::ostream &operator<<(std::ostream &os, const VerbsQpInfo &info);

class VerbsDevice : public Referable {
public:
    static Result Init() noexcept;
    static void Uninit() noexcept;

    static VerbsDevicePtr GetDeviceByIp(const std::string &ipWithMask) noexcept;
    static VerbsContextPtr GetDeviceByName(const std::string &name) noexcept;

    static const std::map<std::string, VerbsDevicePtr> &AllDevices() noexcept;
    static Result FindDevice(const std::string &name, VerbsDevicePtr &out) noexcept;

    static std::string PortState2Str(enum ibv_port_state state);
    static std::string PortMTU2Str(enum ibv_mtu mtu);
    static std::string RoCEVersion2Str(RoCEVersion version);
    static std::string GidToIp(const union ibv_gid &gid) noexcept;
    static Result IpToGid(const std::string &ip, union ibv_gid &gid) noexcept;
    static std::string StripIpMask(const std::string &ipWithMask) noexcept;

public:
    VerbsDevice(const std::string &name, const std::string &sysPath, uint16_t portIndex, uint16_t devIndex,
                const struct ibv_port_attr &portAttr, const struct ibv_device_attr &devAttr,
                struct ibv_device *rawDevice) noexcept;

    const std::string &DeviceName() const noexcept;
    const std::string &SysPath() const noexcept;
    uint16_t PortIndex() const noexcept;
    uint16_t DeviceId() const noexcept;
    uint8_t BandWidth() const noexcept;
    RoCEVersion Version() const noexcept;
    uint16_t GidIndex() const noexcept;
    const union ibv_gid &IbvGid() const noexcept;
    const std::vector<VerbsGidInfo> &GidList() const noexcept;
    const struct ibv_port_attr &PortAttributes() const noexcept;
    const struct ibv_device_attr &DeviceAttributes() const noexcept;
    struct ibv_device *RawDevice() const noexcept;

    void AddGid(const VerbsGidInfo &info) noexcept;
    bool MatchIp(const std::string &ip) const noexcept;
    Result GidForIp(const std::string &ip, VerbsGidInfo &out) const noexcept;

    std::string ToString(bool whole = false, const std::string &prefix = "",
                         const std::string &seperator = "") const noexcept;

private:
    static Result InitInner() noexcept;
    static Result LoadDevices(struct ibv_device **deviceList, int numDevices) noexcept;
    static Result LoadPorts(struct ibv_context *context, struct ibv_device *rawDevice, uint16_t devIndex,
                            const struct ibv_device_attr &devAttr) noexcept;
    static Result LoadGidList(struct ibv_context *context, VerbsDevice &device) noexcept;
    static RoCEVersion ReadRoCEVersion(const std::string &sysPath, uint16_t portIndex, uint16_t gidIndex) noexcept;
    static uint32_t SpeedToMb(uint8_t activeSpeed) noexcept;
    static uint32_t WidthToFactor(uint8_t activeWidth) noexcept;

private:
    /* static variables */
    static std::map<std::string, VerbsDevicePtr> ALL_DEVICES;
    static struct ibv_device **DEVICE_LIST;
    static bool INITED;
    static std::mutex MUTEX;

private:
    const std::string deviceName_;               /* name of device */
    std::string sysPath_;                        /* sys path */
    uint16_t portIndex_ = 0;                     /* port index, start from 1 */
    uint16_t deviceId_ = 0;                      /* device id */
    uint8_t bandWidth_ = 0;                      /* bandwidth of this device in Gb */
    RoCEVersion version_ = RoCE_UNKNOWN_VERSION; /* roce version of the default gid */
    uint16_t gidIndex_ = 0;                      /* default gid index */
    union ibv_gid ibvGid_{};                     /* default structured gid */
    struct ibv_port_attr portAttributes_{};      /* port attr */
    struct ibv_device_attr devAttributes_{};     /* device attr */
    struct ibv_device *rawDevice_ = nullptr;     /* raw device, valid until Uninit */
    std::vector<VerbsGidInfo> gidList_;          /* all gids of this port */
};

ALWAYS_INLINE const std::string &VerbsDevice::DeviceName() const noexcept
{
    return deviceName_;
}

ALWAYS_INLINE const std::string &VerbsDevice::SysPath() const noexcept
{
    return sysPath_;
}

ALWAYS_INLINE uint16_t VerbsDevice::PortIndex() const noexcept
{
    return portIndex_;
}

ALWAYS_INLINE uint16_t VerbsDevice::DeviceId() const noexcept
{
    return deviceId_;
}

ALWAYS_INLINE uint8_t VerbsDevice::BandWidth() const noexcept
{
    return bandWidth_;
}

ALWAYS_INLINE RoCEVersion VerbsDevice::Version() const noexcept
{
    return version_;
}

ALWAYS_INLINE uint16_t VerbsDevice::GidIndex() const noexcept
{
    return gidIndex_;
}

ALWAYS_INLINE const union ibv_gid &VerbsDevice::IbvGid() const noexcept
{
    return ibvGid_;
}

ALWAYS_INLINE const std::vector<VerbsGidInfo> &VerbsDevice::GidList() const noexcept
{
    return gidList_;
}

ALWAYS_INLINE const struct ibv_port_attr &VerbsDevice::PortAttributes() const noexcept
{
    return portAttributes_;
}

ALWAYS_INLINE const struct ibv_device_attr &VerbsDevice::DeviceAttributes() const noexcept
{
    return devAttributes_;
}

ALWAYS_INLINE struct ibv_device *VerbsDevice::RawDevice() const noexcept
{
    return rawDevice_;
}

ALWAYS_INLINE const std::map<std::string, VerbsDevicePtr> &VerbsDevice::AllDevices() noexcept
{
    return ALL_DEVICES;
}

ALWAYS_INLINE void VerbsDevice::AddGid(const VerbsGidInfo &info) noexcept
{
    gidList_.push_back(info);
}

ALWAYS_INLINE bool VerbsDevice::MatchIp(const std::string &ip) const noexcept
{
    std::string target = StripIpMask(ip);
    for (const auto &gid : gidList_) {
        if (!gid.ip.empty() && gid.ip == target) {
            return true;
        }
    }

    return false;
}

ALWAYS_INLINE Result VerbsDevice::GidForIp(const std::string &ip, VerbsGidInfo &out) const noexcept
{
    std::string target = StripIpMask(ip);
    for (const auto &gid : gidList_) {
        if (!gid.ip.empty() && gid.ip == target) {
            out = gid;
            return GEN_OK;
        }
    }

    GEN_LOG_ERROR("No gid matched with ip(" << target << "), device(" << deviceName_ << "), port(" << portIndex_
                                            << ")");
    return GEN_INVALID_PARAM;
}

ALWAYS_INLINE Result VerbsDevice::FindDevice(const std::string &name, VerbsDevicePtr &out) noexcept
{
    auto result = Init();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Find rdma device(" << name << ") failed, as device init failed, result(" << result << ")");
        return result;
    }

    auto iter = ALL_DEVICES.find(name);
    if (iter != ALL_DEVICES.end()) {
        out = iter->second;
        return GEN_OK;
    }

    for (const auto &item : ALL_DEVICES) {
        if (item.second != nullptr && item.second->DeviceName() == name) {
            out = item.second;
            return GEN_OK;
        }
    }

    GEN_LOG_ERROR("Rdma device(" << name << ") not found, device count(" << ALL_DEVICES.size() << ")");
    return GEN_INVALID_PARAM;
}

ALWAYS_INLINE VerbsDevicePtr VerbsDevice::GetDeviceByIp(const std::string &ipWithMask) noexcept
{
    auto result = Init();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Get device by ip(" << ipWithMask << ") failed, as device init failed, result(" << result << ")");
        return nullptr;
    }

    std::string target = StripIpMask(ipWithMask);
    for (const auto &item : ALL_DEVICES) {
        if (item.second != nullptr && item.second->MatchIp(target)) {
            GEN_LOG_DEBUG("Found device(" << item.first << ") for ip(" << target << ")");
            return item.second;
        }
    }

    GEN_LOG_ERROR("No rdma device matched with ip(" << target << "), device count(" << ALL_DEVICES.size() << ")");
    return nullptr;
}

ALWAYS_INLINE std::string VerbsDevice::PortState2Str(enum ibv_port_state state)
{
    if (LIKELY(VerbsApi::IsLoaded())) {
        auto str = VerbsApi::ibv_port_state_str(state);
        return str == nullptr ? "unknown" : str;
    }

    return "unknown";
}

ALWAYS_INLINE std::string VerbsDevice::PortMTU2Str(enum ibv_mtu mtu)
{
    switch (mtu) {
        case IBV_MTU_256:
            return "256bytes";
        case IBV_MTU_512:
            return "512bytes";
        case IBV_MTU_1024:
            return "1024bytes";
        case IBV_MTU_2048:
            return "2048bytes";
        case IBV_MTU_4096:
            return "4096bytes";
        default:
            break;
    }

    return "unknown";
}

ALWAYS_INLINE std::string VerbsDevice::RoCEVersion2Str(RoCEVersion version)
{
    switch (version) {
        case RoCE_V1:
            return "RoCE v1";
        case RoCE_V1_5:
            return "RoCE v1.5";
        case RoCE_V2:
            return "RoCE v2";
        default:
            break;
    }

    return "unknown";
}

ALWAYS_INLINE std::string VerbsDevice::StripIpMask(const std::string &ipWithMask) noexcept
{
    auto pos = ipWithMask.find('/');
    if (pos == std::string::npos) {
        return ipWithMask;
    }

    return ipWithMask.substr(0, pos);
}

ALWAYS_INLINE std::string VerbsDevice::GidToIp(const union ibv_gid &gid) noexcept
{
    if (memcmp(gid.raw, IPV4_MAPPED_PREFIX, sizeof(IPV4_MAPPED_PREFIX)) == 0) {
        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, gid.raw + IPV4_MAPPED_OFFSET, buf, sizeof(buf)) != nullptr) {
            return buf;
        }
        return "";
    }

    /* link local gid of roce v1 is generated from the mac address, which is not a configured ip */
    bool linkLocal = gid.raw[0] == 0xfe && (gid.raw[1] & 0xc0) == 0x80;
    if (linkLocal) {
        return "";
    }

    char buf[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, gid.raw, buf, sizeof(buf)) != nullptr) {
        return buf;
    }

    return "";
}

ALWAYS_INLINE Result VerbsDevice::IpToGid(const std::string &ip, union ibv_gid &gid) noexcept
{
    std::string addr = StripIpMask(ip);
    if (UNLIKELY(addr.empty())) {
        GEN_LOG_ERROR("Convert ip to gid failed, as ip(" << ip << ") is empty");
        return GEN_INVALID_PARAM;
    }

    memset(gid.raw, 0, sizeof(gid.raw));
    if (inet_pton(AF_INET, addr.c_str(), gid.raw + IPV4_MAPPED_OFFSET) == 1) {
        memcpy(gid.raw, IPV4_MAPPED_PREFIX, sizeof(IPV4_MAPPED_PREFIX));
        return GEN_OK;
    }

    if (inet_pton(AF_INET6, addr.c_str(), gid.raw) == 1) {
        return GEN_OK;
    }

    GEN_LOG_ERROR("Convert ip to gid failed, as ip(" << addr << ") is invalid");
    return GEN_INVALID_PARAM;
}

ALWAYS_INLINE uint32_t VerbsDevice::WidthToFactor(uint8_t activeWidth) noexcept
{
    switch (activeWidth) {
        case 1:
            return 1; /* 1x */
        case 2:
            return 4; /* 4x */
        case 4:
            return 8; /* 8x */
        case 8:
            return 12; /* 12x */
        default:
            break;
    }

    return 1;
}

class VerbsContext : public Referable {
public:
    static Result CreateContext(const std::string &devName, VerbsContextPtr &out) noexcept;
    static Result LookupContext(const std::string &devKey, VerbsContextPtr &out) noexcept;
    static void ClearAllContexts() noexcept;

public:
    VerbsContext(struct ibv_context *context, struct ibv_pd *pd, const VerbsDevicePtr &device) noexcept;
    ~VerbsContext() override;

    void Destroy() noexcept;

    Result CreateCq(uint32_t cqDepth, GenettyCQPollingType pollingType, VerbsCQPtr &out) noexcept;
    Result CreateQp(uint32_t sendDepth, uint32_t recvDepth, const VerbsCQPtr &sendCq, const VerbsCQPtr &recvCq,
                    VerbsQPPtr &out) noexcept;
    Result RegisterMr(uint64_t address, uint64_t size, int access, VerbsMRPtr &out) noexcept;
    Result RegisterMrIova(uint64_t address, uint64_t size, uint64_t iova, unsigned int access,
                          VerbsMRPtr &out) noexcept;

    struct ibv_context *Handle() const noexcept;
    struct ibv_pd *PdHandle() const noexcept;
    const VerbsDevicePtr &Device() const noexcept;
    uint32_t MaxSge() const noexcept;
    uint32_t MaxInlineData() const noexcept;
    uint32_t Id() const noexcept;
    void SetUserData(uint64_t userData) noexcept;
    uint64_t UserData() const noexcept;

private:
    Result NewMr(struct ibv_mr *rawMr, uint64_t address, uint64_t size, VerbsMRPtr &out) noexcept;

private:
    struct ibv_context *rawContext_ = nullptr; /* raw context */
    struct ibv_pd *rawPd_ = nullptr;           /* raw pd */
    VerbsDevicePtr device_;                    /* hold the reference */
    uint32_t maxSGECount_ = 1;                 /* max sge count of this device */
    uint32_t maxInlineData_ = 0;               /* max inline data of this device */
    uint32_t contextId_ = 0;                   /* local context id */
    uint64_t userData_ = 0;                    /* user data of this context */

private:
    static std::map<std::string, VerbsContextPtr> ALL_CONTEXTS;
    static std::mutex ALL_CONTEXTS_MUTEX;
    static std::atomic<uint32_t> AUTO_INCREASE_CONTEXT_ID;
};

ALWAYS_INLINE struct ibv_context *VerbsContext::Handle() const noexcept
{
    return rawContext_;
}

ALWAYS_INLINE struct ibv_pd *VerbsContext::PdHandle() const noexcept
{
    return rawPd_;
}

ALWAYS_INLINE const VerbsDevicePtr &VerbsContext::Device() const noexcept
{
    return device_;
}

ALWAYS_INLINE uint32_t VerbsContext::MaxSge() const noexcept
{
    return maxSGECount_;
}

ALWAYS_INLINE uint32_t VerbsContext::MaxInlineData() const noexcept
{
    return maxInlineData_;
}

ALWAYS_INLINE uint32_t VerbsContext::Id() const noexcept
{
    return contextId_;
}

ALWAYS_INLINE void VerbsContext::SetUserData(uint64_t userData) noexcept
{
    userData_ = userData;
}

ALWAYS_INLINE uint64_t VerbsContext::UserData() const noexcept
{
    return userData_;
}

ALWAYS_INLINE Result VerbsContext::LookupContext(const std::string &devKey, VerbsContextPtr &out) noexcept
{
    std::lock_guard<std::mutex> guard(ALL_CONTEXTS_MUTEX);
    auto iter = ALL_CONTEXTS.find(devKey);
    if (iter == ALL_CONTEXTS.end()) {
        return GEN_ERROR;
    }

    out = iter->second;
    return GEN_OK;
}

ALWAYS_INLINE void VerbsContext::ClearAllContexts() noexcept
{
    std::map<std::string, VerbsContextPtr> contexts;
    {
        std::lock_guard<std::mutex> guard(ALL_CONTEXTS_MUTEX);
        contexts.swap(ALL_CONTEXTS);
    }

    if (!contexts.empty()) {
        GEN_LOG_WARN("release " << contexts.size() << " verbs context(s) held by the registry");
    }

    contexts.clear();
}

ALWAYS_INLINE VerbsContext::VerbsContext(struct ibv_context *context, struct ibv_pd *pd,
                                         const VerbsDevicePtr &device) noexcept
    : rawContext_(context), rawPd_(pd), device_(device)
{
    GEN_ASSERT(context != nullptr);
    GEN_ASSERT(pd != nullptr);
    GEN_ASSERT(device != nullptr);

    maxSGECount_ = static_cast<uint32_t>(device->DeviceAttributes().max_sge);
    if (maxSGECount_ == 0) {
        maxSGECount_ = 1;
    }
}

ALWAYS_INLINE VerbsContext::~VerbsContext()
{
    Destroy();
}

ALWAYS_INLINE void VerbsContext::Destroy() noexcept
{
    if (rawPd_ != nullptr) {
        if (LIKELY(VerbsApi::IsLoaded())) {
            auto result = VerbsApi::ibv_dealloc_pd(rawPd_);
            if (result != 0) {
                GEN_LOG_WARN("[IBV_API] ibv_dealloc_pd failed, context(" << contextId_ << "), errno(" << errno << ")");
            }
        }
        rawPd_ = nullptr;
    }

    if (rawContext_ != nullptr) {
        if (LIKELY(VerbsApi::IsLoaded())) {
            auto result = VerbsApi::ibv_close_device(rawContext_);
            if (result != 0) {
                GEN_LOG_WARN("[IBV_API] ibv_close_device failed, context(" << contextId_ << "), errno(" << errno
                                                                           << ")");
            }
        }
        rawContext_ = nullptr;
    }
}

ALWAYS_INLINE Result VerbsContext::NewMr(struct ibv_mr *rawMr, uint64_t address, uint64_t size,
                                         VerbsMRPtr &out) noexcept
{
    auto mr = mf::MakeRef<VerbsMR>(rawMr, this, address, size);
    if (UNLIKELY(mr == nullptr)) {
        GEN_LOG_ERROR("Create VerbsMR object failed, probably out of memory, address(0x"
                      << std::hex << address << std::dec << "), size(" << size << ")");
        VerbsApi::ibv_dereg_mr(rawMr);
        return GEN_MALLOC_FAILED;
    }

    out = mr;
    GEN_LOG_DEBUG("mr registered, address(0x" << std::hex << address << "), lkey(0x" << rawMr->lkey << "), rkey(0x"
                                              << rawMr->rkey << std::dec << ")");
    return GEN_OK;
}

/* VerbsDevice functions which depend on the VerbsContext definition */
ALWAYS_INLINE VerbsContextPtr VerbsDevice::GetDeviceByName(const std::string &name) noexcept
{
    VerbsContextPtr context;
    auto result = VerbsContext::CreateContext(name, context);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Create context for device(" << name << ") failed, result(" << result << ")");
        return nullptr;
    }

    return context;
}

ALWAYS_INLINE void VerbsDevice::Uninit() noexcept
{
    std::lock_guard<std::mutex> lock(MUTEX);
    VerbsContext::ClearAllContexts();
    ALL_DEVICES.clear();

    if (DEVICE_LIST != nullptr) {
        if (LIKELY(VerbsApi::IsLoaded())) {
            VerbsApi::ibv_free_device_list(DEVICE_LIST);
        }
        DEVICE_LIST = nullptr;
    }

    INITED = false;
    GEN_LOG_DEBUG("Uninitialized");
}

class VerbsCQ : public Referable {
public:
    VerbsCQ(struct ibv_cq *cq, struct ibv_comp_channel *channel, GenettyCQPollingType pollingType, uint32_t cqDepth,
            const VerbsContextPtr &context) noexcept;
    ~VerbsCQ() override;

    void Destroy() noexcept;

    struct ibv_cq *Handle() const noexcept;
    GenettyCQPollingType PollingType() const noexcept;
    uint32_t Depth() const noexcept;
    const VerbsContextPtr &Context() const noexcept;
    int Fd() const noexcept;

    Result EventWait(int timeoutMs) noexcept;
    Result Rearm(bool solicitedOnly) noexcept;
    void Ack(uint32_t eventCount) noexcept;
    int Poll(int maxEntries, struct ibv_wc *wcs) noexcept;
    Result SetNonBlocking() noexcept;

private:
    struct ibv_cq *rawCq_ = nullptr;                /* raw cq */
    struct ibv_comp_channel *rawChannel_ = nullptr; /* raw completion channel, null when busy polling */
    const VerbsContextPtr context_;                 /* hold the reference */
    const GenettyCQPollingType pollingType_ = GenettyCQPollingType::GCQ_TYPE_EVENT;
    const uint32_t cqDepth_ = VERBS_DEFAULT_CQ_DEPTH;
    uint32_t compEvents_ = 0; /* events not acked yet */
};
using VerbsCQPtr = Ref<VerbsCQ>;

ALWAYS_INLINE struct ibv_cq *VerbsCQ::Handle() const noexcept
{
    return rawCq_;
}

ALWAYS_INLINE GenettyCQPollingType VerbsCQ::PollingType() const noexcept
{
    return pollingType_;
}

ALWAYS_INLINE uint32_t VerbsCQ::Depth() const noexcept
{
    return cqDepth_;
}

ALWAYS_INLINE const VerbsContextPtr &VerbsCQ::Context() const noexcept
{
    return context_;
}

ALWAYS_INLINE int VerbsCQ::Fd() const noexcept
{
    return rawChannel_ == nullptr ? -1 : rawChannel_->fd;
}

ALWAYS_INLINE VerbsCQ::VerbsCQ(struct ibv_cq *cq, struct ibv_comp_channel *channel, GenettyCQPollingType pollingType,
                               uint32_t cqDepth, const VerbsContextPtr &context) noexcept
    : rawCq_(cq), rawChannel_(channel), context_(context), pollingType_(pollingType), cqDepth_(cqDepth)
{
    GEN_ASSERT(cq != nullptr);
}

ALWAYS_INLINE VerbsCQ::~VerbsCQ()
{
    Destroy();
}

ALWAYS_INLINE Result VerbsCQ::Rearm(bool solicitedOnly) noexcept
{
    if (UNLIKELY(rawCq_ == nullptr)) {
        GEN_LOG_ERROR("Rearm cq failed, as cq is null, depth(" << cqDepth_ << ")");
        return GEN_ERROR;
    }

    if (VerbsApi::ibv_req_notify_cq(rawCq_, solicitedOnly ? 1 : 0) != 0) {
        GEN_LOG_ERROR("[IBV_API] ibv_req_notify_cq failed, cq(" << static_cast<const void *>(rawCq_) << "), errno("
                                                                << errno << ")");
        return GEN_ERROR;
    }

    return GEN_OK;
}

ALWAYS_INLINE void VerbsCQ::Ack(uint32_t eventCount) noexcept
{
    if (UNLIKELY(rawCq_ == nullptr || eventCount == 0)) {
        return;
    }

    VerbsApi::ibv_ack_cq_events(rawCq_, eventCount);
    compEvents_ = eventCount >= compEvents_ ? 0 : compEvents_ - eventCount;
}

ALWAYS_INLINE int VerbsCQ::Poll(int maxEntries, struct ibv_wc *wcs) noexcept
{
    if (UNLIKELY(rawCq_ == nullptr || wcs == nullptr || maxEntries <= 0)) {
        GEN_LOG_ERROR("Poll cq failed, invalid param, cq(" << static_cast<const void *>(rawCq_) << "), max entries("
                                                           << maxEntries << ")");
        return -1;
    }

    return VerbsApi::ibv_poll_cq(rawCq_, maxEntries, wcs);
}

ALWAYS_INLINE Result VerbsCQ::SetNonBlocking() noexcept
{
    if (UNLIKELY(rawChannel_ == nullptr)) {
        GEN_LOG_ERROR("Set comp channel non-blocking failed, as comp channel is null, depth(" << cqDepth_ << ")");
        return GEN_ERROR;
    }

    auto flags = fcntl(rawChannel_->fd, F_GETFL);
    if (flags < 0) {
        GEN_LOG_ERROR("fcntl() get flags of fd(" << rawChannel_->fd << ") failed, errno(" << errno << ")");
        return GEN_ERROR;
    }

    if (fcntl(rawChannel_->fd, F_SETFL, static_cast<uint32_t>(flags) | O_NONBLOCK) < 0) {
        GEN_LOG_ERROR("fcntl() set fd(" << rawChannel_->fd << ") non-blocking failed, errno(" << errno << ")");
        return GEN_ERROR;
    }

    GEN_LOG_DEBUG("Set fd(" << rawChannel_->fd << ") of comp channel to non-blocking");
    return GEN_OK;
}

class VerbsQP : public Referable {
public:
    VerbsQP(struct ibv_qp *qp, const VerbsContextPtr &context, const VerbsCQPtr &sendCq, const VerbsCQPtr &recvCq,
            uint32_t sendDepth, uint32_t recvDepth) noexcept;
    ~VerbsQP() override;

    void Destroy() noexcept;

    struct ibv_qp *Handle() const noexcept;
    uint32_t QpNumber() const noexcept;
    uint32_t Psn() const noexcept;
    uint32_t SendQueueDepth() const noexcept;
    uint32_t RecvQueueDepth() const noexcept;
    enum ibv_qp_state State() const noexcept;
    const VerbsContextPtr &Context() const noexcept;

    /* export the local connection info which should be sent to the peer */
    Result GetLocalInfo(VerbsQpInfo &info) const noexcept;
    /* move qp from reset to init, 'portNum' is the local port index which starts from 1 */
    Result Init(uint8_t portNum) noexcept;
    /* move qp from init to rts with the peer information */
    Result Import(const VerbsQpInfo &remote, uint8_t trafficClass = 0, uint8_t serviceLevel = 0) noexcept;

    int PostSend(struct ibv_send_wr *wr, struct ibv_send_wr **badWr) noexcept;
    int PostRecv(struct ibv_recv_wr *wr, struct ibv_recv_wr **badWr) noexcept;

private:
    Result ModifyToInit(uint8_t portNum) noexcept;
    Result ModifyToRtr(const VerbsQpInfo &remote) noexcept;
    Result ModifyToRts() noexcept;

private:
    struct ibv_qp *rawQp_ = nullptr; /* raw qp */
    const VerbsContextPtr context_;  /* hold the reference */
    const VerbsCQPtr sendCq_;        /* hold the reference */
    const VerbsCQPtr recvCq_;        /* hold the reference */
    uint32_t sendQueueDepth_ = VERBS_DEFAULT_SEND_QUEUE_DEPTH;
    uint32_t recvQueueDepth_ = VERBS_DEFAULT_RECV_QUEUE_DEPTH;
    uint8_t trafficClass_ = 0;                  /* RoCE traffic class */
    uint8_t serviceLevel_ = 0;                  /* RoCE service level */
    uint32_t psn_ = 0;                          /* local start psn */
    uint8_t portNum_ = 1;                       /* local port index */
    uint8_t maxRdAtomic_ = VERBS_MAX_RD_ATOMIC; /* max outstanding read/atomic supported by the device */
    enum ibv_qp_state state_ = IBV_QPS_RESET;
};
using VerbsQPPtr = Ref<VerbsQP>;

ALWAYS_INLINE struct ibv_qp *VerbsQP::Handle() const noexcept
{
    return rawQp_;
}

ALWAYS_INLINE uint32_t VerbsQP::QpNumber() const noexcept
{
    return rawQp_ == nullptr ? 0 : rawQp_->qp_num;
}

ALWAYS_INLINE uint32_t VerbsQP::Psn() const noexcept
{
    return psn_;
}

ALWAYS_INLINE uint32_t VerbsQP::SendQueueDepth() const noexcept
{
    return sendQueueDepth_;
}

ALWAYS_INLINE uint32_t VerbsQP::RecvQueueDepth() const noexcept
{
    return recvQueueDepth_;
}

ALWAYS_INLINE enum ibv_qp_state VerbsQP::State() const noexcept
{
    return state_;
}

ALWAYS_INLINE const VerbsContextPtr &VerbsQP::Context() const noexcept
{
    return context_;
}

ALWAYS_INLINE VerbsQP::VerbsQP(struct ibv_qp *qp, const VerbsContextPtr &context, const VerbsCQPtr &sendCq,
                               const VerbsCQPtr &recvCq, uint32_t sendDepth, uint32_t recvDepth) noexcept
    : rawQp_(qp), context_(context), sendCq_(sendCq), recvCq_(recvCq), sendQueueDepth_(sendDepth),
      recvQueueDepth_(recvDepth)
{
    GEN_ASSERT(qp != nullptr);
    psn_ = qp == nullptr ? 0 : qp->qp_num;

    if (context_ != nullptr && context_->Device() != nullptr) {
        auto maxRdAtomic = context_->Device()->DeviceAttributes().max_qp_rd_atom;
        if (maxRdAtomic > 0 && maxRdAtomic < static_cast<int>(VERBS_MAX_RD_ATOMIC)) {
            maxRdAtomic_ = static_cast<uint8_t>(maxRdAtomic);
        }
    }
}

ALWAYS_INLINE VerbsQP::~VerbsQP()
{
    Destroy();
}

ALWAYS_INLINE void VerbsQP::Destroy() noexcept
{
    if (rawQp_ != nullptr) {
        if (LIKELY(VerbsApi::IsLoaded())) {
            auto result = VerbsApi::ibv_destroy_qp(rawQp_);
            if (result != 0) {
                GEN_LOG_WARN("[IBV_API] ibv_destroy_qp failed, qp(" << rawQp_->qp_num << "), errno(" << errno << ")");
            }
        }
        rawQp_ = nullptr;
    }

    state_ = IBV_QPS_RESET;
}

ALWAYS_INLINE Result VerbsQP::GetLocalInfo(VerbsQpInfo &info) const noexcept
{
    if (UNLIKELY(rawQp_ == nullptr || context_ == nullptr)) {
        GEN_LOG_ERROR("Get local qp info failed, as qp or context is null");
        return GEN_ERROR;
    }

    const auto &device = context_->Device();
    if (UNLIKELY(device == nullptr)) {
        GEN_LOG_ERROR("Get local qp info failed, as device is null, qp(" << rawQp_->qp_num << ")");
        return GEN_ERROR;
    }

    const auto &portAttr = device->PortAttributes();
    info.psn = psn_;
    info.mtu = static_cast<uint32_t>(portAttr.active_mtu);
    info.qpNumber = rawQp_->qp_num;
    info.lid = portAttr.lid;
    info.portNum = portNum_;
    memcpy(info.gid, device->IbvGid().raw, sizeof(info.gid));

    GEN_LOG_DEBUG("local info(" << info << ")");
    return GEN_OK;
}

ALWAYS_INLINE int VerbsQP::PostSend(struct ibv_send_wr *wr, struct ibv_send_wr **badWr) noexcept
{
    if (UNLIKELY(rawQp_ == nullptr || wr == nullptr)) {
        GEN_LOG_ERROR("Post send failed, as qp or wr is null, qp(" << QpNumber() << ")");
        return -1;
    }

    auto result = rawQp_->context->ops.post_send(rawQp_, wr, badWr);
    if (result != 0) {
        GEN_LOG_ERROR("[IBV_API] ibv_post_send failed, qp(" << QpNumber() << "), opcode(" << wr->opcode << "), errno("
                                                            << errno << ")");
    }

    return result;
}

ALWAYS_INLINE int VerbsQP::PostRecv(struct ibv_recv_wr *wr, struct ibv_recv_wr **badWr) noexcept
{
    if (UNLIKELY(rawQp_ == nullptr || wr == nullptr)) {
        GEN_LOG_ERROR("Post recv failed, as qp or wr is null, qp(" << QpNumber() << ")");
        return -1;
    }

    auto result = rawQp_->context->ops.post_recv(rawQp_, wr, badWr);
    if (result != 0) {
        GEN_LOG_ERROR("[IBV_API] ibv_post_recv failed, qp(" << QpNumber() << "), errno(" << errno << ")");
    }

    return result;
}

class VerbsMR : public Referable {
public:
    VerbsMR(struct ibv_mr *mr, const VerbsContextPtr &context, uint64_t address, uint64_t size) noexcept;
    ~VerbsMR() override;

    void Destroy() noexcept;

    struct ibv_mr *Handle() const noexcept;
    uint64_t Address() const noexcept;
    uint64_t Size() const noexcept;
    uint32_t Lkey() const noexcept;
    uint32_t Rkey() const noexcept;
    const VerbsContextPtr &Context() const noexcept;

private:
    struct ibv_mr *rawMr_ = nullptr; /* raw mr */
    const VerbsContextPtr context_;  /* hold the reference */
    uint64_t address_ = 0;           /* registered address */
    uint64_t size_ = 0;              /* registered size */
};

ALWAYS_INLINE struct ibv_mr *VerbsMR::Handle() const noexcept
{
    return rawMr_;
}

ALWAYS_INLINE uint64_t VerbsMR::Address() const noexcept
{
    return address_;
}

ALWAYS_INLINE uint64_t VerbsMR::Size() const noexcept
{
    return size_;
}

ALWAYS_INLINE uint32_t VerbsMR::Lkey() const noexcept
{
    return rawMr_ == nullptr ? 0 : rawMr_->lkey;
}

ALWAYS_INLINE uint32_t VerbsMR::Rkey() const noexcept
{
    return rawMr_ == nullptr ? 0 : rawMr_->rkey;
}

ALWAYS_INLINE const VerbsContextPtr &VerbsMR::Context() const noexcept
{
    return context_;
}

ALWAYS_INLINE VerbsMR::VerbsMR(struct ibv_mr *mr, const VerbsContextPtr &context, uint64_t address,
                               uint64_t size) noexcept
    : rawMr_(mr), context_(context), address_(address), size_(size)
{
    GEN_ASSERT(mr != nullptr);
}

ALWAYS_INLINE VerbsMR::~VerbsMR()
{
    Destroy();
}

ALWAYS_INLINE void VerbsMR::Destroy() noexcept
{
    if (rawMr_ != nullptr) {
        if (LIKELY(VerbsApi::IsLoaded())) {
            auto result = VerbsApi::ibv_dereg_mr(rawMr_);
            if (result != 0) {
                GEN_LOG_WARN("[IBV_API] ibv_dereg_mr failed, address(0x"
                             << std::hex << address_ << std::dec << "), size(" << size_ << "), errno(" << errno << ")");
            }
        }
        rawMr_ = nullptr;
    }
}

} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif //MF_OPENDCC_GENETTY_VERBS_WRAPPER_H
