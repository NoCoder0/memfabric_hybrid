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
#include <cstdlib>
#include <thread>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "zbal_test_constants.h"
#include "zbal_sma_mm_heap.h"

using namespace zbal;
using namespace zbal::sma::heap;
using namespace zbal::sma;

class TestSplitMemoryHeap : public testing::Test {
public:
    void SetUp() override
    {
        buffer_ = static_cast<uint8_t *>(aligned_alloc(ZBAL_UT_NUM_32, ZBAL_UT_HEAP_SIZE));
        ASSERT_NE(buffer_, nullptr) << "aligned_alloc failed";
        heap_ = new SplitMemoryHeap(buffer_, ZBAL_UT_HEAP_SIZE, ZBAL_UT_HEAP_SIZE);
        ASSERT_TRUE(heap_->isInitialized());

        apiBuffer_ = new uint8_t[ZBAL_UT_HEAP_SIZE];
        auto *rawHeap = new SplitMemoryHeap(apiBuffer_, ZBAL_UT_HEAP_SIZE);
        apiHeap_ = std::shared_ptr<CustomMemoryHeap>(rawHeap);
    }

    void TearDown() override
    {
        delete heap_;
        free(buffer_);

        apiHeap_.reset();
        delete[] apiBuffer_;
    }

    uint8_t *buffer_;
    SplitMemoryHeap *heap_;

    uint8_t *apiBuffer_;
    std::shared_ptr<CustomMemoryHeap> apiHeap_;
};

/* ==================== Constructor ==================== */

TEST_F(TestSplitMemoryHeap, ConstructValid)
{
    EXPECT_TRUE(heap_->isInitialized());
    EXPECT_EQ(heap_->getTotalSize(), ZBAL_UT_HEAP_SIZE);
    EXPECT_EQ(heap_->getInUsedSize(), 0u);
}

TEST_F(TestSplitMemoryHeap, ConstructNullBaseNotInitialized)
{
    SplitMemoryHeap heap(nullptr, ZBAL_UT_SIZE_1KB);
    EXPECT_FALSE(heap.isInitialized());
    EXPECT_EQ(heap.alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_128), nullptr);
}

TEST_F(TestSplitMemoryHeap, ConstructZeroSizeNotInitialized)
{
    uint8_t buf[ZBAL_UT_NUM_256];
    SplitMemoryHeap heap(buf, 0);
    EXPECT_FALSE(heap.isInitialized());
    EXPECT_EQ(heap.alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_128), nullptr);
}

/* ==================== Alloc / free / size ==================== */

TEST_F(TestSplitMemoryHeap, AllocFreeRoundTrip)
{
    void *p = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_SIZE_1KB);
    ASSERT_NE(p, nullptr);
    EXPECT_GE(heap_->getInUsedSize(), ZBAL_UT_SIZE_1KB);

    uint64_t sz = 0;
    EXPECT_TRUE(heap_->allocatedSize(p, sz));
    EXPECT_EQ(sz, ZBAL_UT_SIZE_1KB);

    EXPECT_EQ(heap_->release(p), 0);
    EXPECT_EQ(heap_->getInUsedSize(), 0u);
    EXPECT_FALSE(heap_->allocatedSize(p, sz));
}

TEST_F(TestSplitMemoryHeap, AllocZeroSizeReturnsNull)
{
    EXPECT_EQ(heap_->alignedAllocate(ZBAL_UT_NUM_32, 0), nullptr);
}

TEST_F(TestSplitMemoryHeap, AllocAlignmentOne)
{
    void *p = heap_->alignedAllocate(0, ZBAL_UT_NUM_64);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(heap_->release(p), 0);
}

TEST_F(TestSplitMemoryHeap, MultipleAllocsUntilOOM)
{
    std::vector<void *> ptrs;
    size_t blockSize = ZBAL_UT_HEAP_SIZE / ZBAL_UT_NUM_16;
    for (int i = 0; i < ZBAL_UT_NUM_16; i++) {
        void *p = heap_->alignedAllocate(ZBAL_UT_NUM_32, blockSize);
        ASSERT_NE(p, nullptr) << "failed at alloc " << i;
        ptrs.push_back(p);
    }
    EXPECT_EQ(heap_->alignedAllocate(ZBAL_UT_NUM_32, 1), nullptr);

    for (int i = 0; i < ZBAL_UT_NUM_8; i++) {
        EXPECT_EQ(heap_->release(ptrs[i]), 0);
    }
    for (int i = 0; i < ZBAL_UT_NUM_8; i++) {
        void *p = heap_->alignedAllocate(ZBAL_UT_NUM_32, blockSize);
        ASSERT_NE(p, nullptr) << "failed at re-alloc " << i;
    }
    for (int i = ZBAL_UT_NUM_8; i < ZBAL_UT_NUM_16; i++) {
        EXPECT_EQ(heap_->release(ptrs[i]), 0);
    }
}

/* ==================== Alignment ==================== */

TEST_F(TestSplitMemoryHeap, Alignment32)
{
    for (int i = 0; i < ZBAL_UT_NUM_50; i++) {
        void *p = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_7 + (i * ZBAL_UT_NUM_13) % ZBAL_UT_NUM_200);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % ZBAL_UT_NUM_32, 0u);
        heap_->release(p);
    }
}

TEST_F(TestSplitMemoryHeap, Alignment128)
{
    uint8_t bigBuf[ZBAL_UT_SIZE_1MB];
    SplitMemoryHeap heap(bigBuf, sizeof(bigBuf));
    for (int i = 0; i < ZBAL_UT_NUM_20; i++) {
        void *p = heap.alignedAllocate(ZBAL_UT_NUM_128, ZBAL_UT_NUM_63 + (i * ZBAL_UT_NUM_31) % ZBAL_UT_NUM_500);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % ZBAL_UT_NUM_128, 0u);
        heap.release(p);
    }
}

/* ==================== High-to-low allocation strategy ==================== */

TEST_F(TestSplitMemoryHeap, HiLoAllocatesFromHighAddress)
{
    uint8_t buf[ZBAL_UT_SIZE_64KB];
    SplitMemoryHeap heap(buf, ZBAL_UT_SIZE_64KB, 0);

    void *p1 = heap.alignedAllocate(1, ZBAL_UT_SIZE_1KB);
    void *p2 = heap.alignedAllocate(1, ZBAL_UT_SIZE_2KB);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    EXPECT_LT(reinterpret_cast<uintptr_t>(p2), reinterpret_cast<uintptr_t>(p1));

    heap.release(p1);
    heap.release(p2);
}

TEST_F(TestSplitMemoryHeap, HiLoExhaustThenOOM)
{
    constexpr size_t kSize = ZBAL_UT_SIZE_64KB;
    uint8_t buf[kSize];
    SplitMemoryHeap heap(buf, kSize, 0);

    std::vector<void *> ptrs;
    for (int i = 0; i < ZBAL_UT_NUM_32; i++) {
        void *p = heap.alignedAllocate(1, ZBAL_UT_SIZE_2KB);
        if (p == nullptr) break;
        ptrs.push_back(p);
    }
    EXPECT_GT(ptrs.size(), 0u);

    uintptr_t base = reinterpret_cast<uintptr_t>(buf);
    for (auto *p : ptrs) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        EXPECT_GE(addr, base);
        EXPECT_LT(addr, base + kSize);
        heap.release(p);
    }
}

TEST_F(TestSplitMemoryHeap, ThresholdBelowUsesLowToHigh)
{
    uint8_t buf[ZBAL_UT_SIZE_128KB];
    SplitMemoryHeap heap(buf, ZBAL_UT_SIZE_128KB, ZBAL_UT_SIZE_4KB);

    void *p = heap.alignedAllocate(1, ZBAL_UT_SIZE_1KB);
    ASSERT_NE(p, nullptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(buf);
    EXPECT_LT(reinterpret_cast<uintptr_t>(p) - base, ZBAL_UT_SIZE_128KB / 4u);
    heap.release(p);
}

TEST_F(TestSplitMemoryHeap, ThresholdAtUsesHighToLow)
{
    uint8_t buf[ZBAL_UT_SIZE_128KB];
    SplitMemoryHeap heap(buf, ZBAL_UT_SIZE_128KB, ZBAL_UT_SIZE_4KB);

    void *p = heap.alignedAllocate(1, ZBAL_UT_SIZE_4KB);
    ASSERT_NE(p, nullptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(buf);
    EXPECT_GT(reinterpret_cast<uintptr_t>(p) - base, ZBAL_UT_SIZE_128KB / 2u);
    heap.release(p);
}

/* ==================== Coalescing ==================== */

TEST_F(TestSplitMemoryHeap, CoalesceRightNeighbor)
{
    void *a = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_SIZE_1KB);
    void *b = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_SIZE_1KB);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    heap_->release(a);
    heap_->release(b);

    void *c = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_2000);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(heap_->release(c), 0);
}

TEST_F(TestSplitMemoryHeap, CoalesceLeftNeighbor)
{
    void *a = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_SIZE_1KB);
    void *b = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_SIZE_1KB);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    heap_->release(b);
    heap_->release(a);

    void *c = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_2000);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(heap_->release(c), 0);
}

TEST_F(TestSplitMemoryHeap, CoalesceBothNeighbors)
{
    void *a = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_512);
    void *b = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_SIZE_1KB);
    void *c = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_512);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    heap_->release(a);
    heap_->release(c);
    heap_->release(b);

    void *d = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_2000);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(heap_->release(d), 0);
}

/* ==================== Fragment / double-free / size tracking ==================== */

TEST_F(TestSplitMemoryHeap, AlignmentCreatesLowerFragment)
{
    void *p = heap_->alignedAllocate(ZBAL_UT_NUM_256, ZBAL_UT_SIZE_1KB);
    ASSERT_NE(p, nullptr);

    void *tiny = heap_->alignedAllocate(1, ZBAL_UT_NUM_8);
    if (tiny != nullptr) {
        heap_->release(tiny);
    }
    EXPECT_EQ(heap_->release(p), 0);
}

TEST_F(TestSplitMemoryHeap, DoubleFreeReturnsError)
{
    void *p = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_256);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(heap_->release(p), 0);
    EXPECT_EQ(heap_->release(p), -1);
}

TEST_F(TestSplitMemoryHeap, ReleaseNullReturnsError)
{
    EXPECT_EQ(heap_->release(nullptr), -1);
}

TEST_F(TestSplitMemoryHeap, ReleaseInvalidPointerReturnsError)
{
    int dummy = 0;
    EXPECT_EQ(heap_->release(&dummy), -1);
}

TEST_F(TestSplitMemoryHeap, InUsedSizeTracksAllocations)
{
    EXPECT_EQ(heap_->getInUsedSize(), 0u);

    void *p1 = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_100);
    size_t after1 = heap_->getInUsedSize();
    EXPECT_GE(after1, 100u);

    void *p2 = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_250);
    size_t after2 = heap_->getInUsedSize();
    EXPECT_GE(after2, after1 + 250u);

    heap_->release(p1);
    heap_->release(p2);
    EXPECT_EQ(heap_->getInUsedSize(), 0u);
}

TEST_F(TestSplitMemoryHeap, TotalSizeConstant)
{
    EXPECT_EQ(heap_->getTotalSize(), ZBAL_UT_HEAP_SIZE);
    void *p = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_512);
    EXPECT_EQ(heap_->getTotalSize(), ZBAL_UT_HEAP_SIZE);
    heap_->release(p);
    EXPECT_EQ(heap_->getTotalSize(), ZBAL_UT_HEAP_SIZE);
}

/* ==================== Bucket index ==================== */

TEST_F(TestSplitMemoryHeap, BucketIndexZero)
{
    EXPECT_EQ(get_bucket_index(0), 0);
}

TEST_F(TestSplitMemoryHeap, BucketIndexPowersOfTwo)
{
    EXPECT_EQ(get_bucket_index(1), 0);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_2), 1);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_4), ZBAL_UT_NUM_2);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_8), ZBAL_UT_NUM_3);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_16), ZBAL_UT_NUM_4);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_32), ZBAL_UT_NUM_5);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_64), ZBAL_UT_NUM_6);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_128), ZBAL_UT_NUM_7);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_256), ZBAL_UT_NUM_8);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_512), ZBAL_UT_NUM_9);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_SIZE_1KB), ZBAL_UT_NUM_10);
}

TEST_F(TestSplitMemoryHeap, BucketIndexNonPowerOfTwo)
{
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_3), 1);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_5), ZBAL_UT_NUM_2);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_7), ZBAL_UT_NUM_2);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_100), ZBAL_UT_NUM_6);
    EXPECT_EQ(get_bucket_index(ZBAL_UT_NUM_1000), ZBAL_UT_NUM_9);
}

/* ==================== Concurrency ==================== */

TEST_F(TestSplitMemoryHeap, ConcurrentAllocFree)
{
    std::vector<std::thread> threads;
    for (int t = 0; t < ZBAL_UT_THREAD_COUNT; t++) {
        threads.emplace_back([this]() {
            for (int i = 0; i < ZBAL_UT_OPS_PER_THREAD; i++) {
                void *p =
                    heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_NUM_64 + (i % ZBAL_UT_NUM_10) * ZBAL_UT_NUM_13);
                if (p != nullptr) {
                    for (volatile int d = 0; d < ZBAL_UT_NUM_10; d++) {}
                    EXPECT_EQ(heap_->release(p), 0);
                }
            }
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    EXPECT_EQ(heap_->getInUsedSize(), 0u);
}

/* ==================== C API wrappers ==================== */

TEST_F(TestSplitMemoryHeap, CustomHeapAlignedAllocateSuccess)
{
    void *devPtr = nullptr;
    EXPECT_EQ(CustomHeapAlignedAllocate(&devPtr, ZBAL_UT_NUM_256, apiHeap_), Z_OK);
    ASSERT_NE(devPtr, nullptr);
    EXPECT_EQ(CustomHeapRelease(devPtr, apiHeap_), 0);
}

TEST_F(TestSplitMemoryHeap, CustomHeapAlignedAllocateNullPool)
{
    void *devPtr = nullptr;
    EXPECT_EQ(CustomHeapAlignedAllocate(&devPtr, ZBAL_UT_NUM_256, nullptr), Z_ERROR);
}

TEST_F(TestSplitMemoryHeap, CustomHeapAlignedAllocateOOM)
{
    void *devPtr = nullptr;
    EXPECT_EQ(CustomHeapAlignedAllocate(&devPtr, ZBAL_UT_HEAP_SIZE * ZBAL_UT_NUM_2, apiHeap_), Z_ERROR_ALLOC);
    EXPECT_EQ(devPtr, nullptr);
}

TEST_F(TestSplitMemoryHeap, CustomHeapReleaseNullPool)
{
    int dummy = 0;
    EXPECT_EQ(CustomHeapRelease(&dummy, nullptr), Z_ERROR);
}

TEST_F(TestSplitMemoryHeap, CustomGetTotalSize)
{
    size_t size = 0;
    EXPECT_EQ(CustomGetTotalSize(size, apiHeap_), Z_OK);
    EXPECT_EQ(size, ZBAL_UT_HEAP_SIZE);
}

TEST_F(TestSplitMemoryHeap, CustomGetTotalSizeNullPool)
{
    size_t size = 0;
    EXPECT_EQ(CustomGetTotalSize(size, nullptr), Z_ERROR);
}

TEST_F(TestSplitMemoryHeap, CustomGetInUsedSize)
{
    size_t size = 0;
    void *p = nullptr;
    CustomHeapAlignedAllocate(&p, ZBAL_UT_NUM_512, apiHeap_);
    EXPECT_EQ(CustomInUsedSize(size, apiHeap_), Z_OK);
    EXPECT_GE(size, 512u);
    CustomHeapRelease(p, apiHeap_);
}

TEST_F(TestSplitMemoryHeap, CustomGetInUsedSizeNullPool)
{
    size_t size = 0;
    EXPECT_EQ(CustomInUsedSize(size, nullptr), Z_ERROR);
}

/* ==================== Edge cases ==================== */

TEST_F(TestSplitMemoryHeap, AllocEntireHeap)
{
    void *p = heap_->alignedAllocate(1, ZBAL_UT_HEAP_SIZE - ZBAL_UT_NUM_128);
    ASSERT_NE(p, nullptr);
    void *q2 = heap_->alignedAllocate(1, ZBAL_UT_HEAP_SIZE / ZBAL_UT_NUM_2);
    EXPECT_EQ(q2, nullptr);
    EXPECT_EQ(heap_->release(p), 0);
    void *q = heap_->alignedAllocate(ZBAL_UT_NUM_32, ZBAL_UT_SIZE_1KB);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(heap_->release(q), 0);
}

TEST_F(TestSplitMemoryHeap, ManyTinyAllocs)
{
    std::vector<void *> ptrs;
    for (int i = 0; i < ZBAL_UT_NUM_200; i++) {
        void *p = heap_->alignedAllocate(ZBAL_UT_NUM_8, ZBAL_UT_NUM_32);
        if (p == nullptr) break;
        ptrs.push_back(p);
    }
    EXPECT_GT(ptrs.size(), 50u);
    for (auto *p : ptrs) {
        EXPECT_EQ(heap_->release(p), 0);
    }
    EXPECT_EQ(heap_->getInUsedSize(), 0u);
}
