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
#ifndef ZBAL_AICPU_SCATTER_OP_H
#define ZBAL_AICPU_SCATTER_OP_H
#include <cstdint>
#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_comm_alg.h"
#include "executor/zbal_aicpu_dispatcher.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

/*
 * Scatter: root distributes data slices to all ranks.
 *
 * Flattened layout (same as AIV): root's sendBuffer is a contiguous tensor
 *   [rank0_data | rank1_data | ... | rankN-1_data]
 * Each rank's data is at offset myRank * dataSize within the flattened buffer.
 * No indirect pointer dereference — direct offset computation.
 */
class ScatterOp {
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

        if (op.dataSize != 0) {
            /* Read root's flattened sendBuf base GVA from exchange area.
             * Must invalidate cache before reading: FullMeshExchange writes slot[0] via
             * SDMA (cross-device), but its cross-device sync only invalidates the FLAG
             * area cache line, not the DATA area. Without invalidate, CPU may read a
             * stale GVA from a previous operation → wrong rootBufGva → wrong data.
             * Matches the invalidate-before-read pattern in AllReduceOp / ReduceScatterOp. */
            uintptr_t rootSlotAddr = reinterpret_cast<uintptr_t>(
                &reinterpret_cast<const uint64_t *>(op.exchangeGva)[rootRank * ZBAL_AICPU_EXCHANGE_STRIDE]);
            AicpuCacheInvalidate(rootSlotAddr);
            uint64_t rootBufGva = PeerOutputBuf(op.exchangeGva, rootRank);

            /* Flattened layout: my rank's data is at rootBufGva + myRank * dataSize.
             * Matches AIV: rankDataPtr = rootDataAddr + rank * elementsPerRank.
             * No indirect pointer dereference — direct offset into contiguous buffer. */
            uint64_t myDataGva = rootBufGva + static_cast<uint64_t>(myRank) * op.dataSize;

            uint64_t myOff;
            uint64_t myLen;
            AicpuParallelSlice(op.dataSize, op.coreId, op.numCores, myOff, myLen);
            if (myLen != 0) {
                uint64_t rSrc = myDataGva + myOff;
                uint64_t lDst = op.recvBuf + myOff;
                if (AicpuDispatcher::CopyData(ringBufs, 0U, rSrc, lDst, static_cast<uint32_t>(myLen), channels[0])) {
                    return BUILD_ERROR;
                }

                if (AicpuSubmitAndWait(ringBufs, channels, numChPerCore, workspace, coreId) < 0) {
                    return ERR_WAIT_TIMEOUT;
                }
            }
        }

        return BUILD_DONE;
    }
};
#endif
