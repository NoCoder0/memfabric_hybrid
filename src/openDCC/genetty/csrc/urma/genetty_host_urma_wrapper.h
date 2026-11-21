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
#ifndef MF_GENETTY_URMA_WRAPPER_H
#define MF_GENETTY_URMA_WRAPPER_H

#include "common/genetty_common_includes.h"
#include "under_api/urma/genetty_dl_urma_api.h"

namespace ock {
namespace openDCC {
namespace urma {
struct UBEId {
    uint16_t eidIndex = 0;
    urma_eid_t urmaEid{};
    uint8_t bandwidth = 0;
} __attribute__((packed));

class UrmaDevice;
class UrmaContext;
class UrmaJetty;
class UrmaJfs;
class UrmaJfr;
class UrmaJfc;
class UrmaJfce;
class UrmaSegment;
using UrmaDevicePtr = Ref<UrmaDevice>;
using UrmaContextPtr = Ref<UrmaContext>;
using UrmaJettyPtr = Ref<UrmaJetty>;
using UrmaJfsPtr = Ref<UrmaJfs>;
using UrmaJfrPtr = Ref<UrmaJfr>;
using UrmaJfcPtr = Ref<UrmaJfc>;
using UrmaJfcePtr = Ref<UrmaJfce>;
using UrmaSegmentPtr = Ref<UrmaSegment>;

struct UrmaDeviceBandWidth {
    std::string str;
    uint16_t intValue;

    UrmaDeviceBandWidth(const std::string &s, uint16_t v) : str(s), intValue(v) {}
};

class UrmaUtil {
public:
    static Result UrmaInit();
    static void UrmaUnInit();

    static void UrmaSetLogger();
};

class UrmaDevice : public Referable {
public:
    static void Init() noexcept;

    static const std::map<std::string, UrmaDevicePtr> &AllDevices() noexcept;

    static std::string DeviceState2Str(urma_port_state_t s);
    static std::string DeviceMTU2Str(urma_mtu_t s);

public:
    UrmaDevice(const std::string &name, const std::string &sys_path, const urma_device_attr_t &attr);
    ~UrmaDevice() override = default;

    const std::string &DeviceName() const noexcept;

    const urma_device_attr_t &DeviceAttributes() const noexcept;

    std::string ToString(bool whole = false, const std::string &prefix = "",
                         const std::string &seperator = "") const noexcept;

    const std::vector<urma_eid_info_t> &EidList() const noexcept;

private:
    std::string deviceName_;
    std::string deviceSysPath_;
    urma_device_attr_t attributes_{};
    std::vector<urma_eid_info_t> eidList_;

private:
    static std::map<std::string, UrmaDevicePtr> ALL_DEVICES;
    static bool LOADED;
    static std::mutex MUTEX;
    static std::map<urma_speed_t, UrmaDeviceBandWidth> URMA_BANDWIDTHS;
};

ALWAYS_INLINE const std::string &UrmaDevice::DeviceName() const noexcept
{
    return deviceName_;
}

ALWAYS_INLINE std::string UrmaDevice::DeviceState2Str(urma_port_state_t s)
{
    switch (s) {
        case URMA_PORT_DOWN:
            return "down";
        case URMA_PORT_ACTIVE:
            return "active";
        case URMA_PORT_NOP:
        case URMA_PORT_INIT:
        case URMA_PORT_ARMED:
        case URMA_PORT_ACTIVE_DEFER:
            break;
    }
    return "unknown";
}

ALWAYS_INLINE std::string UrmaDevice::DeviceMTU2Str(urma_mtu_t s)
{
    switch (s) {
        case URMA_MTU_256:
            return "256bytes";
        case URMA_MTU_512:
            return "512bytes";
        case URMA_MTU_1024:
            return "1024bytes";
        case URMA_MTU_2048:
            return "2048bytes";
        case URMA_MTU_4096:
            return "4096bytes";
        case URMA_MTU_8192:
            return "8192bytes";
    }

    return "unknown";
}

ALWAYS_INLINE const std::map<std::string, UrmaDevicePtr> &UrmaDevice::AllDevices() noexcept
{
    return ALL_DEVICES;
}

ALWAYS_INLINE const urma_device_attr_t &UrmaDevice::DeviceAttributes() const noexcept
{
    return attributes_;
}

ALWAYS_INLINE const std::vector<urma_eid_info_t> &UrmaDevice::EidList() const noexcept
{
    return eidList_;
}

enum UrmaJfcPollingType { BUSY_POLLING = 0, EVENT_POLLING };

constexpr uint8_t URMA_TOKEN_POLICY_PLAIN_TEXT = URMA_TOKEN_PLAIN_TEXT;
constexpr uint8_t URMA_JFR_RNR_TIMER_DEFAULT = 19; /* RNR single retransmission time: 2us*2^19 = 1.049s */
constexpr uint8_t URMA_JFS_RNR_RETRY_DEFAULT = 6;
constexpr uint8_t URMA_JFS_ERROR_TIMEOUT = 2;

struct UrmaBadPortRecord {
    int16_t last_error = 0;
    uint64_t error_occurred_time = 0;
};

class UrmaContext : public Referable {
public:
    static Result CreateContext(const std::string &devName, uint32_t eidIndex, UrmaContextPtr &out);
    static Result LookupContext(const std::string &devName, uint32_t eidIndex, UrmaContextPtr &out);

    static void ClearAllContexts() noexcept;

    static uint32_t NewJettyId() noexcept;

public:
    UrmaContext(urma_context_t *urmaContext, const UrmaDevicePtr &dev, const urma_eid_info_t &eidInfo)
        : rawContext_(urmaContext), device_(dev), eidInfo_(eidInfo)
    {
        GEN_ASSERT(dev != nullptr);
    }

    ~UrmaContext() override;

    urma_jfc_cfg_t CreateJfcCfg(uint32_t queueDepth, uint64_t userCtx = 0);
    urma_jfs_cfg_t CreateJfsCfg(uint32_t queueDepth, urma_transport_mode_t transMode, uint64_t userCtx = 0,
                                uint8_t priority = UINT8_MAX);
    urma_jfr_cfg_t CreateJfrCfg(uint32_t queueDepth, urma_transport_mode_t transMode, uint32_t tokenValue = 0,
                                uint64_t userCtx = 0);
    urma_jetty_cfg_t CreateJettyCfg(uint64_t userCtx = 0);

    Result CreateJfce(UrmaJfcePtr &out);
    Result CreateJfc(urma_jfc_cfg_t &cfg, UrmaJfcPollingType pollingType, UrmaJfcPtr &out);
    Result CreateJfc(urma_jfc_cfg_t &cfg, UrmaJfcePtr &jfce, UrmaJfcPtr &out);
    Result CreateJfs(urma_jfs_cfg_t &cfg, const UrmaJfcPtr &jfc, UrmaJfsPtr &out);
    Result CreateJfr(urma_jfr_cfg_t &cfg, const UrmaJfcPtr &jfc, UrmaJfrPtr &out);
    Result CreateJetty(urma_jetty_cfg_t &cfg, urma_tp_type_t rtp_ctp_utp, const UrmaJfsPtr &jfs, const UrmaJfrPtr &jfr,
                       UrmaJettyPtr &out);

    Result CreateJfsJfrJetty(const UrmaJfcPtr &jfc, uint32_t queueDepth, urma_transport_mode_t transMode,
                             urma_tp_type_t tpType, UrmaJfsPtr &jfs, UrmaJfrPtr &jfr, UrmaJettyPtr &jetty);

    const urma_eid_info_t &EidInfo() const noexcept;

    uint32_t Id() const noexcept;

    urma_context_t *Handle() const noexcept;

    const UrmaDevicePtr &Device() const noexcept;

    bool IsBadPort() noexcept;

    void RecordPortError(int16_t errorCode) noexcept;

    void SetAffinity(bool affinity) noexcept
    {
        affinity_ = affinity;
    }

    bool IsAffinity() noexcept
    {
        return affinity_;
    }

private:
    urma_context_t *rawContext_{nullptr};
    UrmaDevicePtr device_{nullptr};
    urma_eid_info_t eidInfo_{};
    uint32_t contextId_ = UINT16_MAX;
    UrmaBadPortRecord badRecord_;
    bool affinity_;

private:
    static std::map<std::pair<std::string, uint32_t>, UrmaContextPtr> ALL_CONTEXTS;
    static std::mutex ALL_CONTEXTS_MUTEX;
    static std::atomic<uint32_t> AUTO_INCREASE_JETTY_ID;
    static std::atomic<uint32_t> AUTO_INCREASE_CONTEXT_ID;

    friend class UrmaJetty;
};

ALWAYS_INLINE const urma_eid_info_t &UrmaContext::EidInfo() const noexcept
{
    return eidInfo_;
}

ALWAYS_INLINE uint32_t UrmaContext::Id() const noexcept
{
    return contextId_;
}

ALWAYS_INLINE urma_context_t *UrmaContext::Handle() const noexcept
{
    return rawContext_;
}

ALWAYS_INLINE const UrmaDevicePtr &UrmaContext::Device() const noexcept
{
    return device_;
}

class UrmaJfce : public Referable {
public:
    explicit UrmaJfce(urma_jfce_t *jfce, const UrmaContextPtr &ctx) noexcept : rawJfce_(jfce), context_(ctx)
    {
        GEN_ASSERT(jfce != nullptr);
        GEN_ASSERT(ctx != nullptr);
    }

    ~UrmaJfce() noexcept override;

    /**
     * @param Destroy jfce
     */
    void Destroy() noexcept;

    /**
     * @brief Get the fd for epoll_wait or poll
     *
     * @return real fd if ok, -1 if failed
     */
    int Fd() const noexcept;

    /*
     * @brief Set the fd to non-blocking
     */
    Result SetNonBlocking() noexcept;

    urma_jfce_t *GetRawJfce() noexcept;

private:
    urma_jfce_t *rawJfce_ = nullptr;
    const UrmaContextPtr &context_;

    friend class UrmaContext;
};

ALWAYS_INLINE int UrmaJfce::Fd() const noexcept
{
    if (UNLIKELY(rawJfce_ != nullptr)) {
        return rawJfce_->fd;
    }

    return -1;
}

class UrmaJfc : public Referable {
public:
    UrmaJfc(UrmaJfcPollingType pollingType, urma_jfc_t *jfc, urma_jfce_t *jfce, const UrmaContextPtr &ctx)
        : rawJfc_(jfc), rawJfce_(jfce), pollingType_(pollingType), context_(ctx)
    {
        GEN_ASSERT(jfc != nullptr);
        GEN_ASSERT(jfce != nullptr);
        GEN_ASSERT(ctx != nullptr);
    }

    ~UrmaJfc() override;

    void Destroy() noexcept;

    const UrmaContextPtr &Context() const noexcept;
    const UrmaJfcPollingType &PollingType() const noexcept;

    urma_jfc_t *Handle() const noexcept;
    urma_jfce_t *JfceHandle() const noexcept;

    Result EventWait(int timeout_ms) noexcept;
    int Poll(int cr_count, urma_cr_t *jfc) noexcept;
    Result Rearm(bool solicited_only) noexcept;
    void Ack(uint32_t eventCount) noexcept;
    int GetInterruptFd() const noexcept;

private:
    urma_jfc_t *rawJfc_ = nullptr;         /* raw jfc */
    urma_jfce_t *rawJfce_ = nullptr;       /* raw jfce */
    const UrmaJfcPollingType pollingType_; /* polling type */
    const UrmaContextPtr context_;         /* hold the reference */

    /* Ack residual jfc events and discard residual completions */
    void DrainEventsBeforeDelete() noexcept;

    friend class UrmaContext;
};

ALWAYS_INLINE const UrmaContextPtr &UrmaJfc::Context() const noexcept
{
    return context_;
}

ALWAYS_INLINE urma_jfc_t *UrmaJfc::Handle() const noexcept
{
    return rawJfc_;
}

ALWAYS_INLINE urma_jfce_t *UrmaJfc::JfceHandle() const noexcept
{
    return rawJfce_;
}

ALWAYS_INLINE const UrmaJfcPollingType &UrmaJfc::PollingType() const noexcept
{
    return pollingType_;
}

ALWAYS_INLINE int UrmaJfc::Poll(int cr_count, urma_cr_t *cr) noexcept
{
    return UrmaApi::urma_poll_jfc(rawJfc_, cr_count, cr);
}

ALWAYS_INLINE Result UrmaJfc::Rearm(bool solicited_only) noexcept
{
    return UrmaApi::urma_rearm_jfc(rawJfc_, solicited_only);
}

ALWAYS_INLINE Result UrmaJfc::EventWait(int timeout_ms) noexcept
{
    urma_jfc_t *jfc[1]{};
    auto num = UrmaApi::urma_wait_jfc(rawJfce_, 1, timeout_ms, jfc);
    return num <= 0 ? GEN_ERROR : GEN_OK;
}

ALWAYS_INLINE void UrmaJfc::Ack(uint32_t eventCount) noexcept
{
    UrmaApi::urma_ack_jfc(&rawJfc_, &eventCount, 1);
}

ALWAYS_INLINE int UrmaJfc::GetInterruptFd() const noexcept
{
    return rawJfce_ == nullptr ? -1 : rawJfce_->fd;
}

class UrmaJfs : public Referable {
public:
    UrmaJfs(urma_jfs_t *jfs, const UrmaContextPtr &ctx, const UrmaJfcPtr &jfc) : rawJfs_(jfs), context_(ctx), jfc_(jfc)
    {
        GEN_ASSERT(jfs != nullptr);
    }

    ~UrmaJfs() override;

    void Destroy() noexcept;

    urma_jfs_t *Handle() const noexcept
    {
        return rawJfs_;
    }

private:
    urma_jfs_t *rawJfs_ = nullptr; /* raw jfs */
    const UrmaContextPtr context_; /* hold the reference */
    const UrmaJfcPtr jfc_;         /* hold the reference */

    friend class UrmaContext;
};

class UrmaJfr : public Referable {
public:
    UrmaJfr(urma_jfr_t *jfr, uint32_t token, const UrmaContextPtr &ctx, const UrmaJfcPtr &jfc)
        : rawJfr_(jfr), context_(ctx), jfc_(jfc), token_(token)
    {
        GEN_ASSERT(jfr != nullptr);
    }

    ~UrmaJfr() override;

    void Destroy() noexcept;

    urma_jfr_t *Handle() const noexcept;

    uint32_t Token() const noexcept;

private:
    urma_jfr_t *rawJfr_ = nullptr; /* raw jfr */
    const UrmaContextPtr context_; /* hold the reference */
    const UrmaJfcPtr jfc_;         /* hold the reference */
    const uint32_t token_;         /* token */

    friend class UrmaContext;
};

ALWAYS_INLINE urma_jfr_t *UrmaJfr::Handle() const noexcept
{
    return rawJfr_;
}

ALWAYS_INLINE uint32_t UrmaJfr::Token() const noexcept
{
    return token_;
}

class UrmaJetty : public Referable {
public:
    UrmaJetty(urma_tp_type_t rtp_ctp_utp, uint32_t jettyId, urma_jetty_t *jetty, const UrmaContextPtr &ctx,
              const UrmaJfsPtr &jfs, const UrmaJfrPtr &jfr)
        : rawJetty_(jetty), context_(ctx), jfs_(jfs), jfr_(jfr), jettyId_(jettyId), rtp_ctp_utp_(rtp_ctp_utp)
    {
        GEN_ASSERT(jetty != nullptr);
    }

    ~UrmaJetty() override;

    /**
     * @brief Get int jetty id
     */
    uint32_t JettyId() const noexcept;

    /**
     * @brief Get raw jetty id struct
     */
    urma_jetty_id_t RawJettyId() const noexcept;

    /**
     * @brief Get token of jetty from jfr
     */
    uint32_t Token() const noexcept;

    /**
     * @brief Destroy inner raw jetty
     */
    void Destroy() noexcept;

    urma_jetty_t *Handle() const noexcept;

    Result ImportRemoteJetty(urma_jetty_id_t &jettyId, uint32_t &token) noexcept;

    /**
     * @brief Post a send work request to the jetty
     */
    urma_status_t PostSendWr(urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr) noexcept;

    /**
     * @brief Post a receive work request to the jetty
     */
    urma_status_t PostRecvWr(urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr) noexcept;

    /**
     * @brief Post a WRITE_IMM send work request with the given rw descriptor and user context
     */
    urma_status_t PostWriteImm(const urma_rw_wr_t &rw, uint64_t user_ctx) noexcept;

    friend std::ostream &operator<<(std::ostream &os, const UrmaJetty &o);

private:
    urma_jetty_t *rawJetty_ = nullptr;              /* raw jetty */
    urma_target_jetty_t *rawTargetJetty_ = nullptr; /* imported jetty */
    const UrmaContextPtr context_;                  /* hold the reference */
    const UrmaJfsPtr jfs_;                          /* hold the reference */
    const UrmaJfrPtr jfr_;                          /* hold the reference */
    const uint32_t jettyId_;                        /* generated jetty id */
    const urma_tp_type_t rtp_ctp_utp_;              /* tp type */

    friend class UrmaContext;
};

ALWAYS_INLINE uint32_t UrmaJetty::JettyId() const noexcept
{
    return jettyId_;
}

ALWAYS_INLINE urma_jetty_id_t UrmaJetty::RawJettyId() const noexcept
{
    urma_jetty_id_t tmp{};

    if (LIKELY(rawJetty_ != nullptr)) {
        tmp = rawJetty_->jetty_id;
    }

    return tmp;
}

ALWAYS_INLINE uint32_t UrmaJetty::Token() const noexcept
{
    uint32_t tmp = 0;
    if (LIKELY(jfr_ != nullptr)) {
        tmp = jfr_->Token();
    }

    return tmp;
}

ALWAYS_INLINE urma_jetty_t *UrmaJetty::Handle() const noexcept
{
    return rawJetty_;
}

ALWAYS_INLINE urma_status_t UrmaJetty::PostSendWr(urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr) noexcept
{
    auto pos = wr;
    while (pos != nullptr) {
        pos->tjetty = rawTargetJetty_;
        pos = pos->next;
    }
    return UrmaApi::urma_post_jetty_send_wr(rawJetty_, wr, bad_wr);
}

ALWAYS_INLINE urma_status_t UrmaJetty::PostRecvWr(urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr) noexcept
{
    return UrmaApi::urma_post_jetty_recv_wr(rawJetty_, wr, bad_wr);
}

ALWAYS_INLINE urma_status_t UrmaJetty::PostWriteImm(const urma_rw_wr_t &rw, uint64_t user_ctx) noexcept
{
    urma_jfs_wr_t wr = {};
    wr.opcode = URMA_OPC_WRITE_IMM;
    wr.tjetty = rawTargetJetty_;
    wr.user_ctx = user_ctx;
    wr.rw = rw;
    wr.next = nullptr;
    wr.flag.bs.comp_order = 1;
    wr.flag.bs.complete_enable = 1;
    wr.flag.bs.inline_flag = 0;

    urma_jfs_wr_t *bad_wr = nullptr;
    return PostSendWr(&wr, &bad_wr);
}

std::ostream &operator<<(std::ostream &os, const urma_eid_t &o);
std::ostream &operator<<(std::ostream &os, const urma_jetty_id_t &o);
std::ostream &operator<<(std::ostream &os, const urma_jfc_cfg_t &o);
std::ostream &operator<<(std::ostream &os, const urma_jfr_cfg_t &o);
std::ostream &operator<<(std::ostream &os, const urma_jfs_cfg_t &o);
std::ostream &operator<<(std::ostream &os, const urma_jetty_cfg_t &o);
std::ostream &operator<<(std::ostream &os, const urma_jetty_t &o);
} // namespace urma
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_URMA_WRAPPER_H
