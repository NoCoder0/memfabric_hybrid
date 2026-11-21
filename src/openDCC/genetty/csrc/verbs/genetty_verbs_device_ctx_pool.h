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
#ifndef MF_OPENDCC_GENETTY_VERBS_CONTEXT_DEVICE_H
#define MF_OPENDCC_GENETTY_VERBS_CONTEXT_DEVICE_H

#include "genetty_info_types.h"
#include "genetty_verbs_types.h"
#include "genetty_verbs_device_wrapper.h"

namespace ock {
namespace openDCC {
namespace verbs {
class DeviceVerbsContext : public GenettyContext {
public:
    explicit DeviceVerbsContext(genetty_type_t type, const DeviceRdmaContextPtr &context)
        : GenettyContext(type), wrapCtx_(context)
    {}
    ~DeviceVerbsContext() noexcept override
    {
        Destroy();
    }

    Result RegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept override;

    Result UnRegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept override;

    Result ImportMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept override;

    void Destroy() noexcept override;

public:
    const DeviceRdmaContextPtr wrapCtx_;
    std::map<MemoryInfo, DeviceVerbsMR> mrs_; /* HCCP MR records, key is the memory info */
    std::mutex mutex_;                        /* mutex of the registered memory regions */
};
using DeviceVerbsContextPtr = Ref<DeviceVerbsContext>;

class DeviceVerbsContextPool : public GenettyContextPool {
public:
    DeviceVerbsContextPool() = default;
    ~DeviceVerbsContextPool() override;

    Result AddAllDevices() noexcept override;
    Result AddDevice(uint16_t logicDeviceId) noexcept;

    void CloseAllDevices() noexcept override;

    void RecordBadPort(uint8_t badPortId) noexcept override;

    /**
     * @brief Select context by the device id
     *
     * @param deviceId     [in] logical id of the device, UINT16_MAX means any device
     * @param ctx          [out] context selected
     * @return GEN_OK if successful, others if failed
     */
    Result SelectContext(uint16_t deviceId, DeviceVerbsContextPtr &ctx) noexcept;

    /**
     * @brief Select context by the preferred ip
     *
     * @param ipWithMask   [in] ip with mask 127.0.0.1/24 etc, empty means any ip
     * @param ctx          [out] context selected
     * @return GEN_OK if successful, others if failed
     */
    Result SelectContext(const std::string &ipWithMask, DeviceVerbsContextPtr &ctx) noexcept;

    /**
     * @brief Look up the context by the context handle passed in
     *
     * @param ctx          [in] context handle created from this pool
     * @param out          [out] the found context
     * @return GEN_OK if successful, GEN_NOT_FOUND if the handle is not found
     */
    Result Lookup(genetty_context_t ctx, DeviceVerbsContextPtr &out) noexcept;

private:
    Result AddDevices(uint32_t deviceCount) noexcept;

    void CloseAllDevicesInner() noexcept;

    bool IsBadPort(uint16_t port) const noexcept;

private:
    std::mutex mutex_;                            /* context mutex */
    bool inited_ = false;                         /* inited or not */
    std::map<uint16_t, uint64_t> badPorts_;       /* bad port with ttl */
    std::vector<DeviceVerbsContextPtr> contexts_; /* all context */
};
using DeviceVerbsContextPoolPtr = Ref<DeviceVerbsContextPool>;

ALWAYS_INLINE DeviceVerbsContextPool::~DeviceVerbsContextPool()
{
    CloseAllDevices();
}
} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_GENETTY_VERBS_CONTEXT_DEVICE_H
