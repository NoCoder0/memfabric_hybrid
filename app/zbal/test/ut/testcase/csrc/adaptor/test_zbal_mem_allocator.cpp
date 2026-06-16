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
#include <atomic>
#include <functional>

#include "zbal_test_constants.h"
#include "zbal_mem_allocator.h"

#undef ZBAL_SMA_CONFIG_H
#define private public
#include "zbal_sma_config.h"
#undef private

#include "zbal_defines.h"

using namespace zbal;

extern bool gGVASpaceInited;
extern ska::flat_hash_set<void *> gDmaBlocks;

static const char *kAllocConfEnv = "PYTORCH_NPU_ALLOC_CONF";

extern "C" {
void zbal_pluggable_record_stream(void *ptr, c10_npu::NPUStream stream);
void zbal_pluggable_erase_stream(void *ptr, c10_npu::NPUStream stream);
void *zbal_get_symm_base_addr();
void zbal_pluggable_begin_allocate_to_pool(int device, c10_npu::MempoolId_t mempool_id,
                                           std::function<bool(aclrtStream)> filter);
void zbal_pluggable_end_allocate_to_pool(int device, c10_npu::MempoolId_t mempool_id);
void zbal_pluggable_release_pool(int device, c10_npu::MempoolId_t mempool_id);
c10_npu::NPUCachingAllocator::DeviceStats zbal_pluggable_get_device_stats(int device);
}

namespace c10_npu {
namespace dma {

class CachingAllocatorConfig {
public:
    static CachingAllocatorConfig &instance();
    void parseArgs(const char *env);
};

inline void resetExpandableSegmentsForTest()
{
    const char *env = getenv("PYTORCH_NPU_ALLOC_CONF");
    CachingAllocatorConfig::instance().parseArgs(env);
}

} // namespace dma
} // namespace c10_npu

class TestMemAllocator : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv(kAllocConfEnv);
        setenv("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:False", 1);
        c10_npu::dma::resetExpandableSegmentsForTest();
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }

    void SetVmmMode()
    {
        zbal_pluggable_init(ZBAL_UT_NUM_8);
        zbal_allocator_options_t opts = {};
        zbal_sma_init(&opts, 0);
        zbal::sma::SMAConfig::instance().use_vmm_for_static_memory_ = true;
    }

    void RestoreSmaMode()
    {
        zbal::sma::SMAConfig::instance().use_vmm_for_static_memory_ = false;
        gGVASpaceInited = false;
    }
};

TEST_F(TestMemAllocator, InitAndUninit)
{
    zbal_allocator_options_t opts = {};
    opts.myGva = nullptr;
    opts.size = 0;
    EXPECT_EQ(zbal_sma_init(&opts, 0), ZResultErrorCode::Z_OK);
    zbal_sma_uninit(0);

    zbal::sma::SMAConfig::instance().use_vmm_for_static_memory_ = true;
    gGVASpaceInited = false;
    opts.myGva = reinterpret_cast<void *>(0x2000);
    opts.size = ZBAL_UT_SIZE_4KB;
    EXPECT_EQ(zbal_sma_init(&opts, 0), ZResultErrorCode::Z_OK);
    zbal::sma::SMAConfig::instance().use_vmm_for_static_memory_ = false;
}

TEST_F(TestMemAllocator, PluggableInitAndSimulate)
{
    zbal_pluggable_init(ZBAL_UT_NUM_8);
    SUCCEED();

    zbal_simulate_init(0x100000000000ULL, ZBAL_UT_SIZE_1MB);
    SUCCEED();
}

TEST_F(TestMemAllocator, MallocFreeAndStreams)
{
    zbal_allocator_options_t opts = {};
    ASSERT_EQ(zbal_sma_init(&opts, 0), ZResultErrorCode::Z_OK);

    void *p1 = zbal_pluggable_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    ASSERT_NE(p1, nullptr);

    auto stream = c10_npu::getDefaultNPUStream();
    zbal_pluggable_record_stream(p1, stream);
    zbal_pluggable_erase_stream(p1, stream);
    zbal_pluggable_free(p1, ZBAL_UT_SIZE_1KB, 0, nullptr);

    SetVmmMode();
    void *p2 = zbal_pluggable_malloc(ZBAL_UT_SIZE_2KB, 0, nullptr);
    ASSERT_NE(p2, nullptr);
    zbal_pluggable_record_stream(p2, stream);
    zbal_pluggable_erase_stream(p2, stream);
    zbal_pluggable_free(p2, ZBAL_UT_SIZE_2KB, 0, nullptr);
    RestoreSmaMode();
}

TEST_F(TestMemAllocator, PoolAndEmptyCache)
{
    zbal_pluggable_init(ZBAL_UT_NUM_8);
    zbal_allocator_options_t opts = {};
    zbal_sma_init(&opts, 0);

    c10_npu::MempoolId_t pool_id = {0, 1};
    auto filter = [](aclrtStream) { return true; };

    zbal_pluggable_begin_allocate_to_pool(0, pool_id, filter);
    zbal_pluggable_end_allocate_to_pool(0, pool_id);
    zbal_pluggable_release_pool(0, pool_id);

    zbal_pluggable_empty_cache(false);
    zbal_pluggable_empty_cache(true);

    auto stats = zbal_pluggable_get_device_stats(0);
    SUCCEED();

    zbal::sma::SMAConfig::instance().use_vmm_for_static_memory_ = true;
    zbal_pluggable_begin_allocate_to_pool(0, pool_id, filter);
    zbal_pluggable_end_allocate_to_pool(0, pool_id);
    zbal_pluggable_release_pool(0, pool_id);
    zbal::sma::SMAConfig::instance().use_vmm_for_static_memory_ = false;
}

TEST_F(TestMemAllocator, GetSymmBaseAddr)
{
    zbal_pluggable_init(1);
    zbal_allocator_options_t opts = {};
    opts.myGva = reinterpret_cast<void *>(0x1000);
    opts.size = ZBAL_UT_SIZE_4KB;
    zbal_sma_init(&opts, 0);
    EXPECT_NE(zbal_get_symm_base_addr(), nullptr);

    zbal::sma::SMAConfig::instance().use_vmm_for_static_memory_ = true;
    EXPECT_NE(zbal_get_symm_base_addr(), nullptr);
    zbal::sma::SMAConfig::instance().use_vmm_for_static_memory_ = false;
}
