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

#ifndef MEMFABRIC_HYBRID_SMEM_TYPES_H
#define MEMFABRIC_HYBRID_SMEM_TYPES_H

#include <cstdint>
#include <ostream>
#include <string>

#include "smem.h"

namespace ock {
namespace smem {
using Result = int32_t;

enum SMErrorCode : int32_t {
    SM_OK = 0,
    SM_ERROR = -1,
    SM_INVALID_PARAM = -2000,
    SM_MALLOC_FAILED = -2001,
    SM_NEW_OBJECT_FAILED = -2002,
    SM_NOT_STARTED = -2003,
    SM_TIMEOUT = -2004,
    SM_REPEAT_CALL = -2005,
    SM_DUPLICATED_OBJECT = -2006,
    SM_OBJECT_NOT_EXISTS = -2007,
    SM_NOT_INITIALIZED = -2008,
    SM_RESOURCE_IN_USE = -2009,
    SM_RECONNECT = -2010,
    SM_GET_OBJIECT = -2011,
    SM_PARTIAL_FAILED = -2012,
    SM_INNER_BUSY = -2013,
    SM_NOT_CONNECTED = -2014,
};

constexpr int32_t N16 = 16;
constexpr int32_t N64 = 64;
constexpr int32_t N256 = 256;
constexpr int32_t N1024 = 1024;
constexpr int32_t N8192 = 8192;

constexpr uint32_t UN2 = 2;
constexpr uint32_t UN32 = 32;
constexpr uint32_t UN58 = 58;
constexpr uint32_t UN128 = 128;
constexpr uint32_t UN65536 = 65536;
constexpr uint32_t UN16777216 = 16777216;

constexpr uint32_t SMEM_DEFAUT_WAIT_TIME = 120;           // 120s
constexpr uint64_t SMEM_1G_SIZE = 1ULL << 30;             // 1G
constexpr uint64_t SMEM_LOCAL_HBM_SIZE_MAX = 64ULL << 30; // 64G
constexpr uint64_t SMEM_LOCAL_DRAM_SIZE_MAX = 2ULL << 40; // 2T
constexpr uint64_t HYBM_MAX_POOL_SIZE = 128ULL << 40;     // 128T, 910C暂时只做到128TB, 910B无此限制
constexpr uint64_t HYBM_HBM_SIZE_ALIGNMENT = 1ULL << 30;  // 1GB

constexpr uint32_t SMEM_GROUP_RETRY_TIME = 5U;
constexpr uint32_t MF_GROUP_JOIN_DEFAULT_TIMEOUT = 900U; // 集群加入默认超时时间900s

/*
 * Dynamic group membership: the joined ranks are tracked as a bitmap, one bit per rank.
 */
constexpr uint32_t MAX_RANK_COUNT = SMEM_WORLD_SIZE_MAX;
constexpr uint32_t BITS_COUNT_IN_U64 = 64U;
constexpr uint32_t RANK_BITS_U64_COUNT = MAX_RANK_COUNT / BITS_COUNT_IN_U64;

/*
 * Key of the group listen event, shared by the group engine (writer) and the
 * config store server (reader during recovery). The key is stored on the core
 * store, so every access must go through ConfigStore::GetCoreStore().
 */
inline const std::string SMEM_GROUP_LISTEN_EVENT_KEY = "MF_SMEM_GROUP_EVENT";

/*
 * Payload of the group event: the serialized bytes of SmemGroupInfo.
 */
#pragma pack(push, 4)
struct SmemGroupInfo {
    // dynamic info
    uint32_t version;
    uint32_t groupSize;
    uint32_t curEvent;
    uint32_t targetRank;
    uint32_t submitRank;
    uint64_t joinedRanksBitmap[RANK_BITS_U64_COUNT]; // bit set means the rank is in the group

    friend std::ostream &operator<<(std::ostream &os, const SmemGroupInfo &obj)
    {
        os << "SmemGroupInfo{size:" << obj.groupSize << " event:" << obj.curEvent << " target:" << obj.targetRank
           << " src:" << obj.submitRank << " ver:" << obj.version << " mask:";
        for (uint32_t i = 0; i < RANK_BITS_U64_COUNT; i++) {
            os << std::hex << " " << obj.joinedRanksBitmap[i];
        }
        os << "}";
        return os;
    }
};
#pragma pack(pop)
} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_TYPES_H
