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

#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>

#include "vendor/ascend/dl_hccp_def.h"
#include "provider_control_io.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

constexpr uint32_t RDMA_PROVIDER_LINK_MAGIC = 0x4E505543; // 'NPUC'
constexpr uint32_t RDMA_PROVIDER_LINK_VERSION = 2;
constexpr uint32_t RDMA_PROVIDER_LINK_NOTIFY_COMPLETION = 1;
constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(5);
constexpr uint32_t DEFAULT_RETRY_CNT = 7;
constexpr uint32_t DEFAULT_RETRY_TIME = 14;

struct RdmaProviderLinkHello {
    uint32_t magic;
    uint32_t version;
    uint32_t proxyRank;
    uint32_t proxyPhyId;
    uint32_t proxyDeviceId;
    uint32_t reserved;
};

struct RdmaProviderLinkHelloAck {
    uint32_t magic;
    uint32_t version;
    int32_t status;
    uint32_t selectedPhyId;
    uint32_t providerRank;
    uint64_t providerGvaBase;
    uint64_t totalSize;
};

struct RdmaProviderLinkQpInfo {
    uint32_t magic;
    uint32_t qpn;
    uint32_t psn;
    uint32_t gidIdx;
    uint8_t gid[HCCP_GID_RAW_LEN];
    uint32_t tc;
    uint32_t sl;
    uint32_t retryCnt;
    uint32_t retryTime;
};

struct RdmaProviderLinkMrInfo {
    uint32_t magic;
    int32_t status;
    uint64_t npuAddrBase;
    uint64_t totalSize;
    uint64_t providerGvaBase;
    uint32_t rkey;
    uint32_t selectedPhyId;
    uint64_t notifySourceAddr;
    uint32_t notifySourceRkey;
    uint32_t reserved;
};

struct RdmaProviderLinkDone {
    uint32_t magic;
    int32_t status;
};

template<typename T>
bool SendStruct(int fd, const T &value, ProviderDeadline deadline)
{
    return provider_control::SendAll(fd, &value, sizeof(T), deadline);
}

template<typename T>
bool RecvStruct(int fd, T &value, ProviderDeadline deadline)
{
    return provider_control::RecvAll(fd, &value, sizeof(T), deadline);
}

inline void FromLinkQp(const RdmaProviderLinkQpInfo &in, TypicalQp &out)
{
    out = TypicalQp{};
    out.qpn = in.qpn;
    out.psn = in.psn;
    out.gidIdx = in.gidIdx;
    std::memcpy(out.gid, in.gid, HCCP_GID_RAW_LEN);
    out.tc = in.tc;
    out.sl = in.sl;
    out.retryCnt = in.retryCnt;
    out.retryTime = in.retryTime;
}

inline RdmaProviderLinkQpInfo ToLinkQp(const TypicalQp &in, uint32_t magic)
{
    RdmaProviderLinkQpInfo out{};
    out.magic = magic;
    out.qpn = in.qpn;
    out.psn = in.psn;
    out.gidIdx = in.gidIdx;
    std::memcpy(out.gid, in.gid, HCCP_GID_RAW_LEN);
    out.tc = in.tc;
    out.sl = in.sl;
    out.retryCnt = in.retryCnt;
    out.retryTime = in.retryTime;
    return out;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
