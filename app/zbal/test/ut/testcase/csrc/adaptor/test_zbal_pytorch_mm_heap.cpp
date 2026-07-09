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
#include <memory>
#include <cstring>

#include "zbal_test_constants.h"
#include "zbal_pytorch_mm_heap.h"

using namespace zbal;
using namespace zbal::adaptor;
using namespace zbal::adaptor::heap;

constexpr uint64_t kHeapSize = ZBAL_UT_HEAP_SIZE;
constexpr uint64_t kSmallAlloc = ZBAL_UT_SMALL_ALLOC;
constexpr uint64_t kMediumAlloc = ZBAL_UT_MEDIUM_ALLOC;

class TestMemoryHeap : public testing::Test {
public:
    void SetUp() override
    {
        buffer_ = new uint8_t[kHeapSize];
        std::memset(buffer_, 0, kHeapSize);
        heap_ = std::make_shared<MemoryHeap>(buffer_, kHeapSize);
    }

    void TearDown() override
    {
        heap_.reset();
        delete[] buffer_;
        buffer_ = nullptr;
    }

protected:
    uint8_t *buffer_{nullptr};
    std::shared_ptr<MemoryHeap> heap_{nullptr};
};

/* ==================== Constructor ==================== */

TEST_F(TestMemoryHeap, Constructor)
{
    EXPECT_EQ(heap_->getTotalSize(), kHeapSize);
    EXPECT_EQ(heap_->getInUsedSize(), 0u);

    uint8_t buf[ZBAL_UT_NUM_16];
    auto heapZero = std::make_shared<MemoryHeap>(buf, 0);
    EXPECT_EQ(heapZero->getTotalSize(), 0u);
}

/* ==================== allocate() ==================== */

TEST_F(TestMemoryHeap, Allocate)
{
    void *ptr = heap_->allocate(kSmallAlloc);
    EXPECT_NE(ptr, nullptr);
    EXPECT_GE(reinterpret_cast<uint8_t *>(ptr), buffer_);
    EXPECT_LT(reinterpret_cast<uint8_t *>(ptr), buffer_ + kHeapSize);
    EXPECT_GT(heap_->getInUsedSize(), 0u);
}

TEST_F(TestMemoryHeap, AllocateInvalidInputs)
{
    EXPECT_EQ(heap_->allocate(0), nullptr);
    EXPECT_EQ(heap_->allocate(kHeapSize + 1), nullptr);
}

TEST_F(TestMemoryHeap, AllocateExhaustMemory)
{
    void *ptr1 = heap_->allocate(kHeapSize);
    ASSERT_NE(ptr1, nullptr);
    EXPECT_EQ(heap_->allocate(1), nullptr);
}

TEST_F(TestMemoryHeap, AllocateMultipleBlocks)
{
    void *ptr1 = heap_->allocate(kSmallAlloc);
    void *ptr2 = heap_->allocate(kSmallAlloc);
    void *ptr3 = heap_->allocate(kSmallAlloc);
    EXPECT_NE(ptr1, nullptr);
    EXPECT_NE(ptr2, nullptr);
    EXPECT_NE(ptr3, nullptr);
    EXPECT_NE(ptr1, ptr2);
    EXPECT_NE(ptr2, ptr3);

    auto off1 = reinterpret_cast<uint8_t *>(ptr1) - buffer_;
    auto off2 = reinterpret_cast<uint8_t *>(ptr2) - buffer_;
    EXPECT_GE(off2, off1 + kSmallAlloc);
}

TEST_F(TestMemoryHeap, AllocateExactSize)
{
    void *ptr = heap_->allocate(kHeapSize);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr, buffer_);
    EXPECT_GE(heap_->getInUsedSize(), kHeapSize);
}

/* ==================== alignedAllocate() ==================== */

TEST_F(TestMemoryHeap, AlignedAllocate)
{
    void *ptr = heap_->alignedAllocate(ZBAL_UT_NUM_256, kSmallAlloc);
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ((reinterpret_cast<uint8_t *>(ptr) - buffer_) % ZBAL_UT_NUM_256, 0u);

    void *ptr2 = heap_->alignedAllocate(ZBAL_UT_NUM_64, kSmallAlloc);
    EXPECT_NE(ptr2, nullptr);

    void *ptr3 = heap_->alignedAllocate(ZBAL_UT_SIZE_64KB, kSmallAlloc);
    EXPECT_NE(ptr3, nullptr);
    EXPECT_EQ((reinterpret_cast<uint8_t *>(ptr3) - buffer_) % ZBAL_UT_SIZE_64KB, 0u);
}

TEST_F(TestMemoryHeap, AlignedAllocateInvalidInputs)
{
    EXPECT_EQ(heap_->alignedAllocate(ZBAL_UT_NUM_256, 0), nullptr);
    EXPECT_EQ(heap_->alignedAllocate(0, kSmallAlloc), nullptr);
    EXPECT_EQ(heap_->alignedAllocate(ZBAL_UT_NUM_17, kSmallAlloc), nullptr);
}

TEST_F(TestMemoryHeap, AlignedAllocateWithHeadSkip)
{
    void *ptr1 = heap_->allocate(ZBAL_UT_NUM_16);
    ASSERT_NE(ptr1, nullptr);
    void *ptr2 = heap_->alignedAllocate(ZBAL_UT_SIZE_4KB, kMediumAlloc);
    EXPECT_NE(ptr2, nullptr);
    EXPECT_EQ((reinterpret_cast<uint8_t *>(ptr2) - buffer_) % ZBAL_UT_SIZE_4KB, 0u);

    void *ptr3 = heap_->allocate(ZBAL_UT_NUM_7);
    ASSERT_NE(ptr3, nullptr);
    void *ptr4 = heap_->alignedAllocate(ZBAL_UT_NUM_256, kSmallAlloc);
    EXPECT_NE(ptr4, nullptr);
    EXPECT_EQ((reinterpret_cast<uint8_t *>(ptr4) - buffer_) % ZBAL_UT_NUM_256, 0u);
}

TEST_F(TestMemoryHeap, AlignedAllocateExhausted)
{
    void *ptr1 = heap_->alignedAllocate(1, kHeapSize);
    ASSERT_NE(ptr1, nullptr);
    EXPECT_EQ(heap_->alignedAllocate(1, 1), nullptr);
}

/* ==================== release() ==================== */

TEST_F(TestMemoryHeap, Release)
{
    void *ptr = heap_->allocate(kSmallAlloc);
    ASSERT_NE(ptr, nullptr);
    size_t usedBefore = heap_->getInUsedSize();
    EXPECT_EQ(heap_->release(ptr), Z_OK);
    EXPECT_LT(heap_->getInUsedSize(), usedBefore);
    EXPECT_EQ(heap_->release(nullptr), Z_ERROR_ALLOC);

    uint8_t fakeBuf[ZBAL_UT_NUM_64];
    EXPECT_EQ(heap_->release(fakeBuf), Z_ERROR_ALLOC);

    uint8_t *mid = buffer_ + kHeapSize / ZBAL_UT_NUM_2;
    EXPECT_EQ(heap_->release(mid), Z_ERROR_ALLOC);
}

TEST_F(TestMemoryHeap, ReleaseCoalesceAdjacentBlocks)
{
    void *ptr1 = heap_->allocate(kSmallAlloc);
    void *ptr2 = heap_->allocate(kSmallAlloc);
    void *ptr3 = heap_->allocate(kSmallAlloc);
    ASSERT_NE(ptr1, nullptr);
    ASSERT_NE(ptr2, nullptr);
    ASSERT_NE(ptr3, nullptr);

    heap_->release(ptr2);
    heap_->release(ptr1);
    heap_->release(ptr3);

    void *ptr4 = heap_->allocate(kSmallAlloc * ZBAL_UT_NUM_3);
    EXPECT_NE(ptr4, nullptr);
    EXPECT_EQ(ptr4, ptr1);

    heap_->release(ptr4);
    void *ptr5 = heap_->allocate(kSmallAlloc);
    EXPECT_EQ(ptr5, ptr1);
}

/* ==================== changeSize() ==================== */

TEST_F(TestMemoryHeap, ChangeSize)
{
    void *ptr = heap_->allocate(kSmallAlloc);
    ASSERT_NE(ptr, nullptr);
    size_t usedBefore = heap_->getInUsedSize();
    EXPECT_TRUE(heap_->changeSize(ptr, kSmallAlloc));
    EXPECT_EQ(heap_->getInUsedSize(), usedBefore);

    EXPECT_TRUE(heap_->changeSize(ptr, 0));
    EXPECT_EQ(heap_->getInUsedSize(), 0u);

    EXPECT_FALSE(heap_->changeSize(nullptr, kSmallAlloc));
}

TEST_F(TestMemoryHeap, ChangeSizeShrinkAndGrow)
{
    void *ptr = heap_->allocate(kMediumAlloc);
    ASSERT_NE(ptr, nullptr);
    EXPECT_TRUE(heap_->changeSize(ptr, kMediumAlloc / ZBAL_UT_NUM_2));
    uint64_t size = 0;
    heap_->allocatedSize(ptr, size);
    EXPECT_EQ(size, kMediumAlloc / ZBAL_UT_NUM_2);

    void *ptr1 = heap_->allocate(kSmallAlloc);
    void *ptr2 = heap_->allocate(kSmallAlloc);
    ASSERT_NE(ptr1, nullptr);
    ASSERT_NE(ptr2, nullptr);

    heap_->release(ptr2);
    EXPECT_TRUE(heap_->changeSize(ptr1, kSmallAlloc * ZBAL_UT_NUM_2));
    heap_->allocatedSize(ptr1, size);
    EXPECT_EQ(size, kSmallAlloc * ZBAL_UT_NUM_2);
}

TEST_F(TestMemoryHeap, ChangeSizeGrowFailsWhenAdjacentOccupied)
{
    void *ptr1 = heap_->allocate(kSmallAlloc);
    void *ptr2 = heap_->allocate(kSmallAlloc);
    ASSERT_NE(ptr1, nullptr);
    ASSERT_NE(ptr2, nullptr);
    EXPECT_FALSE(heap_->changeSize(ptr1, kSmallAlloc * ZBAL_UT_NUM_2));
}

/* ==================== allocatedSize() ==================== */

TEST_F(TestMemoryHeap, AllocatedSize)
{
    void *ptr = heap_->allocate(kMediumAlloc);
    ASSERT_NE(ptr, nullptr);
    uint64_t size = 0;
    EXPECT_TRUE(heap_->allocatedSize(ptr, size));
    EXPECT_GE(size, kMediumAlloc);

    EXPECT_FALSE(heap_->allocatedSize(nullptr, size));

    heap_->release(ptr);
    EXPECT_FALSE(heap_->allocatedSize(ptr, size));
}

/* ==================== RangeSizeFirstComparator ==================== */

TEST_F(TestMemoryHeap, RangeSizeFirstComparator)
{
    RangeSizeFirstComparator comp;
    MemoryRange mr1{ZBAL_UT_NUM_0, ZBAL_UT_NUM_128};
    MemoryRange mr2{ZBAL_UT_NUM_64, ZBAL_UT_NUM_256};
    EXPECT_TRUE(comp(mr1, mr2));
    EXPECT_FALSE(comp(mr2, mr1));

    MemoryRange mr3{ZBAL_UT_NUM_64, ZBAL_UT_NUM_128};
    EXPECT_TRUE(comp(mr1, mr3));
    EXPECT_FALSE(comp(mr3, mr1));
}

/* ==================== Wrapper API ==================== */

TEST_F(TestMemoryHeap, HeapWrapperAPI)
{
    void *devPtr = nullptr;
    EXPECT_EQ(HeapAlignedAllocate(&devPtr, kSmallAlloc, heap_), Z_OK);
    EXPECT_NE(devPtr, nullptr);

    void *devPtr2 = nullptr;
    EXPECT_EQ(HeapAlignedAllocate(&devPtr2, kHeapSize * ZBAL_UT_NUM_2, heap_), Z_ERROR_ALLOC);
    EXPECT_EQ(devPtr2, nullptr);

    void *devPtr3 = nullptr;
    EXPECT_EQ(HeapAlignedAllocate(&devPtr3, 0, heap_), Z_ERROR_ALLOC);

    EXPECT_EQ(HeapRelease(devPtr, heap_), Z_OK);

    size_t sz = 0;
    EXPECT_EQ(GetTotalSize(sz, heap_), Z_OK);
    EXPECT_EQ(sz, kHeapSize);

    size_t used = 0;
    EXPECT_EQ(GetInUsedSize(used, heap_), Z_OK);
    void *devPtr4 = nullptr;
    HeapAlignedAllocate(&devPtr4, kSmallAlloc, heap_);
    size_t usedAfter = 0;
    GetInUsedSize(usedAfter, heap_);
    EXPECT_GT(usedAfter, used);
}

/* ==================== Stress ==================== */

TEST_F(TestMemoryHeap, MultipleAllocFreeCycles)
{
    constexpr int kCycles = ZBAL_UT_NUM_5;
    void *ptrs[ZBAL_UT_NUM_10];
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        for (int i = 0; i < ZBAL_UT_NUM_10; ++i) {
            ptrs[i] = heap_->allocate(kSmallAlloc);
            ASSERT_NE(ptrs[i], nullptr) << "cycle=" << cycle << ", i=" << i;
        }
        for (int i = 0; i < ZBAL_UT_NUM_10; ++i) {
            heap_->release(ptrs[i]);
        }
    }
    void *big = heap_->allocate(kHeapSize);
    EXPECT_NE(big, nullptr);
}

TEST_F(TestMemoryHeap, FragmentedAllocation)
{
    constexpr int kBlockCount = ZBAL_UT_NUM_4;
    void *ptrs[kBlockCount];
    for (int i = 0; i < kBlockCount; ++i) {
        ptrs[i] = heap_->allocate(kSmallAlloc);
        ASSERT_NE(ptrs[i], nullptr);
    }
    heap_->release(ptrs[0]);
    heap_->release(ptrs[ZBAL_UT_NUM_2]);
    void *small1 = heap_->allocate(kSmallAlloc / ZBAL_UT_NUM_2);
    EXPECT_NE(small1, nullptr);
    void *small2 = heap_->allocate(kSmallAlloc / ZBAL_UT_NUM_2);
    EXPECT_NE(small2, nullptr);
    heap_->release(ptrs[1]);
    heap_->release(ptrs[ZBAL_UT_NUM_3]);
}
