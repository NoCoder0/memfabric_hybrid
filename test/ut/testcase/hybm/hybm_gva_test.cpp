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

#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#define private public
#include "dl_hal_api.h"
#undef private
#include "devmm_svm_gva.h"
#include "hybm_gva.h"

namespace ock {
namespace mf {
int32_t HybmLegacyInitMetaGva(void **globalMemoryBase, size_t allocSize, uint64_t flags);
} // namespace mf
} // namespace ock

using namespace ock::mf;

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

namespace {
constexpr uintptr_t kMockHandleValue = 0x1234U;
constexpr uint64_t kLegacyFlags = 0x55ULL;
uint32_t g_releaseCount = 0U;
uint32_t g_addressFreeCount = 0U;
uint32_t g_unmapCount = 0U;
uint32_t g_legacyUnreserveCount = 0U;

int MockAddressReserve(void **ptr, size_t size, size_t, void *addr, uint64_t)
{
    EXPECT_EQ(size, GB);
    EXPECT_EQ(addr, reinterpret_cast<void *>(SVM_END_ADDR - GB));
    *ptr = addr;
    return BM_OK;
}

int MockAddressFree(void *ptr)
{
    EXPECT_EQ(ptr, reinterpret_cast<void *>(SVM_END_ADDR - GB));
    ++g_addressFreeCount;
    return BM_OK;
}

int MockMemCreate(drv_mem_handle_t **handle, size_t size, const drv_mem_prop *, uint64_t)
{
    EXPECT_EQ(size, HYBM_DEVICE_CONTROL_SIZE);
    *handle = reinterpret_cast<drv_mem_handle_t *>(kMockHandleValue);
    return BM_OK;
}

int MockMemCreateFail(drv_mem_handle_t **, size_t size, const drv_mem_prop *, uint64_t)
{
    EXPECT_EQ(size, HYBM_DEVICE_CONTROL_SIZE);
    return BM_ERROR;
}

int MockMemRelease(drv_mem_handle_t *handle)
{
    EXPECT_EQ(handle, reinterpret_cast<drv_mem_handle_t *>(kMockHandleValue));
    ++g_releaseCount;
    return BM_OK;
}

int MockMemMapSuccess(void *ptr, size_t size, size_t, drv_mem_handle_t *handle, uint64_t)
{
    EXPECT_EQ(ptr, reinterpret_cast<void *>(HYBM_DEVICE_CONTROL_ADDR));
    EXPECT_EQ(size, HYBM_DEVICE_CONTROL_SIZE);
    EXPECT_EQ(handle, reinterpret_cast<drv_mem_handle_t *>(kMockHandleValue));
    return BM_OK;
}

int MockMemMapFail(void *ptr, size_t size, size_t offset, drv_mem_handle_t *handle, uint64_t flags)
{
    (void)MockMemMapSuccess(ptr, size, offset, handle, flags);
    return BM_ERROR;
}

int MockMemUnmap(void *ptr)
{
    EXPECT_EQ(ptr, reinterpret_cast<void *>(HYBM_DEVICE_CONTROL_ADDR));
    ++g_unmapCount;
    return BM_OK;
}

int32_t MockLegacyReserve(uint64_t *address, size_t size, int32_t, uint64_t flags)
{
    EXPECT_EQ(size, HYBM_DEVICE_INFO_SIZE);
    EXPECT_EQ(flags, kLegacyFlags);
    *address = SVM_END_ADDR - GB;
    return BM_OK;
}

int32_t MockLegacyAlloc(uint64_t address, size_t size, uint64_t flags)
{
    EXPECT_EQ(address, HYBM_DEVICE_META_ADDR);
    EXPECT_EQ(size, HYBM_DEVICE_INFO_SIZE);
    EXPECT_EQ(flags, 0ULL);
    return BM_OK;
}

int32_t MockLegacyUnreserve(uint64_t address)
{
    EXPECT_EQ(address, SVM_END_ADDR - GB);
    ++g_legacyUnreserveCount;
    return BM_OK;
}

struct HalApiGuard {
    halMemAddressReserveFunc addressReserve{DlHalApi::pHalMemAddressReserve};
    halMemAddressFreeFunc addressFree{DlHalApi::pHalMemAddressFree};
    halMemCreateFunc memCreate{DlHalApi::pHalMemCreate};
    halMemReleaseFunc memRelease{DlHalApi::pHalMemRelease};
    halMemMapFunc memMap{DlHalApi::pHalMemMap};
    halMemUnmapFunc memUnmap{DlHalApi::pHalMemUnmap};

    HalApiGuard()
    {
        DlHalApi::pHalMemAddressReserve = MockAddressReserve;
        DlHalApi::pHalMemAddressFree = MockAddressFree;
        DlHalApi::pHalMemCreate = MockMemCreate;
        DlHalApi::pHalMemRelease = MockMemRelease;
        DlHalApi::pHalMemMap = MockMemMapSuccess;
        DlHalApi::pHalMemUnmap = MockMemUnmap;
        g_releaseCount = 0U;
        g_addressFreeCount = 0U;
        g_unmapCount = 0U;
    }

    ~HalApiGuard()
    {
        DlHalApi::pHalMemAddressReserve = addressReserve;
        DlHalApi::pHalMemAddressFree = addressFree;
        DlHalApi::pHalMemCreate = memCreate;
        DlHalApi::pHalMemRelease = memRelease;
        DlHalApi::pHalMemMap = memMap;
        DlHalApi::pHalMemUnmap = memUnmap;
    }
};

class HybmGvaTest : public testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
        g_legacyUnreserveCount = 0U;
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    void MockLegacyApis()
    {
        MOCKER_CPP(drv::HalGvaReserveMemory, int32_t(*)(uint64_t *, size_t, int32_t, uint64_t))
            .stubs()
            .will(invoke(MockLegacyReserve));
        MOCKER_CPP(drv::HalGvaAlloc, int32_t(*)(uint64_t, size_t, uint64_t))
            .stubs()
            .will(invoke(MockLegacyAlloc));
        MOCKER_CPP(drv::HalGvaUnreserveMemory, int32_t(*)(uint64_t)).stubs().will(invoke(MockLegacyUnreserve));
    }
};
} // namespace

TEST_F(HybmGvaTest, ModernMapsAndUnmapsCompleteControlRegion)
{
    HalApiGuard guard;
    void *base = nullptr;
    void *handle = nullptr;

    ASSERT_EQ(HybmModernInitMetaGva(&base, HYBM_DEVICE_CONTROL_SIZE, &handle), BM_OK);
    uint64_t baseAddress = reinterpret_cast<uint64_t>(base);
    HybmModernUninitMetaGva(baseAddress, &handle);

    EXPECT_EQ(baseAddress, 0ULL);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(g_unmapCount, 1U);
    EXPECT_EQ(g_releaseCount, 1U);
    EXPECT_EQ(g_addressFreeCount, 1U);
}

TEST_F(HybmGvaTest, ModernCreateFailureRollsBackReservation)
{
    HalApiGuard guard;
    DlHalApi::pHalMemCreate = MockMemCreateFail;
    void *base = nullptr;
    void *handle = nullptr;

    EXPECT_EQ(HybmModernInitMetaGva(&base, HYBM_DEVICE_CONTROL_SIZE, &handle), BM_ERROR);
    EXPECT_EQ(base, nullptr);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(g_releaseCount, 0U);
    EXPECT_EQ(g_addressFreeCount, 1U);
}

TEST_F(HybmGvaTest, ModernMapFailureRollsBackHandleAndReservation)
{
    HalApiGuard guard;
    DlHalApi::pHalMemMap = MockMemMapFail;
    void *base = nullptr;
    void *handle = nullptr;

    EXPECT_EQ(HybmModernInitMetaGva(&base, HYBM_DEVICE_CONTROL_SIZE, &handle), BM_ERROR);
    EXPECT_EQ(base, nullptr);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(g_releaseCount, 1U);
    EXPECT_EQ(g_addressFreeCount, 1U);
}

TEST_F(HybmGvaTest, LegacyMapsOriginalMetadataRegion)
{
    MockLegacyApis();
    void *base = nullptr;

    ASSERT_EQ(HybmLegacyInitMetaGva(&base, HYBM_DEVICE_INFO_SIZE, kLegacyFlags), BM_OK);
    EXPECT_EQ(base, reinterpret_cast<void *>(SVM_END_ADDR - GB));
    EXPECT_EQ(g_legacyUnreserveCount, 0U);
}
