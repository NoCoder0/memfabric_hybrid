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

#include <chrono>
#include <cstdint>

#include "dl_hcomm_api.h"

struct HybmBatchCopyTiming {
    uint64_t wqeBuildStartNs{0U};
    uint64_t wqeBuildNs{0U};
    uint64_t batchModeStartNs{0U};
    uint64_t batchTransferTotalNs{0U};
    uint64_t batchTransferMaxNs{0U};
    uint64_t fenceNs{0U};
    uint64_t completionReadNs{0U};
    uint64_t launchTaskNs{0U};
    uint64_t completionWaitStartNs{0U};
    uint64_t completionWaitNs{0U};
    uint32_t batchTransferCalls{0U};
    uint32_t batchTransferMaxOffset{0U};
    uint32_t batchTransferMaxDescCount{0U};
    uint32_t completionPolls{0U};
};

inline uint64_t HybmBatchCopyNowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct HybmBatchCopyTransferParam {
    ock::mf::ThreadHandle thread;
    ock::mf::ChannelHandle channel;
    const ock::mf::HcommBatchTransferDesc *descriptors;
    uint32_t descriptorCount;
    uint64_t remoteFlagAddr;
    uint64_t localFlagAddr;
    uint32_t flagSize;
    HybmBatchCopyTiming *timing{nullptr};
};

uint32_t HybmBatchCopyStartBatchMode();
uint32_t HybmBatchCopySubmitDescriptors(const HybmBatchCopyTransferParam &param);
uint32_t HybmBatchCopyFenceAndReadCompletion(const HybmBatchCopyTransferParam &param);
uint32_t HybmBatchCopyEndBatchMode();

#endif // MEM_FABRIC_HYBRID_ACC_OFFLOAD_HYBM_BATCH_COPY_TRANSFER_H
