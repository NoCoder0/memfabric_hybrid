/* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include "acc_offload_operators.h"

extern __global__ __aicpu__ uint32_t AccOffloadAicpuInitialize(void *);

extern "C" void OffloadOpsInitializeAicpu(void *stream)
{
    uint64_t args = 0;
    AccOffloadAicpuInitialize<<<1, nullptr, stream>>>(&args, sizeof(args));
}
