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
#ifndef MEM_FABRIC_HYBRID_ACC_OFFLOAD_HYBM_KVCACHE_SCATTER_COPY_H
#define MEM_FABRIC_HYBRID_ACC_OFFLOAD_HYBM_KVCACHE_SCATTER_COPY_H

#include <cstddef>
#include <cstdint>

struct HybmKvcacheScatterCopyParam {
    void *hbmKpe;
    void *hbmCkv;
    const int32_t *hbmBlockTable;
    const uint64_t *dramBlockTable;
    const int32_t *offloadSlots;
    const int32_t *srcTokenIds;
    const int32_t *dstSlots;
    const int32_t *copyCounts;
    int32_t *readyFlag;
    uint64_t hbmBlockCount;
    uint64_t hbmMaxBlocks;
    uint64_t dramMaxBlocks;
    uint64_t dramBlockTableRows;
    uint64_t batchSize;
    int64_t layerId;
};

static_assert(offsetof(HybmKvcacheScatterCopyParam, hbmKpe) == 0x00U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, hbmCkv) == 0x08U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, hbmBlockTable) == 0x10U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, dramBlockTable) == 0x18U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, offloadSlots) == 0x20U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, srcTokenIds) == 0x28U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, dstSlots) == 0x30U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, copyCounts) == 0x38U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, readyFlag) == 0x40U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, hbmBlockCount) == 0x48U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, hbmMaxBlocks) == 0x50U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, dramMaxBlocks) == 0x58U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, dramBlockTableRows) == 0x60U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, batchSize) == 0x68U);
static_assert(offsetof(HybmKvcacheScatterCopyParam, layerId) == 0x70U);
static_assert(sizeof(HybmKvcacheScatterCopyParam) == 0x78U);

extern "C" uint32_t HybmKvcacheScatterCopy(HybmKvcacheScatterCopyParam *param);

#endif // MEM_FABRIC_HYBRID_ACC_OFFLOAD_HYBM_KVCACHE_SCATTER_COPY_H
