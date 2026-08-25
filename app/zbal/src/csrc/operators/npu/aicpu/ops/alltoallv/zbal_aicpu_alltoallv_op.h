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
#ifndef ZBAL_AICPU_ALLTOALLV_OP_H
#define ZBAL_AICPU_ALLTOALLV_OP_H
#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/zbal_aicpu_workspace.h"
#include "executor/zbal_aicpu_flag.h"
#include "executor/zbal_aicpu_thread.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/* Max ranks tracked by the uint64 pending mask in progressive poll */
constexpr uint32_t A2AV_MAX_POLLED_RANKS = 64;

/*
 * AlltoAllV: variable-length all-to-all.
 *
 * Exchange layout (within each rank's exchange area, STRIDE=8 words per rank):
 *   slot[r * STRIDE + 0] = rank r's sendBuffer GVA
 *   slot[r * STRIDE + 1] = rank r's sendOffset for myRank (cumSum[myRank])
 *   slot[r * STRIDE + 2] = rank r's sendCount for myRank (bytes to read)
 *   flag[r]              = sentinel (ready)
 *
 * Pipelined execution — no cross-device barrier between exchange and pull:
 *   Phase 1: publish (sendBuf, offset, count, flag) to all peers on multiple channels
 *            (per-peer data+flag on the same channel → flag lands after data by FIFO).
 *            Self entry is CPU-written. Waits only LOCAL SDMA completion.
 *   Phase 2: recv layout (count/offset per src) is precomputed locally from
 *            recvSplitCounts (host contract: recvSplitCounts[r] == elements rank r sends
 *            us), so each peer is pulled immediately when its flag arrives — slow peers
 *            don't block fast ones. Pull SQEs are submitted NoWait (start early); the
 *            final submit appends an Mc flag SQE per channel which, by same-channel FIFO,
 *            completes only after ALL earlier SQEs of that channel.
 *   Large pulls (> 1 MiB per core-slice) are striped into 1 MiB chunks round-robin
 *   across ALL channels (offset by src rank) — balances variable per-peer counts,
 *   keeps multiple SQEs in flight per link, and fills channels left idle by
 *   straggler peers.
 *
 * If sendCumSum (param0) is 0, falls back to equal-split (count/rankNum per peer).
 */
/* AlltoAllV exchange sub-slot indices within each rank's STRIDE-wide entry */
constexpr uint32_t A2AV_EXCH_SLOT_SENDBUF = 0;   /* sendBuffer GVA */
constexpr uint32_t A2AV_EXCH_SLOT_OFFSET = 1;    /* sendOffset for target rank */
constexpr uint32_t A2AV_EXCH_SLOT_COUNT = 2;     /* sendCount for target rank (bytes) */
constexpr uint32_t A2AV_EXCH_NUM_DATA_SLOTS = 3; /* data slots per rank in exchange area */
constexpr uint32_t A2AV_SCRATCH_FLAG_SLOT = 3;   /* flag sentinel slot in scratch block */
constexpr uint32_t A2AV_SCRATCH_SLOTS_PER_DST = A2AV_SCRATCH_FLAG_SLOT + 1;

class AlltoAllVOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        return AlltoAllVExchange(ctx);
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t rankNum = alg.ctx->rankNum;
        const uint32_t myRank = alg.ctx->rankId;
        if (rankNum == 0) {
            return BUILD_DONE;
        }
        if (numChPerCore == 0 || rankNum > A2AV_MAX_POLLED_RANKS) {
            return BUILD_ERROR; /* rankNum > 64 overflows uint64 pendingMask */
        }

        /* Local precompute of recv layout (bytes): recvSplitCounts[r] (elements, compact
         * [r]) equals what rank r sends us, so pull length/offset need no exchange data.
         * Only sendBuf GVA + srcOffset come from the exchange area → any ready peer can
         * be pulled immediately, out of order. */
        const uint64_t *recvSplit = reinterpret_cast<const uint64_t *>(op.reserved[1]);
        const uint32_t elemSize = ZBALDataTypeSize(op.dataType);
        uint64_t recvCnt[A2AV_MAX_POLLED_RANKS];
        uint64_t recvOff[A2AV_MAX_POLLED_RANKS];
        uint64_t pendingMask = 0;
        uint32_t pendingCount = 0;
        uint64_t base = 0;
        for (uint32_t r = 0; r < rankNum; r++) {
            recvCnt[r] = (recvSplit != nullptr) ? recvSplit[r] * elemSize : op.dataSize / rankNum;
            recvOff[r] = base;
            base += recvCnt[r];
            if (recvCnt[r] != 0 && r != myRank) {
                pendingMask |= (1ULL << r);
                pendingCount++;
            }
        }

        bool touched[ZBAL_AICPU_MAX_CH_PER_CORE] = {}; /* channels with in-flight NoWait SQEs */
        bool submitted[ZBAL_AICPU_MAX_CH_PER_CORE] = {};

        /* Step 1: self pull — its flag was CPU-written in Phase 1, submit immediately.
         * Runs via NoWait while peer flags are polled; Mc directly if nothing pending. */
        if (SubmitPull(op, ringBufs, myRank, recvCnt[myRank], recvOff[myRank], numChPerCore) != 0) {
            return BUILD_ERROR;
        }
        if (pendingCount == 0) {
            return SubmitMcAndWait(ringBufs, channels, workspace, numChPerCore, coreId, touched, submitted);
        }
        if (SubmitNoWaitAll(ringBufs, channels, numChPerCore, touched) != 0) {
            return BUILD_ERROR;
        }

        /* Step 2: batched progressive poll — pull each peer as soon as its flag arrives */
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));
        volatile uint64_t *flagBase =
            reinterpret_cast<volatile uint64_t *>(op.exchangeGva + static_cast<uint64_t>(rankNum) * strideBytes);

        constexpr uint32_t kPollTimeout = 6000000;
        uint32_t totalTimeout = 0;
        while (pendingCount > 0) {
            uint32_t submittedThisRound = 0;
            uint64_t mask = pendingMask;
            while (mask != 0) {
                uint32_t r = static_cast<uint32_t>(__builtin_ctzll(mask));
                mask &= mask - 1;
                AicpuCacheInvalidate(
                    reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(&flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE])));
                if (flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE] != op.waitSymbol) {
                    continue; /* not ready — check next peer */
                }
                pendingMask &= ~(1ULL << r);
                pendingCount--;
                if (SubmitPull(op, ringBufs, r, recvCnt[r], recvOff[r], numChPerCore) != 0) {
                    return BUILD_ERROR;
                }
                submittedThisRound++;
            }
            if (pendingCount == 0) {
                return SubmitMcAndWait(ringBufs, channels, workspace, numChPerCore, coreId, touched, submitted);
            }
            if (submittedThisRound > 0) {
                if (SubmitNoWaitAll(ringBufs, channels, numChPerCore, touched) != 0) {
                    return BUILD_ERROR;
                }
                totalTimeout = 0;
            } else if (++totalTimeout >= kPollTimeout) {
                return ERR_WAIT_TIMEOUT;
            }
        }
        return BUILD_DONE; /* unreachable */
    }

private:
    /* Read a sub-slot from exchange area: exch[rank * STRIDE + subSlot] */
    static uint64_t ReadExchSlot(uint64_t exchangeGva, uint32_t rank, uint32_t subSlot)
    {
        const uint64_t *base = reinterpret_cast<const uint64_t *>(exchangeGva);
        return base[rank * ZBAL_AICPU_EXCHANGE_STRIDE + subSlot];
    }

    /* Build pull SQEs for src rank r (its flag must be ready): read sendBuf/offset from
     * the exchange entry, core-slice, copy into recvBuf + recvOff.
     * Slices larger than one chunk are striped into A2AV_STRIPE_CHUNK_BYTES chunks
     * round-robin across channels, offset by r so concurrent peers interleave:
     *   - balances channels under variable per-peer counts (MoE skew),
     *   - keeps multiple SQEs in flight per link (single giant SQE serializes),
     *   - straggler peers spread over all channels instead of serializing on r % numCh.
     * Chunks write disjoint recvBuf ranges — cross-channel completion order is free. */
    static constexpr uint64_t A2AV_STRIPE_CHUNK_BYTES = 1024ULL * 1024ULL; /* 1 MiB */

    static int SubmitPull(const CommOpParams &op, SqeLocalRingBuffer *ringBufs, uint32_t r, uint64_t cnt, uint64_t off,
                          uint32_t numChPerCore)
    {
        uint64_t myOff;
        uint64_t myLen;
        AicpuParallelSlice(cnt, op.coreId, op.numCores, myOff, myLen);
        if (myLen == 0) {
            return 0;
        }
        AicpuCacheInvalidate(reinterpret_cast<uintptr_t>(op.exchangeGva) +
                             static_cast<uint64_t>(r) * ZBAL_AICPU_EXCHANGE_STRIDE * sizeof(uint64_t));
        uint64_t srcBufGva = ReadExchSlot(op.exchangeGva, r, A2AV_EXCH_SLOT_SENDBUF);
        uint64_t srcOffset = ReadExchSlot(op.exchangeGva, r, A2AV_EXCH_SLOT_OFFSET);
        uint64_t src = srcBufGva + srcOffset + myOff;
        uint64_t dst = op.recvBuf + off + myOff;
        if (myLen <= A2AV_STRIPE_CHUNK_BYTES) {
            uint32_t sid = r % numChPerCore;
            return AicpuDispatcher::CopyData(ringBufs, sid, src, dst, static_cast<uint32_t>(myLen), op.channels[sid],
                                             op.reduceOp);
        }
        uint32_t chunkIdx = 0;
        while (myLen > 0) {
            uint64_t len = (myLen > A2AV_STRIPE_CHUNK_BYTES) ? A2AV_STRIPE_CHUNK_BYTES : myLen;
            uint32_t sid = (r + chunkIdx) % numChPerCore;
            if (AicpuDispatcher::CopyData(ringBufs, sid, src, dst, static_cast<uint32_t>(len), op.channels[sid],
                                          op.reduceOp) != 0) {
                return BUILD_ERROR;
            }
            src += len;
            dst += len;
            myLen -= len;
            chunkIdx++;
        }
        return 0;
    }

    /* NoWait-submit all channels that hold SQEs; records touched[] so the final Mc
     * also covers channels whose earlier SQEs are still in flight.
     * Errors are propagated: a dropped NoWait batch (SQ full / bad channel) would
     * otherwise hang the final Mc wait until timeout. */
    static int SubmitNoWaitAll(SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                               uint32_t numChPerCore, bool *touched)
    {
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            if (ringBufs[ch].HasWork()) {
                if (AicpuLaunchTaskNoWait(&ringBufs[ch], channels[ch]) != 0) {
                    return BUILD_ERROR;
                }
                touched[ch] = true;
            }
        }
        return 0;
    }

    /* Final Mc submit + wait. Channels holding SQEs get a flag SQE appended (same-channel
     * FIFO → flag completes only after ALL earlier SQEs of that channel). Channels that
     * only have in-flight NoWait SQEs get a harmless 8B self-copy SQE so a flag can be
     * appended for them too — otherwise their completion would never be awaited. */
    static int SubmitMcAndWait(SqeLocalRingBuffer *ringBufs, volatile stars_channel_info_t **channels,
                               volatile uint8_t *workspace, uint32_t numChPerCore, uint32_t coreId, const bool *touched,
                               bool *submitted)
    {
        volatile uint8_t *coreBuf = AicpuWorkspace::CoreRingBuf(workspace, coreId);
        uint64_t dummyAddr = reinterpret_cast<uint64_t>(coreBuf + ZBAL_AICPU_CORE_RINGBUF_SIZE - sizeof(uint64_t));
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            if (!ringBufs[ch].HasWork()) {
                if (!touched[ch]) {
                    continue; /* nothing ever submitted on this channel */
                }
                if (AicpuDispatcher::CopyData(ringBufs, ch, dummyAddr, dummyAddr, sizeof(uint64_t), channels[ch]) !=
                    0) {
                    return BUILD_ERROR;
                }
            }
            uint32_t fid = AicpuWorkspace::FlagIdx(coreId, numChPerCore, ch);
            if (AicpuLaunchTaskMc(&ringBufs[ch], channels[ch], workspace, coreId, 1, ch, fid) < 0) {
                return ERR_DOORBELL_FAILED;
            }
            submitted[ch] = true;
        }
        for (uint32_t ch = 0; ch < numChPerCore; ch++) {
            if (!submitted[ch]) {
                continue;
            }
            CompletionFlag flag(workspace, coreId, numChPerCore, ch);
            if (flag.Wait() < 0) {
                return ERR_WAIT_TIMEOUT;
            }
        }
        return 0;
    }

    static int EnqueuePeerExchangeSqes(SqeLocalRingBuffer *chBufs, uint32_t ch, volatile uint64_t *ds, uint64_t baseOff,
                                       uint64_t flagDst, volatile stars_channel_info_t *channel)
    {
        /* Data slots 0..2 (sendBuf/offset/count) are contiguous in both scratch and dst
         * entry — one 24B SQE instead of three 8B SQEs. */
        if (AicpuDispatcher::CopyData(chBufs, ch, reinterpret_cast<uint64_t>(&ds[A2AV_EXCH_SLOT_SENDBUF]), baseOff,
                                      A2AV_EXCH_NUM_DATA_SLOTS * sizeof(uint64_t), channel) != 0) {
            return BUILD_ERROR;
        }
        /* flag slot: exchange-complete sentinel (waitSymbol) — same channel, lands after data */
        if (AicpuDispatcher::CopyData(chBufs, ch, reinterpret_cast<uint64_t>(&ds[A2AV_SCRATCH_FLAG_SLOT]), flagDst,
                                      sizeof(uint64_t), channel) != 0) {
            return BUILD_ERROR;
        }
        return 0;
    }

    /* Fill the per-dstRank scratch block `ds` with (sendBuf, offset, count, flag).
     * cumSum values are in ELEMENTS — converted to BYTES. When cumSum is null, falls back to
     * equal-split (count / rankNum per peer). elemArr (reserved[2]) supplies the total for the
     * last rank. */
    static void FillPeerScratch(volatile uint64_t *ds, uint32_t dstRank, uint32_t rankNum, uint64_t sendBuf,
                                const uint64_t *cumSum, uint32_t elemSize, uint64_t count, uint64_t waitSymbol,
                                const volatile uint64_t *elemArr)
    {
        ds[A2AV_EXCH_SLOT_SENDBUF] = sendBuf;
        if (cumSum != nullptr) {
            uint64_t curOff = cumSum[dstRank * ZBAL_AICPU_EXCHANGE_STRIDE];
            uint64_t nextOff;
            if (dstRank + 1 < rankNum) {
                nextOff = cumSum[(dstRank + 1) * ZBAL_AICPU_EXCHANGE_STRIDE];
            } else {
                /* Last rank: total from elements array (reserved[2]) */
                nextOff = (elemArr != nullptr) ? elemArr[0] : curOff;
            }
            ds[A2AV_EXCH_SLOT_OFFSET] = curOff * elemSize;
            ds[A2AV_EXCH_SLOT_COUNT] = (nextOff - curOff) * elemSize;
        } else {
            ds[A2AV_EXCH_SLOT_OFFSET] = 0;
            ds[A2AV_EXCH_SLOT_COUNT] = count / rankNum;
        }
        ds[A2AV_SCRATCH_FLAG_SLOT] = waitSymbol;
    }

    /* Submit exchange SQEs per channel and wait for LOCAL SDMA completion only.
     * Cross-device readiness is NOT waited here — Phase 2's progressive flag poll
     * takes over, overlapping data pulls with peers' exchange latency. */
    static int SyncExchangeCompletion(SqeLocalRingBuffer *chBufs, const ExchangeContext &ctx, uint32_t myCore,
                                      uint32_t numCores, uint32_t numCh)
    {
        bool submitted[ZBAL_AICPU_MAX_CH_PER_CORE] = {};
        for (uint32_t s = 0; s < numCh; s++) {
            submitted[s] = chBufs[s].HasWork();
            if (!submitted[s]) {
                continue;
            }
            uint32_t fid = AicpuWorkspace::FlagIdx(myCore, numCh, s);
            if (AicpuLaunchTaskMc(&chBufs[s], ctx.channels[s], ctx.workspace, myCore, 1, s, fid) < 0) {
                return ERR_DOORBELL_FAILED;
            }
        }
        int dbr = 0;
        for (uint32_t s = 0; s < numCh; s++) {
            if (!submitted[s]) {
                continue;
            }
            CompletionFlag flag(ctx.workspace, myCore, numCh, s);
            if (flag.Wait() < 0) {
                dbr = ERR_WAIT_TIMEOUT;
            }
        }
        /* Core barrier: all cores' exchange SQEs issued before Phase 2 polling starts */
        AicpuCoreBarrier(ctx.workspace, numCores);
        return dbr;
    }

    /* AlltoAllV-specific exchange: write sendBuf + offset + count + flag to all peers */
    static int AlltoAllVExchange(const ExchangeContext &ctx)
    {
        const uint32_t rankNum = ctx.aicpuCtx->rankNum;
        if (rankNum <= 1) {
            return 0;
        }

        const uint32_t numCores = ctx.numCores;
        const uint32_t myCore = ctx.coreId;
        const uint32_t myRank = ctx.aicpuCtx->rankId;
        const uint32_t numCh = ctx.numChPerCore;

        /* Divide ranks across cores */
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

        const uint32_t ringBufSize = ZBAL_AICPU_CORE_RINGBUF_SIZE;
        volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(ctx.workspace, myCore);

        /* Scratch area at end of core ring buffer: per-dstRank block of A2AV_SCRATCH_SLOTS_PER_DST
         * uint64_t slots. SQEs hold source ADDRESSES (not inline data), so each dst needs its
         * own block — otherwise the loop overwrites scratch before SDMA executes. SQEs allocate
         * from per-channel segment heads and never reach this tail. */
        constexpr uint32_t slotsPerDst = A2AV_SCRATCH_SLOTS_PER_DST;
        const uint32_t scratchSlots = slotsPerDst * ranksPerCore;
        volatile uint64_t *scratch =
            reinterpret_cast<volatile uint64_t *>(myBuf + ringBufSize - scratchSlots * sizeof(uint64_t));

        /* Per-channel ring buffers — exchange SQEs spread round-robin across channels */
        SqeLocalRingBuffer chBufs[ZBAL_AICPU_MAX_CH_PER_CORE];
        AicpuCoreRingbufsInit(chBufs, ctx.workspace, myCore);

        /* cumSum (reserved[0]) is in ELEMENTS — convert to BYTES for SDMA. */
        const uint64_t *cumSum = reinterpret_cast<const uint64_t *>(ctx.desc->reserved[0]);
        const uint32_t elemSize = ZBALDataTypeSize(ctx.desc->dataType);

        const volatile AicpuInitContext *initCtx =
            reinterpret_cast<const volatile AicpuInitContext *>(ctx.workspace + ZBAL_AICPU_INIT_CTX_OFFSET);
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));
        const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;

        for (uint32_t dstRank = startRank; dstRank < endRank; dstRank++) {
            uint32_t dstIdx = dstRank - startRank;
            volatile uint64_t *ds = scratch + dstIdx * slotsPerDst;

            FillPeerScratch(ds, dstRank, rankNum, ctx.desc->sendBuffer, cumSum, elemSize, ctx.desc->count,
                            ctx.desc->waitSymbol, reinterpret_cast<const volatile uint64_t *>(ctx.desc->reserved[2]));

            if (dstRank == myRank) {
                /* Self entry: CPU direct write to local exchange area (GVA) — no SDMA round-trip.
                 * Flush both lines: Execute re-reads after invalidate, peers never touch this copy. */
                volatile uint64_t *selfData = reinterpret_cast<volatile uint64_t *>(
                    initCtx->exchangeGva + static_cast<uint64_t>(myRank) * strideBytes);
                volatile uint64_t *selfFlag = reinterpret_cast<volatile uint64_t *>(
                    initCtx->exchangeGva + flagAreaOff + static_cast<uint64_t>(myRank) * strideBytes);
                for (uint32_t s = 0; s < A2AV_EXCH_NUM_DATA_SLOTS; s++) {
                    selfData[s] = ds[s];
                }
                *selfFlag = ctx.desc->waitSymbol;
                AicpuCacheFlush(reinterpret_cast<uintptr_t>(&selfData[0]));
                AicpuCacheFlush(reinterpret_cast<uintptr_t>(selfFlag));
                continue;
            }

            AicpuCacheFlush(reinterpret_cast<uintptr_t>(&ds[A2AV_EXCH_SLOT_SENDBUF]));
            AicpuCacheFlush(reinterpret_cast<uintptr_t>(&ds[A2AV_SCRATCH_FLAG_SLOT]));

            int64_t delta = static_cast<int64_t>(dstRank) - static_cast<int64_t>(myRank);
            int64_t devOff = delta * static_cast<int64_t>(initCtx->localDeviceMemSize);
            uint64_t baseOff =
                initCtx->exchangeGva + static_cast<uint64_t>(devOff) + static_cast<uint64_t>(myRank) * strideBytes;
            uint64_t flagDst = initCtx->exchangeGva + static_cast<uint64_t>(devOff) + flagAreaOff +
                               static_cast<uint64_t>(myRank) * strideBytes;
            /* Round-robin channel: per-peer data + flag SQEs share one channel (FIFO keeps
             * flag after data), peers spread across channels for parallel links. */
            uint32_t ch = dstIdx % numCh;
            if (EnqueuePeerExchangeSqes(chBufs, ch, ds, baseOff, flagDst, ctx.channels[ch]) != 0) {
                return BUILD_ERROR;
            }
        }

        return SyncExchangeCompletion(chBufs, ctx, myCore, numCores, numCh);
    }
};
#endif
