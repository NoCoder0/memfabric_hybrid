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

#ifndef MF_HYBM_BATCH_COPY_PROBE_H
#define MF_HYBM_BATCH_COPY_PROBE_H

#include <cstdint>
#include <type_traits>

struct HybmBatchCopyProbeParam {
    uint32_t peerIndex{0};
    uint32_t rangeIndex{0};
    uint64_t srcOffset{0};
    void *dstHbm{nullptr};
    uint64_t length{0};
};

static_assert(std::is_standard_layout<HybmBatchCopyProbeParam>::value);
static_assert(std::is_trivially_copyable<HybmBatchCopyProbeParam>::value);
static_assert(sizeof(HybmBatchCopyProbeParam) == 32U);

extern "C" uint32_t HybmBatchCopyProbe(void *args);

#endif // MF_HYBM_BATCH_COPY_PROBE_H
