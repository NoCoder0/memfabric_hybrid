/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef MF_HYBM_URMA_DEVICE_COPY_H
#define MF_HYBM_URMA_DEVICE_COPY_H

#include <cstdint>
#include <type_traits>
#include "vendor/ascend/dl_hcomm_api.h"
#include "hybm_device_copy_common.h"

struct HybmUrmaDeviceCopyContext {
    uint32_t capacity;
    uint32_t rankId;
    ock::mf::ThreadHandle thread;
    ock::mf::ChannelHandle channel;
    uint64_t poolBase;
    uint64_t poolSize;
    uint64_t importedBase;
    ock::mf::HcommBatchTransferDesc *descriptors;
    ock::mf::HcommBatchTransferDesc marker;
    uint32_t maxRecords;
    uint32_t *recordCount;
};

struct HybmUrmaDeviceCopyArgs {
    const HybmUrmaDeviceCopyContext *context;
    const uint64_t *srcPtrs;
    const uint64_t *dstPtrs;
    const uint32_t *lengths;
    const uint32_t *count;
    uint32_t startIndex;
};

static_assert(std::is_trivially_copyable<HybmUrmaDeviceCopyContext>::value,
              "device context must be a flat copyable ABI");
static_assert(std::is_trivially_copyable<HybmUrmaDeviceCopyArgs>::value,
              "kernel arguments must be a flat copyable ABI");

extern "C" int32_t HybmUrmaDeviceCopy(HybmUrmaDeviceCopyArgs *args);

#endif // MF_HYBM_URMA_DEVICE_COPY_H
