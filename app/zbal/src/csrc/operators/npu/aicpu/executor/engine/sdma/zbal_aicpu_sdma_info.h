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
#ifndef ZBAL_AICPU_SDMA_INFO_H
#define ZBAL_AICPU_SDMA_INFO_H

#include <cstdint>
#include "common/zbal_smem_meta.h"

static inline void *AicpuShmemGetWorkspaceAddr()
{
    uint64_t metaAddr = ZBAL_SMEM_META_ADDR + ZBAL_SMEM_GLOBAL_META_SIZE;
    return *reinterpret_cast<void **>(metaAddr + ZBAL_SMEM_META_SDMA_WORKSPACE_OFFSET);
}

static inline volatile void *AicpuShmemGetChannelBase()
{
    uint8_t *workspace = static_cast<uint8_t *>(AicpuShmemGetWorkspaceAddr());
    if (workspace == nullptr) {
        return nullptr;
    }
    return workspace + ZBAL_SDMA_CHANNEL_FLAG_SIZE;
}

#endif /* ZBAL_AICPU_SDMA_INFO_H */
