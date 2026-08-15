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

#ifndef MEM_FABRIC_HYBRID_ACC_OFFLOAD_HYBM_BATCH_COPY_TRANSFER_H
#define MEM_FABRIC_HYBRID_ACC_OFFLOAD_HYBM_BATCH_COPY_TRANSFER_H

#include <cstdint>

#include "dl_hcomm_api.h"

struct HybmBatchCopyTransferParam {
    ock::mf::ThreadHandle thread;
    ock::mf::ChannelHandle channel;
    const ock::mf::HcommBatchTransferDesc *descriptors;
    uint32_t descriptorCount;
    uint64_t remoteFlagAddr;
    uint64_t localFlagAddr;
    uint32_t flagSize;
};

uint32_t HybmBatchCopyReadDescriptors(const HybmBatchCopyTransferParam &param);

#endif // MEM_FABRIC_HYBRID_ACC_OFFLOAD_HYBM_BATCH_COPY_TRANSFER_H
