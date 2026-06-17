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
#ifndef ZBAL_SMEM_META_H
#define ZBAL_SMEM_META_H

#include <cstdint>

/* Device address space end (before 1GB reserved region) */
constexpr uint64_t ZBAL_SMEM_DEVICE_END_ADDR = 0x180000000000ULL - (1ULL << 30ULL);

/* Per-object metadata size */
constexpr uint64_t ZBAL_SMEM_PRE_META_SIZE = 128ULL;

/* Global metadata size (same as per-object, at the start of meta region) */
constexpr uint64_t ZBAL_SMEM_GLOBAL_META_SIZE = 128ULL;

/* Maximum number of SMEM objects (entities) */
constexpr uint64_t ZBAL_SMEM_OBJECT_NUM_MAX = 511ULL;

/* Per-object user context size */
constexpr uint64_t ZBAL_SMEM_USER_CONTEXT_PRE_SIZE = 64ULL * 1024ULL;

/* Total metadata region size: global_meta + N × per_meta */
constexpr uint64_t ZBAL_SMEM_META_SIZE =
    ZBAL_SMEM_PRE_META_SIZE * ZBAL_SMEM_OBJECT_NUM_MAX + ZBAL_SMEM_GLOBAL_META_SIZE;

/* Total info region size: N × user_context + meta */
constexpr uint64_t ZBAL_SMEM_INFO_SIZE =
    ZBAL_SMEM_USER_CONTEXT_PRE_SIZE * ZBAL_SMEM_OBJECT_NUM_MAX + ZBAL_SMEM_META_SIZE;

/* Base address of the metadata region */
constexpr uint64_t ZBAL_SMEM_META_ADDR = ZBAL_SMEM_DEVICE_END_ADDR - ZBAL_SMEM_INFO_SIZE;

constexpr uint64_t ZBAL_SMEM_META_OBJ_ID_OFFSET = 0;
constexpr uint64_t ZBAL_SMEM_META_RANK_OFFSET = 4;
constexpr uint64_t ZBAL_SMEM_META_RANK_SIZE_OFFSET = 8;
constexpr uint64_t ZBAL_SMEM_META_CONTEXT_OFFSET = 12;
constexpr uint64_t ZBAL_SMEM_META_SYMM_OFFSET = 16;
constexpr uint64_t ZBAL_SMEM_META_QP_INFO_OFFSET = 24;
constexpr uint64_t ZBAL_SMEM_META_SDMA_WORKSPACE_OFFSET = 32;

constexpr uint64_t ZBAL_SDMA_CHANNEL_FLAG_SIZE = 64;
constexpr uint32_t ZBAL_SDMA_MAX_CHANNELS = 48;

#endif /* ZBAL_SMEM_META_H */
