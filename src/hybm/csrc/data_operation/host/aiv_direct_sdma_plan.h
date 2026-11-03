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
#ifndef MF_AIV_DIRECT_SDMA_PLAN_H
#define MF_AIV_DIRECT_SDMA_PLAN_H
#include <algorithm>
#include <cstdint>
#include "../../copy_aiv_direct_sdma/aiv_direct_sdma_abi.h"
namespace ock {
namespace mf {
namespace aiv_direct_sdma {
constexpr uint32_t MAX_DESCRIPTORS = 8192;
constexpr uint32_t MAX_AIV_COUNT = 48;
constexpr uint64_t MAX_SQE_BYTES = 8ULL * 1024 * 1024;
// Leave one empty entry to distinguish full/empty, plus one NotifyRecord SQE.
inline uint32_t WaveCapacity(uint32_t depth, uint32_t aivCount)
{
    if (depth < 4 || depth > UINT16_MAX || aivCount == 0 || aivCount > MAX_AIV_COUNT) {
        return 0;
    }
    return std::min(MAX_DESCRIPTORS, (depth - 2) * aivCount);
}
inline bool ValidRange(uint64_t address, uint64_t bytes)
{
    return address != 0 && bytes != 0 && address <= UINT64_MAX - bytes;
}
struct Cursor {
    uint32_t input = 0;
    uint64_t offset = 0;
};
// Input ranges are validated first. A large packet may span several waves.
inline uint32_t FillWave(void *const *sources, void *const *destinations, const uint64_t *lengths, uint32_t batchSize,
                         uint32_t capacity, Cursor &cursor, AivDirectSdmaDescriptor *output)
{
    uint32_t count = 0;
    while (cursor.input < batchSize && count < capacity) {
        const auto index = cursor.input;
        const auto bytes = std::min(MAX_SQE_BYTES, lengths[index] - cursor.offset);
        output[count++] = {reinterpret_cast<uint64_t>(sources[index]) + cursor.offset,
                           reinterpret_cast<uint64_t>(destinations[index]) + cursor.offset, bytes};
        cursor.offset += bytes;
        if (cursor.offset == lengths[index]) {
            ++cursor.input;
            cursor.offset = 0;
        }
    }
    return count;
}
} // namespace aiv_direct_sdma
} // namespace mf
} // namespace ock
#endif
