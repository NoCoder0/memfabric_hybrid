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

#define private public
#include "dl_hal_api.h"
#undef private
#include "hybm_gva.h"

using namespace ock::mf;

namespace {
constexpr uintptr_t MOCK_HANDLE_VALUE = 0x1234U;
uint32_t g_releaseCount = 0;
uint32_t g_addressFreeCount = 0;

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
    *handle = reinterpret_cast<drv_mem_handle_t *>(MOCK_HANDLE_VALUE);
    return BM_OK;
}

int MockMemRelease(drv_mem_handle_t *handle)
{
    EXPECT_EQ(handle, reinterpret_cast<drv_mem_handle_t *>(MOCK_HANDLE_VALUE));
    ++g_releaseCount;
    return BM_OK;
}

int MockMemMapSuccess(void *ptr, size_t size, size_t, drv_mem_handle_t *handle, uint64_t)
{
    EXPECT_EQ(ptr, reinterpret_cast<void *>(HYBM_DEVICE_CONTROL_ADDR));
    EXPECT_EQ(size, HYBM_DEVICE_CONTROL_SIZE);
    EXPECT_EQ(handle, reinterpret_cast<drv_mem_handle_t *>(MOCK_HANDLE_VALUE));
    return BM_OK;
}

int MockMemMapFail(void *ptr, size_t size, size_t offset, drv_mem_handle_t *handle, uint64_t flags)
{
    (void)MockMemMapSuccess(ptr, size, offset, handle, flags);
    return BM_ERROR;
}

struct HalApiGuard {
    halMemAddressReserveFunc addressReserve{DlHalApi::pHalMemAddressReserve};
    halMemAddressFreeFunc addressFree{DlHalApi::pHalMemAddressFree};
    halMemCreateFunc memCreate{DlHalApi::pHalMemCreate};
    halMemReleaseFunc memRelease{DlHalApi::pHalMemRelease};
    halMemMapFunc memMap{DlHalApi::pHalMemMap};

    HalApiGuard()
    {
        DlHalApi::pHalMemAddressReserve = MockAddressReserve;
        DlHalApi::pHalMemAddressFree = MockAddressFree;
        DlHalApi::pHalMemCreate = MockMemCreate;
        DlHalApi::pHalMemRelease = MockMemRelease;
        DlHalApi::pHalMemMap = MockMemMapSuccess;
        g_releaseCount = 0;
        g_addressFreeCount = 0;
    }

    ~HalApiGuard()
    {
        DlHalApi::pHalMemAddressReserve = addressReserve;
        DlHalApi::pHalMemAddressFree = addressFree;
        DlHalApi::pHalMemCreate = memCreate;
        DlHalApi::pHalMemRelease = memRelease;
        DlHalApi::pHalMemMap = memMap;
    }
};
} // namespace

TEST(HybmGvaTest, Ascend950MapsCompleteControlRegion)
{
    HalApiGuard guard;
    void *base = nullptr;
    void *handle = nullptr;

    EXPECT_EQ(HybmAscend950InitMetaGva(&base, HYBM_DEVICE_CONTROL_SIZE, &handle), BM_OK);
    EXPECT_EQ(base, reinterpret_cast<void *>(SVM_END_ADDR - GB));
    EXPECT_EQ(handle, reinterpret_cast<void *>(MOCK_HANDLE_VALUE));
    EXPECT_EQ(g_releaseCount, 0U);
    EXPECT_EQ(g_addressFreeCount, 0U);
}

TEST(HybmGvaTest, Ascend950MapFailureRollsBackHandleAndReservation)
{
    HalApiGuard guard;
    DlHalApi::pHalMemMap = MockMemMapFail;
    void *base = nullptr;
    void *handle = nullptr;

    EXPECT_EQ(HybmAscend950InitMetaGva(&base, HYBM_DEVICE_CONTROL_SIZE, &handle), BM_ERROR);
    EXPECT_EQ(base, nullptr);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(g_releaseCount, 1U);
    EXPECT_EQ(g_addressFreeCount, 1U);
}
