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
#ifndef ZBAL_AICPU_GATHER_OP_H
#define ZBAL_AICPU_GATHER_OP_H

#include <cstdint>

#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_debug.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/zbal_aicpu_workspace.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/* Gather exchange data sub-slots. */
constexpr uint32_t GATHER_EXCH_SLOT_SENDBUF = 0;
constexpr uint32_t GATHER_EXCH_SLOT_EXCHANGE_GVA = 1;
constexpr uint32_t GATHER_SCRATCH_SENDBUF = 0;
constexpr uint32_t GATHER_SCRATCH_EXCHANGE_GVA = 1;
constexpr uint32_t GATHER_SCRATCH_FLAG = 2;
constexpr uint32_t GATHER_SCRATCH_SLOTS = 3;
constexpr uint32_t GATHER_COMPLETION_SLOT = 1;
constexpr uint64_t GATHER_STATUS_ERROR_MASK = 1ULL << 63;
constexpr uint64_t GATHER_STATUS_SEQUENCE_MASK = ~GATHER_STATUS_ERROR_MASK;
constexpr uint32_t GATHER_WAIT_TIMEOUT = 6000000U;
constexpr uint32_t GATHER_LOG_RANK_SHIFT = 32U;
constexpr uint32_t GATHER_CHANNEL_RESERVED_SQES = 2U;

/*
 * Gather uses receiver-initiated root pull:
 *   1. Every rank publishes sendBuf and its exchange GVA to root.
 *   2. Root pulls rank-ordered data into recvBuf using SDMA.
 *   3. Root publishes completion so peers cannot reuse sendBuf early.
 */
class GatherOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        int ret = ValidateExchange(ctx);
        if (ret < 0 || ctx.desc->count == 0 || ctx.aicpuCtx->rankNum <= 1) {
            return ret;
        }

        ret = PublishInputMetadata(ctx);
        if (ret < 0) {
            LogError(ctx.aicpuCtx->rankId, ctx.desc->root, ret, ctx.desc->count);
            return ret;
        }
        if (ctx.aicpuCtx->rankId != ctx.desc->root) {
            return BUILD_DONE;
        }

        volatile uint64_t *flagBase =
            reinterpret_cast<volatile uint64_t *>(ctx.aicpuCtx->exchangeGva + FlagAreaOffset(ctx.aicpuCtx->rankNum));
        ret = BarrierAllRanks(flagBase, ctx.aicpuCtx->rankNum, ctx.desc->waitSymbol, true);
        if (ret < 0) {
            LogError(ctx.aicpuCtx->rankId, ctx.desc->root, ret, ctx.desc->count);
        }
        return ret;
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t myRank = alg.ctx->rankId;
        const uint32_t rankNum = alg.ctx->rankNum;
        AICPU_DBG(TAG_ALGO_GATHER, (static_cast<uint64_t>(op.root) << GATHER_LOG_RANK_SHIFT) | myRank, op.dataSize);

        if (op.dataSize == 0) {
            return BUILD_DONE;
        }
        if (rankNum <= 1) {
            int ret = ExecuteRootPull(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
            if (ret < 0) {
                LogError(myRank, op.root, ret, op.dataSize);
            }
            return ret;
        }
        if (myRank != op.root) {
            int ret = WaitCompletion(op, rankNum);
            if (ret < 0) {
                LogError(myRank, op.root, ret, op.dataSize);
            }
            return ret;
        }

        int dataRet = ExecuteRootPull(alg, op, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
        if (dataRet < 0) {
            ResetRingBuffers(ringBufs, numChPerCore);
        }
        if (dataRet < 0) {
            LogError(myRank, op.root, dataRet, op.dataSize);
        }

        int notifyRet = PublishCompletion(op, rankNum, ringBufs, channels, numChPerCore, workspace, coreId, dataRet);
        if (notifyRet < 0) {
            LogError(myRank, op.root, notifyRet, op.dataSize);
            return notifyRet;
        }
        return dataRet;
    }

private:
    struct CopyCursor {
        uint32_t rank = 0;
        uint64_t offset = 0;
        uint64_t chunkIndex = 0;
    };

    static uint32_t StrideBytes()
    {
        return ZBAL_AICPU_EXCHANGE_STRIDE * static_cast<uint32_t>(sizeof(uint64_t));
    }

    static uint64_t FlagAreaOffset(uint32_t rankNum)
    {
        return static_cast<uint64_t>(rankNum) * StrideBytes();
    }

    static void LogError(uint32_t myRank, uint32_t root, int ret, uint64_t dataSize)
    {
        AICPU_DBG(TAG_ALGO_GATHER, (static_cast<uint64_t>(root) << GATHER_LOG_RANK_SHIFT) | myRank, dataSize);
        AICPU_DBG(TAG_RETURN, ret, myRank);
    }

    static int ValidateExchange(const ExchangeContext &ctx)
    {
        if (ctx.aicpuCtx == nullptr || ctx.desc == nullptr || ctx.workspace == nullptr || ctx.channels == nullptr) {
            return ERR_CHANNEL_INVALID;
        }
        const uint32_t rankNum = ctx.aicpuCtx->rankNum;
        if (rankNum == 0 || rankNum > ZBAL_MAX_RANKS || ctx.desc->root >= rankNum || ctx.numCores != 1 ||
            ctx.numChPerCore == 0 || ctx.numChPerCore > ZBAL_AICPU_MAX_CH_PER_CORE || ctx.channels[0] == nullptr) {
            LogError(ctx.aicpuCtx->rankId, ctx.desc->root, BUILD_ERROR, ctx.desc->count);
            return BUILD_ERROR;
        }
        if (ctx.desc->count > UINT64_MAX / rankNum) {
            LogError(ctx.aicpuCtx->rankId, ctx.desc->root, BUILD_ERROR, ctx.desc->count);
            return BUILD_ERROR;
        }
        if (rankNum > 1 && ctx.desc->reserved[0] == 0) {
            LogError(ctx.aicpuCtx->rankId, ctx.desc->root, BUILD_ERROR, ctx.desc->reserved[0]);
            return BUILD_ERROR;
        }
        return BUILD_DONE;
    }

    static int EnqueueInputMetadata(SqeLocalRingBuffer &ring, const ExchangeContext &ctx, volatile uint64_t *scratch)
    {
        const uint32_t myRank = ctx.aicpuCtx->rankId;
        const uint64_t rootExchangeGva = ctx.desc->reserved[0];
        const uint64_t dataDst = rootExchangeGva + static_cast<uint64_t>(myRank) * StrideBytes();
        const uint64_t flagDst =
            rootExchangeGva + FlagAreaOffset(ctx.aicpuCtx->rankNum) + static_cast<uint64_t>(myRank) * StrideBytes();

        if (AicpuDispatcher::CopyData(&ring, 0U, reinterpret_cast<uint64_t>(&scratch[GATHER_SCRATCH_SENDBUF]), dataDst,
                                      sizeof(uint64_t), ctx.channels[0]) != 0) {
            return BUILD_ERROR;
        }
        if (AicpuDispatcher::CopyData(&ring, 0U, reinterpret_cast<uint64_t>(&scratch[GATHER_SCRATCH_EXCHANGE_GVA]),
                                      dataDst + sizeof(uint64_t), sizeof(uint64_t), ctx.channels[0]) != 0) {
            return BUILD_ERROR;
        }
        return AicpuDispatcher::CopyData(&ring, 0U, reinterpret_cast<uint64_t>(&scratch[GATHER_SCRATCH_FLAG]), flagDst,
                                         sizeof(uint64_t), ctx.channels[0]);
    }

    static int PublishInputMetadata(const ExchangeContext &ctx)
    {
        volatile uint8_t *coreBuf = AicpuWorkspace::CoreRingBuf(ctx.workspace, ctx.coreId);
        volatile uint64_t *scratch = reinterpret_cast<volatile uint64_t *>(coreBuf + ZBAL_AICPU_CORE_RINGBUF_SIZE -
                                                                           GATHER_SCRATCH_SLOTS * sizeof(uint64_t));
        scratch[GATHER_SCRATCH_SENDBUF] = ctx.desc->sendBuffer;
        scratch[GATHER_SCRATCH_EXCHANGE_GVA] = ctx.aicpuCtx->exchangeGva;
        scratch[GATHER_SCRATCH_FLAG] = ctx.desc->waitSymbol;
        AicpuCacheFlush(reinterpret_cast<uintptr_t>(&scratch[0]));

        SqeLocalRingBuffer ring;
        ring.Init(const_cast<uint8_t *>(coreBuf));
        int ret = EnqueueInputMetadata(ring, ctx, scratch);
        if (ret < 0) {
            return ret;
        }

        uint32_t fid = AicpuWorkspace::FlagIdx(ctx.coreId, ctx.numChPerCore, 0);
        if (AicpuLaunchTaskMc(&ring, ctx.channels[0], ctx.workspace, ctx.coreId, 1, 0, fid) < 0) {
            return ERR_DOORBELL_FAILED;
        }
        return CompletionFlag(ctx.workspace, fid).Wait() < 0 ? ERR_WAIT_TIMEOUT : BUILD_DONE;
    }

    static int ValidateRootExecution(const CommOpParams &op, uint32_t rankNum, uint32_t numChPerCore, uint32_t numCores)
    {
        if (rankNum == 0 || rankNum > ZBAL_MAX_RANKS || op.root >= rankNum || numChPerCore == 0 || numCores != 1 ||
            numChPerCore > ZBAL_AICPU_MAX_CH_PER_CORE || op.recvBuf == 0 || op.sendBuf == 0) {
            return BUILD_ERROR;
        }
        if (op.dataSize > UINT64_MAX / rankNum) {
            return BUILD_ERROR;
        }
        uint64_t totalBytes = op.dataSize * rankNum;
        if (op.recvBuf > UINT64_MAX - totalBytes || op.sendBuf > UINT64_MAX - op.dataSize) {
            return BUILD_ERROR;
        }
        return BUILD_DONE;
    }

    static uint32_t ChannelBatchCapacity(volatile stars_channel_info_t *channel)
    {
        if (channel == nullptr || channel->sq_depth <= GATHER_CHANNEL_RESERVED_SQES) {
            return 0;
        }
        const uint32_t localCapacity = ZBAL_AICPU_MAX_SQE_PER_CORE - 1;
        const uint32_t hardwareCapacity = channel->sq_depth - GATHER_CHANNEL_RESERVED_SQES;
        return localCapacity < hardwareCapacity ? localCapacity : hardwareCapacity;
    }

    static uint64_t ReadExchangeSlot(uint64_t exchangeGva, uint32_t rank, uint32_t slot)
    {
        volatile uint64_t *base = reinterpret_cast<volatile uint64_t *>(exchangeGva);
        volatile uint64_t *addr = &base[rank * ZBAL_AICPU_EXCHANGE_STRIDE + slot];
        AicpuCacheInvalidate(reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(addr)));
        return *addr;
    }

    static uint64_t SourceBuffer(const AicpuAlgorithmCtx &alg, const CommOpParams &op, uint32_t rank)
    {
        return rank == alg.ctx->rankId ? op.sendBuf : ReadExchangeSlot(op.exchangeGva, rank, GATHER_EXCH_SLOT_SENDBUF);
    }

    static bool IsRootInPlace(const AicpuAlgorithmCtx &alg, const CommOpParams &op, const CopyCursor &cursor)
    {
        if (cursor.rank != alg.ctx->rankId || cursor.offset != 0) {
            return false;
        }
        uint64_t rootDst = op.recvBuf + static_cast<uint64_t>(cursor.rank) * op.dataSize;
        return op.sendBuf == rootDst;
    }

    static void AdvanceCopyCursor(CopyCursor &cursor, uint64_t chunkLen, uint64_t dataSize)
    {
        cursor.offset += chunkLen;
        cursor.chunkIndex++;
        if (cursor.offset == dataSize) {
            cursor.rank++;
            cursor.offset = 0;
        }
    }

    static int EnqueueCopyBatch(AicpuAlgorithmCtx &alg, const CommOpParams &op, CopyCursor &cursor)
    {
        while (cursor.rank < alg.ctx->rankNum) {
            if (IsRootInPlace(alg, op, cursor)) {
                cursor.rank++;
                continue;
            }

            uint32_t sid = static_cast<uint32_t>(cursor.chunkIndex % op.numChPerCore);
            uint32_t capacity = ChannelBatchCapacity(op.channels[sid]);
            if (capacity == 0) {
                return ERR_CHANNEL_INVALID;
            }
            if (alg.ringBufs[sid].sqeCnt >= capacity) {
                return BUILD_MORE;
            }

            uint64_t srcBase = SourceBuffer(alg, op, cursor.rank);
            if (srcBase == 0 || srcBase > UINT64_MAX - op.dataSize) {
                return BUILD_ERROR;
            }
            uint64_t remaining = op.dataSize - cursor.offset;
            uint32_t chunkLen = remaining > AicpuDispatcher::kMaxSqeBytes ? AicpuDispatcher::kMaxSqeBytes
                                                                          : static_cast<uint32_t>(remaining);
            uint64_t dst = op.recvBuf + static_cast<uint64_t>(cursor.rank) * op.dataSize + cursor.offset;
            if (AicpuDispatcher::CopyData(alg.ringBufs, sid, srcBase + cursor.offset, dst, chunkLen,
                                          op.channels[sid]) != 0) {
                return BUILD_ERROR;
            }
            AdvanceCopyCursor(cursor, chunkLen, op.dataSize);
        }
        return BUILD_DONE;
    }

    static int ExecuteRootPull(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                               volatile stars_channel_info_t **channels, uint32_t numChPerCore,
                               volatile uint8_t *workspace, uint32_t coreId, uint32_t numCores)
    {
        int ret = ValidateRootExecution(op, alg.ctx->rankNum, numChPerCore, numCores);
        if (ret < 0) {
            return ret;
        }

        CopyCursor cursor;
        do {
            ret = EnqueueCopyBatch(alg, op, cursor);
            if (ret < 0) {
                return ret;
            }
            if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
                return ERR_WAIT_TIMEOUT;
            }
        } while (ret == BUILD_MORE);
        return BUILD_DONE;
    }

    static void ResetRingBuffers(SqeLocalRingBuffer *ringBufs, uint32_t numChPerCore)
    {
        for (uint32_t sid = 0; sid < numChPerCore; sid++) {
            ringBufs[sid].Init(ringBufs[sid].localBuff);
        }
    }

    static uint64_t PeerExchangeGva(const CommOpParams &op, uint32_t rank)
    {
        return rank == op.root ? op.exchangeGva : ReadExchangeSlot(op.exchangeGva, rank, GATHER_EXCH_SLOT_EXCHANGE_GVA);
    }

    static int BuildCompletionBatch(const CommOpParams &op, uint32_t rankNum, SqeLocalRingBuffer *ringBufs,
                                    uint64_t statusSrc, uint32_t &rank)
    {
        const uint64_t flagAreaOff = FlagAreaOffset(rankNum);
        while (rank < rankNum) {
            uint32_t sid = rank % op.numChPerCore;
            uint32_t capacity = ChannelBatchCapacity(op.channels[sid]);
            if (capacity == 0) {
                return ERR_CHANNEL_INVALID;
            }
            if (ringBufs[sid].sqeCnt >= capacity) {
                return BUILD_MORE;
            }

            uint64_t peerExchangeGva = PeerExchangeGva(op, rank);
            if (peerExchangeGva == 0) {
                return BUILD_ERROR;
            }
            uint64_t flagDst = peerExchangeGva + flagAreaOff + static_cast<uint64_t>(op.root) * StrideBytes() +
                               GATHER_COMPLETION_SLOT * sizeof(uint64_t);
            if (AicpuDispatcher::CopyData(ringBufs, sid, statusSrc, flagDst, sizeof(uint64_t), op.channels[sid]) != 0) {
                return BUILD_ERROR;
            }
            rank++;
        }
        return BUILD_DONE;
    }

    static int PublishCompletion(const CommOpParams &op, uint32_t rankNum, SqeLocalRingBuffer *ringBufs,
                                 volatile stars_channel_info_t **channels, uint32_t numChPerCore,
                                 volatile uint8_t *workspace, uint32_t coreId, int dataRet)
    {
        volatile uint8_t *coreBuf = AicpuWorkspace::CoreRingBuf(workspace, coreId);
        volatile uint64_t *scratch =
            reinterpret_cast<volatile uint64_t *>(coreBuf + ZBAL_AICPU_CORE_RINGBUF_SIZE - sizeof(uint64_t));
        uint64_t status = op.waitSymbol & GATHER_STATUS_SEQUENCE_MASK;
        scratch[0] = dataRet < 0 ? status | GATHER_STATUS_ERROR_MASK : status;
        AicpuCacheFlush(reinterpret_cast<uintptr_t>(&scratch[0]));

        uint32_t rank = 0;
        int ret;
        do {
            ret = BuildCompletionBatch(op, rankNum, ringBufs, reinterpret_cast<uint64_t>(&scratch[0]), rank);
            if (ret < 0) {
                return ret;
            }
            if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
                return ERR_WAIT_TIMEOUT;
            }
        } while (ret == BUILD_MORE);
        return BUILD_DONE;
    }

    static int WaitCompletion(const CommOpParams &op, uint32_t rankNum)
    {
        const uint64_t completionOff = FlagAreaOffset(rankNum) + static_cast<uint64_t>(op.root) * StrideBytes() +
                                       GATHER_COMPLETION_SLOT * sizeof(uint64_t);
        volatile uint64_t *completion = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + completionOff);
        const uint64_t expected = op.waitSymbol & GATHER_STATUS_SEQUENCE_MASK;

        for (uint32_t t = 0; t < GATHER_WAIT_TIMEOUT; t++) {
            AicpuCacheInvalidate(reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(completion)));
            uint64_t status = *completion;
            if ((status & GATHER_STATUS_SEQUENCE_MASK) != expected) {
                continue;
            }
            return (status & GATHER_STATUS_ERROR_MASK) != 0 ? BUILD_ERROR : BUILD_DONE;
        }
        return ERR_WAIT_TIMEOUT;
    }
};

#endif /* ZBAL_AICPU_GATHER_OP_H */
