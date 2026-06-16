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
#include <gtest/gtest.h>

#include <unordered_map>
#include <deque>

#include "zbal_test_constants.h"

#define private public
#include "zbal_sma_device_pool.h"
#undef private

using namespace zbal;
using namespace zbal::sma;
using namespace zbal::sma::device;

/* helper: construct a ZEvent wrapping nullptr with a no-op deleter, safe for
   cleanEvents / cleanStream which only std::move and destroy the event */
static ZEvent MakeNullZEvent()
{
    return ZEvent(nullptr, [](c10_npu::NPUEvent *) {});
}

class TestDeviceBlock : public testing::Test {
public:
    static void SetUpTestCase()
    {
        pool_ = new DeviceBlockPool();
        blockSM_ = new DeviceBlock(0, nullptr, ZBAL_UT_NUM_512, pool_, reinterpret_cast<void *>(0x1000), BT_SMALL);
        blockLG_ = new DeviceBlock(0, nullptr, kLargeBuffer, pool_, reinterpret_cast<void *>(0x2000), BT_BIG);
    }

    static void TearDownTestCase()
    {
        delete blockSM_;
        delete blockLG_;
        delete pool_;
    }

    void SetUp() override
    {
        pool_->small_blocks_.clear();
        pool_->large_blocks_.clear();
    }

    static DeviceBlockPool *pool_;
    static DeviceBlock *blockSM_;
    static DeviceBlock *blockLG_;
};

DeviceBlockPool *TestDeviceBlock::pool_ = nullptr;
DeviceBlock *TestDeviceBlock::blockSM_ = nullptr;
DeviceBlock *TestDeviceBlock::blockLG_ = nullptr;

TEST_F(TestDeviceBlock, DefaultConstructor)
{
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    EXPECT_EQ(block.deviceId_, 0);
    EXPECT_EQ(block.stream_, nullptr);
    EXPECT_EQ(block.size_, 1024u);
    EXPECT_EQ(block.ptr_, nullptr);
    EXPECT_EQ(block.block_type_, BT_SMALL);
    EXPECT_EQ(block.allocated_, false);
    EXPECT_EQ(block.event_count_, 0);
    EXPECT_EQ(block.gc_count_, 0);
    EXPECT_EQ(block.prev_, nullptr);
    EXPECT_EQ(block.next_, nullptr);
    EXPECT_FALSE(block.isSplit());
}

TEST_F(TestDeviceBlock, SearchKeyConstructor)
{
    DeviceBlock searchKey(ZBAL_UT_NUM_3, reinterpret_cast<aclrtStream>(0x1000), ZBAL_UT_SIZE_2KB);
    EXPECT_EQ(searchKey.deviceId_, ZBAL_UT_NUM_3);
    EXPECT_EQ(searchKey.size_, 2048u);
}

TEST_F(TestDeviceBlock, LargeBlockType)
{
    DeviceBlock block(0, nullptr, kLargeBuffer, nullptr, nullptr, BT_BIG);
    EXPECT_EQ(block.block_type_, BT_BIG);
}

TEST_F(TestDeviceBlock, IsSplitFalseWhenNoNeighbors)
{
    DeviceBlock a(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    EXPECT_FALSE(a.isSplit());
}

TEST_F(TestDeviceBlock, IsSplitTrueWithPrev)
{
    DeviceBlock a(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    DeviceBlock b(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    a.prev_ = &b;
    EXPECT_TRUE(a.isSplit());
}

TEST_F(TestDeviceBlock, IsSplitTrueWithNext)
{
    DeviceBlock a(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    DeviceBlock b(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    a.next_ = &b;
    EXPECT_TRUE(a.isSplit());
}

TEST_F(TestDeviceBlock, IsSplitTrueWithBothNeighbors)
{
    DeviceBlock before(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    DeviceBlock mid(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    DeviceBlock after(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    before.next_ = &mid;
    mid.prev_ = &before;
    mid.next_ = &after;
    after.prev_ = &mid;
    EXPECT_TRUE(mid.isSplit());
}

TEST_F(TestDeviceBlock, SpliceInsertsBetween)
{
    DeviceBlock before(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    DeviceBlock mid(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    DeviceBlock after(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    before.next_ = &after;
    after.prev_ = &before;

    mid.splice(&before, &after);

    EXPECT_EQ(before.next_, &mid);
    EXPECT_EQ(mid.prev_, &before);
    EXPECT_EQ(mid.next_, &after);
    EXPECT_EQ(after.prev_, &mid);
}

TEST_F(TestDeviceBlock, SpliceAtHead)
{
    DeviceBlock mid(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    DeviceBlock after(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);

    mid.splice(nullptr, &after);

    EXPECT_EQ(mid.prev_, nullptr);
    EXPECT_EQ(mid.next_, &after);
    EXPECT_EQ(after.prev_, &mid);
}

TEST_F(TestDeviceBlock, SpliceAtTail)
{
    DeviceBlock before(0, nullptr, ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    DeviceBlock mid(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    before.next_ = nullptr;

    mid.splice(&before, nullptr);

    EXPECT_EQ(before.next_, &mid);
    EXPECT_EQ(mid.prev_, &before);
    EXPECT_EQ(mid.next_, nullptr);
}

TEST_F(TestDeviceBlock, SpliceBothNull)
{
    DeviceBlock mid(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    mid.splice(nullptr, nullptr);
    EXPECT_EQ(mid.prev_, nullptr);
    EXPECT_EQ(mid.next_, nullptr);
}

TEST_F(TestDeviceBlock, CompareByStream)
{
    DeviceBlock a(0, reinterpret_cast<aclrtStream>(0x100), ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    DeviceBlock b(0, reinterpret_cast<aclrtStream>(0x200), ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    EXPECT_TRUE(DeviceBlockCompareBySize(&a, &b));
    EXPECT_FALSE(DeviceBlockCompareBySize(&b, &a));
}

TEST_F(TestDeviceBlock, CompareBySizeWhenSameStream)
{
    DeviceBlock a(0, reinterpret_cast<aclrtStream>(0x100), ZBAL_UT_NUM_256, nullptr, nullptr, BT_SMALL);
    DeviceBlock b(0, reinterpret_cast<aclrtStream>(0x100), ZBAL_UT_NUM_512, nullptr, nullptr, BT_SMALL);
    EXPECT_TRUE(DeviceBlockCompareBySize(&a, &b));
    EXPECT_FALSE(DeviceBlockCompareBySize(&b, &a));
}

TEST_F(TestDeviceBlock, CompareByPtrWhenSameStreamAndSize)
{
    DeviceBlock a(0, nullptr, ZBAL_UT_NUM_512, nullptr, reinterpret_cast<void *>(0x1000), BT_SMALL);
    DeviceBlock b(0, nullptr, ZBAL_UT_NUM_512, nullptr, reinterpret_cast<void *>(0x2000), BT_SMALL);
    EXPECT_TRUE(DeviceBlockCompareBySize(&a, &b));
    EXPECT_FALSE(DeviceBlockCompareBySize(&b, &a));
}

TEST_F(TestDeviceBlock, PoolDefaultIsNotPrivate)
{
    EXPECT_FALSE(pool_->is_private_);
    EXPECT_EQ(pool_->use_count_, 1);
    EXPECT_EQ(pool_->npuMalloc_count_, 0);
}

TEST_F(TestDeviceBlock, PoolPrivatePool)
{
    DeviceBlockPool privatePool(true);
    EXPECT_TRUE(privatePool.is_private_);
}

TEST_F(TestDeviceBlock, PoolInsertSmallBlock)
{
    pool_->insertBlock(BT_SMALL, blockSM_);
    EXPECT_EQ(pool_->small_blocks_.size(), 1u);
    EXPECT_EQ(pool_->large_blocks_.size(), 0u);
}

TEST_F(TestDeviceBlock, PoolInsertLargeBlock)
{
    pool_->insertBlock(BT_BIG, blockLG_);
    EXPECT_EQ(pool_->small_blocks_.size(), 0u);
    EXPECT_EQ(pool_->large_blocks_.size(), 1u);
}

TEST_F(TestDeviceBlock, PoolInsertBothTypes)
{
    pool_->insertBlock(BT_SMALL, blockSM_);
    pool_->insertBlock(BT_BIG, blockLG_);
    EXPECT_EQ(pool_->small_blocks_.size(), 1u);
    EXPECT_EQ(pool_->large_blocks_.size(), 1u);
}

TEST_F(TestDeviceBlock, PoolEraseSmallBlock)
{
    pool_->insertBlock(BT_SMALL, blockSM_);
    pool_->eraseBlock(BT_SMALL, blockSM_);
    EXPECT_EQ(pool_->small_blocks_.size(), 0u);
}

TEST_F(TestDeviceBlock, PoolEraseLargeBlock)
{
    pool_->insertBlock(BT_BIG, blockLG_);
    pool_->eraseBlock(BT_BIG, blockLG_);
    EXPECT_EQ(pool_->large_blocks_.size(), 0u);
}

TEST_F(TestDeviceBlock, PoolInsertMultipleBlocksSorted)
{
    DeviceBlock b1(0, nullptr, ZBAL_UT_NUM_256, pool_, reinterpret_cast<void *>(0x1000), BT_SMALL);
    DeviceBlock b2(0, nullptr, ZBAL_UT_NUM_128, pool_, reinterpret_cast<void *>(0x2000), BT_SMALL);
    DeviceBlock b3(0, nullptr, ZBAL_UT_NUM_512, pool_, reinterpret_cast<void *>(0x3000), BT_SMALL);

    pool_->insertBlock(BT_SMALL, &b1);
    pool_->insertBlock(BT_SMALL, &b2);
    pool_->insertBlock(BT_SMALL, &b3);
    EXPECT_EQ(pool_->small_blocks_.size(), 3u);

    auto it = pool_->small_blocks_.begin();
    EXPECT_EQ((*it)->size_, 128u);
    ++it;
    EXPECT_EQ((*it)->size_, 256u);
    ++it;
    EXPECT_EQ((*it)->size_, 512u);

    pool_->eraseBlock(BT_SMALL, &b1);
    pool_->eraseBlock(BT_SMALL, &b2);
    pool_->eraseBlock(BT_SMALL, &b3);
}

TEST_F(TestDeviceBlock, AllocParamsConstruction)
{
    DeviceBlockPool pool;
    DeviceAllocParams params(ZBAL_UT_NUM_2, ZBAL_UT_SIZE_1KB, reinterpret_cast<aclrtStream>(0x500), &pool,
                             ZBAL_UT_SIZE_2KB, BT_SMALL);

    EXPECT_EQ(params.device(), ZBAL_UT_NUM_2);
    EXPECT_EQ(params.stream(), reinterpret_cast<aclrtStream>(0x500));
    EXPECT_EQ(params.size(), 1024u);
    EXPECT_EQ(params.alloc_size_, 2048u);
    EXPECT_EQ(params.block_type_, BT_SMALL);
    EXPECT_EQ(params.result_, Z_OK);
    EXPECT_EQ(params.block_, nullptr);
}

TEST_F(TestDeviceBlock, MempoolIdHashSameFirst)
{
    MempoolIdHash hasher;
    c10_npu::MempoolId_t a = {ZBAL_UT_NUM_42, 0};
    c10_npu::MempoolId_t b = {ZBAL_UT_NUM_42, ZBAL_UT_NUM_99};
    EXPECT_EQ(hasher(a), hasher(b));
}

TEST_F(TestDeviceBlock, MempoolIdHashDifferentFirst)
{
    MempoolIdHash hasher;
    c10_npu::MempoolId_t a = {ZBAL_UT_NUM_10, 0};
    c10_npu::MempoolId_t b = {ZBAL_UT_NUM_20, 0};
    EXPECT_NE(hasher(a), hasher(b));
}

TEST_F(TestDeviceBlock, MempoolIdHashZeroFirstUsesSecond)
{
    MempoolIdHash hasher;
    c10_npu::MempoolId_t a = {0, ZBAL_UT_NUM_100};
    c10_npu::MempoolId_t b = {0, ZBAL_UT_NUM_200};
    EXPECT_NE(hasher(a), hasher(b));
}

TEST_F(TestDeviceBlock, MempoolIdHashZeroFirstVsNonZeroFirst)
{
    MempoolIdHash hasher;
    c10_npu::MempoolId_t a = {0, 0};
    c10_npu::MempoolId_t b = {ZBAL_UT_NUM_5, 0};
    EXPECT_NE(hasher(a), hasher(b));
}

TEST_F(TestDeviceBlock, AppendEventsDeferredUntilNoCapture)
{
    GraphDeferPools defers;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    EXPECT_TRUE(defers.needs_events_deferred_until_no_capture_.empty());
    defers.appendEventsDeferredUntilNoCapture(&block);
    EXPECT_EQ(defers.needs_events_deferred_until_no_capture_.size(), 1u);
    EXPECT_EQ(defers.needs_events_deferred_until_no_capture_[0], &block);
}

TEST_F(TestDeviceBlock, InsertEventsDeferredUntilNoCaptureEmptyQueue)
{
    GraphDeferPools defers;
    auto allocator = reinterpret_cast<DeviceSMACachingAllocator *>(0x1);
    std::shared_ptr<c10::GatheredContext> ctx;
    EXPECT_NO_THROW(defers.insertEventsDeferredUntilNoCapture(allocator, ctx));
}

TEST_F(TestDeviceBlock, RemoveNpuGraphStreamUsesBlockNotTracked)
{
    GraphDeferPools defers;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    c10_npu::NPUStream s = c10_npu::getDefaultNPUStream();
    block.stream_uses_.insert(s);

    defers.removeNpuGraphStreamUses(&block);

    EXPECT_EQ(block.stream_uses_.size(), 1u);
}

TEST_F(TestDeviceBlock, RemoveNpuGraphStreamUsesFiltersNpuGraphStreams)
{
    GraphDeferPools defers;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    c10_npu::NPUStream s1 = c10_npu::getDefaultNPUStream();
    c10_npu::NPUStream s2 = c10_npu::getNPUStreamFromPool();

    block.stream_uses_.insert(s1);
    block.stream_uses_.insert(s2);
    defers.block_to_npugraph_stream_uses_[&block].insert(s2);

    defers.removeNpuGraphStreamUses(&block);

    EXPECT_EQ(block.stream_uses_.size(), 1u);
    EXPECT_TRUE(block.stream_uses_.find(s1) != block.stream_uses_.end());
    EXPECT_EQ(defers.block_to_npugraph_stream_uses_.count(&block), 0u);
}

TEST_F(TestDeviceBlock, RemoveNpuGraphStreamUsesAllFiltered)
{
    GraphDeferPools defers;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    c10_npu::NPUStream s1 = c10_npu::getDefaultNPUStream();
    c10_npu::NPUStream s2 = c10_npu::getNPUStreamFromPool();

    block.stream_uses_.insert(s1);
    block.stream_uses_.insert(s2);
    defers.block_to_npugraph_stream_uses_[&block].insert(s1);
    defers.block_to_npugraph_stream_uses_[&block].insert(s2);

    defers.removeNpuGraphStreamUses(&block);

    EXPECT_TRUE(block.stream_uses_.empty());
    EXPECT_EQ(defers.block_to_npugraph_stream_uses_.count(&block), 0u);
}

TEST_F(TestDeviceBlock, CleanEventsEmptyNoOp)
{
    auto allocator = reinterpret_cast<DeviceSMACachingAllocator *>(0x1);
    EventController ec;
    ec.cleanEvents(allocator);
    EXPECT_TRUE(ec.npu_events_.empty());
}

TEST_F(TestDeviceBlock, CleanEventsDecrementsCountsClearsMap)
{
    auto allocator = reinterpret_cast<DeviceSMACachingAllocator *>(0x1);
    EventController ec;
    DeviceBlock b1(0, nullptr, ZBAL_UT_NUM_512, pool_, nullptr, BT_SMALL);
    DeviceBlock b2(0, nullptr, ZBAL_UT_SIZE_1KB, pool_, nullptr, BT_SMALL);
    b1.event_count_ = ZBAL_UT_NUM_2;
    b2.event_count_ = ZBAL_UT_NUM_2;

    c10_npu::NPUStream s = c10_npu::getDefaultNPUStream();
    ec.npu_events_[s].emplace_back(MakeNullZEvent(), &b1);
    ec.npu_events_[s].emplace_back(MakeNullZEvent(), &b2);

    ec.cleanEvents(allocator);

    EXPECT_TRUE(ec.npu_events_.empty());
    EXPECT_EQ(b1.event_count_, 1);
    EXPECT_EQ(b2.event_count_, 1);
}

TEST_F(TestDeviceBlock, CleanStreamNotInMapLeavesCountUnchanged)
{
    auto allocator = reinterpret_cast<DeviceSMACachingAllocator *>(0x1);
    EventController ec;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    block.event_count_ = ZBAL_UT_NUM_2;
    c10_npu::NPUStream s = c10_npu::getDefaultNPUStream();

    ec.cleanStream(allocator, &block, s);

    EXPECT_EQ(block.event_count_, ZBAL_UT_NUM_2);
}

TEST_F(TestDeviceBlock, CleanStreamRemovesMatchingBlockOnly)
{
    auto allocator = reinterpret_cast<DeviceSMACachingAllocator *>(0x1);
    EventController ec;
    DeviceBlock b1(0, nullptr, ZBAL_UT_NUM_512, pool_, nullptr, BT_SMALL);
    DeviceBlock b2(0, nullptr, ZBAL_UT_SIZE_1KB, pool_, nullptr, BT_SMALL);
    b1.event_count_ = ZBAL_UT_NUM_2;
    b2.event_count_ = ZBAL_UT_NUM_2;

    c10_npu::NPUStream s = c10_npu::getDefaultNPUStream();
    ec.npu_events_[s].emplace_back(MakeNullZEvent(), &b1);
    ec.npu_events_[s].emplace_back(MakeNullZEvent(), &b2);

    ec.cleanStream(allocator, &b1, s);

    EXPECT_EQ(b1.event_count_, 1);
    EXPECT_EQ(b2.event_count_, ZBAL_UT_NUM_2);
    EXPECT_EQ(ec.npu_events_[s].size(), 1u);
    EXPECT_EQ(ec.npu_events_[s].front().second, &b2);
}

TEST_F(TestDeviceBlock, InsertBlockToNpuGraphStreamUses)
{
    GraphDeferPools defers;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, nullptr, nullptr, BT_SMALL);
    c10_npu::NPUStream s = c10_npu::getDefaultNPUStream();

    defers.insertBlockToNpuGraphStreamUses(&block, s);

    EXPECT_EQ(defers.block_to_npugraph_stream_uses_.count(&block), 1u);
    EXPECT_EQ(defers.block_to_npugraph_stream_uses_[&block].count(s), 1u);
}
