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

#ifndef ZBAL_AICPU_COMM_ALG_H
#define ZBAL_AICPU_COMM_ALG_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"
#include "executor/engine/sdma/zbal_aicpu_sdma_sqe_context.h"

/* Batch control */
constexpr int BUILD_DONE = 0;
constexpr int BUILD_MORE = 1;
constexpr int BUILD_ERROR = -1;

/* Algorithm context — per-operation runtime state. */
struct AicpuAlgorithmCtx {
    AicpuInitContext *ctx;
    SqeLocalRingBuffer *ringBufs;
    uint64_t chunkRemaining;
    uint64_t chunkCurBase;
    bool chunkActive;
};

inline void AicpuAlgorithmInit(AicpuAlgorithmCtx *alg, AicpuInitContext *ctx, SqeLocalRingBuffer *ringBufs)
{
    alg->ctx = ctx;
    alg->ringBufs = ringBufs;
    alg->chunkRemaining = 0;
    alg->chunkCurBase = 0;
    alg->chunkActive = false;
}

/* Algorithm parameters — read-only, passed to BuildSqes */
struct CommOpParams {
    uint64_t sendBuf;
    uint64_t recvBuf;
    uint64_t buffer; /* scratch buffer GVA (AllReduce temp workspace) */
    uint64_t dataSize;
    uint64_t exchangeGva;
    uint32_t root;     /* root rank (scatter/broadcast) / peer (send/recv) */
    uint32_t dataType; /* zbal_datatype_t enum */
    uint32_t reduceOp; /* zbal_reduce_op_t — maps to SDMA opCode for hardware reduce */
    uint32_t commAlg;  /* resolved algorithm (host-side via SelectCommAlg, device reads desc->commAlg) */
    uint32_t coreId;
    uint32_t numCores;
    uint32_t numChPerCore;
    volatile stars_channel_info_t **channels;
    uint64_t waitSymbol;  /* incrementing barrier flag — all ops use this for cross-device sync */
    uint64_t reserved[4]; /* per-op extension: AlltoAllV uses [0]=sendCumSum, [1]=recvSplitCounts, [2]=elements */
};

/* Read peer rank's sendBuf GVA from exchange area (slot 0) */
inline uint64_t PeerOutputBuf(uint64_t exchangeGva, uint32_t rank)
{
    return reinterpret_cast<const uint64_t *>(exchangeGva)[rank * ZBAL_AICPU_EXCHANGE_STRIDE];
}

/* Read peer rank's buffer (scratch) GVA from exchange area (slot 1) */
inline uint64_t PeerBufferGva(uint64_t exchangeGva, uint32_t rank)
{
    return reinterpret_cast<const uint64_t *>(exchangeGva)[rank * ZBAL_AICPU_EXCHANGE_STRIDE + 1];
}

/* Divide totalBytes evenly across numCores, returning [outOff, outOff+outLen) for coreId */
inline void AicpuParallelSlice(uint64_t totalBytes, uint32_t coreId, uint32_t numCores, uint64_t &outOff,
                               uint64_t &outLen)
{
    if (numCores == 0) {
        outOff = 0;
        outLen = 0;
        return;
    }
    uint64_t sliceSize = totalBytes / numCores;
    outOff = static_cast<uint64_t>(coreId) * sliceSize;
    outLen = (coreId == numCores - 1) ? (totalBytes - outOff) : sliceSize;
}

inline int BarrierAllRanks(volatile uint64_t *flagBase, uint32_t rankNum, uint64_t expectedValue, bool matchExact)
{
    uint32_t timeout = 6000000; /* 6M * 1us sleep = 6s timeout */
    constexpr uint32_t kWords = (ZBAL_MAX_RANKS + 63) / 64;
    uint64_t readyMask[kWords] = {};
    const uint32_t numWords = (rankNum + 63) / 64;
    for (uint32_t t = 0; t < timeout; t++) {
        bool allReady = true;
        for (uint32_t w = 0; w < numWords; w++) {
            const uint64_t fullWordMask =
                (w + 1 == numWords && (rankNum % 64)) ? ((1ULL << (rankNum % 64)) - 1) : ~0ULL;
            if (readyMask[w] == fullWordMask)
                continue;
            for (uint32_t bit = 0; bit < 64; bit++) {
                uint32_t r = w * 64 + bit;
                if (r >= rankNum)
                    break;
                if (readyMask[w] & (1ULL << bit))
                    continue;
                uintptr_t fa =
                    reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(&flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE]));
                AicpuCacheInvalidate(fa);
                bool flagReady = matchExact ? (flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE] == expectedValue)
                                            : (flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE] != 0);
                if (flagReady)
                    readyMask[w] |= (1ULL << bit);
            }
            if (readyMask[w] != fullWordMask)
                allReady = false;
        }
        if (allReady)
            return 0;
    }
    return ERR_WAIT_TIMEOUT;
}

#endif /* ZBAL_AICPU_COMM_ALG_H */
