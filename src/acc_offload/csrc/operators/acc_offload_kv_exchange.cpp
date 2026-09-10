/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/

#include "acc_offload_kv_exchange.h"

extern "C" __global__ __aicore__ void OffloadKvExchangeOps(GM_ADDR meta)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    AscendC::TPipe pipe;
    OffloadKvExchangeKernel op;
    op.Init(&pipe, meta);
    op.Process();
}

extern "C" void OffloadOpsKvExchange(uint64_t *metaPtr, uint32_t blockDim, void *stream)
{
    uint8_t *meta = reinterpret_cast<uint8_t *>(metaPtr);
    OffloadKvExchangeOps<<<blockDim, nullptr, stream>>>(meta);
}
