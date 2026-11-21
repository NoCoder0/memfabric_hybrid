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
#ifndef MF_GENETTY_URMA_HOST_CTX_POOL_H
#define MF_GENETTY_URMA_HOST_CTX_POOL_H

#include <map>
#include <mutex>
#include <vector>

#include "genetty_host_urma_wrapper.h"
#include "genetty_info_types.h"
#include "genetty_types.h"

namespace ock {
namespace openDCC {
namespace urma {
class UrmaHostContext final : public GenettyContext {
public:
    UrmaHostContext(genetty_type_t type, const UrmaContextPtr &context) noexcept;
    ~UrmaHostContext() override;

    Result RegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept override;
    Result UnRegisterMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept override;
    Result ImportMemory(genetty_memory_segment_t *segment, uint32_t flags) noexcept override;

    void Destroy() noexcept override;

    const UrmaContextPtr &Context() const noexcept;

    void SetPeerEid(const urma_eid_t &eid) noexcept;
    const urma_eid_t &PeerEid() const noexcept;

    urma_target_seg_t *FindLocalSeg(uint32_t lkey) noexcept;
    urma_target_seg_t *FindRemoteSeg(uint32_t rkey) noexcept;

private:
    static MemoryInfo MakeMemoryInfo(const genetty_memory_segment_t &segment) noexcept;

private:
    const UrmaContextPtr ctx_;                                     /* hold the reference */
    std::map<MemoryInfo, urma_target_seg_t *> localSegs_;          /* registered segments */
    std::map<MemoryInfo, urma_target_seg_t *> remoteSegs_;         /* imported segments */
    std::map<uint32_t, urma_target_seg_t *> localSegsByToken_;     /* registered segments by token */
    std::map<uint32_t, urma_target_seg_t *> remoteSegsByToken_;    /* imported segments by token */
    std::mutex mutex_;                                             /* mutex of the segments */
    urma_eid_t peerEid_{};                                         /* eid of the peer, set by the genetty import */
    bool inited_ = false;                                          /* init flag */
};

ALWAYS_INLINE const UrmaContextPtr &UrmaHostContext::Context() const noexcept
{
    return ctx_;
}

ALWAYS_INLINE void UrmaHostContext::SetPeerEid(const urma_eid_t &eid) noexcept
{
    peerEid_ = eid;
}

ALWAYS_INLINE const urma_eid_t &UrmaHostContext::PeerEid() const noexcept
{
    return peerEid_;
}

using UrmaHostContextPtr = Ref<UrmaHostContext>;

class UrmaHostContextPool : public GenettyContextPool {
public:
    UrmaHostContextPool() = default;
    ~UrmaHostContextPool() override;

    Result AddAllDevices() noexcept override;

    void CloseAllDevices() noexcept override;

    void RecordBadPort(uint8_t badPortId) noexcept override;

    /**
     * @brief Select context by the device id
     *
     * @param deviceId     [in] id of the device, UINT16_MAX means any device
     * @param ctx          [out] context selected
     * @return GEN_OK if successful, others if failed
     */
    Result SelectContext(uint16_t deviceId, UrmaHostContextPtr &ctx) noexcept;

    /**
     * @brief Select context by the preferred eid
     *
     * @param eidWithMask  [in] eid text of the device, empty means any eid
     * @param ctx          [out] context selected
     * @return GEN_OK if successful, others if failed
     */
    Result SelectContext(const std::string &eidWithMask, UrmaHostContextPtr &ctx) noexcept;

    /**
     * @brief Look up the context by the context handle passed in
     *
     * @param ctx          [in] context handle created from this pool
     * @param out          [out] the found context
     * @return GEN_OK if successful, GEN_NOT_FOUND if the handle is not found
     */
    Result Lookup(genetty_context_t ctx, UrmaHostContextPtr &out) noexcept;

    uint32_t Count() noexcept;

private:
    void CloseAllDevicesInner() noexcept;

    bool IsBadPort(uint16_t port) const noexcept;

private:
    std::mutex mutex_;                      /* context mutex */
    bool inited_ = false;                   /* inited or not */
    std::map<uint16_t, uint64_t> badPorts_; /* bad port with ttl */
    std::vector<UrmaHostContextPtr> contexts_; /* all contexts */
    std::vector<uint16_t> deviceIds_;          /* device id of each context */
};
using UrmaHostContextPoolPtr = Ref<UrmaHostContextPool>;

ALWAYS_INLINE UrmaHostContextPool::~UrmaHostContextPool()
{
    CloseAllDevices();
}
} // namespace urma
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_URMA_HOST_CTX_POOL_H
