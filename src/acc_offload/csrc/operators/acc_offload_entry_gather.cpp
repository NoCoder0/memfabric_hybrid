/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the Mulan PSL v2 for more details.
 */

#include "acc_offload_entry_gather.h"

extern "C" __global__ __aicore__ void OffloadEntryGatherOps(GM_ADDR ids, GM_ADDR dst, GM_ADDR count, uint64_t poolGva,
                                                            uint64_t slotStride, uint32_t entryBytes,
                                                            uint32_t rowsPerSlot)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    OffloadEntryGatherKernel op;
    op.Init(ids, dst, count, poolGva, slotStride, entryBytes, rowsPerSlot);
    op.Process();
}

extern "C" void OffloadOpsEntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint64_t poolGva,
                                      uint64_t slotStride, uint32_t entryBytes, uint32_t rowsPerSlot, uint32_t blockDim,
                                      void *stream)
{
    auto *ids = reinterpret_cast<uint8_t *>(idsPtr);
    auto *dst = reinterpret_cast<uint8_t *>(dstPtr);
    auto *count = reinterpret_cast<uint8_t *>(countPtr);

    OffloadEntryGatherOps<<<blockDim, nullptr, stream>>>(ids, dst, count, poolGva, slotStride, entryBytes, rowsPerSlot);
}
