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

#ifndef ZBAL_AICPU_CHANNEL_H
#define ZBAL_AICPU_CHANNEL_H

#include <cstdint>
#include "executor/engine/sdma/zbal_aicpu_sdma_info.h"
#include "dl_hal_api.h"

using namespace zbal::operators;

struct stars_channel_info_t {
    uint32_t sq_head;
    uint32_t sq_tail;
    uint64_t sq_base;
    uint64_t sq_reg_base;
    uint32_t sq_depth;
    uint32_t sq_id;
    uint32_t cq_id;
    uint32_t logic_cq_id;
    uint64_t cqe_addr;
    uint32_t report_cqe_num;
    uint32_t stream_id;
    uint32_t dev_id;
    uint8_t reserved[4];
};

inline volatile stars_channel_info_t *AicpuGetChannelFromShmem(uint32_t channelIdx)
{
    volatile uint8_t *base = static_cast<volatile uint8_t *>(AicpuShmemGetChannelBase());
    if (base == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<volatile stars_channel_info_t *>(base) + channelIdx;
}

inline int AicpuSqDoorbell(volatile stars_channel_info_t *channel, uint32_t newTail)
{
    halSqCqConfigInfo cfg = {};
    cfg.type = DRV_NORMAL_TYPE;
    cfg.sqId = channel->sq_id;
    cfg.prop = DRV_SQCQ_PROP_SQ_TAIL;
    cfg.value[0] = newTail;
    if (DlHalApi::HalSqCqConfig(channel->dev_id, &cfg) != 0) {
        return -1;
    }
    channel->sq_tail = newTail;
    return 0;
}

class Channel {
public:
    static void GetChannels(volatile stars_channel_info_t **out, uint32_t coreId, uint32_t numCh)
    {
        for (uint32_t s = 0; s < numCh; s++) {
            out[s] = AicpuGetChannelFromShmem(coreId * numCh + s);
            if (out[s] == nullptr || out[s]->sq_base == 0 || out[s]->sq_depth == 0) {
                out[s] = AicpuGetChannelFromShmem(0);
            }
        }
    }
};

#endif /* ZBAL_AICPU_CHANNEL_H */
