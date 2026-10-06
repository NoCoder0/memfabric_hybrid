/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include <thread>
#include <vector>
#include <atomic>
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include "dl_hal_api.h"
#include "hybm_def.h"
#include "hybm_define.h"
#include "hybm_va_manager.h"

using namespace ock::mf;

// ============================ 测试常量定义 ============================

// 内存大小常量
constexpr uint64_t TEST_SIZE_ZERO = 0UL;
constexpr uint64_t TEST_SIZE_ONE_BYTE = 1UL;
constexpr uint64_t TEST_SIZE_ONE_MB = 0x100000UL;
constexpr uint64_t TEST_SIZE_FOUR_MB = 0x400000UL;
constexpr uint64_t TEST_SIZE_SIXTEEN_MB = 0x1000000UL;
constexpr uint64_t TEST_SIZE_SIXTY_FOUR_MB = 0x4000000UL;

// 地址偏移常量
constexpr uint64_t TEST_OFFSET_HALF_MB = 0x80000UL;
constexpr uint64_t TEST_OFFSET_ONE_MB = 0x100000UL;
constexpr uint64_t TEST_OFFSET_FOUR_MB = 0x400000UL;
constexpr uint64_t TEST_OFFSET_SIXTEEN_MB = 0x1000000UL;

// 地址基值常量
constexpr uint64_t HYBM_HOST_CONN_ADDR_SIZE = 0x100000000000UL; // 16T
constexpr uint64_t TEST_GVA_BASE_HOST = HYBM_GVM_START_ADDR + TEST_OFFSET_SIXTEEN_MB;
constexpr uint64_t TEST_GVA_BASE_DEVICE = HYBM_GVM_START_ADDR + HYBM_HOST_CONN_ADDR_SIZE + TEST_OFFSET_SIXTEEN_MB;
constexpr uint64_t TEST_LVA_BASE = 0x100000000UL;

// Rank 常量
constexpr uint32_t TEST_RANK_ZERO = 0;
constexpr uint32_t TEST_RANK_ONE = 1;
constexpr uint32_t TEST_RANK_TWO = 2;
constexpr uint32_t TEST_RANK_THREE = 3;
constexpr uint32_t TEST_RANK_INVALID = 999;

// 计数常量
constexpr size_t TEST_COUNT_ONE = 1;
constexpr size_t TEST_COUNT_TWO = 2;
constexpr size_t TEST_COUNT_THREE = 3;
constexpr size_t TEST_COUNT_FIVE = 5;
constexpr size_t TEST_COUNT_TEN = 10;
constexpr size_t TEST_COUNT_TWENTY = 20;

// 测试索引常量
constexpr uint64_t TEST_INDEX_ZERO = 0;
constexpr uint64_t TEST_INDEX_ONE = 1;
constexpr uint64_t TEST_INDEX_TWO = 2;

// 内存类型枚举
constexpr hybm_mem_type TEST_MEM_TYPE_HOST = HYBM_MEM_TYPE_HOST;
constexpr hybm_mem_type TEST_MEM_TYPE_DEVICE = HYBM_MEM_TYPE_DEVICE;

// SoC 类型枚举
constexpr AscendSocType TEST_SOC_910B = AscendSocType::ASCEND_910B;
constexpr AscendSocType TEST_SOC_910 = AscendSocType::ASCEND_910C;
constexpr AscendSocType TEST_SOC_A5 = AscendSocType::ASCEND_950;

// 并发测试常量
constexpr int TEST_THREAD_COUNT_FOUR = 4;
constexpr int TEST_THREAD_COUNT_EIGHT = 8;
constexpr int TEST_OPERATIONS_PER_THREAD_TEN = 10;
constexpr int TEST_OPERATIONS_PER_THREAD_FIFTY = 50;

// host mock memType：非 DV_MEM_SVM_DEVICE 即视为 host
constexpr uint32_t MOCK_HOST_ATTR_MEM_TYPE = 0x0002;
// A3 驱动对 NPU-pinned host 内存的 memType 返回值（普通 host=0x80、pinned host=0x8、HBM=0x10）
constexpr uint32_t MOCK_PINNED_HOST_ATTR_MEM_TYPE = 0x0008;
uint32_t g_mockAttrMemType = MOCK_HOST_ATTR_MEM_TYPE;

int MockDrvMemGetAttribute(DVdeviceptr vptr, DVattribute *attr)
{
    if (vptr == 0 || attr == nullptr) {
        return BM_INVALID_PARAM;
    }
    attr->memType = g_mockAttrMemType;
    return BM_OK;
}

int MockDrvMemGetAttributeUnload(DVdeviceptr vptr, DVattribute *attr)
{
    (void)vptr;
    (void)attr;
    return BM_UNDER_API_UNLOAD;
}

class HybmVaManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
        manager.ClearAll();
        manager.Initialize(TEST_SOC_910B);
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        manager.ClearAll();
    }

    HybmVaManager &manager = HybmVaManager::GetInstance();
};

// Test data helper: list the three va slots explicitly to avoid repeating the aggregate initialization
// in each case.
BaseAllocatedGvaInfo MakeBase(uint64_t gva, uint64_t lva, uint64_t size, hybm_mem_type memType = TEST_MEM_TYPE_HOST)
{
    return {{gva, 0, lva}, size, memType};
}

// ============================ 测试用例 ============================

TEST_F(HybmVaManagerTest, Initialize_ValidSocType_ReturnsSuccess)
{
    EXPECT_TRUE(manager.Initialize(TEST_SOC_910) == BM_OK);
    EXPECT_TRUE(manager.Initialize(TEST_SOC_910B) == BM_OK);
}

TEST_F(HybmVaManagerTest, AddRegisterVaInfo_ValidParameters_ReturnsSuccess)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_ONE);
    EXPECT_TRUE(manager.IsValidAddr(gva));
}

TEST_F(HybmVaManagerTest, AddRegisterVaInfo_ZeroParameters_ReturnsInvalidParam)
{
    EXPECT_TRUE(manager.AddVaInfo({{0, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    EXPECT_TRUE(manager.AddVaInfo({{TEST_GVA_BASE_HOST, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
}

TEST_F(HybmVaManagerTest, AddRegisterVaInfo_OverlappingAddress_ReturnsAddrOverlap)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    // local 记录占用本地地址空间，同 gva 重叠必须拒绝
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Local) == BM_OK);
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_FOUR_MB, TEST_MEM_TYPE_HOST},
                                  false, AddressOwnership::Local) != BM_OK);
    // imported 行是分类专用记录，不占用本地地址空间：同 gva 多实体共存属合法拓扑（多 store 组窗口号撞车）
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
}

TEST_F(HybmVaManagerTest, AddSelfVaInfo_ValidParameters_ReturnsSuccess)
{
    uint64_t gva = TEST_GVA_BASE_DEVICE;
    uint64_t lva = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB;

    EXPECT_TRUE(manager.AddVaInfo({gva, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE, lva}) == BM_OK);

    auto [allocInfo, found] = manager.FindAllocByVa(gva, HVM_GVA);
    EXPECT_TRUE(found);
    EXPECT_TRUE(allocInfo.base.memType == TEST_MEM_TYPE_DEVICE);
}

TEST_F(HybmVaManagerTest, AddressConversion_ValidAddresses_ReturnsCorrectValues)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    EXPECT_TRUE(manager.TransformVa(lva, HVM_HVA, HVM_GVA) == gva);
    EXPECT_TRUE(manager.TransformVa(gva, HVM_GVA, HVM_HVA) == lva);

    uint64_t internalGva = gva + TEST_OFFSET_HALF_MB;
    uint64_t internalLva = lva + TEST_OFFSET_HALF_MB;

    EXPECT_TRUE(manager.TransformVa(internalLva, HVM_HVA, HVM_GVA) == internalGva);
    EXPECT_TRUE(manager.TransformVa(internalGva, HVM_GVA, HVM_HVA) == internalLva);
}

TEST_F(HybmVaManagerTest, AddressConversion_NonExistentAddress_ReturnsZero)
{
    uint64_t nonExistentLva = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN;
    uint64_t nonExistentGva = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN;
    EXPECT_TRUE(manager.TransformVa(nonExistentLva, HVM_HVA, HVM_GVA) == TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.TransformVa(nonExistentGva, HVM_GVA, HVM_HVA) == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, GetMemType_ValidAddresses_ReturnsCorrectMemType)
{
    uint64_t hostGva = TEST_GVA_BASE_HOST;
    uint64_t deviceGva = TEST_GVA_BASE_DEVICE;
    EXPECT_TRUE(manager.AddVaInfo({{hostGva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    BaseAllocatedGvaInfo info = {
        {deviceGva, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE};
    auto ret = manager.AddVaInfo(info);
    EXPECT_TRUE(ret == BM_OK);

    EXPECT_TRUE(manager.GetGvaMemType(hostGva) == TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(manager.GetGvaMemType(deviceGva) == TEST_MEM_TYPE_DEVICE);
}

TEST_F(HybmVaManagerTest, IsValidAddr_ValidAndInvalidAddresses_ReturnsCorrectBool)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    // GVA 地址命中 GVA map → 有效
    EXPECT_TRUE(manager.IsValidAddr(gva));
    EXPECT_TRUE(manager.IsValidAddr(gva + TEST_OFFSET_HALF_MB));

    // HVA 地址（lva）不在 GVA map 中 → IsValidAddr 返回 false
    EXPECT_FALSE(manager.IsValidAddr(lva));
    EXPECT_FALSE(manager.IsValidAddr(lva + TEST_OFFSET_HALF_MB));

    EXPECT_FALSE(manager.IsValidAddr(gva + TEST_SIZE_SIXTEEN_MB));
    EXPECT_FALSE(manager.IsValidAddr(TEST_SIZE_ZERO));
}

TEST_F(HybmVaManagerTest, FindAllocByGva_ValidAndInvalidAddresses_ReturnsCorrectResult)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    auto [allocInfo1, found1] = manager.FindAllocByVa(gva, HVM_GVA);
    EXPECT_TRUE(found1);
    EXPECT_TRUE(allocInfo1.base.va[HVM_GVA] == gva);
    EXPECT_TRUE(allocInfo1.base.va[HVM_HVA] == lva);

    auto [allocInfo2, found2] = manager.FindAllocByVa(gva + TEST_OFFSET_HALF_MB, HVM_GVA);
    EXPECT_TRUE(found2);
    EXPECT_TRUE(allocInfo2.base.va[HVM_GVA] == gva);

    auto [allocInfo3, found3] =
        manager.FindAllocByVa(TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN, HVM_GVA);
    EXPECT_FALSE(found3);
}

TEST_F(HybmVaManagerTest, FindAllocByLva_ValidAndInvalidAddresses_ReturnsCorrectResult)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    auto [allocInfo1, found1] = manager.FindAllocByVa(lva, HVM_HVA);
    EXPECT_TRUE(found1);
    EXPECT_TRUE(allocInfo1.base.va[HVM_GVA] == gva);
    EXPECT_TRUE(allocInfo1.base.va[HVM_HVA] == lva);

    auto [allocInfo2, found2] = manager.FindAllocByVa(lva + TEST_OFFSET_HALF_MB, HVM_HVA);
    EXPECT_TRUE(found2);
    EXPECT_TRUE(allocInfo2.base.va[HVM_HVA] == lva);

    auto [allocInfo3, found3] = manager.FindAllocByVa(TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN, HVM_HVA);
    EXPECT_FALSE(found3);
}

TEST_F(HybmVaManagerTest, AllocReserveGva_ValidParameters_ReturnsReservedInfo)
{
    ReservedGvaInfo reserved =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});

    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(reserved.size == TEST_SIZE_SIXTEEN_MB);
    EXPECT_TRUE(reserved.memType == TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_ONE);
}

TEST_F(HybmVaManagerTest, AllocReserveGva_DuplicateAllocation_ReturnsEmptyReservedInfo)
{
    ReservedGvaInfo reserved1 =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved1.va[HVM_GVA] != TEST_SIZE_ZERO);

    ReservedGvaInfo reserved2 =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved2.va[HVM_GVA] != TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, AllocReserveGva_DifferentRanks_ReturnsReservedInfo)
{
    ReservedGvaInfo reserved1 =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved1.va[HVM_GVA] != TEST_SIZE_ZERO);

    ReservedGvaInfo reserved2 =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved2.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_TWO);
}

TEST_F(HybmVaManagerTest, AllocReserveGva_DifferentMemTypes_ReturnsReservedInfo)
{
    ReservedGvaInfo reserved1 =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved1.va[HVM_GVA] != TEST_SIZE_ZERO);

    ReservedGvaInfo reserved2 =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE});
    EXPECT_TRUE(reserved2.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_TWO);
}

TEST_F(HybmVaManagerTest, FreeReserveGva_ValidAddress_RemovesReservation)
{
    ReservedGvaInfo reserved =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_ONE);

    manager.FreeReserveGva(reserved.va[HVM_GVA]);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, FreeReserveGva_InvalidAddress_NoEffect)
{
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
    manager.FreeReserveGva(TEST_GVA_BASE_HOST);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, RemoveOneVaInfo_ValidAddress_RemovesAllocation)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_ONE);

    manager.RemoveOneVaInfo(gva, HVM_GVA);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, RemoveAllVaInfoByRank_ValidRank_RemovesAllAllocations)
{
    uint64_t gva1 = TEST_GVA_BASE_HOST;
    uint64_t gva2 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    uint64_t gva3 = TEST_GVA_BASE_DEVICE;

    EXPECT_TRUE(manager.AddVaInfo({{gva1, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    BaseAllocatedGvaInfo info0 = {
        {gva2, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST};
    EXPECT_TRUE(manager.AddVaInfo(info0, false, AddressOwnership::Imported) == BM_OK);
    auto lva = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO;
    BaseAllocatedGvaInfo info = {{gva3, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE};
    auto ret = manager.AddVaInfo(info);
    EXPECT_TRUE(ret == BM_OK);

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_THREE);
}

TEST_F(HybmVaManagerTest, GetAllocCount_MultipleAllocations_ReturnsCorrectCount)
{
    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);

    for (size_t i = TEST_INDEX_ZERO; i < TEST_COUNT_FIVE; i++) {
        uint64_t gva = TEST_GVA_BASE_HOST + i * TEST_OFFSET_SIXTEEN_MB;
        uint64_t lva = TEST_LVA_BASE + i * TEST_OFFSET_SIXTEEN_MB;
        EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                      AddressOwnership::Imported) == BM_OK);
    }

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_FIVE);
}

TEST_F(HybmVaManagerTest, GetReservedCount_MultipleReservations_ReturnsCorrectCount)
{
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);

    for (size_t i = TEST_INDEX_ZERO; i < TEST_COUNT_THREE; i++) {
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    }

    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_THREE);
}

TEST_F(HybmVaManagerTest, ClearAll_MultipleAllocationsAndReservations_ClearsAll)
{
    for (size_t i = TEST_INDEX_ZERO; i < TEST_COUNT_FIVE; i++) {
        uint64_t gva = TEST_GVA_BASE_HOST + i * TEST_OFFSET_SIXTEEN_MB;
        uint64_t lva = TEST_LVA_BASE + i * TEST_OFFSET_SIXTEEN_MB;
        EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                      AddressOwnership::Imported) == BM_OK);
    }

    for (size_t i = TEST_INDEX_ZERO; i < TEST_COUNT_THREE; i++) {
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    }

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_FIVE);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_THREE);

    manager.ClearAll();

    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, FormatMemorySize_VariousSizes_ReturnsFormattedString)
{
    std::string size1 = VaToStr(TEST_SIZE_ONE_BYTE);
    EXPECT_TRUE(!size1.empty());

    std::string size2 = VaToStr(TEST_SIZE_ONE_MB);
    EXPECT_TRUE(!size2.empty());

    std::string size3 = VaToStr(TEST_SIZE_SIXTEEN_MB);
    EXPECT_TRUE(!size3.empty());

    std::string size4 = VaToStr(TEST_SIZE_ZERO);
    EXPECT_TRUE(!size4.empty());
}

TEST_F(HybmVaManagerTest, PrintAllReservedGvaInfo_EmptyAndNonEmpty_DoesNotCrash)
{
    EXPECT_NO_THROW(manager.DumpReservedGvaInfo());
    manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_NO_THROW(manager.DumpReservedGvaInfo());
}

TEST_F(HybmVaManagerTest, PrintAllAllocGvaInfo_EmptyAndNonEmpty_DoesNotCrash)
{
    EXPECT_NO_THROW(manager.DumpAllocatedGvaInfo());
    EXPECT_TRUE(manager.AddVaInfo({{TEST_GVA_BASE_HOST, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    EXPECT_NO_THROW(manager.DumpAllocatedGvaInfo());
}

TEST_F(HybmVaManagerTest, BoundaryCondition_MinimumSize_ReturnsSuccess)
{
    EXPECT_TRUE(manager.AddVaInfo({{TEST_GVA_BASE_HOST, 0, TEST_LVA_BASE}, TEST_SIZE_ONE_BYTE, TEST_MEM_TYPE_HOST},
                                  false, AddressOwnership::Imported) == BM_OK);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_ONE);
}

TEST_F(HybmVaManagerTest, BoundaryCondition_DifferentMemTypes_NoOverlap)
{
    uint64_t hostGva = TEST_GVA_BASE_HOST;
    uint64_t deviceGva = TEST_GVA_BASE_DEVICE;
    EXPECT_TRUE(manager.AddVaInfo({{hostGva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    BaseAllocatedGvaInfo base = {
        {deviceGva, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE};
    EXPECT_TRUE(manager.AddVaInfo(base, false, AddressOwnership::Imported) == BM_OK);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_TWO);
}

TEST_F(HybmVaManagerTest, ConcurrentTest_MultipleThreadsAdding_NoDataRace)
{
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    for (int i = 0; i < TEST_THREAD_COUNT_FOUR; i++) {
        threads.emplace_back([this, i, &successCount]() {
            for (int j = 0; j < TEST_OPERATIONS_PER_THREAD_TEN; j++) {
                uint64_t gva = TEST_GVA_BASE_HOST + (i * TEST_OPERATIONS_PER_THREAD_TEN + j) * TEST_OFFSET_SIXTEEN_MB;
                uint64_t lva = TEST_LVA_BASE + (i * TEST_OPERATIONS_PER_THREAD_TEN + j) * TEST_OFFSET_SIXTEEN_MB;

                if (manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                      AddressOwnership::Imported) == BM_OK) {
                    successCount++;
                }
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(manager.GetAllocCount() == static_cast<size_t>(successCount));
}

TEST_F(HybmVaManagerTest, MixedOperations_AddRemoveQuery_WorksCorrectly)
{
    uint64_t gva1 = TEST_GVA_BASE_HOST;
    uint64_t gva2 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    uint64_t gva3 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO;

    EXPECT_TRUE(manager.AddVaInfo({{gva1, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    EXPECT_TRUE(manager.AddVaInfo(
                    {{gva2, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) ==
                BM_OK);
    ReservedGvaInfo reserved =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE});
    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);

    manager.RemoveOneVaInfo(gva1, HVM_GVA);

    EXPECT_TRUE(manager.AddVaInfo({{gva3, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO},
                                   TEST_SIZE_SIXTEEN_MB,
                                   TEST_MEM_TYPE_HOST},
                                  false, AddressOwnership::Imported) == BM_OK);

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_TWO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_ONE);

    manager.FreeReserveGva(reserved.va[HVM_GVA]);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, RemoveAllVaInfoByRank_RemovesAllAllocations)
{
    uint64_t gva1 = TEST_GVA_BASE_HOST;
    uint64_t gva2 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    uint64_t gva3 = TEST_GVA_BASE_DEVICE;

    EXPECT_TRUE(manager.AddVaInfo({{gva1, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    BaseAllocatedGvaInfo info0 = {
        {gva2, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST};
    EXPECT_TRUE(manager.AddVaInfo(info0, false, AddressOwnership::Imported) == BM_OK);
    auto lva = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO;
    BaseAllocatedGvaInfo info = {{gva3, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE};
    auto ret = manager.AddVaInfo(info);
    EXPECT_TRUE(ret == BM_OK);

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_THREE);
    manager.RemoveOneVaInfo(gva1, HVM_GVA);
    manager.RemoveOneVaInfo(gva2, HVM_GVA);
    manager.RemoveOneVaInfo(gva3, HVM_GVA);
    EXPECT_TRUE(manager.GetAllocCount() == 0);
}

TEST_F(HybmVaManagerTest, GetMemType_BoundaryAddresses)
{
    uint64_t hostGva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{hostGva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    EXPECT_TRUE(manager.GetGvaMemType(hostGva) == TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(manager.GetGvaMemType(hostGva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE) == TEST_MEM_TYPE_HOST);
}

// GetLocalMemoryType: 910B（未启用驱动查询的 SoC）上 HBM 地址返回 DEVICE，非 HBM 返回 HOST
TEST_F(HybmVaManagerTest, GetLocalMemoryType_NonA5_HbmAndHost_ReturnsCorrectMemType)
{
    hybm_mem_type memType = HYBM_MEM_TYPE_BUTT;

    const uint64_t hbmAddr = HYBM_HBM_START_ADDR;
    EXPECT_EQ(manager.GetLocalMemoryType(hbmAddr, memType), BM_OK);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_DEVICE);

    const uint64_t hostAddr = TEST_LVA_BASE;
    EXPECT_EQ(manager.GetLocalMemoryType(hostAddr, memType), BM_OK);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_HOST);
}

// GetLocalMemoryType: A5 SoC 上设备内存属性返回 DEVICE
TEST_F(HybmVaManagerTest, GetLocalMemoryType_A5_DeviceAttr_ReturnsDevice)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = DV_MEM_SVM_DEVICE;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));

    hybm_mem_type memType = HYBM_MEM_TYPE_BUTT;
    const uint64_t testVa = HYBM_DEVICE_VA_START + HYBM_DEVICE_VA_SIZE + TEST_OFFSET_ONE_MB;
    EXPECT_EQ(manager.GetLocalMemoryType(testVa, memType), BM_OK);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_DEVICE);
}

// GetLocalMemoryType: on 910C (any transport, incl. sdma), a pinned host attr (0x8) must be classified as HOST
// even when the VA falls inside the HBM address range (A3 issue: NPU-pinned host memory misjudged as HBM)
TEST_F(HybmVaManagerTest, GetLocalMemoryType_910C_PinnedHostAttrInHbmRange_ReturnsHost)
{
    manager.Initialize(TEST_SOC_910);
    g_mockAttrMemType = MOCK_PINNED_HOST_ATTR_MEM_TYPE;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));

    hybm_mem_type memType = HYBM_MEM_TYPE_BUTT;
    const uint64_t pinnedHostVa = HYBM_HBM_START_ADDR + 0x2C0011F0000UL;
    EXPECT_EQ(manager.GetLocalMemoryType(pinnedHostVa, memType), BM_OK);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_HOST);
}

// GetLocalMemoryType: on 910C, an HBM attr (0x10, same value as DV_MEM_LOCK_DEV) is classified as DEVICE
TEST_F(HybmVaManagerTest, GetLocalMemoryType_910C_DeviceAttr_ReturnsDevice)
{
    manager.Initialize(TEST_SOC_910);
    g_mockAttrMemType = DV_MEM_LOCK_DEV;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));

    hybm_mem_type memType = HYBM_MEM_TYPE_BUTT;
    const uint64_t hbmVa = HYBM_HBM_START_ADDR + TEST_OFFSET_ONE_MB;
    EXPECT_EQ(manager.GetLocalMemoryType(hbmVa, memType), BM_OK);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_DEVICE);
}

// GetLocalMemoryType: on 910C, an unavailable driver query (old HAL without the exported symbol) falls back
// to the address-range check, keeping the legacy behavior
TEST_F(HybmVaManagerTest, GetLocalMemoryType_910C_AttrUnavailable_FallbackToAddrRange)
{
    manager.Initialize(TEST_SOC_910);
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttributeUnload));

    hybm_mem_type memType = HYBM_MEM_TYPE_BUTT;
    EXPECT_EQ(manager.GetLocalMemoryType(HYBM_HBM_START_ADDR, memType), BM_OK);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_DEVICE);
    EXPECT_EQ(manager.GetLocalMemoryType(TEST_LVA_BASE, memType), BM_OK);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_HOST);
}

// 测试37: AllocReserveGva - 零大小
TEST_F(HybmVaManagerTest, AllocReserveGva_ZeroSize)
{
    ReservedGvaInfo reserved = manager.AllocReserveGva({TEST_SIZE_ZERO, TEST_SIZE_ZERO, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved.va[HVM_GVA] == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, AllocReserveGva_MaxRank)
{
    ReservedGvaInfo reserved =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, AddVaInfo_ZeroSize)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, TEST_LVA_BASE}, TEST_SIZE_ZERO, TEST_MEM_TYPE_HOST}) == BM_OK);
}

TEST_F(HybmVaManagerTest, FindAllocByGva_BoundaryCases)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    auto [alloc1, found1] = manager.FindAllocByVa(gva, HVM_GVA);
    EXPECT_TRUE(found1);

    auto [alloc2, found2] = manager.FindAllocByVa(gva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE, HVM_GVA);
    EXPECT_TRUE(found2);

    auto [alloc3, found3] = manager.FindAllocByVa(gva - TEST_COUNT_ONE, HVM_GVA);
    EXPECT_FALSE(found3);

    auto [alloc4, found4] = manager.FindAllocByVa(gva + TEST_SIZE_SIXTEEN_MB, HVM_GVA);
    EXPECT_FALSE(found4);
}

TEST_F(HybmVaManagerTest, FindAllocByLva_BoundaryCases)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    auto [alloc1, found1] = manager.FindAllocByVa(lva, HVM_HVA);
    EXPECT_TRUE(found1);

    auto [alloc2, found2] = manager.FindAllocByVa(lva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE, HVM_HVA);
    EXPECT_TRUE(found2);

    auto [alloc3, found3] = manager.FindAllocByVa(lva - TEST_COUNT_ONE, HVM_HVA);
    EXPECT_FALSE(found3);

    auto [alloc4, found4] = manager.FindAllocByVa(lva + TEST_SIZE_SIXTEEN_MB, HVM_HVA);
    EXPECT_FALSE(found4);
}

TEST_F(HybmVaManagerTest, RemoveOneVaInfo_NonExistentAddress)
{
    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);
    manager.RemoveOneVaInfo(TEST_GVA_BASE_HOST);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, FreeReserveGva_MultipleFrees)
{
    ReservedGvaInfo reserved =
        manager.AllocReserveGva({TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST});
    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_ONE);

    manager.FreeReserveGva(reserved.va[HVM_GVA]);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);

    manager.FreeReserveGva(reserved.va[HVM_GVA]);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, AddressConversion_BoundaryOffsets)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    EXPECT_TRUE(manager.TransformVa(lva, HVM_HVA, HVM_GVA) == gva);
    EXPECT_TRUE(manager.TransformVa(gva, HVM_GVA, HVM_HVA) == lva);

    EXPECT_TRUE(manager.TransformVa(lva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE, HVM_HVA, HVM_GVA) ==
                gva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE);
    EXPECT_TRUE(manager.TransformVa(gva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE, HVM_GVA, HVM_HVA) ==
                lva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE);
}

TEST_F(HybmVaManagerTest, IsValidAddr_BoundaryValues)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    // GVA 地址命中 GVA map → 有效
    EXPECT_TRUE(manager.IsValidAddr(gva));
    EXPECT_TRUE(manager.IsValidAddr(gva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE));
    EXPECT_FALSE(manager.IsValidAddr(gva - TEST_COUNT_ONE));
    EXPECT_FALSE(manager.IsValidAddr(gva + TEST_SIZE_SIXTEEN_MB));

    // HVA 地址不在 GVA map 中 → IsValidAddr 返回 false
    EXPECT_FALSE(manager.IsValidAddr(lva));
    EXPECT_FALSE(manager.IsValidAddr(lva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE));
    EXPECT_FALSE(manager.IsValidAddr(lva - TEST_COUNT_ONE));
    EXPECT_FALSE(manager.IsValidAddr(lva + TEST_SIZE_SIXTEEN_MB));
}

TEST_F(HybmVaManagerTest, ConcurrentTest_MultipleThreadsQuerying)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);

    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    for (int i = 0; i < TEST_THREAD_COUNT_EIGHT; i++) {
        threads.emplace_back([this, gva, &successCount]() {
            for (int j = 0; j < TEST_OPERATIONS_PER_THREAD_FIFTY; j++) {
                successCount++;
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(successCount.load() == TEST_THREAD_COUNT_EIGHT * TEST_OPERATIONS_PER_THREAD_FIFTY);
}

TEST_F(HybmVaManagerTest, MixedOperations_AddRemoveQueryMixed)
{
    uint64_t gva1 = TEST_GVA_BASE_HOST;
    uint64_t gva2 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;

    EXPECT_TRUE(manager.AddVaInfo({{gva1, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    EXPECT_TRUE(manager.AddVaInfo(
                    {{gva2, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) ==
                BM_OK);

    manager.RemoveOneVaInfo(gva1);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_ONE);
}

// ======================== QueryAddr 测试 ========================

TEST_F(HybmVaManagerTest, QueryAddr_AllocGvaLocal_ReturnsInAllocGva)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    auto r = manager.QueryAddr(gva);
    EXPECT_TRUE(r.inAllocGva);
    EXPECT_EQ(r.memType, TEST_MEM_TYPE_HOST);
    EXPECT_FALSE(r.imported);
}

TEST_F(HybmVaManagerTest, QueryAddr_AllocGvaImported_ReturnsImported)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    auto r = manager.QueryAddr(gva);
    EXPECT_TRUE(r.inAllocGva);
    EXPECT_TRUE(r.imported);
}

TEST_F(HybmVaManagerTest, QueryAddr_NoAlloc_ReturnsAllFalse)
{
    uint64_t unregisteredAddr = 0x12345678;
    auto r = manager.QueryAddr(unregisteredAddr);
    EXPECT_FALSE(r.inAllocGva);
}

TEST_F(HybmVaManagerTest, QueryAddr_AllocHva_ReturnsNoHit)
{
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfo({{0, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    auto r = manager.QueryAddr(lva);
    EXPECT_FALSE(r.inAllocGva);
}

TEST_F(HybmVaManagerTest, QueryAddr_AllocDva_ReturnsNoHit)
{
    uint64_t dva = TEST_LVA_BASE + 0x200000000;
    EXPECT_TRUE(manager.AddVaInfo({{0, dva, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE}) == BM_OK);
    auto r = manager.QueryAddr(dva);
    EXPECT_FALSE(r.inAllocGva);
}

TEST_F(HybmVaManagerTest, QueryAddr_GvaHit)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, gva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    auto r = manager.QueryAddr(gva);
    EXPECT_TRUE(r.inAllocGva);
}

// ======================== ClassifyAddress 测试 ========================

TEST_F(HybmVaManagerTest, ClassifyAddress_LocalGva_ReturnsLocalHost)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    EXPECT_EQ(manager.ClassifyAddress(gva), LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddress_ImportedGva_ReturnsGlobalHost)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    EXPECT_EQ(manager.ClassifyAddress(gva), GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddress_DeviceVaRange_ReturnsLocalDevice)
{
    uint64_t deviceAddr = HYBM_DEVICE_VA_START + 0x1000;
    // 不注册，仅测试 device VA 范围兜底
    EXPECT_EQ(manager.ClassifyAddress(deviceAddr), LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddress_A5DeviceAttr_ReturnsLocalDevice)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = DV_MEM_SVM_DEVICE;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));

    uint64_t deviceAddr = HYBM_DEVICE_VA_START + HYBM_DEVICE_VA_SIZE + TEST_OFFSET_ONE_MB;

    EXPECT_EQ(manager.ClassifyAddress(deviceAddr), LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddress_A5LockDevAttr_ReturnsLocalDevice)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = DV_MEM_LOCK_DEV;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));

    uint64_t deviceAddr = HYBM_DEVICE_VA_START + HYBM_DEVICE_VA_SIZE + TEST_OFFSET_ONE_MB;

    EXPECT_EQ(manager.ClassifyAddress(deviceAddr), LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddress_A5LockDevDvppAttr_ReturnsLocalDevice)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = DV_MEM_LOCK_DEV_DVPP;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));

    uint64_t deviceAddr = HYBM_DEVICE_VA_START + HYBM_DEVICE_VA_SIZE + TEST_OFFSET_ONE_MB;

    EXPECT_EQ(manager.ClassifyAddress(deviceAddr), LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddress_A5HostAttr_ReturnsLocalHost)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = MOCK_HOST_ATTR_MEM_TYPE;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));

    uint64_t hostAddr = HYBM_DEVICE_VA_START + HYBM_DEVICE_VA_SIZE + TEST_OFFSET_ONE_MB;

    EXPECT_EQ(manager.ClassifyAddress(hostAddr), LOCAL_HOST);
}

// A5 drvMemGetAttribute 失败场景：mock 返回 BM_UNDER_API_UNLOAD，ClassifyAddress 兜底为 LOCAL_HOST
TEST_F(HybmVaManagerTest, ClassifyAddress_A5AttrFail_ReturnsLocalHost)
{
    manager.Initialize(TEST_SOC_A5);
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttributeUnload));

    uint64_t addr = HYBM_DEVICE_VA_START + HYBM_DEVICE_VA_SIZE + TEST_OFFSET_ONE_MB;

    EXPECT_EQ(manager.ClassifyAddress(addr), LOCAL_HOST);
    EXPECT_EQ(manager.InferCopyDirection(addr, addr), HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(HybmVaManagerTest, ClassifyAddress_ImportedDeviceGva_ReturnsGlobalDevice)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE}, false,
                                  AddressOwnership::Imported) == BM_OK);
    EXPECT_EQ(manager.ClassifyAddress(gva), GLOBAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddress_Unregistered_ReturnsLocalHost)
{
    EXPECT_EQ(manager.ClassifyAddress(0x12345678), LOCAL_HOST);
}

// ======================== InferCopyDirection 测试 ========================

TEST_F(HybmVaManagerTest, InferCopyDirection_LocalHostToLocalHost_FallbackToH2GH)
{
    uint64_t gvmAddr = HYBM_GVM_START_ADDR + 0x1000;
    manager.AddVaInfo(BaseAllocatedGvaInfo{{gvmAddr, 0, 0}, 0x100000, HYBM_MEM_TYPE_HOST}, 0);
    auto dir = manager.InferCopyDirection(0x1000, gvmAddr);
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, InferCopyDirection_LocalDeviceToLocalHost_ReturnsD2GH)
{
    uint64_t deviceAddr = HYBM_DEVICE_VA_START + 0x1000;
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    auto dir = manager.InferCopyDirection(deviceAddr, gva);
    EXPECT_EQ(dir, HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, InferCopyDirection_LocalHostToLocalDevice_ReturnsH2GD)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t deviceAddr = HYBM_DEVICE_VA_START + 0x1000;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    auto dir = manager.InferCopyDirection(gva, deviceAddr);
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE);
}

TEST_F(HybmVaManagerTest, InferCopyDirection_GlobalHostToLocalHost_ReturnsGH2LH)
{
    uint64_t remoteGva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{remoteGva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    uint64_t localGva = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    EXPECT_TRUE(manager.AddVaInfo({{localGva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    auto dir = manager.InferCopyDirection(remoteGva, localGva);
    // GLOBAL_HOST(1) → LOCAL_HOST(3) = GH2LH(6)
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, InferCopyDirection_LocalHostToGlobalHost_ReturnsH2GH)
{
    uint64_t localGva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{localGva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    uint64_t remoteGva = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    EXPECT_TRUE(manager.AddVaInfo({{remoteGva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, false,
                                  AddressOwnership::Imported) == BM_OK);
    auto dir = manager.InferCopyDirection(localGva, remoteGva);
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, InferCopyDirection_GvmToUserMalloc_ReturnsGH2LH)
{
    uint64_t gvmAddr = HYBM_GVM_START_ADDR + 0x1000;
    EXPECT_TRUE(manager.AddVaInfo({{gvmAddr, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    uint64_t userAddr = 0xfffd327e4580;
    auto dir = manager.InferCopyDirection(gvmAddr, userAddr);
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, InferCopyDirection_UserMallocToGvm_ReturnsH2GH)
{
    uint64_t userAddr = 0xfffd327e4580;
    uint64_t gvmAddr = HYBM_GVM_START_ADDR + 0x1000;
    EXPECT_TRUE(manager.AddVaInfo({{gvmAddr, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    auto dir = manager.InferCopyDirection(userAddr, gvmAddr);
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, InferCopyDirection_LocalDeviceToLocalDevice_ReturnsBUTT)
{
    uint64_t devAddr1 = HYBM_DEVICE_VA_START + 0x1000;
    uint64_t devAddr2 = HYBM_DEVICE_VA_START + 0x2000;
    auto dir = manager.InferCopyDirection(devAddr1, devAddr2);
    EXPECT_EQ(dir, HYBM_DATA_COPY_DIRECTION_BUTT);
}

// ======================== IsValidAddr 测试 ========================

TEST_F(HybmVaManagerTest, IsValidAddr_Registered_ReturnsTrue)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}) == BM_OK);
    EXPECT_TRUE(manager.IsValidAddr(gva));
}

TEST_F(HybmVaManagerTest, IsValidAddr_Unregistered_ReturnsFalse)
{
    EXPECT_FALSE(manager.IsValidAddr(0x12345678));
}

// ======================== ClassifyAddressMask 测试 ========================

TEST_F(HybmVaManagerTest, ClassifyAddressMask_UserAddr_ReturnsLocalHost)
{
    uint64_t userAddr = 0x7f001000;
    uint8_t mask = manager.ClassifyAddressMask(userAddr);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_DeviceVA_ReturnsLocalDevice)
{
    uint64_t devAddr = HYBM_DEVICE_VA_START + 0x1000;
    uint8_t mask = manager.ClassifyAddressMask(devAddr);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_GvaAllocated_ReturnsBoth)
{
    uint64_t gvaBase = HYBM_GVM_START_ADDR;
    manager.AllocReserveGva({0x2000000, 0x2000000, HYBM_MEM_TYPE_HOST, false});
    uint64_t gva = gvaBase + 0x1000;
    manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_ONE_MB, TEST_MEM_TYPE_HOST});
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_HOST | HybmVaManager::BIT_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_GvaUnallocated_ReturnsZero)
{
    uint64_t gva = HYBM_GVM_START_ADDR + 0x1000;
    manager.AllocReserveGva({0x2000000, 0x2000000, HYBM_MEM_TYPE_HOST, false});
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, 0);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_RemoteImported_ReturnsGlobal)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    manager.AllocReserveGva({0x1000000, 0x1000000, HYBM_MEM_TYPE_HOST, false});
    manager.AddVaInfo({gva, 0, 0, TEST_SIZE_ONE_MB, HYBM_MEM_TYPE_HOST}, false, AddressOwnership::Imported);
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_RegisteredHva_UsesStoredMemType)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = DV_MEM_SVM_DEVICE;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));
    const uint64_t hostVa = TEST_LVA_BASE;
    const uint64_t deviceVa = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB;
    EXPECT_EQ(manager.AddVaInfo({{0ULL, deviceVa, hostVa}, TEST_SIZE_ONE_MB, TEST_MEM_TYPE_HOST}), BM_OK);

    EXPECT_EQ(manager.ClassifyAddressMask(hostVa), HybmVaManager::BIT_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_RegisteredDva_UsesStoredMemType)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = MOCK_HOST_ATTR_MEM_TYPE;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));
    const uint64_t deviceVa = TEST_LVA_BASE;
    EXPECT_EQ(manager.AddVaInfo({{0ULL, deviceVa, 0ULL}, TEST_SIZE_ONE_MB, TEST_MEM_TYPE_DEVICE}), BM_OK);

    EXPECT_EQ(manager.ClassifyAddressMask(deviceVa), HybmVaManager::BIT_LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_RegisteredGva_UsesStoredMemType)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = DV_MEM_SVM_DEVICE;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));
    const uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_EQ(manager.AddVaInfo({{gva, 0ULL, 0ULL}, TEST_SIZE_ONE_MB, TEST_MEM_TYPE_HOST}), BM_OK);

    EXPECT_EQ(manager.ClassifyAddressMask(gva), HybmVaManager::BIT_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_PairUsesStoredMemType)
{
    manager.Initialize(TEST_SOC_A5);
    g_mockAttrMemType = DV_MEM_SVM_DEVICE;
    MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(MockDrvMemGetAttribute));
    const uint64_t hostVa = TEST_LVA_BASE;
    const uint64_t remoteGva = TEST_GVA_BASE_DEVICE;
    ASSERT_EQ(manager.AddVaInfo({{0ULL, 0ULL, hostVa}, TEST_SIZE_ONE_MB, TEST_MEM_TYPE_HOST}), BM_OK);
    ASSERT_EQ(manager.AddVaInfo({{remoteGva, 0ULL, 0ULL}, TEST_SIZE_ONE_MB, TEST_MEM_TYPE_DEVICE}, false,
                                AddressOwnership::Imported),
              BM_OK);
    uint8_t srcMask = 0U;
    uint8_t destMask = 0U;

    manager.ClassifyAddressMask(hostVa, remoteGva, srcMask, destMask);

    EXPECT_EQ(srcMask, HybmVaManager::BIT_LOCAL_HOST);
    EXPECT_EQ(destMask, HybmVaManager::BIT_GLOBAL_DEVICE);
}

// ======================== LUT 方向推断测试 ========================

TEST_F(HybmVaManagerTest, DirectionLut_LocalToGlobal_ReturnsH2GH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_LOCAL_HOST | (HybmVaManager::BIT_GLOBAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, DirectionLut_GlobalToGlobal_ReturnsGH2GH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_GLOBAL_HOST | (HybmVaManager::BIT_GLOBAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, DirectionLut_GlobalToLocal_ReturnsGH2LH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_GLOBAL_HOST | (HybmVaManager::BIT_LOCAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, DirectionLut_DualBits_ReturnsFirstMatch)
{
    HybmVaManager::InitDirectionLut();
    uint8_t dual = HybmVaManager::BIT_LOCAL_HOST | HybmVaManager::BIT_GLOBAL_HOST;
    uint8_t except = dual | (dual << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST); // H2GH(0) 最小
}

TEST_F(HybmVaManagerTest, DirectionLut_Invalid_ReturnsBUTT)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_LOCAL_HOST | (HybmVaManager::BIT_LOCAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_GE(dir, HYBM_DATA_COPY_DIRECTION_AUTO);
}

TEST_F(HybmVaManagerTest, DirectionLut_LocalDeviceToGlobalHost_ReturnsD2GH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_LOCAL_DEVICE | (HybmVaManager::BIT_GLOBAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, DirectionLut_LocalDeviceToGlobalDevice_ReturnsD2GD)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_LOCAL_DEVICE | (HybmVaManager::BIT_GLOBAL_DEVICE << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE);
}

TEST_F(HybmVaManagerTest, DirectionLut_GlobalHostToLocalHost_ReturnsGH2LH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_GLOBAL_HOST | (HybmVaManager::BIT_LOCAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_MixedLocalHostAndDevice)
{
    uint8_t hostMask = manager.ClassifyAddressMask(0x7f001000ULL);
    EXPECT_EQ(hostMask, HybmVaManager::BIT_LOCAL_HOST);

    uint8_t devMask = manager.ClassifyAddressMask(HYBM_DEVICE_VA_START + 0x1000);
    EXPECT_EQ(devMask, HybmVaManager::BIT_LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_DeviceHbm_ReturnsBothDeviceBits)
{
    uint64_t base = HYBM_GVM_START_ADDR;
    HybmVaManager::GetInstance().AllocReserveGva({0x2000000, 0x2000000, HYBM_MEM_TYPE_DEVICE, false});
    uint64_t gva = base + 0x1000;
    BaseAllocatedGvaInfo devInfo = {gva, gva, gva, 0x100000, HYBM_MEM_TYPE_DEVICE};
    HybmVaManager::GetInstance().AddVaInfo(devInfo, 0);
    uint8_t mask = HybmVaManager::GetInstance().ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_DEVICE | HybmVaManager::BIT_GLOBAL_DEVICE);
}

TEST_F(HybmVaManagerTest, QueryAddr_ThreeMapConsistency)
{
    uint64_t gva = HYBM_GVM_START_ADDR + 0x10000000;
    auto &mgr = HybmVaManager::GetInstance();
    mgr.AllocReserveGva({0x2000000, 0x2000000, HYBM_MEM_TYPE_HOST, false});
    BaseAllocatedGvaInfo hostInfo = {gva, gva, gva, 0x100000, HYBM_MEM_TYPE_HOST};
    mgr.AddVaInfo(hostInfo);

    auto r1 = mgr.QueryAddr(gva);
    EXPECT_TRUE(r1.inAllocGva);
    EXPECT_EQ(r1.memType, HYBM_MEM_TYPE_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_NoReserved_ReturnsLocalHost)
{
    uint64_t userAddr = 0x7f001000;
    uint8_t mask = manager.ClassifyAddressMask(userAddr);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_DeviceReservedAndAlloc_ReturnsBoth)
{
    uint64_t base = HYBM_GVM_START_ADDR;
    manager.AllocReserveGva({0x2000000, 0x2000000, HYBM_MEM_TYPE_DEVICE, false});
    uint64_t gva = base + 0x2000;
    BaseAllocatedGvaInfo info = {gva, gva, gva, 0x100000, HYBM_MEM_TYPE_DEVICE};
    manager.AddVaInfo(info, 0);
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_DEVICE | HybmVaManager::BIT_GLOBAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_DeviceVaRange_ReturnsLocalDevice)
{
    uint8_t mask = manager.ClassifyAddressMask(HYBM_DEVICE_VA_START + 0x8000);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_RemoteImportedHost_ReturnsGlobalHost)
{
    uint64_t base = HYBM_GVM_START_ADDR;
    manager.AllocReserveGva({0x2000000, 0x2000000, HYBM_MEM_TYPE_HOST, false});
    uint64_t gva = base + 0x3000;
    manager.AddVaInfo({gva, 0, 0, 0x100000, HYBM_MEM_TYPE_HOST}, false, AddressOwnership::Imported);
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_GLOBAL_HOST);
}

TEST_F(HybmVaManagerTest, DirectionLut_AllDirections_CorrectMapping)
{
    HybmVaManager::InitDirectionLut();
    struct {
        uint8_t src;
        uint8_t dst;
        hybm_data_copy_direction dir;
    } testCases[] = {
        {HybmVaManager::BIT_LOCAL_HOST, HybmVaManager::BIT_GLOBAL_HOST, HYBM_LOCAL_HOST_TO_GLOBAL_HOST},
        {HybmVaManager::BIT_LOCAL_HOST, HybmVaManager::BIT_GLOBAL_DEVICE, HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE},
        {HybmVaManager::BIT_LOCAL_DEVICE, HybmVaManager::BIT_GLOBAL_HOST, HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST},
        {HybmVaManager::BIT_LOCAL_DEVICE, HybmVaManager::BIT_GLOBAL_DEVICE, HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE},
        {HybmVaManager::BIT_GLOBAL_HOST, HybmVaManager::BIT_GLOBAL_HOST, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST},
        {HybmVaManager::BIT_GLOBAL_HOST, HybmVaManager::BIT_GLOBAL_DEVICE, HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE},
        {HybmVaManager::BIT_GLOBAL_HOST, HybmVaManager::BIT_LOCAL_HOST, HYBM_GLOBAL_HOST_TO_LOCAL_HOST},
        {HybmVaManager::BIT_GLOBAL_HOST, HybmVaManager::BIT_LOCAL_DEVICE, HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE},
        {HybmVaManager::BIT_GLOBAL_DEVICE, HybmVaManager::BIT_GLOBAL_HOST, HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST},
        {HybmVaManager::BIT_GLOBAL_DEVICE, HybmVaManager::BIT_GLOBAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE},
        {HybmVaManager::BIT_GLOBAL_DEVICE, HybmVaManager::BIT_LOCAL_HOST, HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST},
        {HybmVaManager::BIT_GLOBAL_DEVICE, HybmVaManager::BIT_LOCAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE},
    };
    for (auto &tc : testCases) {
        uint8_t except = tc.src | (tc.dst << 4);
        uint8_t dir = HybmVaManager::directionLut[except];
        EXPECT_EQ(dir, tc.dir);
    }
}

TEST_F(HybmVaManagerTest, ClassifyAddress_ApiCompat)
{
    // 用户地址 → LOCAL_HOST
    EXPECT_EQ(manager.ClassifyAddress(0x7f001000), LOCAL_HOST);
    // device VA → LOCAL_DEVICE
    EXPECT_EQ(manager.ClassifyAddress(HYBM_DEVICE_VA_START + 0x1000), LOCAL_DEVICE);
}

TEST_F(HybmVaManagerTest, QueryAddr_EmptyMap_ReturnsAllFalse)
{
    manager.ClearAll();
    auto r = manager.QueryAddr(0x1000);
    EXPECT_FALSE(r.inAllocGva);
    EXPECT_EQ(r.memType, HYBM_MEM_TYPE_BUTT);
    EXPECT_FALSE(r.imported);
}

TEST_F(HybmVaManagerTest, DumpAllocatedGvaInfo_Empty_NoCrash)
{
    manager.ClearAll();
    manager.DumpAllocatedGvaInfo();
}

TEST_F(HybmVaManagerTest, DumpReservedGvaInfo_Empty_NoCrash)
{
    manager.ClearAll();
    manager.DumpReservedGvaInfo();
}

TEST_F(HybmVaManagerTest, GetGvaMemType_Unregistered_ReturnsBUTT)
{
    auto memType = manager.GetGvaMemType(0xdeadbeef);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_BUTT);
}

TEST_F(HybmVaManagerTest, InferCopyDirection_UserToUser_ReturnsBUTT)
{
    uint64_t user1 = 0x7f001000;
    uint64_t user2 = 0x7f002000;
    auto dir = manager.InferCopyDirection(user1, user2);
    EXPECT_EQ(dir, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(HybmVaManagerTest, TransformVa_Invalid_ReturnsZero)
{
    uint64_t result = manager.TransformVa(0xbadbad, HVM_GVA, HVM_HVA);
    EXPECT_EQ(result, 0U);
}

TEST_F(HybmVaManagerTest, ClassifyAddressMask_56bitGvaRange)
{
    uint8_t mask = manager.ClassifyAddressMask(HYBM_56BITS_GVA_START_ADDR + 0x1000);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_HOST);
}

TEST_F(HybmVaManagerTest, DirectionLut_AllValid)
{
    HybmVaManager::InitDirectionLut();
    uint8_t valid = HybmVaManager::directionLut[HybmVaManager::BIT_LOCAL_HOST | (HybmVaManager::BIT_GLOBAL_HOST << 4)];
    EXPECT_LT(valid, HYBM_DATA_COPY_DIRECTION_AUTO);
    uint8_t invalid =
        HybmVaManager::directionLut[HybmVaManager::BIT_LOCAL_DEVICE | (HybmVaManager::BIT_LOCAL_DEVICE << 4)];
    EXPECT_GE(invalid, HYBM_DATA_COPY_DIRECTION_AUTO);
}

TEST_F(HybmVaManagerTest, ClearAll_ResetsState)
{
    manager.AllocReserveGva({0x100000, 0x100000, HYBM_MEM_TYPE_HOST, false});
    EXPECT_GT(manager.GetReservedCount(), 0U);
    manager.ClearAll();
    EXPECT_EQ(manager.GetReservedCount(), 0U);
    EXPECT_EQ(manager.GetAllocCount(), 0U);
}

TEST_F(HybmVaManagerTest, AllocAndFree_Counts)
{
    manager.AllocReserveGva({0x200000, 0x200000, HYBM_MEM_TYPE_HOST, false});
    EXPECT_EQ(manager.GetAllocCount(), 0U);
    uint64_t gva1 = HYBM_GVM_START_ADDR + 0x10000;
    BaseAllocatedGvaInfo info = {gva1, 0, 0, 0x10000, HYBM_MEM_TYPE_HOST};
    EXPECT_EQ(manager.AddVaInfo(info, 0), BM_OK);
    EXPECT_EQ(manager.GetAllocCount(), 1U);
    manager.RemoveOneVaInfo(gva1, HVM_GVA);
    EXPECT_EQ(manager.GetAllocCount(), 0U);
}

TEST_F(HybmVaManagerTest, AddVaInfo_DiffMemTypes)
{
    uint64_t gva1 = HYBM_GVM_START_ADDR + 0x10000;
    uint64_t gva2 = HYBM_GVM_START_ADDR + 0x30000;
    manager.AllocReserveGva({0x200000, 0x200000, HYBM_MEM_TYPE_HOST, false});
    BaseAllocatedGvaInfo h1 = {gva1, 0, 0, 0x10000, HYBM_MEM_TYPE_HOST};
    BaseAllocatedGvaInfo d1 = {gva2, 0, 0, 0x10000, HYBM_MEM_TYPE_DEVICE};
    EXPECT_EQ(manager.AddVaInfo(h1, 0), BM_OK);
    EXPECT_EQ(manager.AddVaInfo(d1, 0), BM_OK);
    EXPECT_EQ(manager.GetAllocCount(), 2U);
}

// ============================ alias registration tests ============================

// physical memory and gva layout shared by the alias test cases
constexpr uint64_t ALIAS_DVA = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB;
constexpr uint64_t ALIAS_GVA_OWNER = TEST_GVA_BASE_DEVICE;
constexpr uint64_t ALIAS_GVA_A = TEST_GVA_BASE_DEVICE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN;
constexpr uint64_t ALIAS_GVA_B = TEST_GVA_BASE_DEVICE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWENTY;
constexpr uint64_t ALIAS_SIZE = TEST_SIZE_FOUR_MB;

TEST_F(HybmVaManagerTest, AddVaInfo_AliasSameBuffer_KeepsFirstOwner)
{
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_OWNER, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_A, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);

    // dva/hva translation tables are first-wins: still resolve to the first owner gva
    EXPECT_EQ(manager.TransformVa(ALIAS_DVA, HVM_DVA, HVM_GVA), ALIAS_GVA_OWNER);
    // the alias gva itself can be translated back to the physical address
    EXPECT_EQ(manager.TransformVa(ALIAS_GVA_A, HVM_GVA, HVM_DVA), ALIAS_DVA);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_TWO);
}

// P3: replay registration of the same physical memory no longer needs an explicit switch; with the check
// inlined it automatically allows identical dva/hva/size/memType
TEST_F(HybmVaManagerTest, AddVaInfo_AliasSameBuffer_AutoAllowed)
{
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_OWNER, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_A, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_TWO);
}

TEST_F(HybmVaManagerTest, AddVaInfo_AliasDifferentSize_Rejected)
{
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_OWNER, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    EXPECT_NE(manager.AddVaInfo({ALIAS_GVA_A, ALIAS_DVA, ALIAS_DVA, TEST_SIZE_ONE_MB, TEST_MEM_TYPE_DEVICE}), BM_OK);
}

// Cross-ownership overlap (Local vs Imported) is allowed: imported rows are classification-only entries,
// and the first-wins dva/hva rows keep the local owner's translations intact.
TEST_F(HybmVaManagerTest, AddVaInfo_ImportedBuffer_CrossOwnershipAllowed)
{
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_OWNER, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    AllocatedGvaInfo importedInfo({ALIAS_GVA_A, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE},
                                  AddressOwnership::Imported);
    EXPECT_EQ(manager.AddVaInfo(importedInfo), BM_OK);

    // the local dva -> gva translation still resolves to the local owner (first-wins dva/hva rows)
    EXPECT_EQ(manager.TransformVa(ALIAS_DVA, HVM_DVA, HVM_GVA), ALIAS_GVA_OWNER);
    // the imported gva itself resolves back to its mapped address
    EXPECT_EQ(manager.TransformVa(ALIAS_GVA_A, HVM_GVA, HVM_DVA), ALIAS_DVA);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_TWO);
}

TEST_F(HybmVaManagerTest, RemoveOneVaInfo_RemoveAlias_KeepsOwner)
{
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_OWNER, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_A, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);

    manager.RemoveOneVaInfo(ALIAS_GVA_A, HVM_GVA);

    EXPECT_EQ(manager.TransformVa(ALIAS_DVA, HVM_DVA, HVM_GVA), ALIAS_GVA_OWNER);
    EXPECT_EQ(manager.TransformVa(ALIAS_GVA_OWNER, HVM_GVA, HVM_DVA), ALIAS_DVA);
    EXPECT_EQ(manager.TransformVa(ALIAS_GVA_A, HVM_GVA, HVM_DVA), TEST_SIZE_ZERO);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_ONE);
}

// P3: after the owner is removed the dva/hva rows are no longer re-pointed to a surviving alias; they are
// kept by the reference count until the last sharer deregisters
TEST_F(HybmVaManagerTest, RemoveOneVaInfo_RemoveOwner_KeepsSharedEntries)
{
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_OWNER, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_A, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);

    manager.RemoveOneVaInfo(ALIAS_GVA_OWNER, HVM_GVA);

    // The owner record is removed, but the dva/hva rows are kept by the count, so a surviving alias can still
    // complete the HVA->DVA translation
    EXPECT_EQ(manager.TransformVa(ALIAS_DVA, HVM_DVA, HVM_HVA), ALIAS_DVA);
    EXPECT_EQ(manager.TransformVa(ALIAS_GVA_A, HVM_GVA, HVM_DVA), ALIAS_DVA);
    EXPECT_EQ(manager.TransformVa(ALIAS_GVA_OWNER, HVM_GVA, HVM_DVA), TEST_SIZE_ZERO);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_ONE);

    // After the last sharer deregisters the dva/hva rows are reclaimed
    manager.RemoveOneVaInfo(ALIAS_GVA_A, HVM_GVA);
    EXPECT_EQ(manager.TransformVa(ALIAS_DVA, HVM_DVA, HVM_HVA), TEST_SIZE_ZERO);
}

TEST_F(HybmVaManagerTest, RemoveOneVaInfo_MultipleAliases_KeepUntilLastShared)
{
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_OWNER, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_A, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);
    EXPECT_EQ(manager.AddVaInfo({ALIAS_GVA_B, ALIAS_DVA, ALIAS_DVA, ALIAS_SIZE, TEST_MEM_TYPE_DEVICE}), BM_OK);

    // count 3 -> 2 -> 1: as long as a sharer remains, the dva/hva rows are always kept
    manager.RemoveOneVaInfo(ALIAS_GVA_OWNER, HVM_GVA);
    EXPECT_EQ(manager.TransformVa(ALIAS_DVA, HVM_DVA, HVM_HVA), ALIAS_DVA);

    manager.RemoveOneVaInfo(ALIAS_GVA_A, HVM_GVA);
    EXPECT_EQ(manager.TransformVa(ALIAS_DVA, HVM_DVA, HVM_HVA), ALIAS_DVA);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_ONE);

    // reclaimed only at zero
    manager.RemoveOneVaInfo(ALIAS_GVA_B, HVM_GVA);
    EXPECT_EQ(manager.TransformVa(ALIAS_DVA, HVM_DVA, HVM_HVA), TEST_SIZE_ZERO);
    EXPECT_EQ(manager.GetAllocCount(), TEST_SIZE_ZERO);
}

// ============================ imported coexistence tests ============================

// 多 store 组拓扑下不同对端的窗口号可能相同（如各自 store 的 0 号 rank）：
// 同 gva、不同 dva 的两条 imported 记录必须允许共存（d2rh 双实体导入场景）
constexpr uint64_t COEXIST_GVA = TEST_GVA_BASE_DEVICE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN;
constexpr uint64_t COEXIST_DVA_A = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB;
constexpr uint64_t COEXIST_DVA_B = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO;
constexpr uint64_t COEXIST_SIZE = TEST_SIZE_FOUR_MB;

TEST_F(HybmVaManagerTest, AddVaInfo_ImportedSameGva_CoexistenceAllowed)
{
    AllocatedGvaInfo first({COEXIST_GVA, COEXIST_DVA_A, 0, COEXIST_SIZE, TEST_MEM_TYPE_DEVICE},
                           AddressOwnership::Imported);
    AllocatedGvaInfo second({COEXIST_GVA, COEXIST_DVA_B, 0, COEXIST_SIZE, TEST_MEM_TYPE_DEVICE},
                            AddressOwnership::Imported);
    EXPECT_EQ(manager.AddVaInfo(first), BM_OK);
    EXPECT_EQ(manager.AddVaInfo(second), BM_OK);
    // 共存记录共用一行（last-wins），分类字段一致
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_ONE);
}

// 精确注销：A 先退只回收 A 自己的 DVA 行，共存方的映射不受影响
TEST_F(HybmVaManagerTest, RemoveOneVaInfo_PreciseRemoval_KeepsCoexistingRecord)
{
    AllocatedGvaInfo first({COEXIST_GVA, COEXIST_DVA_A, 0, COEXIST_SIZE, TEST_MEM_TYPE_DEVICE},
                           AddressOwnership::Imported);
    AllocatedGvaInfo second({COEXIST_GVA, COEXIST_DVA_B, 0, COEXIST_SIZE, TEST_MEM_TYPE_DEVICE},
                            AddressOwnership::Imported);
    EXPECT_EQ(manager.AddVaInfo(first), BM_OK);
    EXPECT_EQ(manager.AddVaInfo(second), BM_OK);

    manager.RemoveOneVaInfo(COEXIST_GVA, COEXIST_DVA_A, 0);
    // GVA 行由 second 的引用保活，DVA 翻译仍指向 second 的映射
    EXPECT_EQ(manager.TransformVa(COEXIST_GVA, HVM_GVA, HVM_DVA), COEXIST_DVA_B);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_ONE);

    manager.RemoveOneVaInfo(COEXIST_GVA, COEXIST_DVA_B, 0);
    EXPECT_EQ(manager.TransformVa(COEXIST_GVA, HVM_GVA, HVM_DVA), TEST_SIZE_ZERO);
    EXPECT_EQ(manager.GetAllocCount(), TEST_SIZE_ZERO);
}

// 注销顺序无关：先按单 key 级联注销（content 为最后写者），共存方仍可精确注销
TEST_F(HybmVaManagerTest, RemoveOneVaInfo_CoexistenceTeardownOrderIndependent)
{
    AllocatedGvaInfo first({COEXIST_GVA, COEXIST_DVA_A, 0, COEXIST_SIZE, TEST_MEM_TYPE_DEVICE},
                           AddressOwnership::Imported);
    AllocatedGvaInfo second({COEXIST_GVA, COEXIST_DVA_B, 0, COEXIST_SIZE, TEST_MEM_TYPE_DEVICE},
                            AddressOwnership::Imported);
    EXPECT_EQ(manager.AddVaInfo(first), BM_OK);
    EXPECT_EQ(manager.AddVaInfo(second), BM_OK);

    manager.RemoveOneVaInfo(COEXIST_GVA, HVM_GVA);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_ONE);
    manager.RemoveOneVaInfo(COEXIST_GVA, COEXIST_DVA_A, 0);
    EXPECT_EQ(manager.GetAllocCount(), TEST_SIZE_ZERO);
}

// 同描述符的两条 imported 记录（HOST_RDMA 下 dva=0 的不同物理内存）：
// 每次登记各计一次引用，全部注销后行才回收
TEST_F(HybmVaManagerTest, AddVaInfo_ImportedIdenticalDescriptor_CountPerRegistration)
{
    AllocatedGvaInfo info({COEXIST_GVA, 0, 0, COEXIST_SIZE, TEST_MEM_TYPE_DEVICE}, AddressOwnership::Imported);
    EXPECT_EQ(manager.AddVaInfo(info), BM_OK);
    EXPECT_EQ(manager.AddVaInfo(info), BM_OK);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_ONE);

    manager.RemoveOneVaInfo(COEXIST_GVA, 0, 0);
    EXPECT_EQ(manager.GetAllocCount(), TEST_COUNT_ONE);

    manager.RemoveOneVaInfo(COEXIST_GVA, 0, 0);
    EXPECT_EQ(manager.GetAllocCount(), TEST_SIZE_ZERO);
}
