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

#include "host_rdma_rx_agg.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace ock {
namespace mf {
namespace transport {
namespace host {

int WaitWriteDone(uint64_t flagHostVa, uint64_t expectSeq, int64_t timeoutUs)
{
    if (flagHostVa == 0) {
        return -1;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs);
    auto *flag = reinterpret_cast<const volatile uint64_t *>(flagHostVa);
    for (;;) {
        if (*flag == expectSeq) {
            return 0;
        }
        if (timeoutUs > 0 && std::chrono::steady_clock::now() >= deadline) {
            return -2;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

int ScatterContiguous(uint64_t stagingHostVa, uint64_t blockSize, const uint64_t *targetHostVas,
                      uint32_t blockCount)
{
    if (stagingHostVa == 0 || targetHostVas == nullptr || blockSize == 0 || blockCount == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < blockCount; ++i) {
        if (targetHostVas[i] == 0) {
            return -1;
        }
        const auto *src = reinterpret_cast<const void *>(stagingHostVa + static_cast<uint64_t>(i) * blockSize);
        auto *dst = reinterpret_cast<void *>(targetHostVas[i]);
        std::memcpy(dst, src, blockSize);
    }
    return 0;
}

} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock
