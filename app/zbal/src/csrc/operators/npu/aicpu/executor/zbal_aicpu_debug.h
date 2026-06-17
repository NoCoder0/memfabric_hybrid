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

#ifndef ZBAL_AICPU_DEBUG_H
#define ZBAL_AICPU_DEBUG_H

#include <cstdint>

/* ================================================================
 * Debug buffer layout constants
 * ================================================================ */
constexpr uint32_t AICPU_DEBUG_MAGIC = 0xA1C0DEB0U;
constexpr uint32_t AICPU_DEBUG_ENTRY_SIZE = 24;
constexpr uint32_t AICPU_DEBUG_HEADER_SIZE = 16;
constexpr uint32_t AICPU_DEBUG_SEQ_SHIFT = 16; /* bit position for seq in info field (upper 16 bits) */

constexpr uint32_t AICPU_DEBUG_MAX_ENTRIES =
    (ZBAL_AICPU_DEBUG_BUFFER_SIZE - AICPU_DEBUG_HEADER_SIZE) / AICPU_DEBUG_ENTRY_SIZE;

/* ================================================================
 * Debug trace entry (24 bytes)
 * ================================================================ */
struct AicpuDebugEntry {
    uint32_t tag;  /* what happened (AicpuDebugTag) */
    uint32_t info; /* packed: line_number | (extra << 16) */
    uint64_t val0; /* primary value */
    uint64_t val1; /* secondary value */
};

/* ================================================================
 * Debug buffer header (16 bytes, at offset 0 of debug area)
 * ================================================================ */
struct AicpuDebugHeader {
    uint32_t magic; /* AICPU_DEBUG_MAGIC */
    uint32_t count; /* number of valid entries */
    uint32_t seq;   /* sequence counter */
    uint32_t reserved;
};

/* ================================================================
 * Debug tags — each value identifies a specific checkpoint
 * ================================================================ */
enum AicpuDebugTag : uint32_t {
    TAG_ENTRY = 0,           /* kernel entered: coreId, numCores */
    TAG_INIT_CTX_RANK = 10,  /* rankId, rankNum */
    TAG_UPDATE_CTX = 13,     /* commType, count */
    TAG_ALGO_ALLGATHER = 30, /* sendBuffer, count */
    TAG_RETURN = 61,         /* return code, coreId */
};

/* ================================================================
 * Device-side write macro
 *
 * Usage:
 *   AICPU_DBG(TAG_ENTRY, 0, 0);
 *   AICPU_DBG(TAG_ALGO_ALLGATHER, sendBufGva, byteCount);
 *
 * The buffer pointer (g_debugBuf) is a volatile pointer into the
 * workspace at ZBAL_AICPU_DEBUG_BUFFER_OFFSET. It must be initialized
 * once in the kernel entry before any AICPU_DBG calls.
 * ================================================================ */
extern volatile AicpuDebugHeader *g_debugBuf;

inline void AicpuDebugWrite(uint32_t tag, uint32_t line, uint64_t val0, uint64_t val1)
{
    if (g_debugBuf == nullptr) {
        return;
    }

    uint32_t idx = g_debugBuf->count;
    if (idx >= AICPU_DEBUG_MAX_ENTRIES) {
        return;
    }

    volatile AicpuDebugEntry *entry = reinterpret_cast<volatile AicpuDebugEntry *>(
        reinterpret_cast<volatile uint8_t *>(g_debugBuf) + AICPU_DEBUG_HEADER_SIZE);

    entry[idx].tag = tag;
    entry[idx].info = line | ((g_debugBuf->seq & 0xFFFFU) << AICPU_DEBUG_SEQ_SHIFT);
    entry[idx].val0 = val0;
    entry[idx].val1 = val1;

    g_debugBuf->count = idx + 1;
    g_debugBuf->seq++;
}

#define AICPU_DBG(tag, v0, v1) AicpuDebugWrite((tag), __LINE__, static_cast<uint64_t>(v0), static_cast<uint64_t>(v1))

#endif /* ZBAL_AICPU_DEBUG_H */
