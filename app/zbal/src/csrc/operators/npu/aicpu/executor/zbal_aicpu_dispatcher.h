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

#ifndef ZBAL_AICPU_DISPATCHER_H
#define ZBAL_AICPU_DISPATCHER_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/engine/sdma/zbal_aicpu_sdma_sqe.h"
#include "executor/engine/sdma/zbal_aicpu_sdma_sqe_context.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"

class AicpuDispatcher {
public:
    /* Max bytes per SQE */
    static constexpr uint32_t kMaxSqeBytes = 32U * 1024U * 1024U;

    static int CopyData(SqeLocalRingBuffer *ringBufs, uint32_t streamId, uint64_t src, uint64_t dst, uint32_t len,
                        volatile stars_channel_info_t *channel, uint8_t opCode = 0)
    {
        uint32_t remaining = len;
        uint64_t srcOff = src;
        uint64_t dstOff = dst;

        while (remaining > 0) {
            uint32_t chunk = (remaining > kMaxSqeBytes) ? kMaxSqeBytes : remaining;

            uint8_t *sqeAddr = ringBufs[streamId].NextAddr();
            if (sqeAddr == nullptr)
                return -1;

            AicpuStarsSdmaSqe *sqe = reinterpret_cast<AicpuStarsSdmaSqe *>(sqeAddr);
            AicpuFillMemcpySqe(sqe, channel->stream_id, srcOff, dstOff, chunk, opCode);

            srcOff += chunk;
            dstOff += chunk;
            remaining -= chunk;
        }
        return 0;
    }
};

#endif /* ZBAL_AICPU_DISPATCHER_H */
