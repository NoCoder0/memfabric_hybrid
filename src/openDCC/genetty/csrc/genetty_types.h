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
#ifndef MF_GENETTY_TYPES_H
#define MF_GENETTY_TYPES_H

#include "genetty_def.h"
#include "genetty_vendor_type.h"
#include "common/genetty_common_includes.h"

namespace ock {
namespace openDCC {
class GenettyContextPool;
class GenettyContext;
class GenettyCQ;
class Genetty;
using GenettyContextPoolPtr = Ref<GenettyContextPool>;
using GenettyContextPtr = Ref<GenettyContext>;
using GenettyCQPtr = Ref<GenettyCQ>;
using GenettyPtr = Ref<Genetty>;

static std::atomic<uint64_t> GENETTY_ID_GENERATOR(1);
static std::atomic<uint64_t> GENETTY_CQ_ID_GENERATOR(1);

enum GennettyCtxGetPolicy {
    GCCP_INVALID = 0,
    GCCP_BY_DEVICE_ID,
    GCCP_BY_IP,
    GCCP_AUTO,

    GCCP_COUNT
};

std::ostream &operator<<(std::ostream &os, GennettyCtxGetPolicy policy) noexcept;

struct GenettyCtxGetOptions {
    genetty_type_t genettyType = HOST_RDMA_GENETTY; /* genetty type of the context */
    GennettyCtxGetPolicy type = GCCP_INVALID;       /*  */
    uint16_t deviceId = UINT16_MAX;                 /* device id logically */
    std::string ipMask;                             /* ip with mask */
    uint32_t flags = 0;                             /* optional flags*/
    uint32_t whichAutoPolicy = 0; /* auto select policy, see GEN_CTX_POLICY_* macros in genetty_def.h */
};

std::ostream &operator<<(std::ostream &os, const GenettyCtxGetOptions &options) noexcept;

enum GenettyCQPollingType : uint8_t {
    GCQ_TYPE_EVENT = 0,
    GCQ_TYPE_BUSY,

    GCQ_TYPE_COUNT
};

std::ostream &operator<<(std::ostream &os, GenettyCQPollingType pollingType) noexcept;
std::ostream &operator<<(std::ostream &os, genetty_type_t type) noexcept;
std::ostream &operator<<(std::ostream &os, genetty_mem_type_t memType) noexcept;

struct GenettyCQOptions {
    genetty_context_t context = nullptr; /* context to create the cq, must be created first */
    uint32_t eventFd = UINT32_MAX; /* external created event fd, UINT32_MAX stand of event create event internally */
    uint32_t queueDepth = 10240;   /* cq depth */
    uint32_t flags = 0;            /* flags */
    VendorType vendor = VendorType::VT_INVALID;         /* vendor type */
    GenettyCQPollingType polling_type = GCQ_TYPE_EVENT; /* polling type */
    uint16_t deviceId = UINT16_MAX;                     /* device id */
    std::string preferred_ip_mask;                      /* prefer id mask */
    bool lockFree = false;                              /* lock free or not */
};

std::ostream &operator<<(std::ostream &os, const GenettyCQOptions &options) noexcept;

struct GenettyOptions {
    GenettyCQPtr cq = nullptr;                  /* sq ptr */
    uint32_t sqDepth = 1024;                    /* send queue depth */
    uint32_t rqDepth = 1024;                    /* receive queue depth */
    uint32_t flags = 0;                         /* flags */
    VendorType vendor = VendorType::VT_INVALID; /* vendor type */
    bool lockFree = false;                      /* lock free or not */
    uint32_t serviceLevel = 0;                  /* service level, for URMA */
    uint32_t trafficClass = 0;                  /* traffic class, for RDMA */
};

std::ostream &operator<<(std::ostream &os, const GenettyOptions &options) noexcept;

struct GenettyBackendOptions {
    uint32_t flags = 0;
    PROTOCOL protocol = 0;
    VendorType vendor = VendorType::VT_INVALID;
};

std::ostream &operator<<(std::ostream &os, const GenettyBackendOptions &options) noexcept;

std::ostream &operator<<(std::ostream &os, const genetty_cq_options_t &options) noexcept;
std::ostream &operator<<(std::ostream &os, const genetty_options_t &options) noexcept;
std::ostream &operator<<(std::ostream &os, const genetty_batch_op_options_t &options) noexcept;
std::ostream &operator<<(std::ostream &os, const genetty_batch_op_data_t &data) noexcept;
std::ostream &operator<<(std::ostream &os, const genetty_memory_segment_t &segment) noexcept;

struct GenettyDeviceInfo {
    uint16_t deviceId = UINT16_MAX;                         /* logic device id */
    uint8_t portId = UINT8_MAX;                             /* eid index | port number */
    bool active = false;                                    /* active or not */
    std::string deviceName;                                 /* name of device */
    std::string ip;                                         /* mapped ip if exsits */
    genetty_type_t type = genetty_type_t::GEN_TYPE_INVALID; /* genetty type of this device */
};

class GenettyBackend {
public:
    virtual ~GenettyBackend() = default;

    /**
     * @brief Initialize the backend with the given options
     *
     * @param options          [in] options to initialize the backend
     * @return GEN_OK if successful, others if failed
     */
    virtual Result Initialize(const GenettyBackendOptions &options) noexcept = 0;

    /**
     * @brief Destroy the backend and release all resources
     */
    virtual void Destroy() noexcept = 0;

    /**
     * @brief Get the information of all the devices which are served by this backend
     *
     * @param info             [out] information of the devices, which is appended to it by this call
     * @return GEN_OK if successful, others if failed
     */
    virtual Result GetDevicesInfo(std::vector<GenettyDeviceInfo> &info) noexcept = 0;

    /**
     * @brief Get the context selected by the given options
     *
     * @param options          [in] options to select the context, such as policy, device id and ip mask
     * @param context          [out] the selected context, nullptr if not found
     * @return GEN_OK if successful, others if failed
     */
    virtual Result GetContext(const GenettyCtxGetOptions &options, genetty_context_t *context) noexcept = 0;

    /**
     * @brief Return the context got by GetContext back to the backend
     *
     * @param context          [in,out] the context to be returned
     * @param flags            [in] optional flags, should be 0 currently
     */
    virtual void ReturnContext(genetty_context_t *context, uint32_t flags) noexcept = 0;

    /**
     * @brief Encode the memory segment into the serialized memory information
     *
     * @param segment          [in] memory segment to be encoded
     * @param flags            [in] optional flags, should be 0 currently
     * @param info             [out] serialized memory information, its data is allocated by this call
     * @return GEN_OK if successful, others if failed
     */
    static Result MemInfoEncode(genetty_memory_segment_t *segment, uint32_t flags, genetty_memory_info_t *info);

    /**
     * @brief Decode the serialized memory information into the memory segment
     *
     * @param info             [in] serialized memory information
     * @param segment          [out] memory segment decoded
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, others if failed
     */
    static Result MemInfoDecode(genetty_memory_info_t *info, genetty_memory_segment_t *segment, uint32_t flags);

    /**
     * @brief Create a completion queue with the given options
     *
     * @param options          [in] options to create the completion queue
     * @return the handle of the cq if successful, nullptr if failed
     */
    virtual genetty_cq_t GenettyCqCreate(genetty_cq_options_t *options) noexcept = 0;

    /**
     * @brief Destroy the completion queue created by GenettyCqCreate
     *
     * @param cq               [in] cq to be destroyed
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, GEN_ERROR if the cq does not belong to this backend
     */
    virtual Result GenettyCqDestroy(genetty_cq_t cq, uint32_t flags) noexcept = 0;

    /**
     * @brief Create a genetty object with the given options
     *
     * @param options          [in] options to create the genetty
     * @return the handle of the genetty if successful, nullptr if failed
     */
    virtual genetty_t GenettyCreate(genetty_options_t *options) noexcept = 0;

    /**
     * @brief Destroy the genetty created by GenettyCreate
     *
     * @param handle           [in] handle of the genetty to be destroyed
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, GEN_ERROR if the genetty does not belong to this backend
     */
    virtual Result GenettyDestroy(genetty_t handle, uint32_t flags) noexcept = 0;
};

class GenettyContext : public Referable {
public:
    /**
     * @brief Construct a context with the genetty type it serves
     *
     * @param t                [in] genetty type of this context
     */
    explicit GenettyContext(genetty_type_t t) noexcept : type_(t) {}

    ~GenettyContext() noexcept override = default;

    /**
     * @brief Register the local memory segment, the key is filled into the segment by this call
     *
     * @param segment          [in,out] memory segment to be registered, the key is filled by this call
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, others if failed
     */
    virtual Result RegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept = 0;

    /**
     * @brief Unregister the local memory segment registered by RegisterMemory
     *
     * @param segment          [in] memory segment registered before
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, others if failed
     */
    virtual Result UnRegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept = 0;

    /**
     * @brief Import the memory segment of the peer, then the memory region can be accessed remotely
     *
     * @param segment          [in] memory segment of the peer, carrying the remote address and keys
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, others if failed
     */
    virtual Result ImportMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept = 0;

    /**
     * @brief Destroy the context and release all resources
     */
    virtual void Destroy() noexcept = 0;

    /**
     * @brief Get the genetty type served by this context
     *
     * @return the genetty type
     */
    genetty_type_t Type() const noexcept
    {
        return type_;
    }

protected:
    genetty_type_t type_ = genetty_type_t::GEN_TYPE_INVALID;
};

class GenettyContextPool : public Referable {
public:
    /**
     * List all devices and add active device and port with context
     *
     * @return GEN_OK if successful, others if failed
     */
    virtual Result AddAllDevices() noexcept = 0;

    /**
     * Close all contexts
     */
    virtual void CloseAllDevices() noexcept = 0;

    /**
     * @brief Record one port fail
     *
     * @param badPortId        [in] id of the bad port
     */
    virtual void RecordBadPort(uint8_t badPortId) noexcept = 0;
};

class GenettyCQ : public Referable {
public:
    /**
     * @brief Construct a cq with the genetty type
     *
     * @param t                [in] genetty type of this cq
     */
    explicit GenettyCQ(genetty_type_t t) noexcept;

    /**
     * @brief Initialize the cq with the given options
     *
     * @param options          [in] options to initialize the cq
     * @return GEN_OK if successful, others if failed
     */
    virtual Result Initialize(const GenettyCQOptions &options) = 0;

    /**
     * @brief Destroy the cq and release all resources
     */
    virtual void Destroy() = 0;

    /**
     * @brief Get the event fd of this cq, which can be used to wait for the completion events
     *
     * @param fd               [out] the event fd of this cq
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, others if failed
     */
    virtual int32_t GetEventFd(int32_t *fd, uint32_t flags) = 0;

    /**
     * @brief Poll the completions of this cq, non-blocking
     *
     * @param num              [in] max number of the completions to be polled
     * @param wc               [out] array to store the polled completions
     * @param flags            [in] optional flags, should be 0 currently
     * @return the number of the polled completions if successful, negative error code if failed
     */
    virtual int32_t Poll(int32_t num, genetty_cq_wc_t *wc, uint32_t flags) = 0;

    /**
     * @brief Rearm this cq, so that the next completion event can be notified
     *
     * @param solicited_only   [in] only notify the solicited completions or not
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, others if failed
     */
    virtual int32_t Rearm(int32_t solicited_only, uint32_t flags) = 0;

    /**
     * @brief Ack the completion events of this cq
     *
     * @param eventCount       [in] number of the events to be acked
     * @param flags            [in] optional flags, should be 0 currently
     * @return GEN_OK if successful, others if failed
     */
    virtual int32_t Ack(int32_t eventCount, uint32_t flags) = 0;

    /**
     * @brief Get the information of this cq which can be accessed by the device, for the device centric
     *        communication
     *
     * @param flags            [in] optional flags, should be 0 currently
     * @param info             [out] device access information of this cq
     * @return GEN_OK if successful, others if failed
     */
    virtual Result GetDeviceAccessInfo(uint32_t flags, genetty_cq_device_access_info_t *info) = 0;

    /**
     * @brief Get the unique id of this cq
     *
     * @return the unique id, 0 if the cq is not initialized
     */
    uint64_t Id() const noexcept;

    /**
     * @brief Get the genetty type of this cq
     *
     * @return the genetty type
     */
    genetty_type_t Type() const noexcept;

protected:
    uint64_t id_ = 0;
    const genetty_type_t type_; /* genetty type, set by the subclass */
};

class Genetty : public Referable {
public:
    virtual Result Quiesce() noexcept
    {
        GEN_LOG_ERROR("Quiesce is not supported by this provider");
        return GEN_ERROR;
    }
    /**
     * @brief Construct a genetty with the genetty type
     *
     * @param t                [in] genetty type of this genetty
     */
    explicit Genetty(genetty_type_t t) noexcept;

    /**
     * @brief Initialize the genetty with the given options
     *
     * @param options          [in] options to initialize the genetty
     * @return GEN_OK if successful, others if failed
     */
    virtual Result Initialize(const GenettyOptions &options) noexcept = 0;

    /**
     * @brief Destroy the genetty and release all resources
     */
    virtual void Destroy() noexcept = 0;

    /**
     * @brief Export the local link information, the given struct is filled by this call
     *
     * @param flags            [in] optional flags, should be 0 currently
     * @param info             [out] link information to be filled
     * @return GEN_OK if successful, others if failed
     */
    virtual Result Export(uint32_t flags, genetty_link_info_t *info) = 0;

    /**
     * @brief Import the link information exported by the peer, and make the connection ready
     *
     * @param flags            [in] optional flags, should be 0 currently
     * @param info             [in] link information exported by the peer
     * @return GEN_OK if successful, others if failed
     */
    virtual Result Import(uint32_t flags, genetty_link_info_t *info) = 0;

    /**
     * @brief Post a batch of read or write operations to this genetty
     *
     * @param op_data          [in] array of the operations, each item carries the local and remote address
     * @param batchSize        [in] count of the operations in op_data
     * @param options          [in] options of this batch, such as the immediate value and the signal flag
     * @return GEN_OK if successful, others if failed
     */
    virtual Result BatchPost(genetty_batch_op_data_t *op_data, uint32_t batchSize,
                             genetty_batch_op_options_t *options) noexcept = 0;

    /**
     * @brief Get the information of this genetty which can be accessed by the device, for the device centric
     *        communication, such as the address of the send queue and the door bell
     *
     * @param flags            [in] optional flags, should be 0 currently
     * @param info             [out] device access information of this genetty
     * @return GEN_OK if successful, others if failed
     */
    virtual Result GetDeviceAccessInfo(uint32_t flags, genetty_device_access_info_t *info) = 0;

    /**
     * @brief Get the unique id of this genetty
     *
     * @return the unique id, 0 if the genetty is not initialized
     */
    uint64_t Id() const noexcept;

    /**
     * @brief Get the genetty type of this genetty
     *
     * @return the genetty type
     */
    genetty_type_t Type() const noexcept;

protected:
    uint64_t id_ = 0;
    const genetty_type_t type_; /* genetty type, set by the subclass */
};

/* inline function of GenettyCQ */
ALWAYS_INLINE GenettyCQ::GenettyCQ(genetty_type_t t) noexcept : type_(t) {}

ALWAYS_INLINE uint64_t GenettyCQ::Id() const noexcept
{
    return id_;
}

ALWAYS_INLINE genetty_type_t GenettyCQ::Type() const noexcept
{
    return type_;
}

/* inline function of Genetty */
ALWAYS_INLINE Genetty::Genetty(genetty_type_t t) noexcept : type_(t) {}

ALWAYS_INLINE uint64_t Genetty::Id() const noexcept
{
    return id_;
}

ALWAYS_INLINE genetty_type_t Genetty::Type() const noexcept
{
    return type_;
}

std::string ProtocolStr(PROTOCOL protocol) noexcept;
std::string CtxPolicyStr(CONTEXT_POLICY policy) noexcept;
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_TYPES_H
