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

#ifndef SMEM_SMEM_GROUP_MANAGER_DEF_H
#define SMEM_SMEM_GROUP_MANAGER_DEF_H

#include <cctype>
#include <string>
#include <vector>
#include <ostream>
#include <functional>
#include "smem_ref.h"

namespace ock::smem {

// Opaque byte blob for serialized rank metadata (e.g. endpoint, capabilities).
using Bytes = std::vector<uint8_t>;

// RankFullInfo::protocol values — identifies which module owns a rank membership.
constexpr uint8_t SMEM_RANK_PROTOCOL_DEFAULT = 0; // BM / unspecified
constexpr uint8_t SMEM_RANK_PROTOCOL_TRANS = 1;   // TRANS

// Collection of Bytes, used for multi-address / multi-URL scenarios.
using MultiBytes = std::vector<Bytes>;

struct RankBaseInfo {
    uint32_t rankId;
    Bytes baseInfo;
    explicit RankBaseInfo(const uint32_t id = 0) noexcept : rankId{id} {}
};

struct RankFullInfo : RankBaseInfo {
    MultiBytes externalInfo;
    // Protocol that owns this rank's membership. 0 = default/BM, 1 = TRANS.
    // Carried in the JOINREQ body so the server can tell a Trans re-join
    // (which always sends a fresh JOINREQ) apart from a stale/duplicate one.
    uint8_t protocol{0};
    explicit RankFullInfo(const uint32_t id = 0) noexcept : RankBaseInfo{id} {}
};

inline std::ostream &operator<<(std::ostream &os, const Bytes &bytes)
{
    os << "[";
    for (auto b : bytes) {
        if (isprint(b)) {
            os << b;
        } else {
            os << "\\x" << std::hex << static_cast<int>(b) << std::dec;
        }
    }
    os << "]";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const RankBaseInfo &info)
{
    os << "{rankId=" << info.rankId << ", baseInfo=" << info.baseInfo << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const RankFullInfo &info)
{
    os << "{rankId=" << info.rankId << ", baseInfo=" << info.baseInfo;
    os << ", externalInfo=[";
    for (size_t i = 0; i < info.externalInfo.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << info.externalInfo[i];
    }
    os << "]}";
    return os;
}

using SmemGroupCommandSender = std::function<int(uint32_t targetRankId, const std::vector<uint8_t> &data)>;

/**
 * @brief Control operation sub-type carried in SmemMessage with mt == CONTROL.
 */
enum ControlOp : int8_t {
    CONTROL_ADD_TO_WHITELIST = 0,
    CONTROL_REMOVE_FROM_WHITELIST = 1,
    CONTROL_ESTABLISH_CONNECTION = 2,
    CONTROL_CLOSE_CONNECTION = 3,
    CONTROL_JOIN = 4,
    CONTROL_LEAVE = 5,
    CONTROL_QUERY_LINK_STATE = 6,
    CONTROL_LINK_STATE_RESPONSE = 7,
    CONTROL_ADD_TO_WHITELIST_ACK = 8,
    CONTROL_REMOVE_FROM_WHITELIST_ACK = 9,
    CONTROL_ESTABLISH_CONNECTION_ACK = 10,
    CONTROL_CLOSE_CONNECTION_ACK = 11,
    CONTROL_LEAVE_NOTIFY = 12,
    CONTROL_PROMOTE_TO_ACTIVE = 13,
    CONTROL_EXTEND_MEMORY = 14,  // client → server: extend memory with new slices
    CONTROL_ADD_SLICES = 15,     // server → clients: import new slices for a rank
    CONTROL_ADD_SLICES_ACK = 16, // client → server: ack add slices
};

struct LinkStateEntry {
    uint32_t dstRankId{0};
    int8_t state{0};
};

template<typename T>
inline std::string JoinRankIds(const T &items) noexcept
{
    std::string s;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            s += ",";
        }
        s += std::to_string(items[i].rankId);
    }
    return s;
}

inline std::string JoinRankIds(const std::vector<uint32_t> &ids) noexcept
{
    std::string s;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            s += ",";
        }
        s += std::to_string(ids[i]);
    }
    return s;
}

} // namespace ock::smem

#endif // SMEM_SMEM_GROUP_MANAGER_DEF_H
