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
#ifndef ZBAL_AICPU_BROADCAST_OP_H
#define ZBAL_AICPU_BROADCAST_OP_H
#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/*
 * Broadcast: root sends same data to all ranks.
 * All non-root ranks SDMA read from root's sendBuf.
 * Root skips copy (host passes sendBuf == recvBuf for broadcast).
 */
class BroadcastOp {
public:
    static int AddrExchange(uint32_t, const ExchangeContext &ctx)
    {
        return FullMeshExchange::Execute(ctx);
    }

    static int Execute(AicpuAlgorithmCtx &alg, const CommOpParams &op, SqeLocalRingBuffer *ringBufs,
                       volatile stars_channel_info_t **channels, uint32_t numChPerCore, volatile uint8_t *workspace,
                       uint32_t coreId, uint32_t numCores)
    {
        const uint32_t rootRank = op.root;
        const uint32_t myRank = alg.ctx->rankId;

        if (op.dataSize == 0) {
            return BUILD_DONE;
        }

        /* Non-root: SDMA copy from root sendBuf to own recvBuf.
         * Root skips copy (host passes sendBuf == recvBuf). */
        if (myRank != rootRank) {
            uint64_t myOff;
            uint64_t myLen;
            AicpuParallelSlice(op.dataSize, op.coreId, op.numCores, myOff, myLen);
            if (myLen != 0) {
                uint64_t rootSrc = PeerOutputBuf(op.exchangeGva, rootRank) + myOff;
                if (AicpuDispatcher::CopyData(ringBufs, 0U, rootSrc, op.recvBuf + myOff, static_cast<uint32_t>(myLen),
                                              channels[0], op.reduceOp)) {
                    return BUILD_ERROR;
                }
                if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
                    return ERR_WAIT_TIMEOUT;
                }
            }
        }

        /* End-of-op barrier (flag slot[1]): root must wait for non-root SDMA
         * completion before exiting Execute. Without this, root returns
         * BUILD_DONE immediately and the next broadcast's FullMeshExchange
         * overwrites exchange area slot[0] (root sendBuf GVA) while non-root
         * ranks still read it via PeerOutputBuf → race → wrong root src GVA
         * → SDMA reads zeros/garbage → broadcast_object_list pickle error
         * "invalid load key '\x00'". Race window widens at 16 ranks
         * (FullMeshExchange AddrExchange SDMA count scales with rankNum).
         * Mirrors MTE BarrierAll(true, true, flagMagic) at ZBALBroadcastFull
         * CopyKernel::Process end; matches AllReduceOp end-of-op pattern. */
        if (alg.ctx->rankNum > 1 && EndOfOpBarrier(alg, op, numChPerCore, workspace, coreId, numCores) < 0) {
            return ERR_WAIT_TIMEOUT;
        }
        return BUILD_DONE;
    }

private:
    /* Cross-rank end-of-op barrier: each rank writes waitSymbol to all peers'
     * flag slot[1], then polls all peers' slot[1] == waitSymbol.
     * Uses slot[1] — FullMeshExchange uses slot[0]; broadcast has no RS→AG
     * intermediate barrier (unlike AllReduceOp), so slot[1] is free. */
    static int EndOfOpBarrier(AicpuAlgorithmCtx &alg, const CommOpParams &op, uint32_t numChPerCore,
                              volatile uint8_t *workspace, uint32_t coreId, uint32_t numCores)
    {
        if (coreId != 0) {
            AicpuCoreBarrier(workspace, numCores);
            return 0;
        }
        if (WriteEndOfOpFlag(alg, op, workspace, numChPerCore) < 0) {
            return -1;
        }
        if (PollEndOfOpFlag(alg, op) < 0) {
            return -1;
        }
        AicpuCoreBarrier(workspace, numCores);
        return 0;
    }

    /* Write waitSymbol to all peers' flag slot[1] via SDMA + self write.
     * Matches AllReduceOp::Execute WriteEndOfOpFlag pattern (slot[1] offset). */
    static int WriteEndOfOpFlag(AicpuAlgorithmCtx &alg, const CommOpParams &op, volatile uint8_t *workspace,
                                uint32_t numChPerCore)
    {
        const uint32_t rankNum = alg.ctx->rankNum;
        const uint32_t myRank = alg.ctx->rankId;
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
        const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;
        const uint64_t waitSymbol = op.waitSymbol;
        constexpr uint32_t SID = 0;

        volatile uint8_t *myBuf = AicpuWorkspace::CoreRingBuf(workspace, 0);
        volatile uint64_t *scratch =
            reinterpret_cast<volatile uint64_t *>(myBuf + ZBAL_AICPU_CORE_RINGBUF_SIZE - sizeof(uint64_t));
        *scratch = waitSymbol;
        AicpuCacheFlush(reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(scratch)));
        uint64_t sentinelSrc = reinterpret_cast<uint64_t>(scratch);

        SqeLocalRingBuffer eb;
        eb.Init(const_cast<uint8_t *>(myBuf));
        for (uint32_t dstRank = 0; dstRank < rankNum; dstRank++) {
            if (dstRank == myRank) {
                continue;
            }
            int64_t delta = (int64_t)dstRank - (int64_t)myRank;
            int64_t devOff = delta * (int64_t)alg.ctx->localDeviceMemSize;
            uint64_t flagDst = alg.ctx->exchangeGva + (uint64_t)devOff + flagAreaOff + (uint64_t)myRank * strideBytes +
                               sizeof(uint64_t);
            if (AicpuDispatcher::CopyData(&eb, 0U, sentinelSrc, flagDst, sizeof(uint64_t), op.channels[SID]) != 0) {
                return -1;
            }
        }
        volatile uint64_t *selfFlag = reinterpret_cast<volatile uint64_t *>(
            alg.ctx->exchangeGva + flagAreaOff + (uint64_t)myRank * strideBytes + sizeof(uint64_t));
        *selfFlag = waitSymbol;
        AicpuCacheFlush(reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(selfFlag)));
        if (eb.HasWork()) {
            uint32_t fid = AicpuWorkspace::FlagIdx(0, numChPerCore, 0);
            if (AicpuLaunchTaskMc(&eb, op.channels[SID], workspace, 0, 1, 0, fid) < 0 ||
                CompletionFlag(workspace, fid).Wait() < 0) {
                return -1;
            }
        }
        return 0;
    }

    /* Poll all peers' flag slot[1] == waitSymbol with cache invalidate. */
    static int PollEndOfOpFlag(AicpuAlgorithmCtx &alg, const CommOpParams &op)
    {
        const uint32_t rankNum = alg.ctx->rankNum;
        const uint32_t myRank = alg.ctx->rankId;
        const uint32_t strideBytes = ZBAL_AICPU_EXCHANGE_STRIDE * (uint32_t)sizeof(uint64_t);
        const uint64_t flagAreaOff = static_cast<uint64_t>(rankNum) * strideBytes;
        const uint64_t waitSymbol = op.waitSymbol;
        volatile uint64_t *flagBase = reinterpret_cast<volatile uint64_t *>(alg.ctx->exchangeGva + flagAreaOff);
        constexpr uint32_t kEndBarrierTimeout = 6000000;
        for (uint32_t r = 0; r < rankNum; r++) {
            if (r == myRank) {
                continue;
            }
            volatile uint64_t *peerFlag = &flagBase[r * ZBAL_AICPU_EXCHANGE_STRIDE + 1];
            bool ready = false;
            for (uint32_t t = 0; t < kEndBarrierTimeout && !ready; t++) {
                uintptr_t fa = reinterpret_cast<uintptr_t>(const_cast<uint64_t *>(peerFlag));
                AicpuCacheInvalidate(fa);
                if (*peerFlag == waitSymbol) {
                    ready = true;
                }
            }
            if (!ready) {
                return -1;
            }
        }
        return 0;
    }
};
#endif
