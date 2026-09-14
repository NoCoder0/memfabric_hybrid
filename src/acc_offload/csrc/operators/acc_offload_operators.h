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

#ifndef ACC_OFFLOAD_OPERATORS_H
#define ACC_OFFLOAD_OPERATORS_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void OffloadOpsSparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr, uint32_t blockDim,
                          void *stream);

void OffloadOpsGroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *numLocalExpertPtr,
                             int64_t *groupList, int64_t *packedGroupList, void *stream);

void OffloadOpsKvExchange(uint64_t *metaPtr, uint32_t blockDim, void *stream);

/* Launch-bound gather entry: the layout (poolGva/slotStride/entryBytes/
 * rowsPerSlot) is resolved from the registration at call time and passed as
 * plain scalars so this header stays independent of the AccOffloadEntryGatherLayout
 * struct in acc_offload_launch.h. */
void AccOffloadEntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint64_t poolGva, uint64_t slotStride,
                           uint32_t entryBytes, uint32_t rowsPerSlot, uint8_t devIdx);

void OffloadOpsEntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint64_t poolGva, uint64_t slotStride,
                           uint32_t entryBytes, uint32_t rowsPerSlot, uint32_t blockDim, void *stream);

#ifdef __cplusplus
}
#endif

#endif // ACC_OFFLOAD_OPERATORS_H
