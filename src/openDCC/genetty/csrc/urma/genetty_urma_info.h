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
#ifndef MF_GENETTY_URMA_INFO_H
#define MF_GENETTY_URMA_INFO_H

#include "genetty_host_urma_wrapper.h"
#include "genetty_info_types.h"

namespace ock {
namespace openDCC {
namespace urma {

constexpr char URMA_INFO_JETTY_EID[] = "openDCC.urma.jetty.eid";
constexpr char URMA_INFO_JETTY_UASID[] = "openDCC.urma.jetty.uasid";
constexpr char URMA_INFO_JETTY_ID[] = "openDCC.urma.jetty.id";
constexpr char URMA_INFO_JETTY_TOKEN[] = "openDCC.urma.jetty.token";

/**
 * @brief Information of a local jetty, which is exchanged with the peer to establish the connection
 */
class UrmaLinkInfo final : public LinkInfo {
public:
    Result Export(genetty_link_info_t *info, uint32_t flags) noexcept override;
    Result Import(genetty_link_info_t *info, uint32_t flags) noexcept override;

    const urma_jetty_id_t &JettyId() const noexcept;
    void JettyId(const urma_jetty_id_t &jettyId) noexcept;

    uint32_t Token() const noexcept;
    void Token(uint32_t token) noexcept;

private:
    urma_jetty_id_t jettyId_{}; /* id of the local jetty */
    uint32_t token_ = 0;        /* token of the local jetty */
};

ALWAYS_INLINE const urma_jetty_id_t &UrmaLinkInfo::JettyId() const noexcept
{
    return jettyId_;
}

ALWAYS_INLINE void UrmaLinkInfo::JettyId(const urma_jetty_id_t &jettyId) noexcept
{
    jettyId_ = jettyId;
}

ALWAYS_INLINE uint32_t UrmaLinkInfo::Token() const noexcept
{
    return token_;
}

ALWAYS_INLINE void UrmaLinkInfo::Token(uint32_t token) noexcept
{
    token_ = token;
}
} // namespace urma
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_URMA_INFO_H
