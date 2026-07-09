/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <vector>
#include <memory>
#include "zbal_sma_device.h"

namespace zbal {
namespace sma {
namespace device {

static std::vector<std::unique_ptr<DeviceBlock>> g_stub_blocks;
static char g_stub_buffer[4096 * 16];

DeviceBlock *DeviceSMACachingAllocator::malloc(int device, size_t orig_size, aclrtStream stream, uint8_t allocator_type)
{
    (void)stream;
    (void)allocator_type;
    auto block = std::make_unique<DeviceBlock>(device, nullptr, orig_size, &default_pool_,
                                               g_stub_buffer + (g_stub_blocks.size() * 4096), BT_SMALL);
    block->allocated_ = true;
    block->requested_size_ = orig_size;
    block->is_safe_ = true;
    auto *raw = block.get();
    g_stub_blocks.push_back(std::move(block));
    return raw;
}

void DeviceSMACachingAllocator::free(DeviceBlock *block, uint8_t allocator_type)
{
    (void)block;
    (void)allocator_type;
}

void DeviceSMACachingAllocator::free_block(DeviceBlock *block, const std::shared_ptr<c10::GatheredContext> &context,
                                           uint8_t allocator_type)
{
    (void)block;
    (void)context;
    (void)allocator_type;
}

void DeviceSMACachingAllocator::insert_events(DeviceBlock *block)
{
    (void)block;
}

DeviceStats DeviceSMACachingAllocator::getStats()
{
    return DeviceStats{};
}

void DeviceSMACachingAllocator::resetAccumulatedStats() {}

void DeviceSMACachingAllocator::resetPeakStats() {}

void DeviceSMACachingAllocator::emptyCache(int device, bool check_error)
{
    (void)device;
    (void)check_error;
}

void DeviceSMACachingAllocator::recordStream(DeviceBlock *block, c10_npu::NPUStream stream)
{
    (void)block;
    (void)stream;
}

void DeviceSMACachingAllocator::eraseStream(DeviceBlock *block, c10_npu::NPUStream stream)
{
    (void)block;
    (void)stream;
}

void DeviceSMACachingAllocator::markAllBlockUnsafe() {}

void DeviceSMACachingAllocator::releaseAndFreeEvents() {}

void DeviceSMACachingAllocator::beginAllocateToPool(c10_npu::MempoolId_t mempool_id,
                                                    std::function<bool(aclrtStream)> filter)
{
    (void)mempool_id;
    (void)filter;
}

void DeviceSMACachingAllocator::endAllocateToPool(c10_npu::MempoolId_t mempool_id)
{
    (void)mempool_id;
}

void DeviceSMACachingAllocator::releasePool(c10_npu::MempoolId_t mempool_id)
{
    (void)mempool_id;
}

void DeviceSMACachingAllocator::snapshot(int device)
{
    (void)device;
}

void DeviceSMACachingAllocator::attachSnapShotObserver(TraceObserver trace_ob_func, SegmentObserver segment_ob_func)
{
    (void)trace_ob_func;
    (void)segment_ob_func;
}

void *DeviceSMACachingAllocator::getBaseAllocation(DeviceBlock *block, size_t *outSize)
{
    (void)block;
    if (outSize)
        *outSize = 0;
    return nullptr;
}

void DeviceSMACachingAllocator::setMemoryFraction(double fraction)
{
    (void)fraction;
}

void DeviceSMACachingAllocator::cacheInfo(size_t *total, size_t *largest)
{
    if (total)
        *total = 0;
    if (largest)
        *largest = 0;
}

} // namespace device
} // namespace sma
} // namespace zbal
