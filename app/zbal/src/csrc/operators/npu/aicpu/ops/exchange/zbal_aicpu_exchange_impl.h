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

#ifndef ZBAL_AICPU_EXCHANGE_IMPL_H
#define ZBAL_AICPU_EXCHANGE_IMPL_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_workspace.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_sdma_sqe_context.h"
#include "executor/zbal_aicpu_flag.h"
#include "executor/zbal_aicpu_debug.h"
#include "executor/zbal_aicpu_thread.h"

/* ================================================================
 * NullExchange — no-op for single-rank, Init, Finalize
 * ================================================================ */
class NullExchange {
public:
    static const char *Name()
    {
        return "None";
    }
    static int Execute(const ExchangeContext &ctx)
    {
        (void)ctx;
        return 0;
    }
};

/* ================================================================
 * FullMeshExchange — all-to-all SDMA write of sendBuf GVA
 *
 * Mirrors AIV ExchangeAddrKernelSmall::ExchangeInputAddrFlag():
 *   1. ranksPerCore = ceil(rankNum / numCores), each core handles subset
 *   2. Per-core scratch (ringBuf end) + ring buffer (ringBuf start), no overlap
 *   3. Each core builds SDMA SQEs for its assigned dstRanks
 *   4. Each core submits via its own channel[0], waits its own flag
 *   5. AicpuCoreBarrier synchronizes all cores
 *
 * Debug: each core logs: chan/fid/sqeCnt → traces per-core independently.
 * ================================================================ */
class FullMeshExchange {
public:
    static const char *Name()
    {
        return "FullMesh";
    }

    static int WaitCrossDeviceFullMesh(uint64_t exchangeGva, uint64_t flagAreaOff, uint32_t rankNum,
                                       uint64_t expectedValue)
    {
        volatile uint64_t *flagBase = reinterpret_cast<volatile uint64_t *>(exchangeGva + flagAreaOff);
        return BarrierAllRanks(flagBase, rankNum, expectedValue, true);
    }

    static int Execute(const ExchangeContext &ctx)
    {
        const uint32_t rankNum = ctx.aicpuCtx->rankNum;
        if (rankNum <= 1)
            return 0;

        const uint32_t numCores = ctx.numCores;
        const uint32_t myCore = ctx.coreId;
        const uint32_t myRank = ctx.aicpuCtx->rankId;

        /* ── Step 1: divide ranks across cores ── */
        const uint32_t ranksPerCore = (rankNum + numCores - 1) / numCores;
        const uint32_t startRank = myCore * ranksPerCore;
        uint32_t endRank = startRank + ranksPerCore;
        if (endRank > rankNum) {
            endRank = rankNum;
        }

        if (startRank >= rankNum) {
            AicpuCoreBarrier(ctx.workspace, numCores);
            return 0;
        }

        /* ── Step 2: per-core scratch + ring buffer ── */
        const uint32_t ringBufSize = ZBAL_AICPU_CORE_RINGBUF_SIZE;
        volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(ctx.workspace, myCore);
        /* scratch area: 2 × uint64_t at end of ring buffer
         * [0] = sendBuf GVA, [1] = flag sentinel */
        constexpr uint32_t scratchSlots = 2;
        volatile uint64_t *scratch =
            reinterpret_cast<volatile uint64_t *>(myBuf + ringBufSize - scratchSlots * sizeof(uint64_t));
        scratch[0] = ctx.desc->sendBuffer;
        scratch[1] = ctx.desc->waitSymbol; /* incrementing flag — avoids stale match */
        uint64_t dataSrcGva = reinterpret_cast<uint64_t>(&scratch[0]);
        uint64_t flagSrcGva = reinterpret_cast<uint64_t>(&scratch[1]);

        SqeLocalRingBuffer eb;
        eb.Init(const_cast<uint8_t *>(myBuf));

        /* ── Step 3: build data + flag SQEs ── */
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
        const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;

        for (uint32_t dstRank = startRank; dstRank < endRank; dstRank++) {
            int64_t delta = (int64_t)dstRank - (int64_t)myRank;
            int64_t devOff = delta * (int64_t)ctx.aicpuCtx->localDeviceMemSize;
            uint64_t dataDst = ctx.aicpuCtx->exchangeGva + (uint64_t)devOff + (uint64_t)myRank * strideBytes;
            uint64_t flagDst =
                ctx.aicpuCtx->exchangeGva + (uint64_t)devOff + flagAreaOff + (uint64_t)myRank * strideBytes;
            if (AicpuDispatcher::CopyData(&eb, 0U, dataSrcGva, dataDst, sizeof(uint64_t), ctx.channels[0]) != 0 ||
                AicpuDispatcher::CopyData(&eb, 0U, flagSrcGva, flagDst, sizeof(uint64_t), ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
        }
        AICPU_DBG(TAG_ALGO_ALLGATHER, eb.sqeCnt, (endRank - startRank));

        /* ── Step 4: submit + local wait ── */
        uint32_t fid = AicpuWorkspace::FlagIdx(myCore, ctx.numChPerCore, 0);
        int dbr = AicpuLaunchTaskMc(&eb, ctx.channels[0], ctx.workspace, myCore, 1, 0, fid);
        if (dbr == 0 && CompletionFlag(ctx.workspace, fid).Wait() < 0) {
            dbr = ERR_WAIT_TIMEOUT;
        }

        /* ── Step 5b: cross-device sync (AIV-style: poll flag area) ── */
        if (myCore == 0 && dbr == 0) {
            if (WaitCrossDeviceFullMesh(ctx.aicpuCtx->exchangeGva, flagAreaOff, rankNum, ctx.desc->waitSymbol) != 0) {
                dbr = ERR_WAIT_TIMEOUT;
            }
        }
        AicpuCoreBarrier(ctx.workspace, numCores);
        return (dbr < 0) ? ERR_WAIT_TIMEOUT : 0;
    }
};

/* ================================================================
 * AllReduceExchange — publish sendBuf (slot 0) + buffer (slot 1) GVA
 *
 * AllReduce uses ReduceScatter + AllGather decomposition:
 *   RS phase reads peers' sendBuf (slot 0) — never modified → in-place safe
 *   AG phase reads peers' buffer  (slot 1) — stable after RS barrier
 *
 * Both GVAs are published in a single full-mesh exchange. The 8-slot
 * per-rank stride provides room for both data slots plus flags.
 * ================================================================ */
/* AllReduceExchange scratch slot indices */
constexpr uint32_t AR_SCRATCH_SENDBUF = 0; /* sendBuf GVA (for RS phase) */
constexpr uint32_t AR_SCRATCH_BUFFER = 1;  /* buffer GVA (for AG phase) */
constexpr uint32_t AR_SCRATCH_FLAG = 2;    /* flag sentinel (waitSymbol) */

class AllReduceExchange {
public:
    static const char *Name()
    {
        return "AllReduceExch";
    }

    static int Execute(const ExchangeContext &ctx)
    {
        uint32_t rankNum = ctx.aicpuCtx->rankNum;
        if (rankNum <= 1)
            return 0;

        uint32_t numCores = ctx.numCores;
        uint32_t myCore = ctx.coreId;
        uint32_t myRank = ctx.aicpuCtx->rankId;
        uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
        uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;

        /* ── Divide ranks across cores ── */
        uint32_t ranksPerCore = (rankNum + numCores - 1) / numCores;
        uint32_t startRank = myCore * ranksPerCore;
        uint32_t endRank = startRank + ranksPerCore;
        if (endRank > rankNum) {
            endRank = rankNum;
        }

        if (startRank >= rankNum) {
            AicpuCoreBarrier(ctx.workspace, numCores);
            return 0;
        }

        /* ── Scratch: [0]=sendBuf, [1]=buffer, [2]=flag sentinel ── */
        uint32_t ringBufSize = ZBAL_AICPU_CORE_RINGBUF_SIZE;
        volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(ctx.workspace, myCore);
        constexpr uint32_t scratchSlots = 3;
        volatile uint64_t *scratch =
            reinterpret_cast<volatile uint64_t *>(myBuf + ringBufSize - scratchSlots * sizeof(uint64_t));
        scratch[AR_SCRATCH_SENDBUF] = ctx.desc->sendBuffer; /* sendBuf GVA (for RS) */
        scratch[AR_SCRATCH_BUFFER] = ctx.desc->buffer;      /* buffer GVA (for AG) */
        scratch[AR_SCRATCH_FLAG] = ctx.desc->waitSymbol;    /* flag sentinel (avoids stale match) */
        uint64_t sendSrcGva = reinterpret_cast<uint64_t>(&scratch[AR_SCRATCH_SENDBUF]);
        uint64_t bufSrcGva = reinterpret_cast<uint64_t>(&scratch[AR_SCRATCH_BUFFER]);
        uint64_t flagSrcGva = reinterpret_cast<uint64_t>(&scratch[AR_SCRATCH_FLAG]);

        SqeLocalRingBuffer eb;
        eb.Init((uint8_t *)(myBuf));

        /* ── Build SQEs: 3 writes per dstRank (sendBuf, buffer, flag) ── */

        for (uint32_t dstRank = startRank; dstRank < endRank; dstRank++) {
            int64_t delta = (int64_t)dstRank - (int64_t)myRank;
            int64_t devOff = delta * (int64_t)ctx.aicpuCtx->localDeviceMemSize;
            uint64_t baseOff = ctx.aicpuCtx->exchangeGva + (uint64_t)devOff + (uint64_t)myRank * strideBytes;
            uint64_t flagDst =
                ctx.aicpuCtx->exchangeGva + (uint64_t)devOff + flagAreaOff + (uint64_t)myRank * strideBytes;

            /* data slot 0: sendBuf GVA */
            if (AicpuDispatcher::CopyData(&eb, 0U, sendSrcGva, baseOff, sizeof(uint64_t), ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
            /* data slot 1: buffer GVA */
            if (AicpuDispatcher::CopyData(&eb, 0U, bufSrcGva, baseOff + sizeof(uint64_t), sizeof(uint64_t),
                                          ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
            /* flag slot 0: exchange-complete sentinel */
            if (AicpuDispatcher::CopyData(&eb, 0U, flagSrcGva, flagDst, sizeof(uint64_t), ctx.channels[0]) != 0) {
                return BUILD_ERROR;
            }
        }

        /* ── Submit + local wait ── */
        uint32_t fid = AicpuWorkspace::FlagIdx(myCore, ctx.numChPerCore, 0);
        int dbr = AicpuLaunchTaskMc(&eb, ctx.channels[0], ctx.workspace, myCore, 1, 0, fid);
        if (dbr == 0 && CompletionFlag(ctx.workspace, fid).Wait() < 0) {
            dbr = ERR_WAIT_TIMEOUT;
        }

        /* ── Cross-device sync: wait for all ranks' flags == waitSymbol ── */
        if (myCore == 0 && dbr == 0) {
            volatile uint64_t *flagBase =
                reinterpret_cast<volatile uint64_t *>(ctx.aicpuCtx->exchangeGva + flagAreaOff);
            if (BarrierAllRanks(flagBase, rankNum, ctx.desc->waitSymbol, true) != 0) {
                dbr = ERR_WAIT_TIMEOUT;
            }
        }
        AicpuCoreBarrier(ctx.workspace, numCores);
        return (dbr < 0) ? ERR_WAIT_TIMEOUT : 0;
    }
};

/* ================================================================
 * RingExchange — neighbor-only exchange (multi-core)
 *
 * Core 0 → prev, Core 1 → next, other cores barrier-only.
 * ================================================================ */
class RingExchange {
public:
    static const char *Name()
    {
        return "Ring";
    }

    static int Execute(const ExchangeContext &ctx)
    {
        uint32_t rankNum = ctx.aicpuCtx->rankNum;
        if (rankNum <= 1)
            return 0;

        uint32_t numCores = ctx.numCores;
        uint32_t myCore = ctx.coreId;
        uint32_t myRank = ctx.aicpuCtx->rankId;

        /* ── Step 1: 2 targets → core 0 (prev), core 1 (next) ── */
        uint32_t targetRank = ZBAL_SDMA_RANK_LOCAL;
        if (myCore == 0)
            targetRank = (myRank + rankNum - 1) % rankNum;
        else if (myCore == 1)
            targetRank = (myRank + 1) % rankNum;

        if (targetRank == ZBAL_SDMA_RANK_LOCAL) {
            AicpuCoreBarrier(ctx.workspace, numCores);
            return 0;
        }

        uint32_t ringBufSize = ZBAL_AICPU_CORE_RINGBUF_SIZE;
        volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(ctx.workspace, myCore);
        /* scratch area: 2 × uint64_t at end of ring buffer
         * [0] = sendBuf GVA, [1] = flag sentinel */
        constexpr uint32_t scratchSlots = 2;
        volatile uint64_t *scratch =
            reinterpret_cast<volatile uint64_t *>(myBuf + ringBufSize - scratchSlots * sizeof(uint64_t));
        scratch[0] = ctx.desc->sendBuffer;
        scratch[1] = ctx.desc->waitSymbol; /* incrementing flag — avoids stale match */
        uint64_t dataSrcGva = reinterpret_cast<uint64_t>(&scratch[0]);
        uint64_t flagSrcGva = reinterpret_cast<uint64_t>(&scratch[1]);

        SqeLocalRingBuffer eb;
        eb.Init(const_cast<uint8_t *>(myBuf));

        /* ── Step 3: build data + flag SQE ── */
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
        const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;

        int64_t delta = (int64_t)targetRank - (int64_t)myRank;
        int64_t devOff = delta * (int64_t)ctx.aicpuCtx->localDeviceMemSize;

        uint64_t dataDst = ctx.aicpuCtx->exchangeGva + (uint64_t)devOff + (uint64_t)myRank * strideBytes;
        uint64_t flagDst = ctx.aicpuCtx->exchangeGva + (uint64_t)devOff + flagAreaOff + (uint64_t)myRank * strideBytes;
        if (AicpuDispatcher::CopyData(&eb, 0U, dataSrcGva, dataDst, sizeof(uint64_t), ctx.channels[0]) != 0 ||
            AicpuDispatcher::CopyData(&eb, 0U, flagSrcGva, flagDst, sizeof(uint64_t), ctx.channels[0]) != 0)
            return BUILD_ERROR;

        /* ── Step 4: submit + wait ── */
        uint32_t fid = AicpuWorkspace::FlagIdx(myCore, ctx.numChPerCore, 0);
        int dbr = AicpuLaunchTaskMc(&eb, ctx.channels[0], ctx.workspace, myCore, 1, 0, fid);
        if (dbr == 0 && CompletionFlag(ctx.workspace, fid).Wait() < 0) {
            dbr = ERR_WAIT_TIMEOUT;
        }

        /* ── Cross-device sync: wait for prev + next neighbors' flags ── */
        if (myCore == 0 && dbr == 0) {
            const uint32_t prev = (myRank + rankNum - 1) % rankNum;
            const uint32_t next = (myRank + 1) % rankNum;
            volatile uint64_t *flagBase =
                reinterpret_cast<volatile uint64_t *>(ctx.aicpuCtx->exchangeGva + flagAreaOff);
            constexpr uint32_t kCrossTimeout = 6000000;
            bool prevOk = false;
            bool nextOk = false;
            for (uint32_t t = 0; t < kCrossTimeout && !(prevOk && nextOk); t++) {
                if (!prevOk) {
                    uintptr_t fa = reinterpret_cast<uintptr_t>(
                        const_cast<uint64_t *>(&flagBase[prev * ZBAL_AICPU_EXCHANGE_STRIDE]));
                    AicpuCacheInvalidate(fa);
                    if (flagBase[prev * ZBAL_AICPU_EXCHANGE_STRIDE] == ctx.desc->waitSymbol) {
                        prevOk = true;
                    }
                }
                if (!nextOk) {
                    uintptr_t fa = reinterpret_cast<uintptr_t>(
                        const_cast<uint64_t *>(&flagBase[next * ZBAL_AICPU_EXCHANGE_STRIDE]));
                    AicpuCacheInvalidate(fa);
                    if (flagBase[next * ZBAL_AICPU_EXCHANGE_STRIDE] == ctx.desc->waitSymbol) {
                        nextOk = true;
                    }
                }
            }
            if (!(prevOk && nextOk)) {
                dbr = ERR_WAIT_TIMEOUT;
            }
        }
        AicpuCoreBarrier(ctx.workspace, numCores);
        return (dbr < 0) ? dbr : 0;
    }
};

#endif /* ZBAL_AICPU_EXCHANGE_IMPL_H */
