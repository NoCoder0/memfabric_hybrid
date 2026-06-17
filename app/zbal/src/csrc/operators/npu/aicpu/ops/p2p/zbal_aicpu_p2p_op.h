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
#ifndef ZBAL_AICPU_P2P_OP_H
#define ZBAL_AICPU_P2P_OP_H
#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/*
 * P2P Protocol:
 *   data[r]:  sendBuf GVA        (offset 0)
 *   flag[r]:  ready sentinel     (offset rankNum * strideBytes)
 *   ack[r]:   completion ack     (offset 2 * rankNum * strideBytes)
 *
 * Send: write data+flag → peer, poll ack, clear ack
 * Recv: poll flag, read data, SDMA copy, clear flag, write ack → sender
 */

class SendOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        return NullExchange::Execute(ctx);
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        (void)ringBufs;
        (void)numChPerCore;
        const uint32_t myRank = alg.ctx->rankId;
        const uint32_t peer = op.root;
        const uint32_t rankNum = alg.ctx->rankNum;
        if (myRank == peer) {
            return BUILD_DONE;
        }

        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
        const uint64_t flagOff = (uint64_t)rankNum * strideBytes;
        const uint64_t ackOff = 2ULL * (uint64_t)rankNum * strideBytes;

        int64_t delta = (int64_t)peer - (int64_t)myRank;
        uint64_t peerExch = op.exchangeGva + (uint64_t)(delta * (int64_t)alg.ctx->localDeviceMemSize);

        const uint32_t rbSize = ZBAL_AICPU_CORE_RINGBUF_SIZE;
        volatile uint8_t *buf = AicpuWorkspace::CoreRingBuf(workspace, 0);
        volatile uint64_t *scratch = reinterpret_cast<volatile uint64_t *>(buf + rbSize - 16);

        /* 1. Write sendBuf GVA + sentinel → peer */
        scratch[0] = op.sendBuf;
        scratch[1] = 0x1U;
        {
            SqeLocalRingBuffer eb;
            eb.Init(const_cast<uint8_t *>(buf));
            uint64_t dataDst = peerExch + (uint64_t)myRank * strideBytes;
            uint64_t flagDst = peerExch + flagOff + (uint64_t)myRank * strideBytes;
            if (AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&scratch[0]), dataDst, sizeof(uint64_t),
                                          channels[0]) != 0 ||
                AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(&scratch[1]), flagDst, sizeof(uint64_t),
                                          channels[0]) != 0) {
                return BUILD_ERROR;
            }
            uint32_t fid = AicpuWorkspace::FlagIdx(0, 1, 0);
            if (AicpuLaunchTaskMc(&eb, channels[0], workspace, 0, 1, 0, fid) < 0 ||
                CompletionFlag(workspace, fid).Wait() < 0) {
                return ERR_WAIT_TIMEOUT;
            }
            AicpuMemBarrier();
        }

        /* 2. Poll local ack[peer] */
        int ret = 0;
        volatile uint64_t *ackBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + ackOff);
        bool got = false;
        for (uint32_t t = 0; t < 12000000U && !got; t++) {
            uintptr_t fa =
                reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(&ackBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE]));
            AicpuCacheInvalidate(fa);
            if (ackBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE] != 0) {
                got = true;
            }
        }
        if (!got) {
            ret = ERR_WAIT_TIMEOUT;
        }

        /* 3. Clear local ack[peer] */
        if (ret == 0) {
            ackBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE] = 0;
            uintptr_t fa =
                reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(&ackBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE]));
            AicpuCacheFlush(fa);
        }

        return ret;
    }
};

class RecvOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        return NullExchange::Execute(ctx);
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t myRank = alg.ctx->rankId;
        const uint32_t peer = op.root;
        const uint32_t rankNum = alg.ctx->rankNum;
        if (myRank == peer || op.dataSize == 0) {
            return BUILD_DONE;
        }

        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
        const uint64_t flagOff = (uint64_t)rankNum * strideBytes;
        const uint64_t ackOff = 2ULL * (uint64_t)rankNum * strideBytes;

        int ret = 0;
        uint64_t sendGva = 0;

        /* Barrier 1: wait for flag poll (core 0 does the poll) */
        if (coreId == 0) {
            volatile uint64_t *flagBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + flagOff);
            /* Aggressive cache invalidation before polling — cross-chip SDMA may need extra barriers */
            __asm__ __volatile__("isb" ::: "memory");
            AicpuMemBarrier();
            bool ready = false;
            const uint32_t timeout = 6000000U;
            for (uint32_t t = 0; t < timeout && !ready; t++) {
                uintptr_t fa =
                    reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(&flagBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE]));
                AicpuCacheInvalidate(fa);
                if (flagBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE] != 0) {
                    ready = true;
                }
            }
            if (!ready) {
                ret = ERR_WAIT_TIMEOUT;
            }
        }
        AicpuCoreBarrier(workspace, numCores);

        /* Barrier 2: read GVA + SDMA copy */
        if (ret == 0) {
            volatile uint64_t *dataBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva);
            sendGva = dataBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE];

            uint64_t myOff;
            uint64_t myLen;
            AicpuParallelSlice(op.dataSize, op.coreId, op.numCores, myOff, myLen);
            if (myLen > 0) {
                if (AicpuDispatcher::CopyData(ringBufs, 0U, sendGva + myOff, op.recvBuf + myOff, (uint32_t)myLen,
                                              channels[0], op.reduceOp) != 0 ||
                    AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
                    ret = ERR_WAIT_TIMEOUT;
                }
            }
        }
        AicpuCoreBarrier(workspace, numCores);

        /* Barrier 3: core 0 clears flag + writes ACK */
        if (coreId == 0 && ret == 0) {
            volatile uint64_t *flagBase = reinterpret_cast<volatile uint64_t *>(op.exchangeGva + flagOff);
            flagBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE] = 0;
            uintptr_t fa =
                reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(&flagBase[peer * ZBAL_AICPU_EXCHANGE_STRIDE]));
            AicpuCacheFlush(fa);

            int64_t delta = (int64_t)peer - (int64_t)myRank;
            uint64_t peerExch = op.exchangeGva + (uint64_t)(delta * (int64_t)alg.ctx->localDeviceMemSize);
            uint64_t ackDst = peerExch + ackOff + (uint64_t)myRank * strideBytes;

            const uint32_t rbSize = ZBAL_AICPU_CORE_RINGBUF_SIZE;
            volatile uint8_t *buf = AicpuWorkspace::CoreRingBuf(workspace, 0);
            volatile uint64_t *scratch = reinterpret_cast<volatile uint64_t *>(buf + rbSize - 8);
            *scratch = 0x1U;
            SqeLocalRingBuffer eb;
            eb.Init(const_cast<uint8_t *>(buf));
            AicpuDispatcher::CopyData(&eb, 0U, reinterpret_cast<uint64_t>(scratch), ackDst, sizeof(uint64_t),
                                      channels[0]);
            uint32_t fid = AicpuWorkspace::FlagIdx(0, 1, 0);
            AicpuLaunchTaskMc(&eb, channels[0], workspace, 0, 1, 0, fid);
            if (CompletionFlag(workspace, fid).Wait() < 0) {
                ret = ERR_WAIT_TIMEOUT;
            }
        }
        AicpuCoreBarrier(workspace, numCores);

        return ret;
    }
};
#endif
