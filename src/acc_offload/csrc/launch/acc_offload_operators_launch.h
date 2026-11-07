/* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#ifndef MF_ACC_OFFLOAD_OPERATORS_LAUNCH_H
#define MF_ACC_OFFLOAD_OPERATORS_LAUNCH_H

#include <cstdint>

struct AccOffloadAicpuParam {
    void *entity;
    uint64_t *srcPtrs;
    uint64_t *dstPtrs;
    uint32_t *lenPtrs;
    uint32_t *sizePtr;
};

using AccOffloadAicpuCallback = int32_t (*)(const AccOffloadAicpuParam &, void *stream);

#endif
