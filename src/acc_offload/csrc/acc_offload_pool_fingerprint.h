/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the Mulan PSL v2 for
 * more details.
 */
#ifndef MEMFABRIC_HYBRID_ACC_OFFLOAD_POOL_FINGERPRINT_H
#define MEMFABRIC_HYBRID_ACC_OFFLOAD_POOL_FINGERPRINT_H

#include <cstdint>
#include "hybm_define.h"
#include "acc_offload.h"
#include "acc_offload_define.h"

namespace ock {
namespace offload {

/* Whole-pool consistency payload, exchanged once via group AllGather after
 * hybm_mmap (hostGva_ only has a value then). One round covers every
 * whole-pool invariant; the verdict is spread with GroupGatherResult so every
 * rank fails together instead of timing out in a later barrier. */
struct PoolFingerprint {
    uint64_t poolGva;        /* whole-pool GVA base this rank sees; must be identical */
    uint64_t reserveSize;    /* slot virtual size (= slotStride); must be identical */
    uint64_t allocSize;      /* per-rank physical DRAM size; ranks MAY differ (each rank's own
                                allocSize, scenario decides); diagnostic only — excluded from
                                the identity check below */
    uint32_t worldSize;      /* global rank total; must be identical */
    uint32_t localWorldSize; /* ranks per machine; must be identical */
    uint32_t globalRank;     /* self report; gathered set must be {0..worldSize-1} */
    uint32_t socType;        /* AscendSocType value; identical, multi-node accepts A3 only */
};

/* localWorldSize == 0 means "single machine": normalize to worldSize so the
 * legacy default config and an explicit localWorldSize == worldSize compare
 * equal inside the fingerprint check. */
inline uint32_t NormalizeLocalWorldSize(uint32_t worldSize, uint32_t localWorldSize) noexcept
{
    return localWorldSize == 0 ? worldSize : localWorldSize;
}

inline PoolFingerprint MakePoolFingerprint(uint64_t poolGva, uint64_t reserveSize, uint64_t allocSize,
                                           uint32_t worldSize, uint32_t localWorldSize, uint32_t globalRank,
                                           uint32_t socType) noexcept
{
    PoolFingerprint fp{};
    fp.poolGva = poolGva;
    fp.reserveSize = reserveSize;
    fp.allocSize = allocSize;
    fp.worldSize = worldSize;
    fp.localWorldSize = localWorldSize;
    fp.globalRank = globalRank;
    fp.socType = socType;
    return fp;
}

/* Log one whole-pool mismatch with both sides' key identifiers (rank / poolGva /
 * slot sizes / world sizes / socType) so the offending rank and field are
 * locatable from a single rank's log. allocSize is deliberately absent: it may
 * differ across ranks by design and must not read as the mismatch cause. */
inline void LogPoolFingerprintMismatch(uint32_t rank, const PoolFingerprint &self, const PoolFingerprint &ref)
{
    OFFLOAD_LOG_ERROR("pool fingerprint mismatch, rank: "
                      << rank << ", poolGva(self): " << self.poolGva << ", poolGva(rank0): " << ref.poolGva
                      << ", reserveSize(self): " << self.reserveSize << ", reserveSize(rank0): " << ref.reserveSize
                      << ", worldSize(self): " << self.worldSize << ", worldSize(rank0): " << ref.worldSize
                      << ", localWorldSize(self): " << self.localWorldSize
                      << ", localWorldSize(rank0): " << ref.localWorldSize << ", socType(self): " << self.socType
                      << ", socType(rank0): " << ref.socType);
}

/* Check the gathered fingerprints (index r = rank r's self report, as placed
 * by GroupAllGather): every layout-defining field identical (allocSize is NOT
 * one of them — each rank allocates its own physical DRAM and ranks may differ,
 * only an INFO line is emitted so mixed-size pools are visible in the logs),
 * globalRank set exactly {0..worldSize-1}, and a multi-node pool
 * (localWorldSize < worldSize) A3-only. Pure function on the gathered array;
 * failures log the root cause here, callers only propagate. */
inline int32_t ValidatePoolFingerprints(const PoolFingerprint *fingerprints, uint32_t rankCount) noexcept
{
    if (fingerprints == nullptr || rankCount == 0) {
        OFFLOAD_LOG_ERROR("invalid pool fingerprint input, fingerprints is null: " << (fingerprints == nullptr)
                                                                                   << ", rankCount: " << rankCount);
        return OFFLOAD_ERROR;
    }
    const PoolFingerprint &ref = fingerprints[0];
    if (ref.worldSize != rankCount) {
        OFFLOAD_LOG_ERROR("pool fingerprint worldSize != rankCount, worldSize: "
                          << ref.worldSize << ", rankCount: " << rankCount << ", poolGva: " << ref.poolGva);
        return OFFLOAD_ERROR;
    }
    uint32_t mixedAllocRanks = 0;
    for (uint32_t rank = 0; rank < rankCount; rank++) {
        const PoolFingerprint &fp = fingerprints[rank];
        bool identical = fp.poolGva == ref.poolGva && fp.reserveSize == ref.reserveSize &&
                         fp.worldSize == ref.worldSize && fp.localWorldSize == ref.localWorldSize &&
                         fp.socType == ref.socType;
        if (!identical) {
            LogPoolFingerprintMismatch(rank, fp, ref);
            return OFFLOAD_ERROR;
        }
        if (fp.allocSize != ref.allocSize) {
            mixedAllocRanks++;
        }
        /* slot = global rank: a duplicated orchestration id would make two ranks
         * write the same slot silently — intercept it at init time. */
        if (fp.globalRank != rank) {
            OFFLOAD_LOG_ERROR("pool fingerprint globalRank mismatch (duplicate or missing rank in the pool "
                              "orchestration), rank: "
                              << rank << ", reported globalRank: " << fp.globalRank << ", worldSize: " << fp.worldSize
                              << ", poolGva: " << fp.poolGva);
            return OFFLOAD_ERROR;
        }
    }
    if (mixedAllocRanks > 0) {
        OFFLOAD_LOG_INFO("pool ranks report different allocSize (allowed by design, equality is scenario-decided), "
                         "allocSize(rank0): "
                         << ref.allocSize << ", ranksDifferingFromRank0: " << mixedAllocRanks
                         << ", worldSize: " << ref.worldSize << ", poolGva: " << ref.poolGva);
    }
    if (ref.localWorldSize != ref.worldSize && ref.socType != ock::mf::ASCEND_910C) {
        OFFLOAD_LOG_ERROR("multi-node pool only supports A3 (Ascend910C) nodes, socType: "
                          << ref.socType << ", worldSize: " << ref.worldSize
                          << ", localWorldSize: " << ref.localWorldSize << ", poolGva: " << ref.poolGva);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

} // namespace offload
} // namespace ock

#endif // MEMFABRIC_HYBRID_ACC_OFFLOAD_POOL_FINGERPRINT_H
