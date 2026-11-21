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
#ifndef MF_VERBS_EXCHANGE_INFO_H
#define MF_VERBS_EXCHANGE_INFO_H

#include "genetty_def.h"
#include "genetty_info_types.h"
#include "genetty_types.h"
#include "genetty_verbs_types.h"

namespace ock {
namespace openDCC {
namespace verbs {

constexpr char VERBS_INFO_QP_PSN[] = "openDCC.verbs.qp.psn";
constexpr char VERBS_INFO_QP_MTU[] = "openDCC.verbs.qp.mtu";
constexpr char VERBS_INFO_QP_QPN[] = "openDCC.verbs.qp.qpn";
constexpr char VERBS_INFO_QP_LID[] = "openDCC.verbs.qp.lid";
constexpr char VERBS_INFO_QP_GID[] = "openDCC.verbs.qp.gid";
constexpr char VERBS_INFO_QP_PORT[] = "openDCC.verbs.qp.port";
constexpr char VERBS_INFO_QP_SQ_DEPTH[] = "openDCC.verbs.qp.sq_depth";
constexpr char VERBS_INFO_QP_RQ_DEPTH[] = "openDCC.verbs.qp.rq_depth";

class VerbsLinkInfo final : public LinkInfo {
public:
    virtual ~VerbsLinkInfo() = default;

    Result Export(genetty_link_info_t *info, uint32_t flags) noexcept override;
    Result Import(genetty_link_info_t *info, uint32_t flags) noexcept override;

    uint32_t Psn() const noexcept
    {
        return psn_;
    }
    void Psn(uint32_t psn) noexcept
    {
        psn_ = psn;
    }
    uint32_t Mtu() const noexcept
    {
        return mtu_;
    }
    void Mtu(uint32_t mtu) noexcept
    {
        mtu_ = mtu;
    }
    uint32_t Qpn() const noexcept;
    void Qpn(uint32_t qpn) noexcept;

    uint16_t Lid() const noexcept;
    void Lid(uint16_t lid) noexcept;

    uint8_t PortNum() const noexcept;
    void PortNum(uint8_t portNum) noexcept;

    uint32_t SqDepth() const noexcept;
    void SqDepth(uint32_t sqDepth) noexcept;

    uint32_t RqDepth() const noexcept;
    void RqDepth(uint32_t rqDepth) noexcept;

    const uint8_t *Gid() const noexcept;
    void Gid(const uint8_t *gid) noexcept;

    friend std::ostream &operator<<(std::ostream &os, const VerbsLinkInfo &info);

private:
    uint32_t psn_ = 0;               /* send packet sequence number */
    uint32_t mtu_ = 0;               /* ibv_mtu value, zero until Device QP reports its path MTU */
    uint16_t lid_ = 0;               /* local id */
    uint8_t portNum_ = 0;            /* port number */
    uint32_t qpn_ = 0;               /* qp id */
    uint32_t sq_depth_ = 0;          /* depth of send queue */
    uint32_t rq_depth_ = 0;          /* depth of receive queue */
    uint8_t gid_[ROCE_GID_LEN] = {}; /* gid */
};

ALWAYS_INLINE uint32_t VerbsLinkInfo::Qpn() const noexcept
{
    return qpn_;
}

ALWAYS_INLINE void VerbsLinkInfo::Qpn(uint32_t qpn) noexcept
{
    qpn_ = qpn;
}

ALWAYS_INLINE uint16_t VerbsLinkInfo::Lid() const noexcept
{
    return lid_;
}

ALWAYS_INLINE void VerbsLinkInfo::Lid(uint16_t lid) noexcept
{
    lid_ = lid;
}

ALWAYS_INLINE uint8_t VerbsLinkInfo::PortNum() const noexcept
{
    return portNum_;
}

ALWAYS_INLINE void VerbsLinkInfo::PortNum(uint8_t portNum) noexcept
{
    portNum_ = portNum;
}

ALWAYS_INLINE uint32_t VerbsLinkInfo::SqDepth() const noexcept
{
    return sq_depth_;
}

ALWAYS_INLINE void VerbsLinkInfo::SqDepth(uint32_t sqDepth) noexcept
{
    sq_depth_ = sqDepth;
}

ALWAYS_INLINE uint32_t VerbsLinkInfo::RqDepth() const noexcept
{
    return rq_depth_;
}

ALWAYS_INLINE void VerbsLinkInfo::RqDepth(uint32_t rqDepth) noexcept
{
    rq_depth_ = rqDepth;
}

ALWAYS_INLINE const uint8_t *VerbsLinkInfo::Gid() const noexcept
{
    return gid_;
}

ALWAYS_INLINE void VerbsLinkInfo::Gid(const uint8_t *gid) noexcept
{
    if (gid != nullptr) {
        memcpy(gid_, gid, ROCE_GID_LEN);
    }
}
} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif // MF_VERBS_EXCHANGE_INFO_H
