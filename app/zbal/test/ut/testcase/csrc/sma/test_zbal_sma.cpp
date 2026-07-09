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
#include <sys/resource.h>

#include "zbal_test_constants.h"

#define private   public
#define protected public
#include "zbal_sma.h"
#include "zbal_sma_device_info.h"
#undef private
#undef protected

#include "zbal_sma_device_pool.h"

using namespace zbal;
using namespace zbal::sma;
using namespace zbal::sma::device;

static char g_heap_buffer[ZBAL_UT_SIZE_4MB];

class TestZBALSMA : public testing::Test {
protected:
    void SetUp() override
    {
        auto *alloc = SecondaryMemoryAllocator::GetInstance().Get();
        alloc->allocated_blocks_.clear();
        alloc->device_allocator_.clear();
        auto &obs = DeviceInfoObserver::getInstance();
        obs.record_history_ = false;
        obs.snapshots_.clear();
    }

    void TearDown() override
    {
        auto *alloc = SecondaryMemoryAllocator::GetInstance().Get();
        alloc->allocated_blocks_.clear();
        alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
    }

    SecondaryMemoryAllocator *Alloc()
    {
        return SecondaryMemoryAllocator::GetInstance().Get();
    }
};

TEST_F(TestZBALSMA, BlockOperations)
{
    auto *alloc = Alloc();
    DeviceBlockPool pool;
    {
        DeviceBlock block(ZBAL_UT_NUM_0, nullptr, ZBAL_UT_NUM_256, &pool, reinterpret_cast<void *>(0x1000), BT_SMALL);
        alloc->add_allocated_block(&block);
        auto *found = alloc->get_allocated_block(reinterpret_cast<void *>(0x1000));
        EXPECT_EQ(found, &block);
    }
    {
        auto *found = alloc->get_allocated_block(reinterpret_cast<void *>(0xDEAD));
        EXPECT_EQ(found, nullptr);
    }
    {
        DeviceBlock block(ZBAL_UT_NUM_0, nullptr, ZBAL_UT_NUM_256, &pool, reinterpret_cast<void *>(0x2000), BT_SMALL);
        alloc->add_allocated_block(&block);
        auto *found = alloc->get_allocated_block(reinterpret_cast<void *>(0x2000), true);
        EXPECT_EQ(found, &block);
        found = alloc->get_allocated_block(reinterpret_cast<void *>(0x2000));
        EXPECT_EQ(found, nullptr);
    }
    {
        DeviceBlock block(ZBAL_UT_NUM_0, nullptr, ZBAL_UT_NUM_256, &pool, reinterpret_cast<void *>(0x3000), BT_SMALL);
        alloc->add_allocated_block(&block);
        auto *first = alloc->get_allocated_block(reinterpret_cast<void *>(0x3000), false);
        EXPECT_EQ(first, &block);
        auto *second = alloc->get_allocated_block(reinterpret_cast<void *>(0x3000));
        EXPECT_EQ(second, &block);
    }
    {
        alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
        void *badPtr = reinterpret_cast<void *>(0xBAD);
        DeviceBlock fakeBlock(ZBAL_UT_NUM_0, nullptr, ZBAL_UT_NUM_64, &pool, badPtr, BT_SMALL);
        alloc->add_allocated_block(&fakeBlock);
        EXPECT_EQ(alloc->Free(badPtr), Z_OK);
    }
}

TEST_F(TestZBALSMA, InitAndValidation)
{
    auto *alloc = Alloc();
    EXPECT_FALSE(alloc->initialized());
    alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
    EXPECT_TRUE(alloc->initialized());

    alloc->allocated_blocks_.clear();
    alloc->device_allocator_.clear();
    ASSERT_EQ(alloc->device_allocator_.size(), ZBAL_UT_NUM_0);
    alloc->Initialize(nullptr, ZBAL_UT_NUM_3);
    EXPECT_EQ(alloc->device_allocator_.size(), ZBAL_UT_NUM_3);
    for (int i = 0; i < ZBAL_UT_NUM_3; i++) {
        EXPECT_NE(alloc->device_allocator_[i], nullptr);
    }

    auto *first = alloc->device_allocator_[ZBAL_UT_NUM_0].get();
    alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
    EXPECT_EQ(alloc->device_allocator_.size(), ZBAL_UT_NUM_3);
    EXPECT_EQ(alloc->device_allocator_[ZBAL_UT_NUM_0].get(), first);

    alloc->device_allocator_.resize(ZBAL_UT_NUM_3);
    EXPECT_THROW(alloc->assertValidDevice(-1), std::runtime_error);
    alloc->device_allocator_.resize(ZBAL_UT_NUM_1);
    EXPECT_THROW(alloc->assertValidDevice(ZBAL_UT_NUM_1), std::runtime_error);
    alloc->device_allocator_.resize(ZBAL_UT_NUM_3);
    EXPECT_NO_THROW(alloc->assertValidDevice(ZBAL_UT_NUM_0));
    EXPECT_NO_THROW(alloc->assertValidDevice(ZBAL_UT_NUM_2));

    alloc->Initialize(nullptr, ZBAL_UT_NUM_2);
    EXPECT_NO_THROW(alloc->markAllBlockUnsafe(ZBAL_UT_NUM_0));
    EXPECT_NO_THROW(alloc->markAllBlockUnsafe(ZBAL_UT_NUM_1));

    EXPECT_NO_THROW(alloc->cleanEvent());
    alloc->Initialize(nullptr, ZBAL_UT_NUM_2);
    EXPECT_NO_THROW(alloc->cleanEvent());
}

TEST_F(TestZBALSMA, BlockSafety)
{
    auto *alloc = Alloc();
    DeviceBlockPool pool;
    {
        c10::DataPtr nullPtr;
        EXPECT_TRUE(alloc->checkBlockIsSafe(nullPtr));
        EXPECT_NO_THROW(alloc->updateBlockToSafe(nullPtr));
    }
    {
        DeviceBlock block(ZBAL_UT_NUM_0, nullptr, ZBAL_UT_NUM_256, &pool, reinterpret_cast<void *>(0x4000), BT_SMALL);
        block.is_safe_ = false;
        alloc->add_allocated_block(&block);
        int *raw = reinterpret_cast<int *>(0x4000);
        c10::DataPtr ptr(raw, raw, [](void *) {}, c10::Device(c10::DeviceType::PrivateUse1, ZBAL_UT_NUM_0));
        EXPECT_FALSE(alloc->checkBlockIsSafe(ptr));
        block.is_safe_ = true;
        EXPECT_TRUE(alloc->checkBlockIsSafe(ptr));
    }
    {
        DeviceBlock block(ZBAL_UT_NUM_0, nullptr, ZBAL_UT_NUM_256, &pool, reinterpret_cast<void *>(0x5000), BT_SMALL);
        block.is_safe_ = false;
        alloc->add_allocated_block(&block);
        int *raw = reinterpret_cast<int *>(0x5000);
        c10::DataPtr ptr(raw, raw, [](void *) {}, c10::Device(c10::DeviceType::PrivateUse1, ZBAL_UT_NUM_0));
        alloc->updateBlockToSafe(ptr);
        EXPECT_TRUE(block.is_safe_);
    }
}

TEST_F(TestZBALSMA, AllocateFree)
{
    auto *alloc = Alloc();
    alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
    {
        void *ptr = nullptr;
        EXPECT_EQ(alloc->Allocate(&ptr, ZBAL_UT_NUM_0, ZBAL_UT_SIZE_1KB, nullptr), Z_OK);
        EXPECT_NE(ptr, nullptr);
        auto *block = alloc->get_allocated_block(ptr);
        EXPECT_NE(block, nullptr);
        EXPECT_EQ(block->requested_size_, ZBAL_UT_SIZE_1KB);
    }
    {
        struct rlimit oldLimit;
        getrlimit(RLIMIT_CORE, &oldLimit);
        struct rlimit zeroLimit {
            ZBAL_UT_NUM_0, ZBAL_UT_NUM_0
        };
        setrlimit(RLIMIT_CORE, &zeroLimit);
        void *ptr = nullptr;
        EXPECT_DEATH(alloc->Allocate(&ptr, ZBAL_UT_NUM_2, ZBAL_UT_SIZE_1KB, nullptr), "");
        setrlimit(RLIMIT_CORE, &oldLimit);
    }
    {
        EXPECT_EQ(alloc->Free(nullptr), Z_INVALID_PTR);
    }
    {
        alloc->allocated_blocks_.clear();
        alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
        void *ptr = nullptr;
        alloc->Allocate(&ptr, ZBAL_UT_NUM_0, ZBAL_UT_NUM_256, nullptr);
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(alloc->Free(ptr), Z_OK);
        EXPECT_EQ(alloc->get_allocated_block(ptr), nullptr);
    }
}

TEST_F(TestZBALSMA, StreamOperations)
{
    auto *alloc = Alloc();
    EXPECT_EQ(alloc->RecordStream(nullptr, c10_npu::getNPUStreamFromPool(ZBAL_UT_NUM_0)), Z_ERROR);
    EXPECT_EQ(alloc->EraseStream(nullptr, c10_npu::getNPUStreamFromPool(ZBAL_UT_NUM_0)), Z_ERROR);

    alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
    {
        void *ptr = nullptr;
        alloc->Allocate(&ptr, ZBAL_UT_NUM_0, ZBAL_UT_NUM_256, nullptr);
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(alloc->RecordStream(ptr, c10_npu::getNPUStreamFromPool(ZBAL_UT_NUM_0)), Z_OK);
    }
    {
        void *ptr = nullptr;
        alloc->Allocate(&ptr, ZBAL_UT_NUM_0, ZBAL_UT_NUM_512, nullptr);
        ASSERT_NE(ptr, nullptr);
        auto *block = alloc->get_allocated_block(ptr);
        ASSERT_NE(block, nullptr);
        block->stream_ = c10_npu::getCurrentNPUStream(ZBAL_UT_NUM_0).stream(false);
        EXPECT_EQ(alloc->EraseStream(ptr, c10_npu::getNPUStreamFromPool(ZBAL_UT_NUM_0)), Z_OK);
    }
    {
        void *ptr = nullptr;
        alloc->Allocate(&ptr, ZBAL_UT_NUM_0, ZBAL_UT_NUM_512, nullptr);
        ASSERT_NE(ptr, nullptr);
        auto *block = alloc->get_allocated_block(ptr);
        ASSERT_NE(block, nullptr);
        block->stream_ = reinterpret_cast<void *>(0xDEAD);
        EXPECT_EQ(alloc->EraseStream(ptr, c10_npu::getNPUStreamFromPool(ZBAL_UT_NUM_0)), Z_ERROR);
    }
}

TEST_F(TestZBALSMA, PoolAndStats)
{
    auto *alloc = Alloc();
    alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
    {
        c10_npu::MempoolId_t id = {ZBAL_UT_NUM_1, ZBAL_UT_NUM_0};
        EXPECT_EQ(alloc->BeginAllocateToPool(ZBAL_UT_NUM_0, id, nullptr), Z_OK);
        EXPECT_EQ(alloc->EndAllocateToPool(ZBAL_UT_NUM_0, id), Z_OK);
        EXPECT_EQ(alloc->ReleasePool(ZBAL_UT_NUM_0, id), Z_OK);
    }
    {
        DeviceStats stats = alloc->GetDeviceStats(ZBAL_UT_NUM_0);
        (void)stats;
        EXPECT_THROW(alloc->GetDeviceStats(ZBAL_UT_NUM_2), std::runtime_error);
    }
    {
        EXPECT_EQ(alloc->ResetAccumulatedStats(ZBAL_UT_NUM_0), Z_OK);
        EXPECT_EQ(alloc->ResetPeakStats(ZBAL_UT_NUM_0), Z_OK);
    }
    {
        alloc->device_allocator_[ZBAL_UT_NUM_0]->setMemHeapPool(g_heap_buffer, sizeof(g_heap_buffer));
        size_t in_used = ZBAL_UT_NUM_0;
        size_t total = ZBAL_UT_NUM_0;
        EXPECT_EQ(alloc->GetHeapState(in_used, total, ZBAL_UT_NUM_0), Z_OK);
        EXPECT_GT(total, ZBAL_UT_NUM_0);
    }
    {
        EXPECT_EQ(alloc->EmptyCache(false), Z_OK);
        EXPECT_EQ(alloc->EmptyCache(true), Z_OK);
        alloc->Initialize(nullptr, ZBAL_UT_NUM_2);
        alloc->device_allocator_[ZBAL_UT_NUM_0]->setMemHeapPool(g_heap_buffer, sizeof(g_heap_buffer));
        EXPECT_EQ(alloc->EmptyCache(false), Z_OK);
    }
}

TEST_F(TestZBALSMA, CAPI_SmaInitMallocFree)
{
    auto *alloc = Alloc();
    ASSERT_EQ(alloc->device_allocator_.size(), ZBAL_UT_NUM_0);
    sma_init(ZBAL_UT_NUM_2);
    EXPECT_EQ(alloc->device_allocator_.size(), ZBAL_UT_NUM_2);

    alloc->device_allocator_.clear();
    sma_init(ZBAL_UT_NUM_0);
    EXPECT_EQ(alloc->device_allocator_.size(), ZBAL_UT_NUM_0);

    alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
    EXPECT_EQ(sma_malloc(ZBAL_UT_NUM_0, ZBAL_UT_NUM_0, nullptr), nullptr);

    void *ptr = sma_malloc(ZBAL_UT_NUM_512, ZBAL_UT_NUM_0, nullptr);
    EXPECT_NE(ptr, nullptr);
    auto *block = alloc->get_allocated_block(ptr);
    EXPECT_NE(block, nullptr);

    sma_free(ptr, ZBAL_UT_NUM_256, ZBAL_UT_NUM_0, nullptr);
    EXPECT_EQ(alloc->get_allocated_block(ptr), nullptr);

    ptr = sma_malloc(ZBAL_UT_NUM_512, ZBAL_UT_NUM_0, nullptr);
    ASSERT_NE(ptr, nullptr);
    sma_record_stream(ptr, c10_npu::getNPUStreamFromPool(ZBAL_UT_NUM_0));

    ptr = sma_malloc(ZBAL_UT_NUM_256, ZBAL_UT_NUM_0, nullptr);
    ASSERT_NE(ptr, nullptr);
    block = alloc->get_allocated_block(ptr);
    ASSERT_NE(block, nullptr);
    block->stream_ = c10_npu::getCurrentNPUStream(ZBAL_UT_NUM_0).stream(false);
    sma_erase_stream(ptr, c10_npu::getNPUStreamFromPool(ZBAL_UT_NUM_0));

    {
        c10_npu::MempoolId_t id = {ZBAL_UT_NUM_1, ZBAL_UT_NUM_0};
        sma_begin_allocate_to_pool(ZBAL_UT_NUM_0, id, nullptr);
        sma_end_allocate_to_pool(ZBAL_UT_NUM_0, id);
        sma_release_pool(ZBAL_UT_NUM_0, id);
    }

    sma_empty_cache(false);
    sma_empty_cache(true);
}

TEST_F(TestZBALSMA, CAPI_StatsDevHeap)
{
    auto *alloc = Alloc();
    alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
    {
        DeviceStats stats = sma_get_device_stats(ZBAL_UT_NUM_0);
        (void)stats;
        EXPECT_THROW(sma_get_device_stats(ZBAL_UT_NUM_2), std::runtime_error);
    }
    {
        alloc->device_allocator_[ZBAL_UT_NUM_0]->setMemHeapPool(g_heap_buffer, sizeof(g_heap_buffer));
        EXPECT_EQ(sma_get_base_addr(ZBAL_UT_NUM_0), g_heap_buffer);
        EXPECT_EQ(sma_get_base_addr(-1), g_heap_buffer);
    }
    {
        alloc->device_allocator_.clear();
        alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
        EXPECT_FALSE(alloc->device_allocator_[ZBAL_UT_NUM_0]->isHeapInited());
        char heapBuf[ZBAL_UT_SIZE_4KB];
        sma_init_heap(heapBuf, ZBAL_UT_SIZE_4KB);
        EXPECT_TRUE(alloc->device_allocator_[ZBAL_UT_NUM_0]->isHeapInited());
        sma_init_heap(heapBuf, sizeof(heapBuf));
        EXPECT_TRUE(alloc->device_allocator_[ZBAL_UT_NUM_0]->isHeapInited());
    }
    {
        sma_init_heap(g_heap_buffer, ZBAL_UT_SIZE_4KB);
    }
    {
        alloc->device_allocator_[ZBAL_UT_NUM_0]->setMemHeapPool(g_heap_buffer, sizeof(g_heap_buffer));
        size_t inUsed = ZBAL_UT_NUM_0, total = ZBAL_UT_NUM_0;
        sma_get_heap_stats(inUsed, total, ZBAL_UT_NUM_0);
        EXPECT_GT(total, ZBAL_UT_NUM_0);
        sma_get_heap_stats(inUsed, total, -1);
        EXPECT_GT(total, ZBAL_UT_NUM_0);
    }
    {
        alloc->allocated_blocks_.clear();
        alloc->Initialize(nullptr, ZBAL_UT_NUM_1);
        size_t inUsed = ZBAL_UT_NUM_0, total = ZBAL_UT_NUM_0;
        sma_get_heap_stats(inUsed, total, ZBAL_UT_NUM_0);
    }
}

TEST_F(TestZBALSMA, RecordMemoryHistory)
{
    auto &obs = DeviceInfoObserver::getInstance();
    sma_record_memory_history(std::string("all"), ZBAL_UT_NUM_50);
    EXPECT_TRUE(obs.record_history_);
    EXPECT_EQ(obs.max_trace_len_, -1);

    sma_record_memory_history(std::string("state"), ZBAL_UT_NUM_100);
    EXPECT_TRUE(obs.record_history_);
    EXPECT_EQ(obs.max_trace_len_, ZBAL_UT_NUM_100);

    sma_record_memory_history(std::nullopt, ZBAL_UT_NUM_50);
    EXPECT_FALSE(obs.record_history_);

    EXPECT_THROW(sma_record_memory_history(std::string("invalid"), ZBAL_UT_NUM_0), std::runtime_error);
}
