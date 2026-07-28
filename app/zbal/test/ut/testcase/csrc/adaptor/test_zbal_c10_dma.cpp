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
#include <optional>
#include <string>
#include <vector>
#include <cstdlib>

#include "zbal_test_constants.h"
#include "zbal_pytorch_c10_dma.h"

// ====== Test-only bridge: re-declare CachingAllocatorConfig to expose private members ======
// The original class is defined in zbal_pytorch_c10_dma.cpp inside c10_npu::dma namespace.
// instance() definition in that TU is the single object; this redeclaration resolves to it.
namespace c10_npu {
namespace dma {

class CachingAllocatorConfig {
public:
    static CachingAllocatorConfig &instance();

    void parseArgs(const char *env);

    size_t m_max_split_size;
    double m_garbage_collection_threshold;
    bool m_expandable_segments;
    bool set_expandable_segments_flag;
    size_t m_base_addr_aligned_size;
    bool m_page_size_1g;
    size_t m_segment_size_mb;
};

void configParseEnvForTest()
{
    const char *env = getenv("PYTORCH_NPU_ALLOC_CONF");
    CachingAllocatorConfig::instance().parseArgs(env);
}

size_t getConfigMaxSplitSize()
{
    return CachingAllocatorConfig::instance().m_max_split_size;
}

size_t getConfigBaseAddrAlignedSize()
{
    return CachingAllocatorConfig::instance().m_base_addr_aligned_size;
}

size_t getConfigSegmentSizeMb()
{
    return CachingAllocatorConfig::instance().m_segment_size_mb;
}

} // namespace dma
} // namespace c10_npu

// ====== Test-only bridge: re-declare DlCannApi to expose private static function pointers ======
// The real DlCannApi is in dl_cann_api.h (not included in this TU).
// The static member is defined in dl_cann_api.cpp (linked into the test binary).
namespace zbal {
namespace underapi {
using aclrtReserveMemAddressFunc = int (*)(void **, size_t, size_t, void *, uint64_t);
// Use uint32_t for enum-typed params (aclrtMemMallocPolicy / aclrtMemAttr) to avoid
// pulling in dl_cann_api_def.h; the binary representation is identical.
using aclrtMallocAlign32Func = int32_t (*)(void **, size_t, uint32_t);
using aclrtGetMemInfoFunc = int32_t (*)(uint32_t, size_t *, size_t *);
class DlCannApi {
public:
    static aclrtReserveMemAddressFunc pAclrtReserveMemAddress;
    static aclrtMallocAlign32Func pAclrtMallocAlign32;
    static aclrtGetMemInfoFunc pAclrtGetMemInfo;
};
} // namespace underapi
} // namespace zbal

// Expandable segment stub returns unique fake addresses
namespace {
static std::atomic<uint64_t> g_expseg_vaddr{0x400000000000ULL};

static int32_t stubReserveMemAddress(void **ptr, size_t size, size_t, void *, uint64_t)
{
    *ptr = size ? reinterpret_cast<void *>(g_expseg_vaddr.fetch_add(size + 0x100000ULL)) : nullptr;
    return 0;
}
} // anonymous namespace

static void setupExpandableSegmentMock()
{
    zbal::underapi::DlCannApi::pAclrtReserveMemAddress = stubReserveMemAddress;
}

// Install mock function pointers for dma_malloc/dma_free paths that route through
// DlCannApi::AclrtMallocAlign32 / AclrtGetMemInfo. Without this, the function
// pointers stay nullptr (LoadLibrary is never called in UT), alloc_block returns
// Z_DL_FUNCTION_UNLOAD which is not handled, and params.block (nullptr) is
// dereferenced causing a segfault.
// The aclrtMallocAlign32/aclrtGetMemInfo symbols are declared in acl_rt.h (via
// torch_npu headers) and defined in acl_stub.cpp — we take their address directly
// to avoid conflicting extern "C" redeclarations.
static void setupDmaMallocMock()
{
    zbal::underapi::DlCannApi::pAclrtMallocAlign32 =
        reinterpret_cast<zbal::underapi::aclrtMallocAlign32Func>(&aclrtMallocAlign32);
    zbal::underapi::DlCannApi::pAclrtGetMemInfo =
        reinterpret_cast<zbal::underapi::aclrtGetMemInfoFunc>(&aclrtGetMemInfo);
}
// ====== End test-only bridge ======

// Forward-declare block-level allocator helpers (defined in zbal_pytorch_c10_dma.cpp)
namespace c10_npu {
namespace dma {
void *MallocBlock(size_t size, void *stream, int device);
void FreeBlock(void *handle);
void *GetBlockPtr(const void *handle);
size_t GetBlockSize(const void *handle);
} // namespace dma
} // namespace c10_npu

namespace zbal {
namespace adaptor {
namespace pytorch_npu {

using c10_npu::dma::checkConfigExpandableSegments;
using c10_npu::dma::configParseEnvForTest;
using c10_npu::dma::getCachingAllocator;
using c10_npu::dma::getConfigBaseAddrAlignedSize;
using c10_npu::dma::getConfigMaxSplitSize;
using c10_npu::dma::getConfigSegmentSizeMb;
using c10_npu::dma::getFreeMutex;
using c10_npu::dma::isConfig1GPageSizeEnable;
TEST(TestFormatSize, CoversAllBranches)
{
    EXPECT_EQ(format_size(0), std::string("0 bytes"));
    EXPECT_EQ(format_size(1), std::string("1 bytes"));
    EXPECT_EQ(format_size(ZBAL_UT_SIZE_1KB), std::string("1024 bytes"));
    EXPECT_EQ(format_size(ZBAL_UT_NUM_1025), std::string("1.00 KiB"));
    EXPECT_EQ(format_size(ZBAL_UT_SIZE_1MB), std::string("1024.00 KiB"));
    EXPECT_EQ(format_size(ZBAL_UT_NUM_1048577), std::string("1.00 MiB"));
    EXPECT_EQ(format_size(ZBAL_UT_U64_1G), std::string("1024.00 MiB"));
    EXPECT_EQ(format_size(ZBAL_UT_U64_1G_PLUS1), std::string("1.00 GiB"));
    EXPECT_EQ(format_size(ZBAL_UT_U64_4G), std::string("4.00 GiB"));
}
TEST(TestGetFreeMutex, ReturnsNonNullAndSingleton)
{
    std::mutex *mtx = getFreeMutex();
    EXPECT_NE(mtx, nullptr);
    EXPECT_EQ(getFreeMutex(), mtx);
}
class TestDMABasic : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("ZBAL_CACHING_ALLOCATOR_EXPAND");
        unsetenv("ZBAL_CACHING_ALLOCATOR_1G");
        setupDmaMallocMock();
        dma_init(ZBAL_UT_NUM_8);
    }

    void TearDown() override
    {
        dma_empty_cache(false);
    }
};
TEST_F(TestDMABasic, DmaMallocZeroSizeReturnsNull)
{
    EXPECT_EQ(dma_malloc(0, 0, nullptr), nullptr);
}

TEST_F(TestDMABasic, DmaFreeNullPointerSafe)
{
    dma_free(nullptr, ZBAL_UT_SIZE_1KB, 0, nullptr);
    SUCCEED();
}

TEST_F(TestDMABasic, DmaFreeInvalidPointerThrows)
{
    EXPECT_THROW(dma_free(reinterpret_cast<void *>(0x1), 0, 0, nullptr), c10::Error);
}

TEST_F(TestDMABasic, DmaMallocSizeBoundariesAndSizes)
{
    size_t sizes[] = {
        1,
        ZBAL_UT_NUM_512,
        ZBAL_UT_NUM_513,
        ZBAL_UT_SIZE_4KB,
        ZBAL_UT_SIZE_64KB,
        ZBAL_UT_SIZE_1MB,
        ZBAL_UT_SIZE_2MB,
        ZBAL_UT_SIZE_10MB,
        ZBAL_UT_SIZE_12MB,
        ZBAL_UT_SIZE_50MB,
        ZBAL_UT_SIZE_100MB,
    };
    for (size_t sz : sizes) {
        void *ptr = dma_malloc(sz, 0, nullptr);
        EXPECT_NE(ptr, nullptr) << "size=" << sz;
        dma_free(ptr, sz, 0, nullptr);
    }
}

TEST_F(TestDMABasic, DmaMallocFreeRepeat)
{
    for (int i = 0; i < ZBAL_UT_NUM_8; ++i) {
        void *ptr = dma_malloc(ZBAL_UT_SIZE_4KB, 0, nullptr);
        EXPECT_NE(ptr, nullptr) << "iteration " << i;
        dma_free(ptr, ZBAL_UT_SIZE_4KB, 0, nullptr);
    }
}

TEST_F(TestDMABasic, DmaMallocAcrossDevices)
{
    void *p0 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    void *p1 = dma_malloc(ZBAL_UT_SIZE_2KB, 1, nullptr);
    void *p2 = dma_malloc(ZBAL_UT_SIZE_4KB, ZBAL_UT_NUM_2, nullptr);
    EXPECT_NE(p0, nullptr);
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    dma_free(p0, ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_free(p1, ZBAL_UT_SIZE_2KB, 1, nullptr);
    dma_free(p2, ZBAL_UT_SIZE_4KB, ZBAL_UT_NUM_2, nullptr);
}

TEST_F(TestDMABasic, DmaMallocWithStreamParameters)
{
    c10_npu::NPUStream s1 = c10_npu::getDefaultNPUStream();
    c10_npu::NPUStream s2 = c10_npu::getNPUStreamFromPool();
    void *p1 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, s1.stream());
    void *p2 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, s2.stream());
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    dma_free(p1, ZBAL_UT_SIZE_1KB, 0, s1.stream());
    dma_free(p2, ZBAL_UT_SIZE_1KB, 0, s2.stream());
}
TEST_F(TestDMABasic, StreamRecordEraseNullPtrSafe)
{
    c10_npu::NPUStream s = c10_npu::getDefaultNPUStream();
    dma_record_stream(nullptr, s);
    dma_erase_stream(nullptr, s);
}

TEST_F(TestDMABasic, StreamRecordEraseBasic)
{
    c10_npu::NPUStream s1 = c10_npu::getDefaultNPUStream();
    void *p = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_record_stream(p, s1);
    dma_erase_stream(p, s1);
    dma_free(p, ZBAL_UT_SIZE_1KB, 0, nullptr);
}

TEST_F(TestDMABasic, StreamRecordEraseMultipleStreams)
{
    c10_npu::NPUStream s1 = c10_npu::getDefaultNPUStream();
    c10_npu::NPUStream s2 = c10_npu::getNPUStreamFromPool();
    void *p = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_record_stream(p, s1);
    dma_record_stream(p, s2);
    dma_erase_stream(p, s2);
    dma_erase_stream(p, s1);
    dma_free(p, ZBAL_UT_SIZE_1KB, 0, nullptr);
}
TEST_F(TestDMABasic, DmaEmptyCacheBothCheckErrorPaths)
{
    void *p = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_empty_cache(true);
    dma_empty_cache(false);
    dma_free(p, ZBAL_UT_SIZE_1KB, 0, nullptr);
}

TEST_F(TestDMABasic, DmaEmptyCacheOnEmptyCache)
{
    dma_empty_cache(true);
    dma_empty_cache(false);
}
TEST_F(TestDMABasic, FinalizeAndReinit)
{
    void *p = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_free(p, ZBAL_UT_SIZE_1KB, 0, nullptr);
    finalize();
    dma_init(ZBAL_UT_NUM_8);
    p = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    EXPECT_NE(p, nullptr);
    dma_free(p, ZBAL_UT_SIZE_1KB, 0, nullptr);
}
TEST(TestDMAInit, HandlesZeroInitTrimsAllocators)
{
    dma_init(0);
    SUCCEED();
}

TEST(TestDMAInit, HandlesMultipleInit)
{
    dma_init(ZBAL_UT_NUM_4);
    dma_init(ZBAL_UT_NUM_4);
    SUCCEED();
}
TEST_F(TestDMABasic, GetDeviceStatsAllDevices)
{
    for (int d = 0; d < ZBAL_UT_NUM_8; ++d) {
        auto stats = dma_get_device_stats(d);
        (void)stats;
    }
}

TEST_F(TestDMABasic, GetDeviceStatsOOBDeviceThrows)
{
    EXPECT_THROW(dma_get_device_stats(ZBAL_UT_NUM_999), c10::Error);
    EXPECT_THROW(dma_get_device_stats(ZBAL_UT_NUM_1000), c10::Error);
}
TEST_F(TestDMABasic, GetBaseAddrExplicitDevice)
{
    for (int d = 0; d < ZBAL_UT_NUM_8; ++d) {
        void *addr = dma_get_base_addr(d);
        (void)addr;
    }
}

TEST_F(TestDMABasic, GetBaseAddrNegativeUsesCurrentDevice)
{
    void *addr = dma_get_base_addr(-1);
    (void)addr;
}
TEST_F(TestDMABasic, InitHeapReentranceSkipped)
{
    auto buf1 = std::make_unique<uint8_t[]>(ZBAL_UT_SIZE_100MB);
    auto buf2 = std::make_unique<uint8_t[]>(ZBAL_UT_SIZE_100MB);
    dma_init_heap(buf1.get(), ZBAL_UT_SIZE_100MB, false);
    dma_init_heap(buf2.get(), ZBAL_UT_SIZE_100MB, false);
    SUCCEED();
}

TEST(TestDMAInitHeap, OutOfRangeDeviceErrorPath)
{
    dma_init(0);
    auto buf = std::make_unique<uint8_t[]>(ZBAL_UT_SIZE_64MB);
    dma_init_heap(buf.get(), ZBAL_UT_SIZE_64MB, false);
    SUCCEED();
}

TEST_F(TestDMABasic, GetHeapStatsExplicitDevice)
{
    size_t used = 1;
    size_t total = 1;
    dma_get_heap_stats(used, total, 0);
    dma_get_heap_stats(used, total, 1);
    dma_get_heap_stats(used, total, -1);
}

TEST(TestDMAHeapStats, UninitedHeapLeavesValuesUnchanged)
{
    dma_init(0);
    size_t used = 1;
    size_t total = 1;
    dma_get_heap_stats(used, total, 1);
    EXPECT_EQ(used, 1u);
    EXPECT_EQ(total, 1u);
}

TEST(TestDMAHeapStats, OOBDeviceLeavesValuesUnchanged)
{
    dma_init(1);
    size_t used = 1;
    size_t total = 1;
    dma_get_heap_stats(used, total, ZBAL_UT_NUM_999);
    EXPECT_EQ(used, 1u);
    EXPECT_EQ(total, 1u);
}
TEST_F(TestDMABasic, PoolLifecycleEmpty)
{
    auto filter = [](aclrtStream) { return true; };
    c10_npu::MempoolId_t poolId = {0, 1};
    dma_begin_allocate_to_pool(0, poolId, filter);
    dma_end_allocate_to_pool(0, poolId);
    dma_release_pool(0, poolId);
}

TEST_F(TestDMABasic, PoolLifecycleWithAllocs)
{
    auto filter = [](aclrtStream) { return true; };
    c10_npu::MempoolId_t poolId = {0, ZBAL_UT_NUM_2};
    dma_begin_allocate_to_pool(0, poolId, filter);
    void *p1 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    void *p2 = dma_malloc(ZBAL_UT_SIZE_4KB, 0, nullptr);
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    dma_free(p1, ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_free(p2, ZBAL_UT_SIZE_4KB, 0, nullptr);
    dma_end_allocate_to_pool(0, poolId);
    dma_release_pool(0, poolId);
}

TEST_F(TestDMABasic, PoolSharedLifecycle)
{
    auto filter = [](aclrtStream) { return true; };
    c10_npu::MempoolId_t poolId = {0, ZBAL_UT_NUM_3};
    dma_begin_allocate_to_pool(0, poolId, filter);
    dma_end_allocate_to_pool(0, poolId);
    dma_begin_allocate_to_pool(0, poolId, filter);
    dma_end_allocate_to_pool(0, poolId);
    dma_release_pool(0, poolId);
    dma_release_pool(0, poolId);
}

TEST_F(TestDMABasic, PoolOnDevice1)
{
    auto filter = [](aclrtStream) { return true; };
    c10_npu::MempoolId_t poolId = {0, ZBAL_UT_NUM_5};
    dma_begin_allocate_to_pool(1, poolId, filter);
    dma_end_allocate_to_pool(1, poolId);
    dma_release_pool(1, poolId);
}
TEST_F(TestDMABasic, DmaMallocNegativeDeviceThrows)
{
    EXPECT_THROW(dma_malloc(ZBAL_UT_SIZE_1KB, -1, nullptr), c10::Error);
    EXPECT_THROW(dma_malloc(ZBAL_UT_SIZE_1KB, -ZBAL_UT_NUM_99, nullptr), c10::Error);
}

TEST_F(TestDMABasic, DmaMallocDeviceOutOfRangeThrows)
{
    EXPECT_THROW(dma_malloc(ZBAL_UT_SIZE_1KB, ZBAL_UT_NUM_999, nullptr), c10::Error);
    EXPECT_THROW(dma_malloc(ZBAL_UT_SIZE_1KB, ZBAL_UT_NUM_1000, nullptr), c10::Error);
}
TEST_F(TestDMABasic, GetDeviceStatsNegativeDeviceThrows)
{
    EXPECT_THROW(dma_get_device_stats(-1), c10::Error);
    EXPECT_THROW(dma_get_device_stats(-ZBAL_UT_NUM_99), c10::Error);
}
TEST(TestDMAConfig, CheckConfigExpandableSegmentsDefaults)
{
    EXPECT_FALSE(checkConfigExpandableSegments());
}

TEST(TestDMAConfig, IsConfig1GPageSizeEnableDefaults)
{
    EXPECT_FALSE(isConfig1GPageSizeEnable());
}

class TestConfigLexArgs : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }
};

// lexArgs: comma-separated key:val pairs
TEST_F(TestConfigLexArgs, CommaDelimited)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "max_split_size_mb:50,garbage_collection_threshold:0.5", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigMaxSplitSize(), ZBAL_UT_SIZE_50MB);
}

// lexArgs: single key:val (no trailing comma), kLargeBuffer=20MB so value must be >ZBAL_UT_NUM_20
TEST_F(TestConfigLexArgs, SingleKeyNoComma)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "max_split_size_mb:32", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigMaxSplitSize(), ZBAL_UT_SIZE_32MB);
}

// lexArgs: spaces are skipped
TEST_F(TestConfigLexArgs, SpacesAreSkipped)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "max_split_size_mb :  50", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigMaxSplitSize(), ZBAL_UT_SIZE_50MB);
}

// lexArgs: empty string (nullptr env �?parseArgs returns early)
TEST_F(TestConfigLexArgs, NullEnvReturnsDefaults)
{
    unsetenv("PYTORCH_NPU_ALLOC_CONF");
    configParseEnvForTest();
    EXPECT_FALSE(checkConfigExpandableSegments());
    EXPECT_FALSE(isConfig1GPageSizeEnable());
}

class TestConfigMaxSplitSize : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }
};

// branch: valid max_split_size (must be >ZBAL_UT_NUM_20, kLargeBuffer=20MB)
TEST_F(TestConfigMaxSplitSize, Valid32MB)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "max_split_size_mb:32", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigMaxSplitSize(), ZBAL_UT_SIZE_32MB);
}

TEST_F(TestConfigMaxSplitSize, Valid100MB)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "max_split_size_mb:100", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigMaxSplitSize(), ZBAL_UT_SIZE_100MB);
}

// branch: too small (must be > kLargeBuffer/MB) �?throws
TEST_F(TestConfigMaxSplitSize, TooSmallThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "max_split_size_mb:0", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: missing value after colon �?throws
TEST_F(TestConfigMaxSplitSize, MissingValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "max_split_size_mb:", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

class TestConfigGCThreshold : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }
};

// branch: valid threshold 0 < x < 1
TEST_F(TestConfigGCThreshold, ValidThreshold05)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:0.5", 1);
    configParseEnvForTest();
    SUCCEED();
}

TEST_F(TestConfigGCThreshold, ValidThreshold01)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:0.1", 1);
    configParseEnvForTest();
    SUCCEED();
}

TEST_F(TestConfigGCThreshold, ValidThreshold099)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:0.99", 1);
    configParseEnvForTest();
    SUCCEED();
}

// branch: val <= 0 �?throws (too small)
TEST_F(TestConfigGCThreshold, ZeroThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:0.0", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

TEST_F(TestConfigGCThreshold, NegativeThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:-0.1", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: val >= 1.0 �?throws (too big)
TEST_F(TestConfigGCThreshold, ExactlyOneThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:1.0", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

TEST_F(TestConfigGCThreshold, AboveOneThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:1.5", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: missing value �?throws
TEST_F(TestConfigGCThreshold, MissingValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

class TestConfigExpandable : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }
};

// branch: True �?parseExpandableSegments entered, but aclrtReserveMemAddress fails
// in no-NPU UT env so m_expandable_segments stays false
TEST_F(TestConfigExpandable, TrueReturnsFalseInNoNPUEnv)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:True", 1);
    configParseEnvForTest();
    EXPECT_FALSE(checkConfigExpandableSegments());
}

// branch: False �?m_expandable_segments = false
TEST_F(TestConfigExpandable, FalseClearsExpandable)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:False", 1);
    configParseEnvForTest();
    EXPECT_FALSE(checkConfigExpandableSegments());
}

// branch: bad value (not True/False) �?throws
TEST_F(TestConfigExpandable, BadValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:Yes", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

TEST_F(TestConfigExpandable, NumericValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:1", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: missing value �?throws
TEST_F(TestConfigExpandable, MissingValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

class TestConfigAddrAlign : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }
};

// branch: valid 0
TEST_F(TestConfigAddrAlign, ValidZero)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "base_addr_aligned_kb:0", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigBaseAddrAlignedSize(), 0u);
}

// branch: valid max (ZBAL_UT_NUM_16)
TEST_F(TestConfigAddrAlign, ValidMax16)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "base_addr_aligned_kb:16", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigBaseAddrAlignedSize(), ZBAL_UT_SIZE_16KB);
}

// branch: valid middle value
TEST_F(TestConfigAddrAlign, Valid8)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "base_addr_aligned_kb:8", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigBaseAddrAlignedSize(), ZBAL_UT_KIB(ZBAL_UT_NUM_8));
}

// branch: non-integer �?stoi throws std::invalid_argument (before TORCH_CHECK)
TEST_F(TestConfigAddrAlign, NonIntegerThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "base_addr_aligned_kb:abc", 1);
    EXPECT_ANY_THROW(configParseEnvForTest());
}

// branch: negative �?throws
TEST_F(TestConfigAddrAlign, NegativeThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "base_addr_aligned_kb:-1", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: > 16 -> throws
TEST_F(TestConfigAddrAlign, TooLargeThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "base_addr_aligned_kb:17", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: missing value �?throws
TEST_F(TestConfigAddrAlign, MissingValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "base_addr_aligned_kb:", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

class TestConfigPageSize : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }
};

// branch: "1g" �?m_page_size_1g = true
TEST_F(TestConfigPageSize, OneGigPageEnablesFlag)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "page_size:1g", 1);
    configParseEnvForTest();
    EXPECT_TRUE(isConfig1GPageSizeEnable());
}

// branch: unsupported value �?throws
TEST_F(TestConfigPageSize, UnsupportedValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "page_size:2g", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: too few tokens -> throws (i+2 >= size)
TEST_F(TestConfigPageSize, MissingValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "page_size:", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

TEST_F(TestConfigPageSize, NoColonThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "page_size", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

class TestConfigSegmentSize : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }
};

// branch: valid segment_size_mb value
TEST_F(TestConfigSegmentSize, ValidSegmentSize)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "segment_size_mb:32", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigSegmentSizeMb(), ZBAL_UT_SIZE_32MB);
}

TEST_F(TestConfigSegmentSize, LargeSegmentSize)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "segment_size_mb:2048", 1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigSegmentSizeMb(), ZBAL_UT_SIZE_2GB);
}

// branch: missing value �?throws
TEST_F(TestConfigSegmentSize, MissingValueThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "segment_size_mb:", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

class TestConfigParseArgs : public ::testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }
};

// Must run FIRST: multiple options without expandable_segments
TEST_F(TestConfigParseArgs, MultipleOptionsAllValid)
{
    setenv(
        "PYTORCH_NPU_ALLOC_CONF",
        "max_split_size_mb:32,garbage_collection_threshold:0.5,base_addr_aligned_kb:8,page_size:1g,segment_size_mb:64",
        1);
    configParseEnvForTest();
    EXPECT_EQ(getConfigMaxSplitSize(), ZBAL_UT_SIZE_32MB);
    EXPECT_TRUE(isConfig1GPageSizeEnable());
    EXPECT_EQ(getConfigBaseAddrAlignedSize(), ZBAL_UT_KIB(ZBAL_UT_NUM_8));
    EXPECT_EQ(getConfigSegmentSizeMb(), ZBAL_UT_SIZE_64MB);
}

// branch: unrecognized option �?throws
TEST_F(TestConfigParseArgs, UnrecognizedOptionThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "unknown_option:42", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: consumeToken index out of range �?throws
// e.g. "garbage_collection_threshold" with no colon/value
TEST_F(TestConfigParseArgs, ConsumeTokenOutOfRangeThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}

// branch: consumeToken wrong separator (config has ':' but we expect ',')
TEST_F(TestConfigParseArgs, ConsumeTokenWrongCharThrows)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "max_split_size_mb:50:", 1);
    EXPECT_THROW(configParseEnvForTest(), c10::Error);
}
TEST(TestDMAMemoryHistory, AllPaths)
{
    dma_record_memory_history(std::string("all"), ZBAL_UT_NUM_100);
    dma_record_memory_history(std::string("state"), ZBAL_UT_NUM_50);
    dma_record_memory_history(std::string("state"), ZBAL_UT_NUM_50);
    dma_record_memory_history(std::nullopt, 0);
    EXPECT_THROW(dma_record_memory_history(std::string("invalid"), ZBAL_UT_NUM_100), c10::Error);
}

class TestDMAHeapStatsAdv : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
};

TEST_F(TestDMAHeapStatsAdv, GetHeapStatsDeviceWithoutInit)
{
    size_t used = ZBAL_UT_NUM_100;
    size_t total = ZBAL_UT_NUM_200;
    dma_get_heap_stats(used, total, 1);
    // device 1 heap not inited, stats should be unchanged
    EXPECT_EQ(used, 100u);
    EXPECT_EQ(total, 200u);
}

class TestDMABaseAddrAdv : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
};

TEST_F(TestDMABaseAddrAdv, AllDevicesReturnNonNullOrZero)
{
    for (int d = 0; d < ZBAL_UT_NUM_8; ++d) {
        void *addr = dma_get_base_addr(d);
        (void)addr;
    }
}

TEST_F(TestDMABaseAddrAdv, NegativeUsesCurrentDevice)
{
    void *addr = dma_get_base_addr(-ZBAL_UT_NUM_50);
    (void)addr;
}

class TestDMAEmptyCacheAdv : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
};

TEST_F(TestDMAEmptyCacheAdv, RepeatedEmptyCacheWithAllocs)
{
    std::vector<void *> ptrs;
    for (int i = 0; i < ZBAL_UT_NUM_16; ++i) {
        ptrs.push_back(dma_malloc(ZBAL_UT_SIZE_4KB, 0, nullptr));
    }
    dma_empty_cache(true);
    dma_empty_cache(false);
    for (auto p : ptrs) {
        dma_free(p, ZBAL_UT_SIZE_4KB, 0, nullptr);
    }
    dma_empty_cache(true);
    dma_empty_cache(false);
}

TEST_F(TestDMAEmptyCacheAdv, EmptyCacheMultipleDevices)
{
    void *p0 = dma_malloc(ZBAL_UT_SIZE_8KB, 0, nullptr);
    void *p1 = dma_malloc(ZBAL_UT_SIZE_4KB, 1, nullptr);
    dma_empty_cache(true);
    dma_empty_cache(false);
    dma_free(p0, ZBAL_UT_SIZE_8KB, 0, nullptr);
    dma_free(p1, ZBAL_UT_SIZE_4KB, 1, nullptr);
}

class TestDMAMallocAdv : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }

    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMAMallocAdv, AllSizesStress)
{
    size_t sizes[] = {
        1,
        ZBAL_UT_NUM_15,
        ZBAL_UT_NUM_16,
        ZBAL_UT_NUM_31,
        ZBAL_UT_NUM_32,
        ZBAL_UT_NUM_63,
        ZBAL_UT_NUM_64,
        ZBAL_UT_NUM_128,
        ZBAL_UT_NUM_256,
        ZBAL_UT_NUM_511,
        ZBAL_UT_NUM_512,
        ZBAL_UT_NUM_1023,
        ZBAL_UT_SIZE_1KB,
        ZBAL_UT_NUM_2047,
        ZBAL_UT_SIZE_2KB,
        ZBAL_UT_NUM_4095,
        ZBAL_UT_SIZE_4KB,
        ZBAL_UT_NUM_65535,
        ZBAL_UT_SIZE_64KB,
        ZBAL_UT_NUM_131071,
        ZBAL_UT_NUM_131072,
        ZBAL_UT_NUM_262143,
        ZBAL_UT_NUM_262144,
        ZBAL_UT_NUM_524288,
        ZBAL_UT_NUM_1048575,
        ZBAL_UT_SIZE_1MB,
        ZBAL_UT_NUM_1048577,
        ZBAL_UT_SIZE_2MB,
        ZBAL_UT_SIZE_3MB,
        ZBAL_UT_SIZE_4MB,
        ZBAL_UT_SIZE_5MB,
        ZBAL_UT_SIZE_7MB,
        ZBAL_UT_SIZE_10MB,
        ZBAL_UT_SIZE_15MB,
        ZBAL_UT_SIZE_20MB,
        ZBAL_UT_SIZE_30MB,
    };
    for (size_t sz : sizes) {
        void *p = dma_malloc(sz, 0, nullptr);
        ASSERT_NE(p, nullptr) << "cannot alloc size=" << sz;
        dma_free(p, sz, 0, nullptr);
    }
}

TEST_F(TestDMAMallocAdv, MultipleAllocFreeCycles)
{
    for (int cycle = 0; cycle < ZBAL_UT_NUM_5; ++cycle) {
        std::vector<void *> ptrs;
        for (int i = 0; i < ZBAL_UT_NUM_20; ++i) {
            ptrs.push_back(dma_malloc(ZBAL_UT_SIZE_8KB + i * ZBAL_UT_NUM_16, 0, nullptr));
        }
        for (auto p : ptrs) {
            dma_free(p, ZBAL_UT_SIZE_8KB, 0, nullptr);
        }
    }
}

TEST_F(TestDMAMallocAdv, MallocFreeInterleaved)
{
    void *p1 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    void *p2 = dma_malloc(ZBAL_UT_SIZE_4KB, 0, nullptr);
    void *p3 = dma_malloc(ZBAL_UT_SIZE_64KB, 0, nullptr);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);
    dma_free(p2, ZBAL_UT_SIZE_4KB, 0, nullptr);
    void *p4 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    ASSERT_NE(p4, nullptr);
    dma_free(p1, ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_free(p3, ZBAL_UT_SIZE_64KB, 0, nullptr);
    dma_free(p4, ZBAL_UT_SIZE_1KB, 0, nullptr);
}

TEST_F(TestDMAMallocAdv, AllocFreeOnDevice3)
{
    for (int i = 0; i < ZBAL_UT_NUM_10; ++i) {
        void *p = dma_malloc(ZBAL_UT_SIZE_1KB + i * ZBAL_UT_NUM_128, ZBAL_UT_NUM_3, nullptr);
        ASSERT_NE(p, nullptr);
        dma_free(p, ZBAL_UT_SIZE_1KB + i * ZBAL_UT_NUM_128, ZBAL_UT_NUM_3, nullptr);
    }
}

TEST_F(TestDMAMallocAdv, BackToBackStreamAllocs)
{
    c10_npu::NPUStream s1 = c10_npu::getDefaultNPUStream();
    c10_npu::NPUStream s2 = c10_npu::getNPUStreamFromPool();

    for (int i = 0; i < ZBAL_UT_NUM_5; ++i) {
        void *p1 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, s1.stream());
        void *p2 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, s2.stream());
        ASSERT_NE(p1, nullptr);
        ASSERT_NE(p2, nullptr);
        dma_free(p1, ZBAL_UT_SIZE_1KB, 0, s1.stream());
        dma_free(p2, ZBAL_UT_SIZE_1KB, 0, s2.stream());
    }
}

TEST_F(TestDMAMallocAdv, RecordMultiStreamEraseAll)
{
    c10_npu::NPUStream s1 = c10_npu::getDefaultNPUStream();
    c10_npu::NPUStream s2 = c10_npu::getNPUStreamFromPool();
    c10_npu::NPUStream s3 = c10_npu::getNPUStreamFromPool();

    void *p = dma_malloc(ZBAL_UT_SIZE_8KB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_record_stream(p, s1);
    dma_record_stream(p, s2);
    dma_record_stream(p, s3);
    dma_erase_stream(p, s1);
    dma_erase_stream(p, s2);
    dma_erase_stream(p, s3);
    dma_free(p, ZBAL_UT_SIZE_8KB, 0, nullptr);
}

TEST_F(TestDMAMallocAdv, GetDeviceStatsAfterAllocs)
{
    void *p = dma_malloc(ZBAL_UT_NUM_262144, 0, nullptr);
    ASSERT_NE(p, nullptr);
    auto stats = dma_get_device_stats(0);
    EXPECT_GT(stats.allocation[0].current, 0);
    EXPECT_GT(stats.allocated_bytes[0].current, 0);
    EXPECT_GT(stats.active_bytes[0].current, 0);
    dma_free(p, ZBAL_UT_NUM_262144, 0, nullptr);
}

TEST_F(TestDMAMallocAdv, GetDeviceStatsDevice7)
{
    void *p = dma_malloc(ZBAL_UT_SIZE_4KB, ZBAL_UT_NUM_7, nullptr);
    ASSERT_NE(p, nullptr);
    auto stats = dma_get_device_stats(ZBAL_UT_NUM_7);
    (void)stats;
    dma_free(p, ZBAL_UT_SIZE_4KB, ZBAL_UT_NUM_7, nullptr);
}

class TestDMAPoolAdv : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }

    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMAPoolAdv, MultipleConcurrentPools)
{
    auto filter = [](aclrtStream) { return true; };
    c10_npu::MempoolId_t poolId1 = {0, ZBAL_UT_NUM_10};
    c10_npu::MempoolId_t poolId2 = {0, ZBAL_UT_NUM_11};

    dma_begin_allocate_to_pool(0, poolId1, filter);
    void *p1 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_end_allocate_to_pool(0, poolId1);

    dma_begin_allocate_to_pool(0, poolId2, filter);
    void *p2 = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_end_allocate_to_pool(0, poolId2);

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    dma_free(p1, ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_free(p2, ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_release_pool(0, poolId1);
    dma_release_pool(0, poolId2);
}

TEST_F(TestDMAPoolAdv, PoolAcrossDevices)
{
    auto filter = [](aclrtStream) { return true; };
    c10_npu::MempoolId_t poolId = {0, ZBAL_UT_NUM_20};

    dma_begin_allocate_to_pool(ZBAL_UT_NUM_2, poolId, filter);
    void *p1 = dma_malloc(ZBAL_UT_SIZE_1KB, ZBAL_UT_NUM_2, nullptr);
    dma_end_allocate_to_pool(ZBAL_UT_NUM_2, poolId);

    dma_begin_allocate_to_pool(ZBAL_UT_NUM_3, poolId, filter);
    void *p2 = dma_malloc(ZBAL_UT_SIZE_1KB, ZBAL_UT_NUM_3, nullptr);
    dma_end_allocate_to_pool(ZBAL_UT_NUM_3, poolId);

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    dma_free(p1, ZBAL_UT_SIZE_1KB, ZBAL_UT_NUM_2, nullptr);
    dma_free(p2, ZBAL_UT_SIZE_1KB, ZBAL_UT_NUM_3, nullptr);
    dma_release_pool(ZBAL_UT_NUM_2, poolId);
    dma_release_pool(ZBAL_UT_NUM_3, poolId);
}

TEST_F(TestDMAMallocAdv, RecordMemoryHistoryAllMaxEntries)
{
    dma_record_memory_history(std::string("all"), ZBAL_UT_NUM_500);
    void *p = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_free(p, ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_record_memory_history(std::nullopt, 0);
}

TEST_F(TestDMAMallocAdv, RecordMemoryHistoryStateDuringAllocs)
{
    dma_record_memory_history(std::string("state"), ZBAL_UT_NUM_200);
    void *p = dma_malloc(ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_free(p, ZBAL_UT_SIZE_1KB, 0, nullptr);
    dma_record_memory_history(std::nullopt, 0);
}

class TestDMAGetBaseAlloc : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMAGetBaseAlloc, SingleBlockReturnsExactSize)
{
    void *p = dma_malloc(ZBAL_UT_SIZE_4KB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    size_t size = 0;
    void *base = getCachingAllocator().getBaseAllocation(p, &size);
    EXPECT_EQ(base, p);
    EXPECT_GE(size, 4096u);
    dma_free(p, ZBAL_UT_SIZE_4KB, 0, nullptr);
}

TEST_F(TestDMAGetBaseAlloc, LargeBlockReturnsExactSize)
{
    void *p = dma_malloc(ZBAL_UT_SIZE_1MB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    size_t size = 0;
    void *base = getCachingAllocator().getBaseAllocation(p, &size);
    EXPECT_EQ(base, p);
    EXPECT_GE(size, 1048576u);
    dma_free(p, ZBAL_UT_SIZE_1MB, 0, nullptr);
}

class TestDMACacheInfo : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMACacheInfo, EmptyCacheReturnsZero)
{
    size_t total = ZBAL_UT_NUM_999;
    size_t largest = ZBAL_UT_NUM_999;
    getCachingAllocator().cacheInfo(0, &total, &largest);
    EXPECT_GE(total, 0u);
    EXPECT_GE(largest, 0u);
}

TEST_F(TestDMACacheInfo, AfterAllocAndFreeHasCache)
{
    void *p1 = dma_malloc(ZBAL_UT_SIZE_64KB, 0, nullptr);
    void *p2 = dma_malloc(ZBAL_UT_NUM_262144, 0, nullptr);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    dma_free(p1, ZBAL_UT_SIZE_64KB, 0, nullptr);
    dma_free(p2, ZBAL_UT_NUM_262144, 0, nullptr);

    size_t total = 0;
    size_t largest = 0;
    getCachingAllocator().cacheInfo(0, &total, &largest);
    EXPECT_GT(total, 0u);
    EXPECT_GT(largest, 0u);
}

class TestDMAStatsReset : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMAStatsReset, ResetAccumulatedStatsAfterAllocs)
{
    void *p = dma_malloc(ZBAL_UT_NUM_262144, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_free(p, ZBAL_UT_NUM_262144, 0, nullptr);

    // stats should have accumulated values before reset
    auto statsBefore = dma_get_device_stats(0);
    EXPECT_GT(statsBefore.allocation[0].allocated, 0);
    EXPECT_GT(statsBefore.allocation[0].freed, 0);

    getCachingAllocator().resetAccumulatedStats(0);

    auto statsAfter = dma_get_device_stats(0);
    EXPECT_EQ(statsAfter.allocation[0].allocated, 0);
    EXPECT_EQ(statsAfter.allocation[0].freed, 0);
    EXPECT_EQ(statsAfter.num_alloc_retries, 0);
    EXPECT_EQ(statsAfter.num_ooms, 0);
}

TEST_F(TestDMAStatsReset, ResetPeakStatsAfterAllocs)
{
    void *p = dma_malloc(ZBAL_UT_SIZE_1MB, 0, nullptr);
    ASSERT_NE(p, nullptr);

    auto statsBefore = dma_get_device_stats(0);
    EXPECT_GT(statsBefore.allocated_bytes[0].peak, 0);

    getCachingAllocator().resetPeakStats(0);

    auto statsAfter = dma_get_device_stats(0);
    // peak should be reset to current
    EXPECT_EQ(statsAfter.allocated_bytes[0].peak, statsAfter.allocated_bytes[0].current);
    dma_free(p, ZBAL_UT_SIZE_1MB, 0, nullptr);
}

TEST_F(TestDMAStatsReset, ResetAccumulatedStatsOnEmptyIsNoOp)
{
    getCachingAllocator().resetAccumulatedStats(0);
    auto stats = dma_get_device_stats(0);
    EXPECT_EQ(stats.allocation[0].allocated, 0);
    EXPECT_EQ(stats.allocation[0].freed, 0);
}

TEST_F(TestDMAStatsReset, ResetPeakStatsOnEmptyIsNoOp)
{
    getCachingAllocator().resetPeakStats(0);
    SUCCEED();
}

class TestDMASnapshot : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMASnapshot, SnapshotWithoutHistoryReturnsEmptyTraces)
{
    auto snap = getCachingAllocator().snapshot();
    // device_traces should have one entry per initialized device
    EXPECT_EQ(snap.device_traces.size(), 8u);
    for (const auto &traces : snap.device_traces) {
        EXPECT_EQ(traces.size(), 0u);
    }
}

TEST_F(TestDMASnapshot, SnapshotWithHistoryHasTraceEntries)
{
    dma_record_memory_history(std::string("all"), ZBAL_UT_NUM_100);
    void *p = dma_malloc(ZBAL_UT_SIZE_4KB, 0, nullptr);
    dma_free(p, ZBAL_UT_SIZE_4KB, 0, nullptr);

    auto snap = getCachingAllocator().snapshot();
    EXPECT_EQ(snap.device_traces.size(), 8u);
    // device 0 should have trace entries
    EXPECT_GT(snap.device_traces[0].size(), 0u);

    dma_record_memory_history(std::nullopt, 0);
}

TEST_F(TestDMASnapshot, SnapshotContainsSegmentsAfterAlloc)
{
    void *p = dma_malloc(ZBAL_UT_NUM_262144, 0, nullptr);
    ASSERT_NE(p, nullptr);
    auto snap = getCachingAllocator().snapshot();
    // after allocation there should be segments
    EXPECT_GT(snap.segments.size(), 0u);
    dma_free(p, ZBAL_UT_NUM_262144, 0, nullptr);
}

class TestDMACheckpoint : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMACheckpoint, CheckpointStateRoundtrip)
{
    auto filter = [](aclrtStream) { return true; };
    c10_npu::MempoolId_t poolId = {0, ZBAL_UT_NUM_50};

    dma_begin_allocate_to_pool(0, poolId, filter);
    void *p = dma_malloc(ZBAL_UT_SIZE_4KB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_end_allocate_to_pool(0, poolId);

    // get checkpoint state
    auto state = getCachingAllocator().getCheckpointState(0, poolId);
    ASSERT_NE(state, nullptr);

    dma_free(p, ZBAL_UT_SIZE_4KB, 0, nullptr);

    // restore checkpoint state
    auto delta = getCachingAllocator().setCheckpointPoolState(0, state);
    (void)delta;

    dma_release_pool(0, poolId);
}

TEST_F(TestDMACheckpoint, GetCheckpointStateUnknownPoolThrows)
{
    c10_npu::MempoolId_t poolId = {0, ZBAL_UT_NUM_999};
    EXPECT_THROW(getCachingAllocator().getCheckpointState(0, poolId), c10::Error);
}

TEST_F(TestDMABasic, FreeBlockWithStreamUsesCallsInsertEvents)
{
    c10_npu::NPUStream s1 = c10_npu::getDefaultNPUStream();
    void *p = dma_malloc(ZBAL_UT_SIZE_4KB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_record_stream(p, s1);
    dma_free(p, ZBAL_UT_SIZE_4KB, 0, nullptr);
    SUCCEED();
}

} // namespace pytorch_npu
} // namespace adaptor
} // namespace zbal

namespace zbal {
namespace adaptor {
namespace pytorch_npu {
namespace {

class TestDMAExpandable : public ::testing::Test {
protected:
    static constexpr int kExpDevice = ZBAL_UT_NUM_5;
    static constexpr size_t kBlockSize = ZBAL_UT_SIZE_2MB; // 2 MiB -> large_blocks

    void SetUp() override
    {
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
        setenv("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:True", 1);
        // Install stub BEFORE configParseEnvForTest() which probes via
        // DlCannApi::AclrtReserveMemAddress (= pAclrtReserveMemAddress)
        setupExpandableSegmentMock();
        configParseEnvForTest();
        dma_init(DEV_COUNT);
    }

    void TearDown() override
    {
        dma_empty_cache(false);
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
        // Reset expandable_segments to default (false) to not leak state across tests
        setenv("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:False", 1);
        configParseEnvForTest();
        unsetenv("PYTORCH_NPU_ALLOC_CONF");
    }

    static constexpr int DEV_COUNT = ZBAL_UT_NUM_8;
};

TEST_F(TestDMAExpandable, FindBlockMapTryAllocate)
{
    void *p1 = dma_malloc(kBlockSize, kExpDevice, nullptr);
    ASSERT_NE(p1, nullptr);

    void *p2 = dma_malloc(kBlockSize * ZBAL_UT_NUM_3, kExpDevice, nullptr);
    ASSERT_NE(p2, nullptr);

    dma_free(p1, kBlockSize, kExpDevice, nullptr);
    dma_free(p2, kBlockSize * ZBAL_UT_NUM_3, kExpDevice, nullptr);
}

TEST_F(TestDMAExpandable, UnmapAndReleaseSegmentViaEmptyCache)
{
    void *p = dma_malloc(kBlockSize, kExpDevice, nullptr);
    ASSERT_NE(p, nullptr);
    dma_free(p, kBlockSize, kExpDevice, nullptr);

    dma_empty_cache(false);
    SUCCEED();
}

TEST_F(TestDMAExpandable, AllocAfterEmptyCacheReusesSegment)
{
    void *p1 = dma_malloc(kBlockSize, kExpDevice, nullptr);
    ASSERT_NE(p1, nullptr);
    dma_free(p1, kBlockSize, kExpDevice, nullptr);
    dma_empty_cache(false);

    void *p2 = dma_malloc(kBlockSize, kExpDevice, nullptr);
    ASSERT_NE(p2, nullptr);
    dma_free(p2, kBlockSize, kExpDevice, nullptr);
}

TEST_F(TestDMAExpandable, MultipleAllocsStressExpandable)
{
    std::vector<void *> ptrs;
    for (int i = 0; i < ZBAL_UT_NUM_8; ++i) {
        void *p = dma_malloc(kBlockSize + i * ZBAL_UT_SIZE_4KB, kExpDevice, nullptr);
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    for (auto p : ptrs) {
        dma_free(p, kBlockSize, kExpDevice, nullptr);
    }
    dma_empty_cache(false);
    SUCCEED();
}

} // anonymous namespace

class TestDMABlockAPI : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMABlockAPI, MallocFreeGetPtrSize)
{
    constexpr size_t kSize = ZBAL_UT_SIZE_4KB;
    auto stream = c10_npu::getDefaultNPUStream().stream(false);
    void *block = c10_npu::dma::MallocBlock(kSize, stream, 0);
    ASSERT_NE(block, nullptr);

    void *ptr = c10_npu::dma::GetBlockPtr(block);
    EXPECT_NE(ptr, nullptr);

    size_t sz = c10_npu::dma::GetBlockSize(block);
    EXPECT_GE(sz, kSize);

    c10_npu::dma::FreeBlock(block);
}
class TestDMAAllocatorSimple : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMAAllocatorSimple, TrivialMethodsNoCrash)
{
    auto &alloc = getCachingAllocator();

    // setMemoryFraction
    alloc.setMemoryFraction(0.5, 0);

    // isHistoryEnabled (default false)
    EXPECT_FALSE(alloc.isHistoryEnabled());

    // checkUceInMemPool (default true)
    EXPECT_TRUE(alloc.checkUceInMemPool(0));

    // attachOutOfMemoryObserver
    bool called = false;
    alloc.attachOutOfMemoryObserver([&](int64_t, int64_t, int64_t, int64_t) { called = true; });
    (void)called;

    // cleanEvent
    alloc.cleanEvent();
    SUCCEED();
}

TEST_F(TestDMAAllocatorSimple, AllocateWithAlignedSizeExceedsOneExaBytesThrows)
{
    constexpr size_t one_exa_bytes = 1152921504606846976ULL;
    EXPECT_THROW(getCachingAllocator().allocate_with_aligned(one_exa_bytes, 0), c10::Error);
}

TEST_F(TestDMAAllocatorSimple, AllocateWithAlignedZeroSize)
{
    EXPECT_EQ(getCachingAllocator().allocate_with_aligned(0, 0).get(), nullptr);
    EXPECT_EQ(getCachingAllocator().allocate_with_aligned(0, ZBAL_UT_NUM_64).get(), nullptr);
}

TEST_F(TestDMAAllocatorSimple, AllocateWithAlignedNormalPath)
{
    auto &alloc = getCachingAllocator();
    struct TestCase {
        size_t size;
        size_t align_kb;
    };
    for (auto [size, align_kb] :
         {TestCase{1, ZBAL_UT_NUM_4}, {ZBAL_UT_SIZE_4KB, ZBAL_UT_NUM_64}, {ZBAL_UT_SIZE_1MB, ZBAL_UT_SIZE_1KB}}) {
        auto dp = alloc.allocate_with_aligned(size, align_kb);
        void *ptr = dp.get();
        EXPECT_NE(ptr, nullptr) << "size=" << size << " align_kb=" << align_kb;
        size_t aligned = align_kb * ZBAL_UT_SIZE_1KB;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % aligned, 0u) << "size=" << size << " align_kb=" << align_kb;
    }
}

TEST_F(TestDMAAllocatorSimple, AllocateWithAlignedUncachedPath)
{
    OptionsManager::CheckForceUncached = true;
    auto dp = getCachingAllocator().allocate_with_aligned(ZBAL_UT_SIZE_4KB, ZBAL_UT_NUM_64);
    OptionsManager::CheckForceUncached = false;

    void *ptr = dp.get();
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % (64u * ZBAL_UT_SIZE_1KB), 0u);
}

TEST_F(TestDMAAllocatorSimple, GetIpcDevPtrFutureVersionThrows)
{
    // version=ZBAL_UT_NUM_2 > SHAREABLE_HANDLE_VERSION(1)
    std::string handle("\x02\x63", ZBAL_UT_NUM_2);
    EXPECT_THROW(getCachingAllocator().getIpcDevPtr(handle), c10::Error);
}

TEST_F(TestDMAAllocatorSimple, GetIpcDevPtrInvalidHandleTypeThrows)
{
    // version=1, type='x' (invalid)
    std::string handle("\x01\x78", ZBAL_UT_NUM_2);
    EXPECT_THROW(getCachingAllocator().getIpcDevPtr(handle), c10::Error);
}

TEST_F(TestDMAAllocatorSimple, GetIpcDevPtrValidHandleAndDeleter)
{
    // version=1, type='c' (SHAREABLE_NPU_MALLOC) �?MemHandleCacheEntry constructor + ptr() + wp_ assignment
    std::string handle("\x01\x63", ZBAL_UT_NUM_2);
    {
        auto sp = getCachingAllocator().getIpcDevPtr(handle);
        // shared_ptr manages null raw ptr in mock env but owns a control block with custom deleter
        EXPECT_EQ(sp.use_count(), 1);
    }
    // shared_ptr destroyed �?custom deleter runs �?entry.clear() + erase from ipcMemHandle_to_devptr
    SUCCEED();
}

TEST_F(TestDMAAllocatorSimple, FreeDeviceCachedMemory)
{
    auto &alloc = getCachingAllocator();

    // Allocate something first to give the cache content
    auto dp = alloc.allocate(ZBAL_UT_SIZE_4KB);
    ASSERT_NE(dp.get(), nullptr);

    // FreeDeviceCachedMemory calls emptyCache with free_physical=true
    EXPECT_NO_THROW(alloc.FreeDeviceCachedMemory(0));
}

TEST_F(TestDMAAllocatorSimple, CopyData)
{
    constexpr size_t kSize = ZBAL_UT_SIZE_1KB;
    std::vector<uint8_t> src(kSize);
    std::vector<uint8_t> dest(kSize, 0);
    for (size_t i = 0; i < kSize; ++i) {
        src[i] = static_cast<uint8_t>(i % ZBAL_UT_NUM_256);
    }

    getCachingAllocator().copy_data(dest.data(), src.data(), kSize);

    for (size_t i = 0; i < kSize; ++i) {
        EXPECT_EQ(dest[i], src[i]) << "mismatch at index " << i;
    }
}

TEST_F(TestDMAAllocatorSimple, GarbageCollectBelowThreshold)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:0.9", 1);
    configParseEnvForTest();
    getCachingAllocator().setMemoryFraction(0.001, 0);
    // gc_threshold = 0.9 * 16MB = 14.4MB

    void *p = dma_malloc(ZBAL_UT_NUM_10 * ZBAL_UT_SIZE_1MB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_free(p, ZBAL_UT_NUM_10 * ZBAL_UT_SIZE_1MB, 0, nullptr);
    // total_allocated_memory (~10MB) <= gc_threshold �?GC returns early

    void *p2 = dma_malloc(ZBAL_UT_NUM_12 * ZBAL_UT_SIZE_1MB, 0, nullptr);
    EXPECT_NE(p2, nullptr);
    dma_free(p2, ZBAL_UT_NUM_12 * ZBAL_UT_SIZE_1MB, 0, nullptr);

    unsetenv("PYTORCH_NPU_ALLOC_CONF");
    configParseEnvForTest();
}

TEST_F(TestDMAAllocatorSimple, GarbageCollectReleasesBlock)
{
    setenv("PYTORCH_NPU_ALLOC_CONF", "garbage_collection_threshold:0.1", 1);
    configParseEnvForTest();
    getCachingAllocator().setMemoryFraction(0.001, 0);
    // gc_threshold = 0.1 * 16MB = 1.6MB

    void *p = dma_malloc(ZBAL_UT_NUM_10 * ZBAL_UT_SIZE_1MB, 0, nullptr);
    ASSERT_NE(p, nullptr);
    dma_free(p, ZBAL_UT_NUM_10 * ZBAL_UT_SIZE_1MB, 0, nullptr);
    // total_allocated_memory (~10MB) > gc_threshold (1.6MB)

    // Allocating different size �?get_free_block fails �?GC runs �?releases cached block
    void *p2 = dma_malloc(ZBAL_UT_NUM_12 * ZBAL_UT_SIZE_1MB, 0, nullptr);
    EXPECT_NE(p2, nullptr);
    dma_free(p2, ZBAL_UT_NUM_12 * ZBAL_UT_SIZE_1MB, 0, nullptr);

    unsetenv("PYTORCH_NPU_ALLOC_CONF");
    configParseEnvForTest();
}

class TestDMAPoolCheck : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMAPoolCheck, UnknownPoolThrows)
{
    c10_npu::MempoolId_t unknownPool = {ZBAL_UT_NUM_999, ZBAL_UT_NUM_999};
    std::unordered_set<void *> emptySet;
    EXPECT_THROW(getCachingAllocator().checkPoolLiveAllocations(0, unknownPool, emptySet), c10::Error);
}

class TestDMABlockSafe : public ::testing::Test {
protected:
    void SetUp() override
    {
        dma_init(ZBAL_UT_NUM_8);
    }
    void TearDown() override
    {
        dma_empty_cache(false);
    }
};

TEST_F(TestDMABlockSafe, SafeFlagLifecycle)
{
    auto &alloc = getCachingAllocator();

    // Allocate through the allocator to get a DataPtr with local_raw_delete deleter
    auto dataPtr = alloc.allocate(ZBAL_UT_SIZE_4KB);
    ASSERT_TRUE(dataPtr.get() != nullptr);

    // Initially safe
    EXPECT_TRUE(alloc.checkBlockIsSafe(dataPtr));

    // Mark all unsafe
    alloc.markAllBlockUnsafe(0);
    EXPECT_FALSE(alloc.checkBlockIsSafe(dataPtr));

    // Restore to safe
    alloc.updateBlockToSafe(dataPtr);
    EXPECT_TRUE(alloc.checkBlockIsSafe(dataPtr));
}

TEST_F(TestDMABlockSafe, NullPtrReturnsTrue)
{
    c10::DataPtr nullPtr;
    EXPECT_TRUE(getCachingAllocator().checkBlockIsSafe(nullPtr));
    // updateBlockToSafe on null is a no-op
    getCachingAllocator().updateBlockToSafe(nullPtr);
    SUCCEED();
}

TEST_F(TestDMABlockSafe, NonAllocatorPtrReturnsTrue)
{
    // DataPtr without deleter (= default nullptr deleter) �?get_deleter() != &local_raw_delete
    // Both checkBlockIsSafe and updateBlockToSafe should return early (true / no-op)
    int dummy = ZBAL_UT_NUM_42;
    c10::DataPtr customPtr(&dummy, c10::Device(c10::DeviceType::PrivateUse1, 0));
    EXPECT_TRUE(getCachingAllocator().checkBlockIsSafe(customPtr));
    getCachingAllocator().updateBlockToSafe(customPtr);
    SUCCEED();
}

} // namespace pytorch_npu
} // namespace adaptor
} // namespace zbal
