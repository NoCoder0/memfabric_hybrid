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
#ifndef MF_HYBM_CORE_HYBM_DEVICE_COPY_H
#define MF_HYBM_CORE_HYBM_DEVICE_COPY_H

#include "hybm_def.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint64_t *srcPtrs;
    const uint64_t *dstPtrs;
    const uint32_t *lenPtrs;
    const uint32_t *sizePtr;
    uint32_t rankId;
    void *stream;
    uint64_t reserved[4];
} hybm_device_copy_params;

/* A3 local host pool (rankId = 0). Prepare outside capture; stop/drain execution and
 * destroy referencing graphs before entity teardown. */
int32_t hybm_prepare_device_copy(hybm_entity_t e, uint32_t rankId, uint64_t poolBase, uint64_t poolSize);
int32_t hybm_data_device_copy(hybm_entity_t e, const hybm_device_copy_params *params);

#ifdef __cplusplus
}
#endif

#endif // MF_HYBM_CORE_HYBM_DEVICE_COPY_H
