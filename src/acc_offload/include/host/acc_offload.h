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
#ifndef __MEMFABRIC_ACC_OFFLOAD_H__
#define __MEMFABRIC_ACC_OFFLOAD_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CPU HOST_RDMA sparse copy; does not require offload_init or an NPU.
 * bmHandle is a joined, prepared smem_bm_t. Call smem_bm_prepare_host_rdma_sparse
 * on both ranks first, with libmf_acc_offload loaded throughout the BM lifetime.
 * Rank 1 must explicitly call smem_bm_poll_host_rdma_sparse to process requests.
 * Call this operation on rank 0: sources are rank 1 pool GVAs; destinations are
 * rank 0 pool GVAs. Every block has blockBytes bytes. Targets cannot overlap,
 * and no source/target may overlap workspace. Mode is selected at preparation
 * by MF_HOST_RDMA_SPARSE_MODE=baseline|cont|gather. Returns 0 on synchronous success.
 * Transport failure/timeout invalidates the context. Coordinate both peers
 * before BM destruction. All memory preparation remains in MF.
 */
int32_t offload_sparse_copy_host_rdma(void *bmHandle, const uint64_t *sources, const uint64_t *destinations,
                                    uint32_t count, uint64_t blockBytes);

typedef enum {
    OFFLOAD_SCENE_LOCAL = 0,  /* single-card local DRAM memory pool */
    OFFLOAD_SCENE_SHARED = 1, /* multi-card shared DRAM memory pool */
} offload_scene_t;

/**
 * @brief offload_config_t.flags bits.
 *
 * OFFLOAD_FLAG_GIANT_PAGE: allocate the dram pool in URMA-compatible mode
 * (conn-based segment: plain host va + an independent HalHostRegister device
 * mapping). Required when the pool is registered to smem_trans with
 * DEVICE_URMA for cross-node remote writes. LOCAL scene only. In this mode
 * AIV operators must use the device address from offload_get_dva() instead
 * of the malloc address.
 */
#define OFFLOAD_FLAG_GIANT_PAGE (1U << 0)

/* Upper bound on the entry_gather entry size: one entry must fit one UB
 * ping-pong slot inside the AIV kernel (ENTRY_GATHER_UB_ONCE_SIZE / 2 —
 * 120KB on A5, 88KB on A3). The bound is platform-independent: 64KB sits
 * below every supported SoC's slot. */
#define OFFLOAD_ENTRY_GATHER_MAX_ENTRY_BYTES (64u * 1024u)

typedef struct {
    uint32_t deviceId;       /* Device ID to bind */
    uint64_t reserveSize;    /* Reserved DRAM pool size in bytes (slot virtual size, whole-pool
                                                 uniform in the multi-node SHARED pool), will be
                                                 aligned up to GB. SHARED: must be identical on every
                                                 rank — it is the pool-wide slot stride the whole-pool
                                                 GVA layout is built from (enforced by the init-time
                                                 fingerprint exchange). */
    uint64_t allocSize;      /* Allocated local physical DRAM size in bytes, will be aligned
                                                 up to GB. LOCAL: must equal reserveSize. SHARED: this
                                                 rank's own contribution, must be <= reserveSize. Ranks
                                                 of one SHARED pool MAY pass different values (scenarios
                                                 that need equal allocations must pass equal values
                                                 themselves — equality is caller-guaranteed, NOT
                                                 validated across ranks). */
    uint32_t worldSize;      /* number of ranks in the group (SHARED scene): the GLOBAL rank
                                                 total across all machines of the pool. */
    uint32_t rankId;         /* GLOBAL rank id in the group (SHARED scene); 0 is the allocator
                                                 and hosts the store server; slot = global rank, so
                                                 machineRank = rankId / localWorldSize,
                                                 localRank = rankId % localWorldSize. */
    offload_scene_t scene;   /* LOCAL: single-card pool; SHARED: multi-card shared pool */
    char storeUrl[64];       /* Explicit config store url for the shared pool rendezvous
                                                 (e.g. "tcp://127.0.0.1:8500", "etcd://...", "reg://...").
                                                 Resolution order: this field > ASCEND_MF_STORE_URL env
                                                 > derived loopback port 8500 + deviceId / localWorldSize
                                                 (single machine only). */
    uint32_t flags;          /* optional flags, see OFFLOAD_FLAG_xxx; 0 by default */
    uint32_t localWorldSize; /* ranks of this machine within the pool (SHARED scene); 0 = worldSize
                                                 (single machine, backward compatible). Must divide worldSize;
                                                 all ranks of one pool must report the same value. A
                                                 multi-machine pool (< worldSize) currently accepts A3
                                                 (Ascend910C) nodes only — rejected at initialization. */
} offload_config_t;

/**
 * @brief Initialize the offload module.
 *
 * This function initializes the hybm big memory entity and loads the
 * offload library for sparse copy operations.
 *
 * SHARED scene per-rank size semantics: every rank of one pool MUST pass the
 * same reserveSize (slot virtual size / whole-pool GVA stride — validated at
 * init), while allocSize (each rank's local physical DRAM) MAY differ across
 * ranks: some scenarios allocate unequal per-rank sizes, others need every
 * rank equal. Equality of allocSize is NOT validated — a scenario that
 * requires equal allocations must guarantee it on the caller side. Per rank,
 * allocSize must be <= reserveSize; offload_malloc hands out addresses only
 * within this rank's allocSize span.
 *
 * @param config  [in] Init config, see offload_config_t.
 * @return 0 on success, non-zero error code on failure.
 */
int32_t offload_init(const offload_config_t &config);

/**
 * @brief Uninitialize the offload module.
 *
 * Releases the hybm big memory entity, unloads the extend library and
 * performs cleanup. Safe to call when not initialized.
 */
void offload_uninit();

/**
 * @brief Allocate host memory from the offload memory pool.
 *
 * Allocates a contiguous block from the pre-reserved hybm host memory.
 * The returned pointer is 16-byte aligned.
 *
 * @param size  [in] Memory size in bytes.
 * @param flags [in] optional flags
 * @return Non-zero address on success, 0 on failure.
 */
uint64_t offload_malloc(uint64_t size, uint64_t flags);

/**
 * @brief Free host memory previously allocated by offload malloc.
 *
 * Returns the memory block to the offload memory pool. The pointer must
 * have been obtained from offload malloc.
 *
 * @param ptr   [in] Address returned by offload malloc.
 * @param flags [in] optional flags
 */
void offload_free(uint64_t ptr, uint64_t flags);

/**
 * @brief Get the device virtual address (DVA) of a pool address from offload malloc.
 *
 * For URMA_POOL mode pools the DVA differs from the malloc address (an
 * independent HalHostRegister device mapping); for vmm unified pools the DVA
 * equals the malloc address. AIV operators (sparse_copy etc.) must use the DVA.
 *
 * @param hostPtr  [in] Address returned by offload malloc (or an interior address of it).
 * @param dvaPtr   [out] Device virtual address corresponding to the input address.
 * @return 0 on success, non-zero error code on failure.
 */
int32_t offload_get_dva(uint64_t hostPtr, uint64_t *dvaPtr);

/**
 * @brief Batch copy sparse data from host to device or from device to host.
 *
 * Submits a batch of h2d or d2h copy requests. Each request copies
 * data from srcPtrs[i] to dstPtrs[i] with length lenPtrs[i]. The copy is
 * executed asynchronously on the device stream.
 *
 * @param srcPtr            [in] Array of source addresses.
 * @param dstPtr            [in] Array of destination addresses.
 * @param lenPtr            [in] Array of byte counts to copy for each pair.
 * @param sizePtr           [in] Pointer to the number of entries in the arrays above.
 * @param deviceId          [in] Device ID to perform the copy on.
 * @return 0 on success, non-zero error code on failure.
 */
int32_t offload_sparse_copy(uint64_t srcPtr, uint64_t dstPtr, uint64_t lenPtr, uint64_t sizePtr, uint16_t deviceId);

/**
 * @brief Register the pool's single entry-table layout for entry_gather.
 *
 * The SHARED pool lays every rank's slot out as ONE uniform row grid: rows of
 * entryBytes back to back from the slot start; a gather of global row id `id`
 * reads pool_base + (id / rowsPerSlot) * slot_size + (id % rowsPerSlot) *
 * entryBytes. Exactly ONE layout per pool lifetime — callers with several
 * same-width tables fold their per-segment offsets into the id space. The
 * layout must be SYMMETRIC across ranks (same entryBytes/rowsPerSlot, grid
 * starting at every slot start), which lets the kernel resolve any row on any
 * rank from the row id alone. On a SHARED pool whose ranks pass different
 * allocSize values the grid must fit EVERY rank's allocated span (each rank
 * validates it against its own allocSize here). The layout is validated once
 * here: row pitch in (0, OFFLOAD_ENTRY_GATHER_MAX_ENTRY_BYTES], grid fits one
 * slot. Register after offload_init and before the first gather; cleared by
 * offload_uninit.
 *
 * @param entryBytes  [in] Row pitch in bytes (the model's row width).
 * @param rowsPerSlot [in] Rows the grid holds per slot (across all segments).
 * @return 0 on success, non-zero error code on failure.
 */
int32_t offload_register_entry_table(uint32_t entryBytes, uint32_t rowsPerSlot);

/**
 * @brief Fused random-entry gather over the registered uniform row grid.
 *
 * Gathers the rows selected by the device-resident int64 GLOBAL row ids at
 * idsPtr (< 2^32) with the layout registered above and packs the results
 * contiguously at dstPtr + i * entryBytes; no (src, dst, len) descriptor
 * table is built on the host. Like offload_sparse_copy, EVERY data parameter
 * is a device address: the entry count is read by the kernel from the device
 * memory at countPtr, so no host scalar is baked into the launch and
 * stream/graph capture replays with the count the caller last wrote (any
 * uint32 count; 0 gathers nothing).
 *
 * @param dstPtr   [in] Device address of the packed destination window; must be
 *                     entryBytes-aligned and hold room for count * entryBytes bytes.
 * @param idsPtr   [in] Device address of the int64 global row id array.
 * @param countPtr [in] Device address of a uint32 holding the number of ids to gather.
 * @param deviceId [in] Device ID to perform the copy on.
 * @return 0 on success, non-zero error code on failure.
 */
int32_t offload_entry_gather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint16_t deviceId);

/**
 * @brief Group-pack compacted copy: compact non-zero groupList entries to front.
 *
 * inputs / outputs / lens / groupList are ALL arrays of length N (= *numLocalExpertPtr). The kernel
 * scans [0, N); for the j-th non-zero entry (original index i where groupList[i] != 0,
 * j = 0..M-1), copies inputs[i] (lenPtrs[i] bytes) to outputs[j] and writes groupList[i]
 * to packedGroupList[j]. Zero groupList entries are skipped. After the call, outputs[0..M)
 * and packedGroupList[0..M) hold the compacted results; outputs[M..N) and the tail of
 * packedGroupList are left untouched.
 *
 * @param srcPtr             [in] Base address of the uint64_t inputs[] array (length N).
 * @param dstPtr             [in] Base address of the uint64_t outputs[] array (length N).
 * @param lenPtr             [in] Base address of the uint32_t lenPtrs[] array (length N).
 * @param numLocalExpertPtr           [in] Base address of a uint32_t scalar holding N (common array length).
 * @param groupListPtr       [in] Base address of the int64_t groupList[] array (length N).
 * @param packedGroupListPtr [in] Base address of the int64_t packedGroupList[] array (length >= M).
 * @param deviceId           [in] Device ID to perform the copy on.
 * @return 0 on success, non-zero error code on failure.
 */
int32_t offload_group_pack_copy(uint64_t srcPtr, uint64_t dstPtr, uint64_t lenPtr, uint64_t numLocalExpertPtr,
                                uint64_t groupListPtr, uint64_t packedGroupListPtr, uint16_t deviceId);

/**
 * @brief Fused page_first <-> layer_first KV cache exchange copy.
 *
 * Unlike offload_sparse_copy, no (src, dst, len) entry table is built on the
 * host.  Every (page, layer, split) block address is derived from the token
 * indices and the per-component layout metadata, so no entry table is
 * uploaded by the caller.
 *
 * variable (read on every call):
 *   AIV MTE kernel path. metaPtr points to an
 *       int64 array on device memory; the kernel derives every block address
 *       on device, the indices never round-trip through the CPU. host_base
 *       must already be rewritten to DVA by the caller (the Python
 *       kv_exchange_copy wrapper does it via offload_get_dva, because
 *       conn-based DRAM pools use an independent HalHostRegister dva != hva).
 * Unknown values fall back to aiv with a warning.
 *
 * meta layout, int64 values (42 total):
 *   [0] num_components (<= 4, k/v/index_k/index_k_scale order)
 *   [1] num_pages      [2] page_size
 *   [3] direction: 1 = H2D, 2 = D2H
 *   [4] device_indices_ptr [5] host_indices_ptr (int64 token indices,
 *       device memory in aiv mode, host-accessible memory in aicpu mode)
 *   per component c (9 values starting at 6 + 9*c):
 *   [0] device_base  [1] host_base (DVA in aiv mode, HVA in aicpu mode)
 *   [2] device_layer_pitch  [3] device_page_pitch   (bytes)
 *   [4] host_page_pitch     [5] host_layer_pitch    (bytes)
 *   [6] width (bytes of one (page, layer) row)  [7] layer_lo  [8] layer_hi
 * A component with layer_lo >= layer_hi (or width <= 0) transfers nothing.
 *
 * @param metaPtr  [in] Device (aiv) or host-accessible (aicpu) address of the
 *                   int64 metadata array above.
 * @param deviceId [in] Device ID to perform the copy on.
 * @return 0 on success, non-zero error code on failure.
 */
int32_t offload_kv_exchange_copy(uint64_t metaPtr, uint16_t deviceId);

#ifdef __cplusplus
}
#endif

#endif //__MEMFABRIC_ACC_OFFLOAD_H__
