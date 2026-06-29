/*
* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
* ZBAL is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
* See the Mulan PSL v2 for more details.
*/
#ifndef ZBAL_COMM_ALG_SELECTOR_H
#define ZBAL_COMM_ALG_SELECTOR_H

#include "zbal_comm_host_device_struct.h" /* AicpuCommType / AicpuCommAlg enums */

/* Heuristic thresholds for AllGather algorithm selection. */
constexpr uint64_t ZBAL_AICPU_ALLGATHER_RING_THRESHOLD = 4 * 1024 * 1024; /* 4 MiB */
constexpr uint32_t ZBAL_AICPU_ALLGATHER_FULLMESH_RANK = 4;                /* ≤ 4 ranks + small data → FULL_MESH */

/* Per-op execution configuration: how many cores and channels-per-core to use.
 *  Decided on host (via GetCommOpConfig), passed to device through AicpuWorkDesc.
 *  Always ≤ (ZBAL_AICPU_MAX_NUM_CORES, ZBAL_AICPU_MAX_CH_PER_CORE). */
struct OpExecConfig {
    uint32_t numCores;
    uint32_t numChPerCore;
};

/* Resolve per-op (numCores, numChPerCore) from (commType, commAlg).
 *  Single source of truth — host launcher and device use the same mapping.
 *  Bounds: result ≤ (MAX_NUM_CORES, MAX_CH_PER_CORE). */
inline OpExecConfig GetCommOpConfig(uint32_t commType, uint32_t commAlg)
{
    switch (commType) {
        case ZBAL_CMD_ALLGATHER:
            if (commAlg == ZBAL_COMM_ALG_DOUBLE_RING || commAlg == ZBAL_COMM_ALG_MESH_DOUBLE_RING) {
                return {2, 1}; /* CW/CCW each occupy one core, 1 channel per core */
            }
            return {1, 8}; /* FULL_MESH default: parallelize via channels */
        case ZBAL_CMD_ALLTOALLV:
            return {1, 8};
        case ZBAL_CMD_REDUCE_SCATTER:
            return {1, 16};
        case ZBAL_CMD_ALLREDUCE:
        case ZBAL_CMD_BROADCAST:
        case ZBAL_CMD_SCATTER:
            return {1, 8};
        case ZBAL_CMD_SEND:
        case ZBAL_CMD_RECV:
            return {1, 1};
        default:
            return {1, 1};
    }
}

/* Host-side algorithm selection — single source of truth for commAlg.
*  Host calls this BEFORE kernel launch (to look up numCores/numChPerCore via
*  GetCommOpConfig and size numBlocks). The resolved commAlg is written to
*  AicpuWorkDesc.commAlg and read directly by the device — no device-side
*  re-resolution.
*
*  AllGather heuristics:
*    rankNum ≤ 4  AND  dataSize ≤ 4 MiB → FULL_MESH       (small scale: direct peer copy)
*    rankNum > 4  AND  dataSize ≤ 4 MiB → MESH_DOUBLE_RING (small data: direct peer copy, dual ring)
*    dataSize > 4 MiB                    → DOUBLE_RING     (large data: ring forwarding)
*    rankNum ≤ 1                         → FULL_MESH       (degenerate)
*  others : FULL_MESH (default)
*
*  To add a new op's heuristic: add a `case ZBAL_CMD_XXX:` below. */
inline uint32_t SelectCommAlg(uint32_t commType, uint32_t rankNum, uint64_t dataSize)
{
    switch (commType) {
        case ZBAL_CMD_ALLGATHER:
            if (rankNum <= 1) {
                return ZBAL_COMM_ALG_FULL_MESH;
            }
            if (dataSize > ZBAL_AICPU_ALLGATHER_RING_THRESHOLD) {
                return ZBAL_COMM_ALG_DOUBLE_RING;
            }
            return (rankNum <= ZBAL_AICPU_ALLGATHER_FULLMESH_RANK) ? ZBAL_COMM_ALG_FULL_MESH
                                                                   : ZBAL_COMM_ALG_MESH_DOUBLE_RING;
        default:
            return ZBAL_COMM_ALG_FULL_MESH;
    }
}

#endif /* ZBAL_COMM_ALG_SELECTOR_H */
