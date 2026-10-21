/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 */
#include <gtest/gtest.h>
#include <limits>
#include <mockcpp/mockcpp.hpp>
#include "dl_acl_api.h"
#include "hybm_mem_segment.h"
#include "hybm_vmm_based_segment.h"
#include "smem.h"

#define private public
#include "hybm_entity_default.h"
#undef private

using namespace ock::mf;

namespace {
constexpr uintptr_t ADDRESS = 0x4000000000ULL;
constexpr uint64_t GIB = 1ULL << 30;
int destroyed = 0;
int released = 0;
uint64_t allocatedSize = 0;
int reserveResult = BM_OK;
int registerResult = BM_OK;

class TestSegment : public HybmVmmBasedSegment {
public:
    explicit TestSegment(const MemSegmentOptions &options) : HybmVmmBasedSegment(options, -1) {}
    ~TestSegment() override
    {
        ++destroyed;
    }
    int32_t ReserveMemorySpace(void **address) noexcept override
    {
        *address = reinterpret_cast<void *>(ADDRESS);
        return reserveResult;
    }
    int32_t AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept override
    {
        allocatedSize = size;
        slice = std::make_shared<MemSlice>(0, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_GVM, ADDRESS, ADDRESS, size);
        return BM_OK;
    }
    int32_t Export(const MemSlicePtr &, std::string &) noexcept override
    {
        return BM_OK;
    }
    int32_t RegisterMemory(const void *address, uint64_t size, MemSlicePtr &slice) noexcept override
    {
        if (registerResult != BM_OK) {
            return registerResult;
        }
        const auto value = reinterpret_cast<uint64_t>(address);
        slice = std::make_shared<MemSlice>(0, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_GVM, value, value, size);
        return BM_OK;
    }
    int32_t Unmap() noexcept override
    {
        return BM_OK;
    }
    int32_t UnReserveMemorySpace() noexcept override
    {
        EXPECT_EQ(released, 0);
        return BM_OK;
    }
};

MemSegmentPtr MakeSegment(const MemSegmentOptions &options, int)
{
    return std::make_shared<TestSegment>(options);
}

int GetContext(void **context)
{
    *context = reinterpret_cast<void *>(1);
    return BM_OK;
}

int GetDevice(int32_t *device)
{
    *device = 0;
    return BM_OK;
}

void Uninitialize()
{
    ++released;
}
} // namespace

class MemSegmentHostMemoryTest : public testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
        destroyed = 0;
        released = 0;
        allocatedSize = 0;
        reserveResult = BM_OK;
        registerResult = BM_OK;
        MOCKER(hybm_init).stubs().will(returnValue(BM_OK));
        MOCKER(hybm_uninit).stubs().will(invoke(Uninitialize));
        MOCKER(DlAclApi::AclrtGetDevice).stubs().will(invoke(GetDevice));
        MOCKER(DlAclApi::AclrtGetCurrentContext).stubs().will(invoke(GetContext));
        MOCKER(DlAclApi::AclrtSetCurrentContext).stubs().will(returnValue(BM_OK));
        MOCKER(DlAclApi::GetAscendSocType).stubs().will(returnValue(AscendSocType::ASCEND_910C));
        MOCKER(HybmGetGvaVersion).stubs().will(returnValue(HYBM_GVA_V4));
        MOCKER(MemSegment::Create).stubs().will(invoke(MakeSegment));
    }
    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }
};

TEST_F(MemSegmentHostMemoryTest, ValidatesPublicParameters)
{
    void *address = reinterpret_cast<void *>(1);
    EXPECT_NE(smem_mem_alloc(0, 0, 0, &address), 0);
    EXPECT_EQ(address, nullptr);
    EXPECT_NE(smem_mem_alloc(0, 1, 1, &address), 0);
    EXPECT_NE(smem_mem_alloc(0, MemSegment::MAX_HOST_ALLOC_SIZE + 1ULL, 0, &address), 0);
    EXPECT_NE(smem_mem_alloc(0, 1, 0, nullptr), 0);
    EXPECT_EQ(smem_mem_free(nullptr), 0);
    EXPECT_EQ(hybm_alloc_host_memory(0, 1, 1, &address), BM_INVALID_PARAM);
    EXPECT_EQ(address, nullptr);
    EXPECT_EQ(hybm_free_host_memory(nullptr), BM_OK);
}

TEST_F(MemSegmentHostMemoryTest, RoundsPhysicalSizeAndReleases)
{
    void *address = nullptr;
    ASSERT_EQ(smem_mem_alloc(0, GIB + 1, 0, &address), 0);
    EXPECT_EQ(allocatedSize, 2 * GIB);
    ASSERT_EQ(smem_mem_free(address), 0);
    EXPECT_EQ(released, 1);
    EXPECT_NE(smem_mem_free(address), 0);
}

TEST_F(MemSegmentHostMemoryTest, SupportsAllocationsAbove32GiB)
{
    constexpr uint64_t previousLimit = 32ULL * GIB;
    constexpr uint64_t size = previousLimit + 1ULL;
    constexpr int expectedAllocations = 1;
    void *address = nullptr;
    ASSERT_EQ(smem_mem_alloc(0U, size, 0U, &address), 0);
    EXPECT_EQ(allocatedSize, previousLimit + GIB);
    EXPECT_EQ(smem_mem_free(address), 0);
    EXPECT_EQ(released, expectedAllocations);
}

TEST_F(MemSegmentHostMemoryTest, RejectsSizesThatExceedVmmAddressSpaceBeforeRounding)
{
    const uint64_t sizes[] = {MemSegment::MAX_HOST_ALLOC_SIZE + 1ULL, std::numeric_limits<uint64_t>::max()};
    for (const auto size : sizes) {
        void *address = reinterpret_cast<void *>(1);
        EXPECT_NE(smem_mem_alloc(0U, size, 0U, &address), 0);
        EXPECT_EQ(address, nullptr);
    }
    EXPECT_EQ(allocatedSize, 0ULL);
    EXPECT_EQ(released, 0);
}

TEST_F(MemSegmentHostMemoryTest, RegistrationRetainsBackingAfterCallerFree)
{
    void *address = nullptr;
    ASSERT_EQ(smem_mem_alloc(0, 4096, 0, &address), 0);
    MemEntityDefault sender(0);
    MemEntityDefault receiver(1);
    for (auto *entity : {&sender, &receiver}) {
        entity->hbmSegment_ = MakeSegment(MemSegmentOptions{}, 0);
        entity->initialized_ = true;
        hybm_mem_slice_t slice = nullptr;
        ASSERT_EQ(entity->RegisterLocalMemory(address, 4096, 0, slice), BM_OK);
        ASSERT_EQ(entity->RegisterLocalMemory(address, 4096, 0, slice), BM_OK);
        EXPECT_EQ(entity->managedAllocations_.size(), 1U);
    }
    EXPECT_EQ(smem_mem_free(address), 0);
    EXPECT_EQ(released, 0);
    sender.UnInitialize();
    EXPECT_EQ(released, 0);
    receiver.UnInitialize();
    EXPECT_EQ(released, 1);
}

TEST_F(MemSegmentHostMemoryTest, FailedRegistrationRetainsBackingUntilEntityTeardown)
{
    void *address = nullptr;
    ASSERT_EQ(hybm_alloc_host_memory(0, 4096, 0, &address), BM_OK);
    MemEntityDefault entity(0);
    entity.hbmSegment_ = MakeSegment(MemSegmentOptions{}, 0);
    entity.initialized_ = true;
    registerResult = BM_ERROR;
    hybm_mem_slice_t slice = nullptr;
    EXPECT_EQ(entity.RegisterLocalMemory(address, 4096, 0, slice), BM_ERROR);
    EXPECT_EQ(hybm_free_host_memory(address), BM_OK);
    EXPECT_EQ(released, 0);
    entity.UnInitialize();
    EXPECT_EQ(released, 1);
}

TEST_F(MemSegmentHostMemoryTest, RejectsInteriorFreeAndCrossAllocationRegistration)
{
    void *address = nullptr;
    ASSERT_EQ(smem_mem_alloc(0, 4096, 0, &address), 0);
    auto *interior = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(address) + 64);
    EXPECT_NE(smem_mem_free(interior), 0);
    std::shared_ptr<void> owner;
    EXPECT_NE(MemSegment::RetainHostMemory(interior, GIB, owner), 0);
    EXPECT_FALSE(owner);
    EXPECT_EQ(smem_mem_free(address), 0);
}

TEST_F(MemSegmentHostMemoryTest, ExternalMemoryDoesNotAcquireNativeOwner)
{
    int external = 0;
    std::shared_ptr<void> owner;
    EXPECT_EQ(MemSegment::RetainHostMemory(&external, sizeof(external), owner), 0);
    EXPECT_FALSE(owner);
}

TEST_F(MemSegmentHostMemoryTest, AllocationFailureCleansAcquiredResources)
{
    reserveResult = BM_ERROR;
    void *address = nullptr;
    EXPECT_NE(smem_mem_alloc(0, 4096, 0, &address), 0);
    EXPECT_EQ(address, nullptr);
    EXPECT_EQ(released, 1);
}

TEST_F(MemSegmentHostMemoryTest, RejectsDifferentCurrentDevice)
{
    void *address = nullptr;
    EXPECT_NE(smem_mem_alloc(1, 4096, 0, &address), 0);
    EXPECT_EQ(address, nullptr);
    EXPECT_EQ(destroyed, 0);
}
