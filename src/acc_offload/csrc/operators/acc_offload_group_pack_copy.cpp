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

#include "acc_offload_group_pack_copy.h"

extern "C" __global__ __aicore__ void OffloadGroupPackCopyOps(GM_ADDR inputs, GM_ADDR outputs, GM_ADDR lens,
                                                              GM_ADDR numLocalExpert, GM_ADDR groupList,
                                                              GM_ADDR packedGroupList)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);

    OffloadGroupPackCopyKernel<uint8_t> op;
    op.Init(inputs, outputs, lens, numLocalExpert, groupList, packedGroupList);
    op.Process();
}

extern "C" void OffloadOpsGroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                        uint32_t *numLocalExpertPtr, int64_t *groupList, int64_t *packedGroupList,
                                        void *stream)
{
    constexpr uint32_t blockDim = 32;
    uint8_t *inputs = reinterpret_cast<uint8_t *>(srcPtrs);
    uint8_t *outputs = reinterpret_cast<uint8_t *>(dstPtrs);
    uint8_t *lens = reinterpret_cast<uint8_t *>(lenPtrs);
    uint8_t *numLocalExpert = reinterpret_cast<uint8_t *>(numLocalExpertPtr);
    uint8_t *groupListPtr = reinterpret_cast<uint8_t *>(groupList);
    uint8_t *packedGroupListPtr = reinterpret_cast<uint8_t *>(packedGroupList);

    OffloadGroupPackCopyOps<<<blockDim, nullptr, stream>>>(inputs, outputs, lens, numLocalExpert, groupListPtr,
                                                           packedGroupListPtr);
}
