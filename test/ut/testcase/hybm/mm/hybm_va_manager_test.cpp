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

// 并发测试常量
constexpr int TEST_THREAD_COUNT_FOUR = 4;
constexpr int TEST_THREAD_COUNT_EIGHT = 8;
constexpr int TEST_OPERATIONS_PER_THREAD_TEN = 10;
constexpr int TEST_OPERATIONS_PER_THREAD_FIFTY = 50;

class HybmVaManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        manager.ClearAll();
        manager.Initialize(TEST_SOC_910B);
    }

    void TearDown() override
    {
        manager.ClearAll();
    }

    HybmVaManager &manager = HybmVaManager::GetInstance();
};

// ============================ 测试用例 ============================

// 测试1: Initialize 方法
TEST_F(HybmVaManagerTest, Initialize_ValidSocType_ReturnsSuccess)
{
    EXPECT_TRUE(manager.Initialize(TEST_SOC_910) == BM_OK);
    EXPECT_TRUE(manager.Initialize(TEST_SOC_910B) == BM_OK);
}

// 测试2: AddVaInfoFromExternal 基本功能
TEST_F(HybmVaManagerTest, AddRegisterVaInfo_ValidParameters_ReturnsSuccess)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0 ,lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_ONE);
    EXPECT_TRUE(manager.IsValidAddr(gva));
}

// 测试3: AddVaInfoFromExternal 参数为0
TEST_F(HybmVaManagerTest, AddRegisterVaInfo_ZeroParameters_ReturnsInvalidParam)
{
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{0, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    EXPECT_TRUE(
        manager.AddVaInfoFromExternal({{TEST_GVA_BASE_HOST, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                      TEST_RANK_ZERO) == BM_OK);
}

// 测试4: AddVaInfoFromExternal 地址重叠
TEST_F(HybmVaManagerTest, AddRegisterVaInfo_OverlappingAddress_ReturnsAddrOverlap)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    EXPECT_TRUE(
        manager.AddVaInfoFromExternal({{gva, 0, lva + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_FOUR_MB, TEST_MEM_TYPE_HOST},
                                      TEST_RANK_ZERO) != BM_OK);
}

// 测试5: AddVaInfo 基本功能
TEST_F(HybmVaManagerTest, AddSelfVaInfo_ValidParameters_ReturnsSuccess)
{
    uint64_t gva = TEST_GVA_BASE_DEVICE;
    uint64_t lva = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB;

    EXPECT_TRUE(manager.AddVaInfo({gva, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE, lva}, TEST_RANK_ONE) == BM_OK);

    auto [allocInfo, found] = manager.FindAllocByVa(gva, HVM_GVA);
    EXPECT_TRUE(found);
    EXPECT_TRUE(allocInfo.RankId() == TEST_RANK_ONE);
    EXPECT_TRUE(allocInfo.base.memType == TEST_MEM_TYPE_DEVICE);
}

// 测试6: 地址转换功能
TEST_F(HybmVaManagerTest, AddressConversion_ValidAddresses_ReturnsCorrectValues)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

    EXPECT_TRUE(manager.TransformVa(lva, HVM_HVA, HVM_GVA) == gva);
    EXPECT_TRUE(manager.TransformVa(gva, HVM_GVA, HVM_HVA) == lva);

    uint64_t internalGva = gva + TEST_OFFSET_HALF_MB;
    uint64_t internalLva = lva + TEST_OFFSET_HALF_MB;

    EXPECT_TRUE(manager.TransformVa(internalLva, HVM_HVA, HVM_GVA) == internalGva);
    EXPECT_TRUE(manager.TransformVa(internalGva, HVM_GVA, HVM_HVA) == internalLva);
}

// 测试7: 地址转换 - 不存在的地址
TEST_F(HybmVaManagerTest, AddressConversion_NonExistentAddress_ReturnsZero)
{
    uint64_t nonExistentLva = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN;
    uint64_t nonExistentGva = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN;
    EXPECT_TRUE(manager.TransformVa(nonExistentLva, HVM_HVA, HVM_GVA) == TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.TransformVa(nonExistentGva, HVM_GVA, HVM_HVA) == TEST_SIZE_ZERO);
}

// 测试9: GetMemType 功能测试
TEST_F(HybmVaManagerTest, GetMemType_ValidAddresses_ReturnsCorrectMemType)
{
    uint64_t hostGva = TEST_GVA_BASE_HOST;
    uint64_t deviceGva = TEST_GVA_BASE_DEVICE;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{hostGva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    BaseAllocatedGvaInfo info = {{deviceGva, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB, 0},
                                 TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE};
    auto ret = manager.AddVaInfo(info, TEST_RANK_ONE);
    EXPECT_TRUE(ret == BM_OK);

    EXPECT_TRUE(manager.GetGvaMemType(hostGva) == TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(manager.GetGvaMemType(deviceGva) == TEST_MEM_TYPE_DEVICE);
}

// 测试11: IsValidAddr 功能测试
TEST_F(HybmVaManagerTest, IsValidAddr_ValidAndInvalidAddresses_ReturnsCorrectBool)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

    // GVA 地址命中 GVA map → 有效
    EXPECT_TRUE(manager.IsValidAddr(gva));
    EXPECT_TRUE(manager.IsValidAddr(gva + TEST_OFFSET_HALF_MB));

    // HVA 地址（lva）不在 GVA map 中 → IsValidAddr 返回 false
    EXPECT_FALSE(manager.IsValidAddr(lva));
    EXPECT_FALSE(manager.IsValidAddr(lva + TEST_OFFSET_HALF_MB));

    EXPECT_FALSE(manager.IsValidAddr(gva + TEST_SIZE_SIXTEEN_MB));
    EXPECT_FALSE(manager.IsValidAddr(TEST_SIZE_ZERO));
}

// 测试12: FindAllocByGva 功能测试
TEST_F(HybmVaManagerTest, FindAllocByGva_ValidAndInvalidAddresses_ReturnsCorrectResult)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

    auto [allocInfo1, found1] = manager.FindAllocByVa(gva, HVM_GVA);
    EXPECT_TRUE(found1);
    EXPECT_TRUE(allocInfo1.base.va[HVM_GVA] == gva);
    EXPECT_TRUE(allocInfo1.base.va[HVM_HVA] == lva);

    auto [allocInfo2, found2] = manager.FindAllocByVa(gva + TEST_OFFSET_HALF_MB, HVM_GVA);
    EXPECT_TRUE(found2);
    EXPECT_TRUE(allocInfo2.base.va[HVM_GVA] == gva);

    auto [allocInfo3, found3] = manager.FindAllocByVa(TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TEN,
                                                      HVM_GVA);
    EXPECT_FALSE(found3);
}

// 测试13: FindAllocByLva 功能测试
TEST_F(HybmVaManagerTest, FindAllocByLva_ValidAndInvalidAddresses_ReturnsCorrectResult)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

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

// 测试14: AllocReserveGva 基本功能
TEST_F(HybmVaManagerTest, AllocReserveGva_ValidParameters_ReturnsReservedInfo)
{
    ReservedGvaInfo reserved = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);

    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(reserved.size == TEST_SIZE_SIXTEEN_MB);
    EXPECT_TRUE(reserved.memType == TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved.localRankId == TEST_RANK_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_ONE);
}

// 测试15: AllocReserveGva 重复分配
TEST_F(HybmVaManagerTest, AllocReserveGva_DuplicateAllocation_ReturnsEmptyReservedInfo)
{
    ReservedGvaInfo reserved1 = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved1.va[HVM_GVA] != TEST_SIZE_ZERO);

    ReservedGvaInfo reserved2 = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved2.va[HVM_GVA] != TEST_SIZE_ZERO);
}

// 测试16: AllocReserveGva 不同Rank分配
TEST_F(HybmVaManagerTest, AllocReserveGva_DifferentRanks_ReturnsReservedInfo)
{
    ReservedGvaInfo reserved1 = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved1.va[HVM_GVA] != TEST_SIZE_ZERO);

    ReservedGvaInfo reserved2 = manager.AllocReserveGva(TEST_RANK_ONE, TEST_SIZE_SIXTEEN_MB,
                                                        TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved2.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_TWO);
}

// 测试17: AllocReserveGva 不同内存类型分配
TEST_F(HybmVaManagerTest, AllocReserveGva_DifferentMemTypes_ReturnsReservedInfo)
{
    ReservedGvaInfo reserved1 = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved1.va[HVM_GVA] != TEST_SIZE_ZERO);

    ReservedGvaInfo reserved2 = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE);
    EXPECT_TRUE(reserved2.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_TWO);
}

// 测试18: FreeReserveGva 功能测试
TEST_F(HybmVaManagerTest, FreeReserveGva_ValidAddress_RemovesReservation)
{
    ReservedGvaInfo reserved = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_ONE);

    manager.FreeReserveGva(reserved.va[HVM_GVA]);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

// 测试19: FreeReserveGva 无效地址
TEST_F(HybmVaManagerTest, FreeReserveGva_InvalidAddress_NoEffect)
{
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
    manager.FreeReserveGva(TEST_GVA_BASE_HOST);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

// 测试20: RemoveOneVaInfo 功能测试
TEST_F(HybmVaManagerTest, RemoveOneVaInfo_ValidAddress_RemovesAllocation)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_ONE);

    manager.RemoveOneVaInfo(gva, HVM_GVA);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);
}

// 测试22: RemoveAllVaInfoByRank 功能测试
TEST_F(HybmVaManagerTest, RemoveAllVaInfoByRank_ValidRank_RemovesAllAllocations)
{
    uint64_t gva1 = TEST_GVA_BASE_HOST;
    uint64_t gva2 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    uint64_t gva3 = TEST_GVA_BASE_DEVICE;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva1, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    BaseAllocatedGvaInfo info0 = {{gva2, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB},
                                  TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST};
    EXPECT_TRUE(manager.AddVaInfoFromExternal(info0, TEST_RANK_ZERO) == BM_OK);
    auto lva = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO;
    BaseAllocatedGvaInfo info = {{gva3, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE};
    auto ret = manager.AddVaInfo(info, TEST_RANK_ONE);
    EXPECT_TRUE(ret == BM_OK);

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_THREE);
}

// 测试24: GetAllocCount 功能测试
TEST_F(HybmVaManagerTest, GetAllocCount_MultipleAllocations_ReturnsCorrectCount)
{
    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);

    for (size_t i = TEST_INDEX_ZERO; i < TEST_COUNT_FIVE; i++) {
        uint64_t gva = TEST_GVA_BASE_HOST + i * TEST_OFFSET_SIXTEEN_MB;
        uint64_t lva = TEST_LVA_BASE + i * TEST_OFFSET_SIXTEEN_MB;
        EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                                  TEST_RANK_ZERO) == BM_OK);
    }

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_FIVE);
}

// 测试25: GetReservedCount 功能测试
TEST_F(HybmVaManagerTest, GetReservedCount_MultipleReservations_ReturnsCorrectCount)
{
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);

    for (size_t i = TEST_INDEX_ZERO; i < TEST_COUNT_THREE; i++) {
        manager.AllocReserveGva(i, TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    }

    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_THREE);
}

// 测试26: ClearAll 功能测试
TEST_F(HybmVaManagerTest, ClearAll_MultipleAllocationsAndReservations_ClearsAll)
{
    for (size_t i = TEST_INDEX_ZERO; i < TEST_COUNT_FIVE; i++) {
        uint64_t gva = TEST_GVA_BASE_HOST + i * TEST_OFFSET_SIXTEEN_MB;
        uint64_t lva = TEST_LVA_BASE + i * TEST_OFFSET_SIXTEEN_MB;
        EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                                  TEST_RANK_ZERO) == BM_OK);
    }

    for (size_t i = TEST_INDEX_ZERO; i < TEST_COUNT_THREE; i++) {
        manager.AllocReserveGva(i, TEST_SIZE_SIXTEEN_MB, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    }

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_FIVE);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_THREE);

    manager.ClearAll();

    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

// 测试27: FormatMemorySize 静态方法测试
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

// 测试28: PrintAllReservedGvaInfo 不崩溃测试
TEST_F(HybmVaManagerTest, PrintAllReservedGvaInfo_EmptyAndNonEmpty_DoesNotCrash)
{
    EXPECT_NO_THROW(manager.DumpReservedGvaInfo());
    manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_NO_THROW(manager.DumpReservedGvaInfo());
}

// 测试29: PrintAllAllocGvaInfo 不崩溃测试
TEST_F(HybmVaManagerTest, PrintAllAllocGvaInfo_EmptyAndNonEmpty_DoesNotCrash)
{
    EXPECT_NO_THROW(manager.DumpAllocatedGvaInfo());
    EXPECT_TRUE(
        manager.AddVaInfoFromExternal({{TEST_GVA_BASE_HOST, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                      TEST_RANK_ZERO) == BM_OK);
    EXPECT_NO_THROW(manager.DumpAllocatedGvaInfo());
}

// 测试30: 边界条件 - 最小大小
TEST_F(HybmVaManagerTest, BoundaryCondition_MinimumSize_ReturnsSuccess)
{
    EXPECT_TRUE(
        manager.AddVaInfoFromExternal({{TEST_GVA_BASE_HOST, 0, TEST_LVA_BASE}, TEST_SIZE_ONE_BYTE, TEST_MEM_TYPE_HOST},
                                      TEST_RANK_ZERO) == BM_OK);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_ONE);
}

// 测试31: 边界条件 - 不同内存类型不重叠
TEST_F(HybmVaManagerTest, BoundaryCondition_DifferentMemTypes_NoOverlap)
{
    uint64_t hostGva = TEST_GVA_BASE_HOST;
    uint64_t deviceGva = TEST_GVA_BASE_DEVICE;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{hostGva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    BaseAllocatedGvaInfo base = {{deviceGva, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB},
                                 TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE};
    EXPECT_TRUE(manager.AddVaInfoFromExternal(base, TEST_RANK_ZERO) == BM_OK);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_TWO);
}

// 测试32: 并发测试 - 多线程添加
TEST_F(HybmVaManagerTest, ConcurrentTest_MultipleThreadsAdding_NoDataRace)
{
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    for (int i = 0; i < TEST_THREAD_COUNT_FOUR; i++) {
        threads.emplace_back([this, i, &successCount]() {
            for (int j = 0; j < TEST_OPERATIONS_PER_THREAD_TEN; j++) {
                uint64_t gva = TEST_GVA_BASE_HOST + (i * TEST_OPERATIONS_PER_THREAD_TEN + j) * TEST_OFFSET_SIXTEEN_MB;
                uint64_t lva = TEST_LVA_BASE + (i * TEST_OPERATIONS_PER_THREAD_TEN + j) * TEST_OFFSET_SIXTEEN_MB;

                if (manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                                  i % TEST_COUNT_THREE) == BM_OK) {
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

// 测试33: 混合操作测试
TEST_F(HybmVaManagerTest, MixedOperations_AddRemoveQuery_WorksCorrectly)
{
    uint64_t gva1 = TEST_GVA_BASE_HOST;
    uint64_t gva2 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    uint64_t gva3 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva1, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    EXPECT_TRUE(
        manager.AddVaInfo({{gva2, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                          TEST_RANK_ONE) == BM_OK);
    ReservedGvaInfo reserved = manager.AllocReserveGva(TEST_RANK_TWO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE);
    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);

    manager.RemoveOneVaInfo(gva1, HVM_GVA);

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva3, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO},
                                               TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_THREE) == BM_OK);

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_TWO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_ONE);

    manager.FreeReserveGva(reserved.va[HVM_GVA]);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

// 测试34: RemoveOneVaInfo - 验证删除功能
TEST_F(HybmVaManagerTest, RemoveAllVaInfoByRank_RemovesAllAllocations)
{
    uint64_t gva1 = TEST_GVA_BASE_HOST;
    uint64_t gva2 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    uint64_t gva3 = TEST_GVA_BASE_DEVICE;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva1, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    BaseAllocatedGvaInfo info0 = {{gva2, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB},
                                  TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST};
    EXPECT_TRUE(manager.AddVaInfoFromExternal(info0, TEST_RANK_ZERO) == BM_OK);
    auto lva = TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB * TEST_COUNT_TWO;
    BaseAllocatedGvaInfo info = {{gva3, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE};
    auto ret = manager.AddVaInfo(info, TEST_RANK_ONE);
    EXPECT_TRUE(ret == BM_OK);

    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_THREE);
    manager.RemoveOneVaInfo(gva1, HVM_GVA);
    manager.RemoveOneVaInfo(gva2, HVM_GVA);
    manager.RemoveOneVaInfo(gva3, HVM_GVA);
    EXPECT_TRUE(manager.GetAllocCount() == 0);
}

// 测试36: GetMemType - 边界地址
TEST_F(HybmVaManagerTest, GetMemType_BoundaryAddresses)
{
    uint64_t hostGva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{hostGva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

    EXPECT_TRUE(manager.GetGvaMemType(hostGva) == TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(manager.GetGvaMemType(hostGva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE) == TEST_MEM_TYPE_HOST);
}

// 测试37: AllocReserveGva - 零大小
TEST_F(HybmVaManagerTest, AllocReserveGva_ZeroSize)
{
    ReservedGvaInfo reserved = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_ZERO, TEST_SIZE_ZERO,
                                                       TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved.va[HVM_GVA] == TEST_SIZE_ZERO);
}

// 测试38: AllocReserveGva - 最大rank
TEST_F(HybmVaManagerTest, AllocReserveGva_MaxRank)
{
    ReservedGvaInfo reserved = manager.AllocReserveGva(TEST_RANK_INVALID, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(reserved.localRankId == TEST_RANK_INVALID);
}

// 测试39: AddVaInfo - 零大小
TEST_F(HybmVaManagerTest, AddVaInfo_ZeroSize)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfo({{gva, 0, TEST_LVA_BASE}, TEST_SIZE_ZERO, TEST_MEM_TYPE_HOST},
                                  TEST_RANK_ZERO) == BM_OK);
}

// 测试40: FindAllocByGva - 边界情况
TEST_F(HybmVaManagerTest, FindAllocByGva_BoundaryCases)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

    auto [alloc1, found1] = manager.FindAllocByVa(gva, HVM_GVA);
    EXPECT_TRUE(found1);

    auto [alloc2, found2] = manager.FindAllocByVa(gva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE, HVM_GVA);
    EXPECT_TRUE(found2);

    auto [alloc3, found3] = manager.FindAllocByVa(gva - TEST_COUNT_ONE, HVM_GVA);
    EXPECT_FALSE(found3);

    auto [alloc4, found4] = manager.FindAllocByVa(gva + TEST_SIZE_SIXTEEN_MB, HVM_GVA);
    EXPECT_FALSE(found4);
}

// 测试41: FindAllocByLva - 边界情况
TEST_F(HybmVaManagerTest, FindAllocByLva_BoundaryCases)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

    auto [alloc1, found1] = manager.FindAllocByVa(lva, HVM_HVA);
    EXPECT_TRUE(found1);

    auto [alloc2, found2] = manager.FindAllocByVa(lva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE, HVM_HVA);
    EXPECT_TRUE(found2);

    auto [alloc3, found3] = manager.FindAllocByVa(lva - TEST_COUNT_ONE, HVM_HVA);
    EXPECT_FALSE(found3);

    auto [alloc4, found4] = manager.FindAllocByVa(lva + TEST_SIZE_SIXTEEN_MB, HVM_HVA);
    EXPECT_FALSE(found4);
}

// 测试42: RemoveOneVaInfo - 不存在的地址
TEST_F(HybmVaManagerTest, RemoveOneVaInfo_NonExistentAddress)
{
    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);
    manager.RemoveOneVaInfo(TEST_GVA_BASE_HOST);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_SIZE_ZERO);
}

// 测试43: FreeReserveGva - 多次释放
TEST_F(HybmVaManagerTest, FreeReserveGva_MultipleFrees)
{
    ReservedGvaInfo reserved = manager.AllocReserveGva(TEST_RANK_ZERO, TEST_SIZE_SIXTEEN_MB,
                                                       TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST);
    EXPECT_TRUE(reserved.va[HVM_GVA] != TEST_SIZE_ZERO);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_COUNT_ONE);

    manager.FreeReserveGva(reserved.va[HVM_GVA]);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);

    manager.FreeReserveGva(reserved.va[HVM_GVA]);
    EXPECT_TRUE(manager.GetReservedCount() == TEST_SIZE_ZERO);
}

// 测试44: 地址转换 - 边界偏移
TEST_F(HybmVaManagerTest, AddressConversion_BoundaryOffsets)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

    EXPECT_TRUE(manager.TransformVa(lva, HVM_HVA, HVM_GVA) == gva);
    EXPECT_TRUE(manager.TransformVa(gva, HVM_GVA, HVM_HVA) == lva);

    EXPECT_TRUE(manager.TransformVa(lva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE, HVM_HVA, HVM_GVA) ==
                gva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE);
    EXPECT_TRUE(manager.TransformVa(gva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE, HVM_GVA, HVM_HVA) ==
                lva + TEST_SIZE_SIXTEEN_MB - TEST_COUNT_ONE);
}

// 测试46: IsValidAddr - 边界值
TEST_F(HybmVaManagerTest, IsValidAddr_BoundaryValues)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

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

// 测试47: 并发测试 - 多线程查询
TEST_F(HybmVaManagerTest, ConcurrentTest_MultipleThreadsQuerying)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);

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

// 测试48: 混合操作 - 添加、删除、查询混合
TEST_F(HybmVaManagerTest, MixedOperations_AddRemoveQueryMixed)
{
    uint64_t gva1 = TEST_GVA_BASE_HOST;
    uint64_t gva2 = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;

    EXPECT_TRUE(manager.AddVaInfoFromExternal({{gva1, 0, TEST_LVA_BASE}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                                              TEST_RANK_ZERO) == BM_OK);
    EXPECT_TRUE(
        manager.AddVaInfo({{gva2, 0, TEST_LVA_BASE + TEST_OFFSET_SIXTEEN_MB}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST},
                          TEST_RANK_ONE) == BM_OK);

    manager.RemoveOneVaInfo(gva1);
    EXPECT_TRUE(manager.GetAllocCount() == TEST_COUNT_ONE);
}

// ======================== QueryAddr 测试 ========================

// 测试49: QueryAddr 查询 GVA 命中的本地地址
TEST_F(HybmVaManagerTest, QueryAddr_AllocGvaLocal_ReturnsInAllocGva)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(
        manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    auto r = manager.QueryAddr(gva);
    EXPECT_TRUE(r.inAllocGva);
    EXPECT_EQ(r.memType, TEST_MEM_TYPE_HOST);
    EXPECT_EQ(r.importedRankId, INVALID_RANK_ID);
}

// 测试50: QueryAddr 查询 GVA 命中的 import 地址
TEST_F(HybmVaManagerTest, QueryAddr_AllocGvaImported_ReturnsImported)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfoFromExternal(
        {{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO, TEST_RANK_ONE) == BM_OK);
    auto r = manager.QueryAddr(gva);
    EXPECT_TRUE(r.inAllocGva);
    EXPECT_EQ(r.importedRankId, TEST_RANK_ONE);
}

// 测试51: QueryAddr 查询未注册地址
TEST_F(HybmVaManagerTest, QueryAddr_NoAlloc_ReturnsAllFalse)
{
    uint64_t unregisteredAddr = 0x12345678;
    auto r = manager.QueryAddr(unregisteredAddr);
    EXPECT_FALSE(r.inAllocGva);
}

// 测试52: QueryAddr 只查 GVA map，HVA 地址不命中
TEST_F(HybmVaManagerTest, QueryAddr_AllocHva_ReturnsNoHit)
{
    uint64_t lva = TEST_LVA_BASE;
    EXPECT_TRUE(
        manager.AddVaInfo({{0, 0, lva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    auto r = manager.QueryAddr(lva);
    EXPECT_FALSE(r.inAllocGva);
}

// 测试53: QueryAddr 只查 GVA map，DVA 地址不命中
TEST_F(HybmVaManagerTest, QueryAddr_AllocDva_ReturnsNoHit)
{
    uint64_t dva = TEST_LVA_BASE + 0x200000000;
    EXPECT_TRUE(
        manager.AddVaInfo({{0, dva, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE}, TEST_RANK_ZERO) == BM_OK);
    auto r = manager.QueryAddr(dva);
    EXPECT_FALSE(r.inAllocGva);
}

// 测试54: QueryAddr GVA 命中
TEST_F(HybmVaManagerTest, QueryAddr_GvaHit)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(
        manager.AddVaInfo({{gva, 0, gva}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    auto r = manager.QueryAddr(gva);
    EXPECT_TRUE(r.inAllocGva);
}

// ======================== ClassifyAddress 测试 ========================

// 测试55: ClassifyAddress 本地 GVA → LOCAL_HOST
TEST_F(HybmVaManagerTest, ClassifyAddress_LocalGva_ReturnsLocalHost)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(
        manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    EXPECT_EQ(manager.ClassifyAddress(gva), LOCAL_HOST);
}

// 测试56: ClassifyAddress import 远端 GVA → GLOBAL_HOST
TEST_F(HybmVaManagerTest, ClassifyAddress_ImportedGva_ReturnsGlobalHost)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfoFromExternal(
        {{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO, TEST_RANK_ONE) == BM_OK);
    EXPECT_EQ(manager.ClassifyAddress(gva), GLOBAL_HOST);
}

// 测试57: ClassifyAddress 本地 DEVICE GVA → LOCAL_DEVICE（在 device VA 范围）
TEST_F(HybmVaManagerTest, ClassifyAddress_DeviceVaRange_ReturnsLocalDevice)
{
    uint64_t deviceAddr = HYBM_DEVICE_VA_START + 0x1000;
    // 不注册，仅测试 device VA 范围兜底
    EXPECT_EQ(manager.ClassifyAddress(deviceAddr), LOCAL_DEVICE);
}

// 测试58: ClassifyAddress import 远端 DEVICE GVA → GLOBAL_DEVICE
TEST_F(HybmVaManagerTest, ClassifyAddress_ImportedDeviceGva_ReturnsGlobalDevice)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfoFromExternal(
        {{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_DEVICE}, TEST_RANK_ZERO, TEST_RANK_ONE) == BM_OK);
    EXPECT_EQ(manager.ClassifyAddress(gva), GLOBAL_DEVICE);
}

// 测试59: ClassifyAddress 未注册地址 → LOCAL_HOST
TEST_F(HybmVaManagerTest, ClassifyAddress_Unregistered_ReturnsLocalHost)
{
    EXPECT_EQ(manager.ClassifyAddress(0x12345678), LOCAL_HOST);
}

// ======================== InferCopyDirection 测试 ========================

// 测试60: InferCopyDirection 本地 HOST → 本地 HOST → H2GH(0) fallback
TEST_F(HybmVaManagerTest, InferCopyDirection_LocalHostToLocalHost_FallbackToH2GH)
{
    uint64_t gvmAddr = HYBM_GVM_START_ADDR + 0x1000;
    manager.AddVaInfo(BaseAllocatedGvaInfo{{gvmAddr, 0, 0}, 0x100000, HYBM_MEM_TYPE_HOST}, 0);
    auto dir = manager.InferCopyDirection(0x1000, gvmAddr);
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST);
}

// 测试61: InferCopyDirection 本地 DEVICE → 本地 HOST → table[2][3]=D2GH(2)
TEST_F(HybmVaManagerTest, InferCopyDirection_LocalDeviceToLocalHost_ReturnsD2GH)
{
    uint64_t deviceAddr = HYBM_DEVICE_VA_START + 0x1000;
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(
        manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    auto dir = manager.InferCopyDirection(deviceAddr, gva);
    EXPECT_EQ(dir, HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST);
}

// 测试62: InferCopyDirection 本地 HOST → 本地 DEVICE → table[3][2]=H2GD(1)
TEST_F(HybmVaManagerTest, InferCopyDirection_LocalHostToLocalDevice_ReturnsH2GD)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    uint64_t deviceAddr = HYBM_DEVICE_VA_START + 0x1000;
    EXPECT_TRUE(
        manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    auto dir = manager.InferCopyDirection(gva, deviceAddr);
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE);
}

// 测试63: InferCopyDirection import 远端 HOST → 本地 HOST → H2GH/SRC
TEST_F(HybmVaManagerTest, InferCopyDirection_GlobalHostToLocalHost_ReturnsGH2LH)
{
    uint64_t remoteGva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(manager.AddVaInfoFromExternal(
        {{remoteGva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO, TEST_RANK_ONE) == BM_OK);
    uint64_t localGva = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    EXPECT_TRUE(
        manager.AddVaInfo({{localGva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ONE) == BM_OK);
    auto dir = manager.InferCopyDirection(remoteGva, localGva);
    // GLOBAL_HOST(1) → LOCAL_HOST(3) = GH2LH(6)
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_LOCAL_HOST);
}

// 测试64: InferCopyDirection 本地 HOST → 远端 HOST → H2GH(0)
TEST_F(HybmVaManagerTest, InferCopyDirection_LocalHostToGlobalHost_ReturnsH2GH)
{
    uint64_t localGva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(
        manager.AddVaInfo({{localGva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    uint64_t remoteGva = TEST_GVA_BASE_HOST + TEST_OFFSET_SIXTEEN_MB;
    EXPECT_TRUE(manager.AddVaInfoFromExternal(
        {{remoteGva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO, TEST_RANK_ONE) == BM_OK);
    auto dir = manager.InferCopyDirection(localGva, remoteGva);
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST);
}

// 测试65: InferCopyDirection GVM pool → tensor.cpu → fallback GH2LH(6)
TEST_F(HybmVaManagerTest, InferCopyDirection_GvmToUserMalloc_ReturnsGH2LH)
{
    uint64_t gvmAddr = HYBM_GVM_START_ADDR + 0x1000;
    EXPECT_TRUE(
        manager.AddVaInfo({{gvmAddr, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    uint64_t userAddr = 0xfffd327e4580;
    auto dir = manager.InferCopyDirection(gvmAddr, userAddr);
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_LOCAL_HOST);
}

// 测试66: InferCopyDirection tensor.cpu → GVM pool → fallback H2GH(0)
TEST_F(HybmVaManagerTest, InferCopyDirection_UserMallocToGvm_ReturnsH2GH)
{
    uint64_t userAddr = 0xfffd327e4580;
    uint64_t gvmAddr = HYBM_GVM_START_ADDR + 0x1000;
    EXPECT_TRUE(
        manager.AddVaInfo({{gvmAddr, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    auto dir = manager.InferCopyDirection(userAddr, gvmAddr);
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST);
}

// 测试67: InferCopyDirection 两个都在 DVA 范围 → 同类型不映射
TEST_F(HybmVaManagerTest, InferCopyDirection_LocalDeviceToLocalDevice_ReturnsBUTT)
{
    uint64_t devAddr1 = HYBM_DEVICE_VA_START + 0x1000;
    uint64_t devAddr2 = HYBM_DEVICE_VA_START + 0x2000;
    auto dir = manager.InferCopyDirection(devAddr1, devAddr2);
    EXPECT_EQ(dir, HYBM_DATA_COPY_DIRECTION_BUTT);
}

// ======================== IsValidAddr 测试 ========================

// 测试68: IsValidAddr 注册过的地址 → true
TEST_F(HybmVaManagerTest, IsValidAddr_Registered_ReturnsTrue)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    EXPECT_TRUE(
        manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_SIXTEEN_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO) == BM_OK);
    EXPECT_TRUE(manager.IsValidAddr(gva));
}

// 测试69: IsValidAddr 未注册地址 → false
TEST_F(HybmVaManagerTest, IsValidAddr_Unregistered_ReturnsFalse)
{
    EXPECT_FALSE(manager.IsValidAddr(0x12345678));
}

// ======================== ClassifyAddressMask 测试 ========================

// 测试70: ClassifyAddressMask 用户地址 → 仅 LOCAL_HOST
TEST_F(HybmVaManagerTest, ClassifyAddressMask_UserAddr_ReturnsLocalHost)
{
    uint64_t userAddr = 0x7f001000;
    uint8_t mask = manager.ClassifyAddressMask(userAddr);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_HOST);
}

// 测试71: ClassifyAddressMask device VA → LOCAL_DEVICE
TEST_F(HybmVaManagerTest, ClassifyAddressMask_DeviceVA_ReturnsLocalDevice)
{
    uint64_t devAddr = HYBM_DEVICE_VA_START + 0x1000;
    uint8_t mask = manager.ClassifyAddressMask(devAddr);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_DEVICE);
}

// 测试72: ClassifyAddressMask 预留+已分配 GVA → LOCAL|GLOBAL 双位
TEST_F(HybmVaManagerTest, ClassifyAddressMask_GvaAllocated_ReturnsBoth)
{
    uint64_t gvaBase = HYBM_GVM_START_ADDR;
    manager.AllocReserveGva(TEST_RANK_ZERO, 0x2000000, 0x2000000, HYBM_MEM_TYPE_HOST, false);
    uint64_t gva = gvaBase + 0x1000;
    manager.AddVaInfo({{gva, 0, 0}, TEST_SIZE_ONE_MB, TEST_MEM_TYPE_HOST}, TEST_RANK_ZERO);
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_HOST | HybmVaManager::BIT_GLOBAL_HOST);
}

// 测试73: ClassifyAddressMask 预留但未分配 GVA → 0（未join）
TEST_F(HybmVaManagerTest, ClassifyAddressMask_GvaUnallocated_ReturnsZero)
{
    uint64_t gva = HYBM_GVM_START_ADDR + 0x1000;
    manager.AllocReserveGva(TEST_RANK_ZERO, 0x2000000, 0x2000000, HYBM_MEM_TYPE_HOST, false);
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, 0);
}

// 测试74: ClassifyAddressMask 远端 import GVA → 单 GLOBAL
TEST_F(HybmVaManagerTest, ClassifyAddressMask_RemoteImported_ReturnsGlobal)
{
    uint64_t gva = TEST_GVA_BASE_HOST;
    manager.AllocReserveGva(TEST_RANK_ZERO, 0x1000000, 0x1000000, HYBM_MEM_TYPE_HOST, false);
    manager.AddVaInfoFromExternal({gva, 0, 0, TEST_SIZE_ONE_MB, HYBM_MEM_TYPE_HOST},
                                  TEST_RANK_ZERO, TEST_RANK_ONE);
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_GLOBAL_HOST);
}

// ======================== LUT 方向推断测试 ========================

// 测试75: directionLut LH→GH → H2GH
TEST_F(HybmVaManagerTest, DirectionLut_LocalToGlobal_ReturnsH2GH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_LOCAL_HOST | (HybmVaManager::BIT_GLOBAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST);
}

// 测试76: directionLut GH→GH → GH2GH
TEST_F(HybmVaManagerTest, DirectionLut_GlobalToGlobal_ReturnsGH2GH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_GLOBAL_HOST | (HybmVaManager::BIT_GLOBAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST);
}

// 测试77: directionLut GH→LH → GH2LH
TEST_F(HybmVaManagerTest, DirectionLut_GlobalToLocal_ReturnsGH2LH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_GLOBAL_HOST | (HybmVaManager::BIT_LOCAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_LOCAL_HOST);
}

// 测试78: directionLut 双位 → 取最小方向值
TEST_F(HybmVaManagerTest, DirectionLut_DualBits_ReturnsFirstMatch)
{
    HybmVaManager::InitDirectionLut();
    uint8_t dual = HybmVaManager::BIT_LOCAL_HOST | HybmVaManager::BIT_GLOBAL_HOST;
    uint8_t except = dual | (dual << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_LOCAL_HOST_TO_GLOBAL_HOST); // H2GH(0) 最小
}

// 测试79: directionLut 无效组合 → BUTT
TEST_F(HybmVaManagerTest, DirectionLut_Invalid_ReturnsBUTT)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_LOCAL_HOST | (HybmVaManager::BIT_LOCAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_GE(dir, HYBM_DATA_COPY_DIRECTION_AUTO);
}

// 测试80: directionLut Device→Host 方向
TEST_F(HybmVaManagerTest, DirectionLut_LocalDeviceToGlobalHost_ReturnsD2GH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_LOCAL_DEVICE | (HybmVaManager::BIT_GLOBAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST);
}

// 测试81: directionLut Device→Device
TEST_F(HybmVaManagerTest, DirectionLut_LocalDeviceToGlobalDevice_ReturnsD2GD)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_LOCAL_DEVICE | (HybmVaManager::BIT_GLOBAL_DEVICE << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE);
}

// 测试82: directionLut GlobalHost→LocalHost
TEST_F(HybmVaManagerTest, DirectionLut_GlobalHostToLocalHost_ReturnsGH2LH)
{
    HybmVaManager::InitDirectionLut();
    uint8_t except = HybmVaManager::BIT_GLOBAL_HOST | (HybmVaManager::BIT_LOCAL_HOST << 4);
    uint8_t dir = HybmVaManager::directionLut[except];
    EXPECT_EQ(dir, HYBM_GLOBAL_HOST_TO_LOCAL_HOST);
}

// 测试83: ClassifyAddressMask 用户地址+device VA → 正确双位
TEST_F(HybmVaManagerTest, ClassifyAddressMask_MixedLocalHostAndDevice)
{
    uint8_t hostMask = manager.ClassifyAddressMask(0x7f001000ULL);
    EXPECT_EQ(hostMask, HybmVaManager::BIT_LOCAL_HOST);

    uint8_t devMask = manager.ClassifyAddressMask(HYBM_DEVICE_VA_START + 0x1000);
    EXPECT_EQ(devMask, HybmVaManager::BIT_LOCAL_DEVICE);
}

// 测试84: ClassifyAddressMask HBM设备地址（DEVICE类型）
TEST_F(HybmVaManagerTest, ClassifyAddressMask_DeviceHbm_ReturnsBothDeviceBits)
{
    uint64_t base = HYBM_GVM_START_ADDR;
    HybmVaManager::GetInstance().AllocReserveGva(0, 0x2000000, 0x2000000, HYBM_MEM_TYPE_DEVICE, false);
    uint64_t gva = base + 0x1000;
    BaseAllocatedGvaInfo devInfo = {gva, gva, gva, 0x100000, HYBM_MEM_TYPE_DEVICE};
    HybmVaManager::GetInstance().AddVaInfo(devInfo, 0);
    uint8_t mask = HybmVaManager::GetInstance().ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_DEVICE | HybmVaManager::BIT_GLOBAL_DEVICE);
}

// 测试85: QueryAddr 三个map一致性
TEST_F(HybmVaManagerTest, QueryAddr_ThreeMapConsistency)
{
    uint64_t gva = HYBM_GVM_START_ADDR + 0x10000000;
    auto &mgr = HybmVaManager::GetInstance();
    mgr.AllocReserveGva(0, 0x2000000, 0x2000000, HYBM_MEM_TYPE_HOST, false);
    BaseAllocatedGvaInfo hostInfo = {gva, gva, gva, 0x100000, HYBM_MEM_TYPE_HOST};
    mgr.AddVaInfo(hostInfo, 0);

    auto r1 = mgr.QueryAddr(gva);
    EXPECT_TRUE(r1.inAllocGva);
    EXPECT_EQ(r1.memType, HYBM_MEM_TYPE_HOST);
}

// 测试86: ClassifyAddressMask reservedMap_未命中 → 非 GVA 地址
TEST_F(HybmVaManagerTest, ClassifyAddressMask_NoReserved_ReturnsLocalHost)
{
    uint64_t userAddr = 0x7f001000;
    uint8_t mask = manager.ClassifyAddressMask(userAddr);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_HOST);
}

// 测试87: ClassifyAddressMask 本端 DEVICE 预留+分配
TEST_F(HybmVaManagerTest, ClassifyAddressMask_DeviceReservedAndAlloc_ReturnsBoth)
{
    uint64_t base = HYBM_GVM_START_ADDR;
    manager.AllocReserveGva(0, 0x2000000, 0x2000000, HYBM_MEM_TYPE_DEVICE, false);
    uint64_t gva = base + 0x2000;
    BaseAllocatedGvaInfo info = {gva, gva, gva, 0x100000, HYBM_MEM_TYPE_DEVICE};
    manager.AddVaInfo(info, 0);
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_DEVICE | HybmVaManager::BIT_GLOBAL_DEVICE);
}

// 测试88: ClassifyAddressMask 预留+分配+位于DEVICE VA范围
TEST_F(HybmVaManagerTest, ClassifyAddressMask_DeviceVaRange_ReturnsLocalDevice)
{
    uint8_t mask = manager.ClassifyAddressMask(HYBM_DEVICE_VA_START + 0x8000);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_DEVICE);
}

// 测试89: ClassifyAddressMask 远端 import 的 HOST 地址
TEST_F(HybmVaManagerTest, ClassifyAddressMask_RemoteImportedHost_ReturnsGlobalHost)
{
    uint64_t base = HYBM_GVM_START_ADDR;
    manager.AllocReserveGva(0, 0x2000000, 0x2000000, HYBM_MEM_TYPE_HOST, false);
    uint64_t gva = base + 0x3000;
    manager.AddVaInfoFromExternal({gva, 0, 0, 0x100000, HYBM_MEM_TYPE_HOST}, 0, 1);
    uint8_t mask = manager.ClassifyAddressMask(gva);
    EXPECT_EQ(mask, HybmVaManager::BIT_GLOBAL_HOST);
}

// 测试90: directionLut 所有合法方向一一验证
TEST_F(HybmVaManagerTest, DirectionLut_AllDirections_CorrectMapping)
{
    HybmVaManager::InitDirectionLut();
    struct {
        uint8_t src;
        uint8_t dst;
        hybm_data_copy_direction dir;
    } testCases[] = {
        {HybmVaManager::BIT_LOCAL_HOST,   HybmVaManager::BIT_GLOBAL_HOST,   HYBM_LOCAL_HOST_TO_GLOBAL_HOST},
        {HybmVaManager::BIT_LOCAL_HOST,   HybmVaManager::BIT_GLOBAL_DEVICE, HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE},
        {HybmVaManager::BIT_LOCAL_DEVICE, HybmVaManager::BIT_GLOBAL_HOST,   HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST},
        {HybmVaManager::BIT_LOCAL_DEVICE, HybmVaManager::BIT_GLOBAL_DEVICE, HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE},
        {HybmVaManager::BIT_GLOBAL_HOST,   HybmVaManager::BIT_GLOBAL_HOST,   HYBM_GLOBAL_HOST_TO_GLOBAL_HOST},
        {HybmVaManager::BIT_GLOBAL_HOST,   HybmVaManager::BIT_GLOBAL_DEVICE, HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE},
        {HybmVaManager::BIT_GLOBAL_HOST,   HybmVaManager::BIT_LOCAL_HOST,   HYBM_GLOBAL_HOST_TO_LOCAL_HOST},
        {HybmVaManager::BIT_GLOBAL_HOST,   HybmVaManager::BIT_LOCAL_DEVICE, HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE},
        {HybmVaManager::BIT_GLOBAL_DEVICE, HybmVaManager::BIT_GLOBAL_HOST,   HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST},
        {HybmVaManager::BIT_GLOBAL_DEVICE, HybmVaManager::BIT_GLOBAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE},
        {HybmVaManager::BIT_GLOBAL_DEVICE, HybmVaManager::BIT_LOCAL_HOST,   HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST},
        {HybmVaManager::BIT_GLOBAL_DEVICE, HybmVaManager::BIT_LOCAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE},
    };
    for (auto &tc : testCases) {
        uint8_t except = tc.src | (tc.dst << 4);
        uint8_t dir = HybmVaManager::directionLut[except];
        EXPECT_EQ(dir, tc.dir);
    }
}

// 测试91: ClassifyAddress 旧API兼容
TEST_F(HybmVaManagerTest, ClassifyAddress_ApiCompat)
{
    // 用户地址 → LOCAL_HOST
    EXPECT_EQ(manager.ClassifyAddress(0x7f001000), LOCAL_HOST);
    // device VA → LOCAL_DEVICE
    EXPECT_EQ(manager.ClassifyAddress(HYBM_DEVICE_VA_START + 0x1000), LOCAL_DEVICE);
}

// 测试92: QueryAddr 空map
TEST_F(HybmVaManagerTest, QueryAddr_EmptyMap_ReturnsAllFalse)
{
    manager.ClearAll();
    auto r = manager.QueryAddr(0x1000);
    EXPECT_FALSE(r.inAllocGva);
    EXPECT_EQ(r.memType, HYBM_MEM_TYPE_BUTT);
    EXPECT_EQ(r.importedRankId, INVALID_RANK_ID);
}

// 测试93: DumpAllocatedGvaInfo 空map不崩溃
TEST_F(HybmVaManagerTest, DumpAllocatedGvaInfo_Empty_NoCrash)
{
    manager.ClearAll();
    manager.DumpAllocatedGvaInfo();
}

// 测试94: DumpReservedGvaInfo 空map不崩溃
TEST_F(HybmVaManagerTest, DumpReservedGvaInfo_Empty_NoCrash)
{
    manager.ClearAll();
    manager.DumpReservedGvaInfo();
}

// 测试95: GetRank 未注册地址
TEST_F(HybmVaManagerTest, GetRank_Unregistered_ReturnsFalse)
{
    auto [rank, found] = manager.GetRank(0xbadbad);
    EXPECT_FALSE(found);
    EXPECT_EQ(rank, 0U);
}

// 测试96: GetGvaMemType 未注册地址
TEST_F(HybmVaManagerTest, GetGvaMemType_Unregistered_ReturnsBUTT)
{
    auto memType = manager.GetGvaMemType(0xdeadbeef);
    EXPECT_EQ(memType, HYBM_MEM_TYPE_BUTT);
}

// 测试97: InferCopyDirection 用户地址+用户地址
TEST_F(HybmVaManagerTest, InferCopyDirection_UserToUser_ReturnsBUTT)
{
    uint64_t user1 = 0x7f001000;
    uint64_t user2 = 0x7f002000;
    auto dir = manager.InferCopyDirection(user1, user2);
    EXPECT_EQ(dir, HYBM_DATA_COPY_DIRECTION_BUTT);
}

// 测试98: TransformVa 无效转换
TEST_F(HybmVaManagerTest, TransformVa_Invalid_ReturnsZero)
{
    uint64_t result = manager.TransformVa(0xbadbad, HVM_GVA, HVM_HVA);
    EXPECT_EQ(result, 0U);
}

// 测试99: ClassifyAddressMask 56bit GVA范围
TEST_F(HybmVaManagerTest, ClassifyAddressMask_56bitGvaRange)
{
    uint8_t mask = manager.ClassifyAddressMask(HYBM_56BITS_GVA_START_ADDR + 0x1000);
    EXPECT_EQ(mask, HybmVaManager::BIT_LOCAL_HOST);
}

// 测试100: DirectionLut 合法方向都有映射
TEST_F(HybmVaManagerTest, DirectionLut_AllValid)
{
    HybmVaManager::InitDirectionLut();
    uint8_t valid = HybmVaManager::directionLut[HybmVaManager::BIT_LOCAL_HOST | (HybmVaManager::BIT_GLOBAL_HOST << 4)];
    EXPECT_LT(valid, HYBM_DATA_COPY_DIRECTION_AUTO);
    uint8_t invalid =
        HybmVaManager::directionLut[HybmVaManager::BIT_LOCAL_DEVICE | (HybmVaManager::BIT_LOCAL_DEVICE << 4)];
    EXPECT_GE(invalid, HYBM_DATA_COPY_DIRECTION_AUTO);
}

// 测试102: ClearAll 后状态重置
TEST_F(HybmVaManagerTest, ClearAll_ResetsState)
{
    manager.AllocReserveGva(0, 0x100000, 0x100000, HYBM_MEM_TYPE_HOST, false);
    EXPECT_GT(manager.GetReservedCount(), 0U);
    manager.ClearAll();
    EXPECT_EQ(manager.GetReservedCount(), 0U);
    EXPECT_EQ(manager.GetAllocCount(), 0U);
}

// 测试103: 分配和释放统计
TEST_F(HybmVaManagerTest, AllocAndFree_Counts)
{
    manager.AllocReserveGva(0, 0x200000, 0x200000, HYBM_MEM_TYPE_HOST, false);
    EXPECT_EQ(manager.GetAllocCount(), 0U);
    uint64_t gva1 = HYBM_GVM_START_ADDR + 0x10000;
    BaseAllocatedGvaInfo info = {gva1, 0, 0, 0x10000, HYBM_MEM_TYPE_HOST};
    EXPECT_EQ(manager.AddVaInfo(info, 0), BM_OK);
    EXPECT_EQ(manager.GetAllocCount(), 1U);
    manager.RemoveOneVaInfo(gva1, HVM_GVA);
    EXPECT_EQ(manager.GetAllocCount(), 0U);
}

// 测试104: 不同 memType 不影响分配
TEST_F(HybmVaManagerTest, AddVaInfo_DiffMemTypes)
{
    uint64_t gva1 = HYBM_GVM_START_ADDR + 0x10000;
    uint64_t gva2 = HYBM_GVM_START_ADDR + 0x30000;
    manager.AllocReserveGva(0, 0x200000, 0x200000, HYBM_MEM_TYPE_HOST, false);
    BaseAllocatedGvaInfo h1 = {gva1, 0, 0, 0x10000, HYBM_MEM_TYPE_HOST};
    BaseAllocatedGvaInfo d1 = {gva2, 0, 0, 0x10000, HYBM_MEM_TYPE_DEVICE};
    EXPECT_EQ(manager.AddVaInfo(h1, 0), BM_OK);
    EXPECT_EQ(manager.AddVaInfo(d1, 0), BM_OK);
    EXPECT_EQ(manager.GetAllocCount(), 2U);
}

// 测试105: GetRank 注册地址
TEST_F(HybmVaManagerTest, GetRank_Registered)
{
    uint64_t gva = HYBM_GVM_START_ADDR + 0x10000;
    manager.AllocReserveGva(0, 0x200000, 0x200000, HYBM_MEM_TYPE_HOST, false);
    manager.AddVaInfoFromExternal({gva, 0, 0, 0x10000, HYBM_MEM_TYPE_HOST}, 0, 1);
    auto [rank, found] = manager.GetRank(gva);
    EXPECT_TRUE(found);
    EXPECT_EQ(rank, 1U);
}
