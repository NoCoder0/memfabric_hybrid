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
#ifndef MF_OPENDCC_GENETTY_VERBS_DEVICE_WRAPPER_H
#define MF_OPENDCC_GENETTY_VERBS_DEVICE_WRAPPER_H

#include <cstdint>
#include <memory>
#include <string>

#include "genetty_info_types.h"
#include "genetty_verbs_types.h"
#include "genetty_verbs_device_queue.h"

namespace ock {
namespace openDCC {
namespace verbs {
struct DeviceVerbsMR {
    void *handle = nullptr; /* HCCP MR handle */
    uint64_t address = 0;   /* address of the memory region */
    uint64_t size = 0;      /* size of the memory region */
    uint32_t lkey = 0;      /* local access key */
    uint32_t rkey = 0;      /* remote access key */
    uint32_t references = 1;
};

class DeviceRdmaContext;
using DeviceRdmaContextPtr = Ref<DeviceRdmaContext>;

class DeviceRdmaContext : public Referable {
public:
    /* An empty device IP selects the IPv4 address of eth<physicalDeviceId>. */
    static Result CreateContext(uint32_t userDeviceId, const std::string &deviceIp, DeviceRdmaContextPtr &out) noexcept;

    /**
     * @brief Retry unfinished memory and device cleanup before unloading the runtime libraries
     *
     * @return GEN_OK if successful, others if cleanup failed
     */
    static Result RetryPendingCleanup() noexcept;

public:
    DeviceRdmaContext();
    ~DeviceRdmaContext() override;

    void Destroy() noexcept;
    /* Take ownership of the MRs and release them before closing the device. */
    void Destroy(std::map<MemoryInfo, DeviceVerbsMR> &mrs) noexcept;

    Result CreateQueue(const GenettyOptions &options, uint32_t cqDepth, uint64_t id, DeviceVerbsQueuePtr &out) noexcept;
    Result DestroyQueue(const DeviceVerbsQueuePtr &queue) noexcept;
    bool HasPendingQueueCleanup() noexcept;
    void *Handle() const noexcept;
    uint32_t UserDeviceId() const noexcept;
    uint32_t LogicDeviceId() const noexcept;
    uint32_t PhysicalDeviceId() const noexcept;
    const std::string &Ip() const noexcept;

private:
    struct PendingCleanupTag {};
    explicit DeviceRdmaContext(PendingCleanupTag) {}

    static Result InitializeApis() noexcept;
    Result Initialize(uint32_t userDeviceId, const std::string &deviceIp) noexcept;
    Result DestroyAllQueues() noexcept;
    Result UnRegisterAllMemory() noexcept;
    Result TryDestroy() noexcept;
    void KeepPendingCleanup() noexcept;

    /* Select the current thread's device after ACL initialization. */
    Result SelectDevice(uint32_t userDeviceId) noexcept;

    /* Query IDs after ACL initialization and device selection. */
    Result QueryDeviceIds() noexcept;

    /* Query the device RDMA IP after RA initialization. */
    Result QueryDeviceIp() noexcept;

    /* Open TSD after ACL initialization and device selection. */
    Result OpenTsd() noexcept;

    /* Close TSD after the RDMA device is closed and RA is deinitialized. */
    Result CloseTsd() noexcept;

    /* Initialize RA after TSD is opened and the physical device ID is prepared. */
    Result InitRa() noexcept;

    /* Deinitialize RA after the RDMA device has been closed. */
    Result DeinitRa() noexcept;

    /* Open the RDMA device after TSD and RA initialization, with device IDs and IP prepared. */
    Result OpenRdmaDevice() noexcept;

    /* Close the RDMA device after its QPs and memory registrations have been released. */
    Result CloseRdmaDevice() noexcept;

private:
    void *rdmaHandle_ = nullptr;             /* HCCP RDMA device handle */
    bool tsdOpened_ = false;                 /* TSD open state of this context */
    bool raInitialized_ = false;             /* RA initialization state of this context */
    uint32_t userDeviceId_ = UINT32_MAX;     /* user device id used by ACL and TSD */
    uint32_t logicDeviceId_ = UINT32_MAX;    /* logical device id */
    uint32_t physicalDeviceId_ = UINT32_MAX; /* physical device id used by RA and Rdev */
    std::string deviceIp_;                   /* local device RDMA IP */

    std::mutex queueMutex_;
    bool closing_ = false;
    std::map<DeviceVerbsQueue *, DeviceVerbsQueuePtr> queues_; /* owned HCCP QPs and TS resources */
    std::map<MemoryInfo, DeviceVerbsMR> pendingMrs_;           /* memory regions awaiting cleanup */

    /* Reserve the cleanup owner before acquiring resources; destruction must not allocate memory. */
    std::unique_ptr<DeviceRdmaContext> cleanupContext_;
    DeviceRdmaContext *pendingNext_ = nullptr;
    static DeviceRdmaContext *PENDING_CLEANUP;
    static std::mutex PENDING_CLEANUP_MUTEX;
};

ALWAYS_INLINE DeviceRdmaContext::~DeviceRdmaContext()
{
    Destroy();
}

ALWAYS_INLINE void *DeviceRdmaContext::Handle() const noexcept
{
    return rdmaHandle_;
}

ALWAYS_INLINE uint32_t DeviceRdmaContext::UserDeviceId() const noexcept
{
    return userDeviceId_;
}

ALWAYS_INLINE uint32_t DeviceRdmaContext::LogicDeviceId() const noexcept
{
    return logicDeviceId_;
}

ALWAYS_INLINE uint32_t DeviceRdmaContext::PhysicalDeviceId() const noexcept
{
    return physicalDeviceId_;
}

ALWAYS_INLINE const std::string &DeviceRdmaContext::Ip() const noexcept
{
    return deviceIp_;
}

ALWAYS_INLINE void DeviceRdmaContext::Destroy() noexcept
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        closing_ = true;
    }
    (void)TryDestroy();
    if (!queues_.empty() || !pendingMrs_.empty() || rdmaHandle_ != nullptr || raInitialized_ || tsdOpened_) {
        KeepPendingCleanup();
    }
}
} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_GENETTY_VERBS_DEVICE_WRAPPER_H
