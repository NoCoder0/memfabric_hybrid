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
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include <thread>
#include <set>
#include "smem_net_group_engine.h"
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include "hybm_big_mem.h"
#include "acc_offload_launch.h"
#include "dl_acl_api.h"

#define private public
#include "acc_offload_local_dram_entry.h"
#include "acc_offload_shared_dram_entry.h"
#undef private

using namespace ock::offload;
using namespace ock::mf;
using namespace ock::smem;

namespace {
constexpr uint64_t GB = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t POOL_SIZE = GB;
constexpr size_t BACKING_BYTES = 64 * 1024;
constexpr uint32_t TEST_DEVICE = 0;
constexpr uint64_t GVA_DVA = 0x1234abcd00ULL;
constexpr uint64_t BLOCK_BYTES = 16;
constexpr size_t WORKER_COUNT = 4;
constexpr size_t BLOCKS_PER_WORKER = 128;
constexpr size_t EXPORT_DESC_BYTES = 8;
constexpr char SHARED_STORE_URL[] = "tcp://127.0.0.1:19871";
constexpr char MMAP_FAILURE_STORE_URL[] = "tcp://127.0.0.1:19872";
constexpr char MALFORMED_STORE_URL[] = "not-a-url";

alignas(64) std::array<std::array<uint8_t, BACKING_BYTES>, HYBM_ENTITY_ID_SEGMENT_SIZE> gPools;
std::vector<uint16_t> gCreatedIds;
std::vector<hybm_entity_t> gDestroyed;
std::vector<hybm_mem_slice_t> gFreedSlices;
std::vector<uint64_t> gAllocSizes;
hybm_options gLastOptions{};
uint32_t gInitCount = 0;
uint32_t gUninitCount = 0;
uint32_t gCleanupCount = 0;
int32_t gInitResult = OFFLOAD_OK;
int32_t gLaunchResult = OFFLOAD_OK;
int32_t gReserveResult = OFFLOAD_OK;
int32_t gGvaResult = OFFLOAD_OK;
bool gEntityNull = false;
bool gAllocNull = false;
bool gSliceVaNull = false;
uint32_t gExportCount = 0;
uint32_t gEntityExportCount = 0;
uint32_t gMmapCount = 0;
int32_t gMmapResult = OFFLOAD_OK;

int32_t InitHybm(uint16_t, uint64_t)
{
    ++gInitCount;
    return gInitResult;
}

void UninitHybm()
{
    ++gUninitCount;
}

int32_t LoadLaunch()
{
    return gLaunchResult;
}

void CleanupLaunch()
{
    ++gCleanupCount;
}

AscendSocType GetSocType()
{
    return ASCEND_910C;
}

hybm_entity_t CreateEntity(uint16_t id, const hybm_options *options, uint32_t)
{
    gCreatedIds.push_back(id);
    if (options != nullptr) {
        gLastOptions = *options;
    }
    if (gEntityNull) {
        return nullptr;
    }
    return gPools.at(id - HYBM_ENTITY_ID_OFFLOAD_BASE).data();
}

void DestroyEntity(hybm_entity_t entity, uint32_t)
{
    gDestroyed.push_back(entity);
}

int32_t ReserveMemSpace(hybm_entity_t, uint32_t)
{
    return gReserveResult;
}

int32_t UnreserveMemSpace(hybm_entity_t, uint32_t)
{
    return OFFLOAD_OK;
}

hybm_mem_slice_t AllocSlice(hybm_entity_t entity, hybm_mem_type, uint64_t size, uint32_t)
{
    gAllocSizes.push_back(size);
    return gAllocNull ? nullptr : entity;
}

void *SliceVa(hybm_entity_t entity, hybm_mem_slice_t, uint32_t)
{
    return gSliceVaNull ? nullptr : entity;
}

int32_t FreeSlice(hybm_entity_t, hybm_mem_slice_t slice, uint32_t, uint32_t)
{
    gFreedSlices.push_back(slice);
    return OFFLOAD_OK;
}

int32_t GvaToVa(uint64_t, hybm_mem_type, uint64_t *va)
{
    if (va == nullptr) {
        return OFFLOAD_ERROR;
    }
    if (gGvaResult != OFFLOAD_OK) {
        return gGvaResult;
    }
    *va = GVA_DVA;
    return OFFLOAD_OK;
}

void *GetMemoryPtr(hybm_entity_t, hybm_mem_type)
{
    return gPools.front().data();
}

int32_t ExportSlice(hybm_entity_t, hybm_mem_slice_t, uint32_t flags, hybm_exchange_info *info)
{
    if ((flags & HYBM_FLAG_EXPORT_ENTITY) != 0U) {
        ++gEntityExportCount;
    } else {
        ++gExportCount;
    }
    if (info == nullptr) {
        return OFFLOAD_ERROR;
    }
    // hybm_export_info_free owns and deletes the exported descriptor.
    info->desc = new (std::nothrow) uint8_t[EXPORT_DESC_BYTES]{};
    if (info->desc == nullptr) {
        return OFFLOAD_ERROR;
    }
    info->descLen = EXPORT_DESC_BYTES;
    return OFFLOAD_OK;
}

int32_t MmapEntity(hybm_entity_t, uint32_t)
{
    ++gMmapCount;
    return gMmapResult;
}

bool StoreEnvSet()
{
    const char *primary = getenv("MF_CONFIG_STORE_URL");
    const char *fallback = getenv("ASCEND_MF_STORE_URL");
    return (primary != nullptr && primary[0] != '\0') || (fallback != nullptr && fallback[0] != '\0');
}
} // namespace

class AccOffloadDramEntryTest : public testing::Test {
protected:
    void SetUp() override
    {
        gCreatedIds.clear();
        gDestroyed.clear();
        gFreedSlices.clear();
        gAllocSizes.clear();
        gLastOptions = {};
        gInitCount = 0;
        gUninitCount = 0;
        gCleanupCount = 0;
        gInitResult = OFFLOAD_OK;
        gLaunchResult = OFFLOAD_OK;
        gReserveResult = OFFLOAD_OK;
        gGvaResult = OFFLOAD_OK;
        gEntityNull = false;
        gAllocNull = false;
        gSliceVaNull = false;
        gExportCount = 0;
        gEntityExportCount = 0;
        gMmapCount = 0;
        gMmapResult = OFFLOAD_OK;
        config_ = {};
        config_.deviceId = TEST_DEVICE;
        config_.reserveSize = POOL_SIZE;
        config_.allocSize = POOL_SIZE;
        config_.worldSize = 1;
        config_.scene = OFFLOAD_SCENE_LOCAL;
        MOCKER(hybm_init).stubs().will(invoke(InitHybm));
        MOCKER(hybm_uninit).stubs().will(invoke(UninitHybm));
        MOCKER(AccOffloadLaunchApi::TryLoadLibrary).stubs().will(invoke(LoadLaunch));
        MOCKER(AccOffloadLaunchApi::CleanupLibrary).stubs().will(invoke(CleanupLaunch));
        MOCKER(DlAclApi::GetAscendSocType).stubs().will(invoke(GetSocType));
        MOCKER(hybm_create_entity).stubs().will(invoke(CreateEntity));
        MOCKER(hybm_destroy_entity).stubs().will(invoke(DestroyEntity));
        MOCKER(hybm_reserve_mem_space).stubs().will(invoke(ReserveMemSpace));
        MOCKER(hybm_unreserve_mem_space).stubs().will(invoke(UnreserveMemSpace));
        MOCKER(hybm_alloc_local_memory).stubs().will(invoke(AllocSlice));
        MOCKER(hybm_get_slice_va).stubs().will(invoke(SliceVa));
        MOCKER(hybm_free_local_memory).stubs().will(invoke(FreeSlice));
        MOCKER(hybm_gva_to_va).stubs().will(invoke(GvaToVa));
        MOCKER(hybm_get_memory_ptr).stubs().will(invoke(GetMemoryPtr));
        MOCKER(hybm_export).stubs().will(invoke(ExportSlice));
        MOCKER(hybm_mmap).stubs().will(invoke(MmapEntity));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    AccOffloadLocalDramEntry MakeEntry()
    {
        return AccOffloadLocalDramEntry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    }

    static uint64_t PoolBase(const AccOffloadLocalDramEntry &entry)
    {
        return reinterpret_cast<uint64_t>(entry.base_);
    }

    offload_config_t config_{};
};

TEST_F(AccOffloadDramEntryTest, ValidateRejectsBadSharedGeometry)
{
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    config_.scene = OFFLOAD_SCENE_SHARED;
    config_.worldSize = 0;
    EXPECT_NE(entry.ValidateSharedConfig(config_), OFFLOAD_OK);
    config_.worldSize = 4;
    config_.rankId = 4;
    EXPECT_NE(entry.ValidateSharedConfig(config_), OFFLOAD_OK);
    config_.rankId = 0;
    config_.localWorldSize = 8;
    EXPECT_NE(entry.ValidateSharedConfig(config_), OFFLOAD_OK);
    config_.localWorldSize = 3;
    EXPECT_NE(entry.ValidateSharedConfig(config_), OFFLOAD_OK);
    config_.localWorldSize = 2;
    EXPECT_EQ(entry.ValidateSharedConfig(config_), OFFLOAD_OK);
}

TEST_F(AccOffloadDramEntryTest, ResolveStoreUrlPrefersExplicitConfig)
{
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    config_.storeUrl[0] = '\0';
    const char url[] = "tcp://127.0.0.1:19999";
    snprintf(config_.storeUrl, sizeof(config_.storeUrl), "%s", url);
    EXPECT_EQ(entry.ResolveStoreUrl(config_), OFFLOAD_OK);
    EXPECT_EQ(entry.storeUrl_, url);
}

TEST_F(AccOffloadDramEntryTest, ResolveStoreUrlDerivesLoopbackPortPerDeviceGroup)
{
    if (StoreEnvSet()) {
        GTEST_SKIP() << "store url env is set, derived-port branch is unreachable";
    }
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    config_.worldSize = 4;
    config_.deviceId = 5;
    EXPECT_EQ(entry.ResolveStoreUrl(config_), OFFLOAD_OK);
    EXPECT_EQ(entry.storeUrl_, "tcp://127.0.0.1:8501");
    config_.deviceId = 0;
    entry.storeUrl_.clear();
    EXPECT_EQ(entry.ResolveStoreUrl(config_), OFFLOAD_OK);
    EXPECT_EQ(entry.storeUrl_, "tcp://127.0.0.1:8500");
}

TEST_F(AccOffloadDramEntryTest, ResolveStoreUrlRejectsMultiMachineWithoutExplicitStore)
{
    if (StoreEnvSet()) {
        GTEST_SKIP() << "store url env is set, multi-machine rejection is unreachable";
    }
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    config_.worldSize = 8;
    config_.localWorldSize = 4;
    EXPECT_NE(entry.ResolveStoreUrl(config_), OFFLOAD_OK);
}

TEST_F(AccOffloadDramEntryTest, InitializePreservesLocalGiantPagePolicy)
{
    auto entry = MakeEntry();
    config_.flags |= OFFLOAD_FLAG_GIANT_PAGE;
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_TRUE(entry.inited_);
    ASSERT_EQ(gAllocSizes.size(), 1U);
    EXPECT_EQ(gAllocSizes.front(), POOL_SIZE);
    EXPECT_EQ(gLastOptions.maxDRAMSize, POOL_SIZE);
    EXPECT_EQ(gLastOptions.hostVASpace, POOL_SIZE);
    EXPECT_EQ(gLastOptions.rankCount, 1U);
    EXPECT_EQ(gLastOptions.memType, HYBM_MEM_TYPE_HOST);
    EXPECT_NE(gLastOptions.flags & HYBM_FLAG_DRAM_MAP_HOST_VA, 0U);
    EXPECT_NE(gLastOptions.flags & HYBM_FLAG_OFFLOAD_GIANT_PAGE, 0U);
    EXPECT_EQ(gLastOptions.flags & HYBM_FLAG_UNRESTRICTED_MEM, 0U);
    EXPECT_NE(entry.base_, nullptr);
    EXPECT_NE(entry.memMng_, nullptr);
}

TEST_F(AccOffloadDramEntryTest, InitializeTwiceIsIdempotent)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gInitCount, 1U);
}

TEST_F(AccOffloadDramEntryTest, FailedHybmInitReleasesNothing)
{
    auto entry = MakeEntry();
    gInitResult = OFFLOAD_ERROR;
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gUninitCount, 0U);
    EXPECT_EQ(gCleanupCount, 0U);
    EXPECT_TRUE(gDestroyed.empty());
}

TEST_F(AccOffloadDramEntryTest, FailedCreateEntityReleasesHybmAndLaunch)
{
    auto entry = MakeEntry();
    gEntityNull = true;
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(gCleanupCount, 1U);
    EXPECT_TRUE(gDestroyed.empty());
    EXPECT_TRUE(gFreedSlices.empty());
}

TEST_F(AccOffloadDramEntryTest, FailedReserveReleasesEntityAndHybm)
{
    auto entry = MakeEntry();
    gReserveResult = OFFLOAD_ERROR;
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gDestroyed.size(), 1U);
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(gCleanupCount, 1U);
    EXPECT_TRUE(gFreedSlices.empty());
}

TEST_F(AccOffloadDramEntryTest, FailedPoolAllocReleasesEntityAndHybm)
{
    auto entry = MakeEntry();
    gAllocNull = true;
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gDestroyed.size(), 1U);
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(gCleanupCount, 1U);
    EXPECT_TRUE(gFreedSlices.empty());
    EXPECT_TRUE(gAllocSizes.size() >= 1U);
}

TEST_F(AccOffloadDramEntryTest, FailedSliceVaFreesPoolSliceAndRuntime)
{
    auto entry = MakeEntry();
    gSliceVaNull = true;
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gFreedSlices.size(), 1U);
    EXPECT_EQ(gDestroyed.size(), 1U);
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(gCleanupCount, 1U);
}

TEST_F(AccOffloadDramEntryTest, FailedLaunchKeepsOtherInstancesLibraryLoaded)
{
    auto entry = MakeEntry();
    gLaunchResult = OFFLOAD_ERROR;
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gCleanupCount, 0U);
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_TRUE(gDestroyed.empty());
}

TEST_F(AccOffloadDramEntryTest, ReinitializeAfterUninitializeWorks)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    entry.UnInitialize();
    EXPECT_FALSE(entry.inited_);
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_TRUE(entry.inited_);
    EXPECT_EQ(gInitCount, 2U);
    EXPECT_EQ(gCreatedIds.size(), 2U);
}

TEST_F(AccOffloadDramEntryTest, UninitializeReleasesWholePoolOnceAndIsThenNoop)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto slice = entry.slice_;
    entry.UnInitialize();
    ASSERT_EQ(gFreedSlices.size(), 1U);
    EXPECT_EQ(gFreedSlices.front(), slice);
    EXPECT_EQ(gDestroyed.size(), 1U);
    EXPECT_EQ(gCleanupCount, 1U);
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(entry.memMng_, nullptr);
    entry.UnInitialize();
    EXPECT_EQ(gFreedSlices.size(), 1U);
    EXPECT_EQ(gUninitCount, 1U);
}

TEST_F(AccOffloadDramEntryTest, UninitializeOnFreshEntryTouchesNoRuntime)
{
    auto entry = MakeEntry();
    entry.UnInitialize();
    EXPECT_EQ(gInitCount, 0U);
    EXPECT_EQ(gUninitCount, 0U);
    EXPECT_EQ(gCleanupCount, 0U);
    EXPECT_TRUE(gDestroyed.empty());
    EXPECT_TRUE(gFreedSlices.empty());
}

TEST_F(AccOffloadDramEntryTest, HostMallocRequiresInitAndRejectsZero)
{
    auto entry = MakeEntry();
    EXPECT_EQ(entry.HostMalloc(16, 0), 0U);
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(entry.HostMalloc(0, 0), 0U);
}

TEST_F(AccOffloadDramEntryTest, HostMallocReturnsAlignedTightBump)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto base = PoolBase(entry);
    auto p1 = entry.HostMalloc(1, 0);
    auto p2 = entry.HostMalloc(33, 0);
    auto p3 = entry.HostMalloc(16, 0);
    EXPECT_NE(p1, 0U);
    EXPECT_EQ(p1 % 16, 0U);
    EXPECT_EQ(p2 - p1, 16U);
    EXPECT_EQ(p3 - p1, 64U);
    EXPECT_GE(p1, base);
    EXPECT_LT(p1, base + POOL_SIZE);
}

TEST_F(AccOffloadDramEntryTest, HostMallocExhaustsPoolExactlyAndRecoversAfterFree)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto base = PoolBase(entry);
    auto almostAll = entry.HostMalloc(POOL_SIZE - 16, 0);
    EXPECT_EQ(almostAll, base);
    EXPECT_EQ(entry.HostMalloc(17, 0), 0U);
    EXPECT_EQ(entry.HostMalloc(POOL_SIZE + 1, 0), 0U);
    EXPECT_EQ(entry.HostFree(almostAll, 0), OFFLOAD_OK);
    EXPECT_EQ(entry.HostMalloc(32, 0), base);
}

TEST_F(AccOffloadDramEntryTest, HostMallocFitsExactPoolOnce)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto base = PoolBase(entry);
    EXPECT_EQ(entry.HostMalloc(POOL_SIZE, 0), base);
    EXPECT_EQ(entry.HostMalloc(16, 0), 0U);
}

TEST_F(AccOffloadDramEntryTest, HostFreeRejectsForeignAndDoubleFree)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto base = PoolBase(entry);
    EXPECT_NE(entry.HostFree(0, 0), OFFLOAD_OK);
    EXPECT_NE(entry.HostFree(base + 2 * POOL_SIZE, 0), OFFLOAD_OK);
    auto ptr = entry.HostMalloc(16, 0);
    ASSERT_NE(ptr, 0U);
    EXPECT_EQ(entry.HostFree(ptr, 0), OFFLOAD_OK);
    EXPECT_NE(entry.HostFree(ptr, 0), OFFLOAD_OK);
}

TEST_F(AccOffloadDramEntryTest, HostFreeMergesAdjacentHolesWithoutStitching)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto base = PoolBase(entry);
    auto a = entry.HostMalloc(16, 0);
    auto b = entry.HostMalloc(16, 0);
    auto c = entry.HostMalloc(16, 0);
    auto d = entry.HostMalloc(16, 0);
    ASSERT_EQ(d - a, 48U);
    EXPECT_EQ(entry.HostFree(a, 0), OFFLOAD_OK);
    EXPECT_EQ(entry.HostFree(c, 0), OFFLOAD_OK);
    EXPECT_EQ(entry.HostMalloc(32, 0), base + 64U);
    EXPECT_EQ(entry.HostFree(b, 0), OFFLOAD_OK);
    auto merged = entry.HostMalloc(48, 0);
    EXPECT_EQ(merged, a);
}

TEST_F(AccOffloadDramEntryTest, HostRegisterRequiresInitAndStaysIdempotentInsidePool)
{
    auto entry = MakeEntry();
    EXPECT_NE(entry.HostRegister(0x1000), OFFLOAD_OK);
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto base = PoolBase(entry);
    auto ptr = entry.HostMalloc(16, 0);
    EXPECT_NE(entry.HostRegister(base + 2 * POOL_SIZE), OFFLOAD_OK);
    EXPECT_EQ(entry.HostRegister(ptr), OFFLOAD_OK);
    EXPECT_EQ(entry.HostRegister(ptr), OFFLOAD_OK);
}

TEST_F(AccOffloadDramEntryTest, HostGetDevicePtrTranslatesAnyInPoolAddress)
{
    auto entry = MakeEntry();
    uint64_t dva = 0;
    EXPECT_NE(entry.HostGetDevicePtr(0x1000, &dva), OFFLOAD_OK);
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto base = PoolBase(entry);
    auto ptr = entry.HostMalloc(16, 0);
    EXPECT_NE(entry.HostGetDevicePtr(base + 2 * POOL_SIZE, &dva), OFFLOAD_OK);
    EXPECT_EQ(entry.HostGetDevicePtr(ptr, &dva), OFFLOAD_OK);
    EXPECT_EQ(dva, GVA_DVA);
    gGvaResult = OFFLOAD_ERROR;
    EXPECT_NE(entry.HostGetDevicePtr(ptr, &dva), OFFLOAD_OK);
}

TEST_F(AccOffloadDramEntryTest, InPoolHonorsPoolBounds)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto base = PoolBase(entry);
    EXPECT_EQ(entry.HostRegister(base), OFFLOAD_OK);
    EXPECT_EQ(entry.HostRegister(base + POOL_SIZE - 1), OFFLOAD_OK);
    EXPECT_NE(entry.HostRegister(base + POOL_SIZE), OFFLOAD_OK);
    EXPECT_NE(entry.HostRegister(0), OFFLOAD_OK);
}

TEST_F(AccOffloadDramEntryTest, InitializePreservesCapacityAndDefaultAllocationPolicy)
{
    auto entry = MakeEntry();
    config_.allocSize = 0;
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_TRUE(gCreatedIds.empty());
    config_.allocSize = POOL_SIZE;
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gLastOptions.maxDRAMSize, POOL_SIZE);
    EXPECT_EQ(gLastOptions.hostVASpace, POOL_SIZE);
    EXPECT_NE(gLastOptions.flags & HYBM_FLAG_UNRESTRICTED_MEM, 0U);
    EXPECT_EQ(gLastOptions.flags & HYBM_FLAG_OFFLOAD_GIANT_PAGE, 0U);
}

TEST_F(AccOffloadDramEntryTest, LegacyAndHostMethodsShareTheSamePool)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto ptr = reinterpret_cast<uint64_t>(entry.MallocHost(BLOCK_BYTES));
    ASSERT_NE(ptr, 0U);
    uint64_t dva = 0;
    EXPECT_EQ(entry.HostGetDevicePtr(ptr, &dva), OFFLOAD_OK);
    EXPECT_EQ(dva, GVA_DVA);
    EXPECT_EQ(entry.GetDva(ptr, &dva), OFFLOAD_OK);
    EXPECT_EQ(entry.HostFree(ptr, 0), OFFLOAD_OK);
    EXPECT_EQ(entry.HostMalloc(BLOCK_BYTES, 0), ptr);
    entry.FreeHost(reinterpret_cast<void *>(ptr));
    EXPECT_EQ(entry.HostMalloc(BLOCK_BYTES, 0), ptr);
}

TEST_F(AccOffloadDramEntryTest, PoolOperationsRejectAddressesAfterUninitialize)
{
    auto entry = MakeEntry();
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    auto ptr = entry.HostMalloc(BLOCK_BYTES, 0);
    uint64_t dva = 0;
    EXPECT_NE(entry.GetDva(ptr, nullptr), OFFLOAD_OK);
    EXPECT_NE(entry.GetDva(PoolBase(entry) + POOL_SIZE, &dva), OFFLOAD_OK);
    EXPECT_NE(entry.HostFree(ptr + 1, 0), OFFLOAD_OK);
    entry.UnInitialize();
    EXPECT_EQ(entry.HostMalloc(BLOCK_BYTES, 0), 0U);
    EXPECT_NE(entry.HostRegister(ptr), OFFLOAD_OK);
    EXPECT_NE(entry.HostGetDevicePtr(ptr, &dva), OFFLOAD_OK);
    EXPECT_NE(entry.HostFree(ptr, 0), OFFLOAD_OK);
}

TEST_F(AccOffloadDramEntryTest, SharedCapacityUsesAllocSizeAndRejectsSlotPadding)
{
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    config_.scene = OFFLOAD_SCENE_SHARED;
    config_.reserveSize = 2 * POOL_SIZE;
    config_.allocSize = POOL_SIZE;
    ASSERT_EQ(entry.ValidateSharedConfig(config_), OFFLOAD_OK);
    entry.options_.maxDRAMSize = config_.reserveSize;
    entry.options_.hostVASpace = config_.allocSize;
    ASSERT_EQ(entry.MapPool(config_), OFFLOAD_OK);
    entry.inited_ = true;
    EXPECT_EQ(entry.size_, POOL_SIZE);
    auto base = reinterpret_cast<uint64_t>(entry.base_);
    uint64_t dva = 0;
    EXPECT_EQ(entry.HostMalloc(POOL_SIZE, 0), base);
    EXPECT_EQ(entry.HostMalloc(BLOCK_BYTES, 0), 0U);
    EXPECT_EQ(entry.HostGetDevicePtr(base + POOL_SIZE - 1, &dva), OFFLOAD_OK);
    EXPECT_EQ(dva, base + POOL_SIZE - 1);
    EXPECT_NE(entry.HostRegister(base + POOL_SIZE), OFFLOAD_OK);
    EXPECT_NE(entry.HostGetDevicePtr(base + POOL_SIZE, &dva), OFFLOAD_OK);
    config_.allocSize = config_.reserveSize + POOL_SIZE;
    EXPECT_NE(entry.ValidateSharedConfig(config_), OFFLOAD_OK);
}

TEST_F(AccOffloadDramEntryTest, SharedStillRejectsStoreUrlAlreadyUsedByAnotherInstance)
{
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    const std::string storeUrl = "tcp://127.0.0.1:19999";
    entry.storeUrl_ = storeUrl;
    ASSERT_TRUE(AccOffloadSharedDramEntry::gStores.insert(storeUrl).second);
    EXPECT_EQ(entry.AcquireStore(config_), nullptr);
    EXPECT_TRUE(entry.storeUrl_.empty());
    EXPECT_EQ(AccOffloadSharedDramEntry::gStores.erase(storeUrl), 1U);
}

TEST_F(AccOffloadDramEntryTest, InitializeSharedSingleRankRegistersWholePoolOnce)
{
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    config_.scene = OFFLOAD_SCENE_SHARED;
    snprintf(config_.storeUrl, sizeof(config_.storeUrl), "%s", SHARED_STORE_URL);
    ASSERT_EQ(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_TRUE(entry.inited_);
    EXPECT_EQ(gExportCount, 1U);
    EXPECT_EQ(gEntityExportCount, 1U);
    EXPECT_EQ(gMmapCount, 1U);
    EXPECT_EQ(entry.hostGva_, gPools.front().data());
    auto ptr = entry.HostMalloc(BLOCK_BYTES, 0);
    ASSERT_NE(ptr, 0U);
    uint64_t dva = 0;
    EXPECT_EQ(entry.HostGetDevicePtr(ptr, &dva), OFFLOAD_OK);
    EXPECT_EQ(dva, ptr);
    EXPECT_EQ(entry.HostRegister(ptr), OFFLOAD_OK);
    EXPECT_EQ(entry.HostRegister(ptr), OFFLOAD_OK);
    EXPECT_EQ(gExportCount, 1U);
    EXPECT_EQ(gEntityExportCount, 1U);
    EXPECT_EQ(gMmapCount, 1U);
    entry.UnInitialize();
    EXPECT_EQ(gFreedSlices.size(), 1U);
    EXPECT_EQ(gDestroyed.size(), 1U);
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(gCleanupCount, 1U);
    EXPECT_EQ(AccOffloadSharedDramEntry::gStores.count(SHARED_STORE_URL), 0U);
}

TEST_F(AccOffloadDramEntryTest, InitializeSharedRejectsMalformedStoreUrl)
{
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    config_.scene = OFFLOAD_SCENE_SHARED;
    snprintf(config_.storeUrl, sizeof(config_.storeUrl), "%s", MALFORMED_STORE_URL);
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_FALSE(entry.inited_);
    EXPECT_EQ(gExportCount, 0U);
    EXPECT_EQ(gEntityExportCount, 0U);
    EXPECT_EQ(gMmapCount, 0U);
    // The original SHARED entry validates the store before allocating the pool.
    EXPECT_TRUE(gCreatedIds.empty());
    EXPECT_TRUE(gFreedSlices.empty());
    EXPECT_TRUE(gDestroyed.empty());
    EXPECT_EQ(gCleanupCount, 1U);
    EXPECT_EQ(gUninitCount, 1U);
}

TEST_F(AccOffloadDramEntryTest, InitializeSharedFailsCleanlyWhenPoolMmapFails)
{
    AccOffloadSharedDramEntry entry(HYBM_ENTITY_ID_OFFLOAD_BASE);
    config_.scene = OFFLOAD_SCENE_SHARED;
    snprintf(config_.storeUrl, sizeof(config_.storeUrl), "%s", MMAP_FAILURE_STORE_URL);
    gMmapResult = OFFLOAD_ERROR;
    EXPECT_NE(entry.Initialize(config_), OFFLOAD_OK);
    EXPECT_FALSE(entry.inited_);
    EXPECT_EQ(gExportCount, 1U);
    EXPECT_EQ(gEntityExportCount, 1U);
    EXPECT_EQ(gMmapCount, 1U);
    EXPECT_EQ(gFreedSlices.size(), 1U);
    EXPECT_EQ(gDestroyed.size(), 1U);
    EXPECT_EQ(gCleanupCount, 1U);
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(entry.entity_, nullptr);
    EXPECT_EQ(entry.memMng_, nullptr);
    EXPECT_EQ(AccOffloadSharedDramEntry::gStores.count(MMAP_FAILURE_STORE_URL), 0U);
}

// Exercise both entries' pool operations without a distributed group or device runtime.
class AccOffloadDramPoolTest : public testing::TestWithParam<offload_scene_t> {
protected:
    void SetUp() override
    {
        MOCKER(hybm_uninit).stubs().will(invoke(UninitHybm));
        if (GetParam() == OFFLOAD_SCENE_LOCAL) {
            auto entry = std::make_unique<AccOffloadLocalDramEntry>(HYBM_ENTITY_ID_OFFLOAD_BASE);
            PreparePool(*entry);
            entry_ = std::move(entry);
        } else {
            auto entry = std::make_unique<AccOffloadSharedDramEntry>(HYBM_ENTITY_ID_OFFLOAD_BASE);
            PreparePool(*entry);
            entry_ = std::move(entry);
        }
    }

    void TearDown() override
    {
        entry_.reset();
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    template<typename Entry>
    void PreparePool(Entry &entry)
    {
        entry.inited_ = true;
        entry.base_ = backing_.data();
        entry.size_ = backing_.size();
        entry.memMng_ = std::make_shared<AccOffloadMemManager>(entry.base_, entry.size_);
    }

    alignas(16) std::array<uint8_t, BACKING_BYTES> backing_{};
    std::unique_ptr<AccOffloadEntry> entry_;
};

TEST_P(AccOffloadDramPoolTest, RegistrationChecksPoolRangeRatherThanBlockState)
{
    auto base = reinterpret_cast<uint64_t>(backing_.data());
    EXPECT_EQ(entry_->HostRegister(base), OFFLOAD_OK);
    EXPECT_EQ(entry_->HostRegister(base + backing_.size() - 1), OFFLOAD_OK);
    EXPECT_NE(entry_->HostRegister(base - 1), OFFLOAD_OK);
    EXPECT_NE(entry_->HostRegister(base + backing_.size()), OFFLOAD_OK);
    auto ptr = entry_->HostMalloc(BLOCK_BYTES, 0);
    EXPECT_EQ(entry_->HostRegister(ptr), OFFLOAD_OK);
    EXPECT_EQ(entry_->HostRegister(ptr), OFFLOAD_OK);
    EXPECT_EQ(entry_->HostFree(ptr, 0), OFFLOAD_OK);
    EXPECT_EQ(entry_->HostRegister(ptr), OFFLOAD_OK);
    EXPECT_NE(entry_->HostFree(ptr, 0), OFFLOAD_OK);
    entry_->UnInitialize();
    EXPECT_NE(entry_->HostRegister(ptr), OFFLOAD_OK);
    EXPECT_NE(entry_->HostFree(ptr, 0), OFFLOAD_OK);
    EXPECT_EQ(entry_->MallocHost(BLOCK_BYTES), nullptr);
}

TEST_P(AccOffloadDramPoolTest, ConcurrentLegacyAndHostAllocationsRemainDistinctAndReusable)
{
    std::array<std::array<uint64_t, BLOCKS_PER_WORKER>, WORKER_COUNT> blocks{};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < WORKER_COUNT; ++worker) {
        workers.emplace_back([&, worker]() {
            for (size_t idx = 0; idx < BLOCKS_PER_WORKER; ++idx) {
                blocks[worker][idx] = idx % 2 == 0 ? entry_->HostMalloc(BLOCK_BYTES, 0)
                                                   : reinterpret_cast<uint64_t>(entry_->MallocHost(BLOCK_BYTES));
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    std::set<uint64_t> distinct;
    for (const auto &batch : blocks) {
        distinct.insert(batch.begin(), batch.end());
    }
    EXPECT_EQ(distinct.count(0), 0U);
    EXPECT_EQ(distinct.size(), WORKER_COUNT * BLOCKS_PER_WORKER);
    workers.clear();
    for (size_t worker = 0; worker < WORKER_COUNT; ++worker) {
        workers.emplace_back([&, worker]() {
            for (auto ptr : blocks[worker]) {
                entry_->FreeHost(reinterpret_cast<void *>(ptr));
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    EXPECT_EQ(entry_->HostMalloc(backing_.size(), 0), reinterpret_cast<uint64_t>(backing_.data()));
}

INSTANTIATE_TEST_SUITE_P(LocalAndShared, AccOffloadDramPoolTest,
                         testing::Values(OFFLOAD_SCENE_LOCAL, OFFLOAD_SCENE_SHARED));
