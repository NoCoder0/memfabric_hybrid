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
#include <algorithm>
#include <mutex>
#include <vector>
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include "hybm_big_mem.h"
#include "hybm_device_copy.h"
#include "vendor/vendor_api.h"
#include "acc_offload_mem_manager.h"
#include "smem_net_group_engine.h"
#include "smem_store_factory.h"
#include "smem_tcp_config_store.h"

#define private   public
#define protected public
#include "acc_offload_launch.h"
#include "acc_offload_entry_manager.h"
#include "aicore/acc_offload_local_dram_mte_entry.h"
#include "aicore/acc_offload_shared_dram_mte_entry.h"
#undef private
#undef protected

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::offload;
using namespace ock::smem;

namespace {
constexpr uint64_t POOL_SIZE = 1ULL << 30;
constexpr size_t BLOCK_SIZE = 16;
constexpr size_t BACKING_SIZE = BLOCK_SIZE * 8;
constexpr uint32_t TEST_DEVICE = 0;
constexpr uint32_t OTHER_DEVICE = 1;
constexpr uint32_t TEST_WORLD_SIZE = 1;
constexpr char STORE_URL[] = "tcp://127.0.0.1:19871";
constexpr char OTHER_STORE_URL[] = "tcp://127.0.0.1:19872";
constexpr uint64_t DEVICE_ADDRESS_OFFSET = 2 * POOL_SIZE;

std::array<std::array<uint8_t, BACKING_SIZE>, HYBM_ENTITY_ID_SEGMENT_SIZE> gPools;
std::vector<uint16_t> gCreatedIds;
std::vector<hybm_options> gCreatedOptions;
std::vector<hybm_entity_t> gDestroyed;
uint32_t gUninitCount = 0;
uint32_t gLaunchCleanupCount = 0;
int32_t gLaunchResult = OFFLOAD_OK;

int32_t InitHybm(uint16_t device, uint64_t)
{
    return device == TEST_DEVICE ? OFFLOAD_OK : OFFLOAD_ERROR;
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
    ++gLaunchCleanupCount;
}

hybm_entity_t CreateEntity(uint16_t id, const hybm_options *options, uint32_t)
{
    gCreatedIds.push_back(id);
    gCreatedOptions.push_back(*options);
    return gPools.at(id - HYBM_ENTITY_ID_OFFLOAD_BASE).data();
}

void DestroyEntity(hybm_entity_t entity, uint32_t)
{
    gDestroyed.push_back(entity);
}

hybm_mem_slice_t AllocSlice(hybm_entity_t entity, hybm_mem_type, uint64_t, uint32_t)
{
    return entity;
}

void *SliceVa(hybm_entity_t entity, hybm_mem_slice_t, uint32_t)
{
    return entity;
}

int32_t TranslateHostAddress(uint64_t hostPtr, hybm_mem_type, uint64_t *dva)
{
    *dva = hostPtr + DEVICE_ADDRESS_OFFSET;
    return OFFLOAD_OK;
}
} // namespace

class AccOffloadInstanceTest : public testing::Test {
protected:
    void SetUp() override
    {
        AccOffloadEntryManager::nextEntityId_ = 0;
        gCreatedIds.clear();
        gCreatedOptions.clear();
        gDestroyed.clear();
        gUninitCount = 0;
        gLaunchCleanupCount = 0;
        gLaunchResult = OFFLOAD_OK;
        config_ = {};
        config_.deviceId = TEST_DEVICE;
        config_.reserveSize = POOL_SIZE;
        config_.allocSize = POOL_SIZE;
        config_.worldSize = TEST_WORLD_SIZE;
        config_.scene = OFFLOAD_SCENE_LOCAL;
        MOCKER(hybm_init).stubs().will(invoke(InitHybm));
        MOCKER(hybm_uninit).stubs().will(invoke(UninitHybm));
        MOCKER(AccOffloadLaunchApi::TryLoadLibrary).stubs().will(invoke(LoadLaunch));
        MOCKER(AccOffloadLaunchApi::CleanupLibrary).stubs().will(invoke(CleanupLaunch));
        MOCKER(AccOffloadLaunchApi::InitializeAicpu).stubs().will(returnValue(OFFLOAD_OK));
        MOCKER(hybm_create_entity).stubs().will(invoke(CreateEntity));
        MOCKER(hybm_destroy_entity).stubs().will(invoke(DestroyEntity));
        MOCKER(hybm_reserve_mem_space).stubs().will(returnValue(OFFLOAD_OK));
        MOCKER(hybm_unreserve_mem_space).stubs().will(returnValue(OFFLOAD_OK));
        MOCKER(hybm_alloc_local_memory).stubs().will(invoke(AllocSlice));
        MOCKER(hybm_get_slice_va).stubs().will(invoke(SliceVa));
        MOCKER(hybm_free_local_memory).stubs().will(returnValue(OFFLOAD_OK));
        MOCKER(hybm_gva_to_va).stubs().will(invoke(TranslateHostAddress));
    }

    void TearDown() override
    {
        AccOffloadEntryManager::Instance().UnInitialize();
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    offload_config_t config_{};
};

TEST_F(AccOffloadInstanceTest, DefaultAndExplicitPoolsRemainIndependent)
{
    AccOffloadEntryManager first;
    AccOffloadEntryManager second;
    auto &defaultPool = AccOffloadEntryManager::Instance();
    ASSERT_EQ(defaultPool.Initialize(config_), OFFLOAD_OK);
    ASSERT_EQ(first.Initialize(config_), OFFLOAD_OK);
    ASSERT_EQ(second.Initialize(config_), OFFLOAD_OK);
    ASSERT_EQ(gCreatedIds.size(), 3U);
    EXPECT_EQ(gCreatedIds.front(), HYBM_ENTITY_ID_OFFLOAD_BASE);
    EXPECT_NE(gCreatedIds[1], gCreatedIds[2]);
    auto firstPtr = first.MallocHost(BLOCK_SIZE);
    auto secondPtr = second.MallocHost(BLOCK_SIZE);
    EXPECT_NE(firstPtr, secondPtr);
    EXPECT_NE(defaultPool.MallocHost(BLOCK_SIZE), firstPtr);
    first.UnInitialize();
    ASSERT_EQ(gDestroyed.size(), 1U);
    EXPECT_EQ(gDestroyed.front(), firstPtr);
    second.FreeHost(secondPtr);
    EXPECT_EQ(second.MallocHost(BLOCK_SIZE), secondPtr);
    defaultPool.UnInitialize();
    EXPECT_NE(second.MallocHost(BLOCK_SIZE), nullptr);
}

TEST_F(AccOffloadInstanceTest, FailedDeviceInitializationDoesNotReleaseLiveRuntime)
{
    AccOffloadEntryManager active;
    ASSERT_EQ(active.Initialize(config_), OFFLOAD_OK);
    config_.deviceId = OTHER_DEVICE;
    for (auto scene : {OFFLOAD_SCENE_LOCAL, OFFLOAD_SCENE_SHARED}) {
        AccOffloadEntryManager failed;
        config_.scene = scene;
        EXPECT_NE(failed.Initialize(config_), OFFLOAD_OK);
        failed.UnInitialize();
    }
    EXPECT_EQ(gUninitCount, 0U);
    EXPECT_EQ(gLaunchCleanupCount, 0U);
    EXPECT_TRUE(gDestroyed.empty());
    EXPECT_NE(active.MallocHost(BLOCK_SIZE), nullptr);
}

TEST_F(AccOffloadInstanceTest, FailedLaunchAcquisitionDoesNotReleaseAnotherInstancesLibrary)
{
    AccOffloadEntryManager active;
    ASSERT_EQ(active.Initialize(config_), OFFLOAD_OK);
    gLaunchResult = OFFLOAD_ERROR;
    for (auto scene : {OFFLOAD_SCENE_LOCAL, OFFLOAD_SCENE_SHARED}) {
        AccOffloadEntryManager failed;
        config_.scene = scene;
        EXPECT_NE(failed.Initialize(config_), OFFLOAD_OK);
    }
    EXPECT_EQ(gUninitCount, 2U);
    EXPECT_EQ(gLaunchCleanupCount, 0U);
    EXPECT_TRUE(gDestroyed.empty());
    EXPECT_NE(active.MallocHost(BLOCK_SIZE), nullptr);
    active.UnInitialize();
    EXPECT_EQ(gLaunchCleanupCount, 1U);
    EXPECT_EQ(gUninitCount, 3U);
    EXPECT_EQ(gDestroyed.size(), 1U);
}

TEST_F(AccOffloadInstanceTest, EntityIdIncreasesAfterPoolCloses)
{
    AccOffloadEntryManager pool;
    ASSERT_EQ(pool.Initialize(config_), OFFLOAD_OK);
    ASSERT_EQ(gCreatedIds.size(), 1U);
    auto originalId = gCreatedIds.front();
    EXPECT_EQ(pool.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gCreatedIds.size(), 1U);
    pool.UnInitialize();
    ASSERT_EQ(pool.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gCreatedIds.back(), originalId + 1);
    EXPECT_NE(pool.MallocHost(BLOCK_SIZE), nullptr);
}

TEST_F(AccOffloadInstanceTest, ExhaustedIdsDoNotReuseReleasedIds)
{
    AccOffloadEntryManager::nextEntityId_ = HYBM_ENTITY_ID_SEGMENT_SIZE - 1;
    AccOffloadEntryManager last;
    AccOffloadEntryManager exhausted;
    ASSERT_EQ(last.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gCreatedIds.back(), HYBM_ENTITY_ID_OFFLOAD_BASE + HYBM_ENTITY_ID_SEGMENT_SIZE - 1);
    EXPECT_NE(exhausted.Initialize(config_), OFFLOAD_OK);
    last.UnInitialize();
    EXPECT_NE(exhausted.Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gCreatedIds.size(), 1U);
    EXPECT_NE(AccOffloadEntryManager::Instance().Initialize(config_), OFFLOAD_OK);
    EXPECT_EQ(gCreatedIds.size(), 1U);
}

TEST_F(AccOffloadInstanceTest, HostMethodsUseTheLocalPoolWithoutAnExtraFlag)
{
    AccOffloadEntryManager pool;
    ASSERT_EQ(pool.Initialize(config_), OFFLOAD_OK);
    ASSERT_NE(dynamic_cast<AccOffloadLocalDramMteEntry *>(pool.entry_.get()), nullptr);
    auto ptr = pool.HostMalloc(BLOCK_SIZE, 0);
    ASSERT_NE(ptr, 0U);
    uint64_t dva = 0;
    EXPECT_EQ(pool.HostGetDevicePtr(ptr, &dva), OFFLOAD_OK);
    EXPECT_EQ(dva, ptr + DEVICE_ADDRESS_OFFSET);
    EXPECT_EQ(pool.GetDva(ptr, &dva), OFFLOAD_OK);
    EXPECT_EQ(dva, ptr + DEVICE_ADDRESS_OFFSET);
    EXPECT_EQ(pool.HostRegister(ptr), OFFLOAD_OK);
    EXPECT_EQ(pool.HostRegister(ptr + BLOCK_SIZE), OFFLOAD_OK);
    EXPECT_NE(pool.HostRegister(ptr + POOL_SIZE), OFFLOAD_OK);
    EXPECT_NE(pool.HostFree(ptr + 1, 0), OFFLOAD_OK);
    EXPECT_EQ(pool.HostFree(ptr, 0), OFFLOAD_OK);
    EXPECT_NE(pool.HostFree(ptr, 0), OFFLOAD_OK);
    EXPECT_EQ(reinterpret_cast<uint64_t>(pool.MallocHost(BLOCK_SIZE)), ptr);
    pool.FreeHost(reinterpret_cast<void *>(ptr));
    EXPECT_EQ(pool.HostMalloc(BLOCK_SIZE, 0), ptr);
    pool.UnInitialize();
    EXPECT_EQ(pool.HostMalloc(BLOCK_SIZE, 0), 0U);
    EXPECT_NE(pool.HostRegister(ptr), OFFLOAD_OK);
    EXPECT_NE(pool.HostGetDevicePtr(ptr, &dva), OFFLOAD_OK);
    EXPECT_NE(pool.HostFree(ptr, 0), OFFLOAD_OK);
}

TEST_F(AccOffloadInstanceTest, SharedSelectionPreservesStoreUrlExclusivity)
{
    MOCKER_CPP(&ock::mf::AscendApiImpl::GetSocName, ock::mf::AscendSocType(*)(ock::mf::AscendApiImpl *))
        .stubs()
        .will(returnValue(ock::mf::AscendSocType::ASCEND_910C));
    AccOffloadEntryManager pool;
    config_.scene = OFFLOAD_SCENE_SHARED;
    std::copy_n(STORE_URL, sizeof(STORE_URL), config_.storeUrl);
    ASSERT_TRUE(AccOffloadSharedDramMteEntry::gStores.insert(STORE_URL).second);
    EXPECT_NE(pool.Initialize(config_), OFFLOAD_OK);
    EXPECT_FALSE(pool.IsInitialized());
    EXPECT_TRUE(gCreatedIds.empty());
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(gLaunchCleanupCount, 1U);
    EXPECT_EQ(AccOffloadSharedDramMteEntry::gStores.erase(STORE_URL), 1U);
}

namespace {
struct AicpuImport {
    hybm_entity_t entity;
    const uint8_t *descriptor;
    uint32_t flags;
};

std::mutex gAicpuExchangeMutex;
std::vector<AicpuImport> gAicpuImports;
std::vector<std::pair<hybm_entity_t, uint32_t>> gAicpuConnections;
std::array<uint8_t, 3> gAicpuDescriptors{1U, 2U, 3U};
hybm_entity_t gCopyEntity = nullptr;
hybm_device_copy_params gCopyParams{};
} // namespace

class AccOffloadAicpuInstanceTest : public AccOffloadInstanceTest {
protected:
    void SetUp() override
    {
        AccOffloadInstanceTest::SetUp();
        config_.scene = OFFLOAD_SCENE_LOCAL;
        config_.executor = OFFLOAD_EXECUTOR_AICPU;
        gAicpuImports.clear();
        gAicpuConnections.clear();
        gCopyEntity = nullptr;
        gCopyParams = {};
        MOCKER(hybm_export)
            .stubs()
            .will(invoke(+[](hybm_entity_t entity, hybm_mem_slice_t slice, uint32_t flags, hybm_exchange_info *info) {
                if (slice != nullptr) {
                    EXPECT_EQ(entity, gPools[0].data());
                    EXPECT_EQ(slice, entity);
                    EXPECT_EQ(flags, 0U);
                    info->desc = &gAicpuDescriptors[2];
                } else {
                    EXPECT_EQ(flags, HYBM_FLAG_EXPORT_ENTITY);
                    EXPECT_TRUE(entity == gPools[0].data() || entity == gPools[1].data());
                    info->desc = &gAicpuDescriptors[entity == gPools[0].data() ? 0 : 1];
                }
                info->descLen = 1U;
                return OFFLOAD_OK;
            }));
        MOCKER(hybm_import)
            .stubs()
            .will(invoke(+[](hybm_entity_t entity, const hybm_exchange_info *info, uint32_t count, void **addresses,
                             uint32_t flags) {
                std::lock_guard<std::mutex> lock(gAicpuExchangeMutex);
                EXPECT_EQ(count, 1U);
                EXPECT_EQ(addresses, nullptr);
                EXPECT_EQ(info->descLen, 1U);
                if (flags == 0U) {
                    EXPECT_EQ(gAicpuConnections.size(), 2U);
                }
                gAicpuImports.push_back({entity, info->desc, flags});
                return OFFLOAD_OK;
            }));
        MOCKER(hybm_transport_connect)
            .stubs()
            .will(invoke(+[](hybm_entity_t entity, const uint32_t *ranks, uint32_t count, uint32_t flags) {
                std::lock_guard<std::mutex> lock(gAicpuExchangeMutex);
                EXPECT_EQ(count, 1U);
                EXPECT_EQ(flags, 0U);
                const auto imported = std::find_if(gAicpuImports.begin(), gAicpuImports.end(),
                                                   [entity](const AicpuImport &item) { return item.entity == entity; });
                EXPECT_NE(imported, gAicpuImports.end());
                gAicpuConnections.emplace_back(entity, ranks[0]);
                return OFFLOAD_OK;
            }));
        savedLaunch_ = AccOffloadLaunchApi::pAccOffloadAicpuLaunch;
        AccOffloadLaunchApi::pAccOffloadAicpuLaunch = [](AccOffloadAicpuCallback callback,
                                                         const AccOffloadAicpuParam &param, uint8_t device) {
            EXPECT_EQ(device, TEST_DEVICE);
            EXPECT_EQ(callback(param, reinterpret_cast<void *>(0x1234U)), OFFLOAD_OK);
        };
        MOCKER(hybm_data_device_copy)
            .stubs()
            .will(invoke(+[](hybm_entity_t entity, const hybm_device_copy_params *params) {
                gCopyEntity = entity;
                gCopyParams = *params;
                return OFFLOAD_OK;
            }));
    }

    void TearDown() override
    {
        AccOffloadLaunchApi::pAccOffloadAicpuLaunch = savedLaunch_;
        AccOffloadInstanceTest::TearDown();
    }

    AccOffloadAicpuLaunchFunc savedLaunch_{nullptr};
};

TEST_F(AccOffloadAicpuInstanceTest, Ascend910CKeepsTheSingleEntitySdmaFlow)
{
    MOCKER_CPP(&ock::mf::AscendApiImpl::GetSocName, ock::mf::AscendSocType(*)(ock::mf::AscendApiImpl *))
        .stubs()
        .will(returnValue(ock::mf::AscendSocType::ASCEND_910C));
    auto host = static_cast<hybm_entity_t>(gPools[0].data());
    MOCKER(hybm_prepare_device_copy)
        .expects(once())
        .with(eq(host), eq(0U), eq(reinterpret_cast<uint64_t>(host)), eq(POOL_SIZE))
        .will(returnValue(OFFLOAD_OK));
    AccOffloadEntryManager pool;

    ASSERT_EQ(pool.Initialize(config_), OFFLOAD_OK);

    ASSERT_EQ(gCreatedOptions.size(), 1U);
    const auto &options = gCreatedOptions.front();
    EXPECT_EQ(options.bmType, HYBM_TYPE_AICPU_INITIATE);
    EXPECT_EQ(options.bmDataOpType, HYBM_DOP_TYPE_AIV_SDMA);
    EXPECT_EQ(options.rankCount, 1U);
    EXPECT_EQ(options.flags, HYBM_FLAG_DRAM_MAP_HOST_VA);
    EXPECT_TRUE(gAicpuImports.empty());
    EXPECT_TRUE(gAicpuConnections.empty());
    EXPECT_EQ(pool.MallocHost(BLOCK_SIZE), host);
    uint64_t sources[] = {reinterpret_cast<uint64_t>(host)};
    uint64_t destinations[] = {0x800000U};
    uint32_t lengths[] = {BLOCK_SIZE};
    uint32_t count = 1U;
    ASSERT_EQ(pool.SparseCopy(sources, destinations, lengths, &count, TEST_DEVICE), OFFLOAD_OK);
    EXPECT_EQ(gCopyEntity, host);
    EXPECT_EQ(gCopyParams.rankId, 0U);

    pool.UnInitialize();

    EXPECT_EQ(gDestroyed, (std::vector<hybm_entity_t>{host}));
    EXPECT_EQ(gUninitCount, 1U);
    EXPECT_EQ(gLaunchCleanupCount, 1U);
}
