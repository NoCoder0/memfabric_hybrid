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
#ifndef MEMFABRIC_SMEM_BM_SPARSE_H
#define MEMFABRIC_SMEM_BM_SPARSE_H

#include "smem_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct smem_bm_host_rdma_sparse_options {
    uint64_t workspaceGva;
    uint64_t workspaceBytes;
    uint64_t maxBlockBytes;
    uint32_t maxBlocks;
    uint32_t progressInterval;
    uint32_t timeoutMs;
    uint32_t gatherThreads;
    uint32_t scatterThreads;
} smem_bm_host_rdma_sparse_options_t;

/** Workspace size in bytes; zero on invalid/overflowing arguments. No allocation. */
uint64_t smem_bm_host_rdma_sparse_workspace_size(uint32_t maxBlocks, uint64_t maxBlockBytes, uint32_t links,
                                                 uint32_t progressInterval);

/**
 * Collective preparation after join on a two-rank, HOST_RDMA-only BM.
 * MF_HOST_RDMA_SPARSE_MODE must be baseline, cont or gather on both ranks.
 * Reserve a disjoint, 64-byte-aligned region of the local BM DRAM pool. Both
 * ranks must use the same workspace offset, limits, mode and progress interval.
 * One preparation per BM lifetime; workspace remains exclusive until destroy.
 * Rank 0 requests copies, rank 1 explicitly calls smem_bm_poll_host_rdma_sparse.
 * Preparation does not start a polling thread. No NPU required.
 * Load libmf_acc_offload before preparation and keep it loaded until BM destruction.
 */
int32_t smem_bm_prepare_host_rdma_sparse(smem_bm_t handle, const smem_bm_host_rdma_sparse_options_t *options);

/** Optional case preparation outside the measured loop, after sparse preparation.
 * Rank 0 passes count destination GVAs; rank 1 passes nullptr and the same count/bytes.
 * Caches local destination VAs and recreates the original gather/scatter pool for this case.
 * Call on idle peers, then synchronize before issuing requests. Does not reset request sequence.
 */
int32_t smem_bm_prepare_host_rdma_sparse_case(smem_bm_t handle, const uint64_t *destinations,
                                            uint32_t count, uint64_t blockBytes);

/**
 * Poll and synchronously process at most one request on prepared rank 1.
 * Returns 1 when processed, 0 when idle, or a negative error code.
 * timeoutMs == 0 checks once; positive values busy-poll with yield until a
 * request arrives or the idle-wait deadline expires. There is no infinite wait.
 * The timeout bounds waiting for a request, not the subsequent data transfer.
 * Creates no threads. Calls are serialized with copy/leave/destroy on this BM.
 * Stop issuing requests and finish polling before synchronizing and destroying BM.
 */
int32_t smem_bm_poll_host_rdma_sparse(smem_bm_t handle, uint32_t timeoutMs);

#ifdef __cplusplus
}
#endif
#endif
