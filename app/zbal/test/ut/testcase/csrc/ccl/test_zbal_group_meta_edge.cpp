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

#include "zbal_test_constants.h"
#include "zbal_comm_group_meta.h"

using namespace zbal;
using namespace zbal::operators;

constexpr uint16_t kMetaSpaceSize = ZBAL_UT_NUM_512;
constexpr uint16_t kGroupCap = ZBAL_UT_NUM_4;
constexpr uintptr_t kBaseAddress = 0x10000;

class TestZBALGroupMetaEdge : public testing::Test {
public:
    void SetUp() override
    {
        arranger_.UnInitialize();
    }

    void TearDown() override
    {
        arranger_.UnInitialize();
    }

    void InitArranger(uint16_t groupCap = kGroupCap, uint64_t extraBytes = 0)
    {
        ZBALInitStateExt ext;
        ext.commMetaSpaceSize = kMetaSpaceSize;
        ext.commGroupCap = groupCap;
        ext.myCommMetaDeviceGva = reinterpret_cast<void *>(kBaseAddress);
        uint64_t required = static_cast<uint64_t>(kMetaSpaceSize) * ZBAL_UT_SIZE_1KB * groupCap;
        ext.metaSizeOfDevice = required + extraBytes;
        ASSERT_EQ(arranger_.Initialize(ext), Z_OK);
    }

    GroupMetaArranger &arranger_ = GroupMetaArranger::Instance();
};

TEST_F(TestZBALGroupMetaEdge, InitUninitLifecycle)
{
    EXPECT_FALSE(arranger_.Initialized());

    InitArranger();
    EXPECT_TRUE(arranger_.Initialized());
    arranger_.UnInitialize();
    EXPECT_FALSE(arranger_.Initialized());

    InitArranger();
    uint32_t idx;
    uintptr_t gvaBefore;
    uintptr_t param;
    uintptr_t exch;
    ASSERT_EQ(arranger_.CurrentGroup(idx, gvaBefore, param, exch), Z_OK);
    EXPECT_EQ(arranger_.Initialize(ZBALInitStateExt{}), Z_OK);
    uintptr_t gvaAfter;
    ASSERT_EQ(arranger_.CurrentGroup(idx, gvaAfter, param, exch), Z_OK);
    EXPECT_EQ(gvaBefore, gvaAfter);
    arranger_.UnInitialize();
    EXPECT_FALSE(arranger_.Initialized());

    ZBALInitStateExt ext;
    ext.commMetaSpaceSize = ZBAL_UT_SIZE_1KB;
    ext.commGroupCap = ZBAL_UT_NUM_2;
    ext.myCommMetaDeviceGva = reinterpret_cast<void *>(0x20000);
    ext.metaSizeOfDevice = static_cast<uint64_t>(ZBAL_UT_SIZE_1KB) * ZBAL_UT_SIZE_1KB * ZBAL_UT_NUM_2;
    EXPECT_EQ(arranger_.Initialize(ext), Z_OK);
    EXPECT_EQ(arranger_.GetSingleMetaSpaceSize(), 1024u * 1024u);

    arranger_.UnInitialize();
    arranger_.UnInitialize();
    EXPECT_FALSE(arranger_.Initialized());

    EXPECT_NO_THROW(arranger_.Move2NextGroup());

    EXPECT_EQ(arranger_.GetSingleMetaSpaceSize(), ZBAL_UT_NUM_0);
    EXPECT_EQ(arranger_.GetCommGroupInfoSpaceSize(), sizeof(CommGroupInfo));
    EXPECT_EQ(arranger_.GetParamSpaceSize(), ZBAL_OPERATE_PARAM_SIZE - sizeof(CommGroupInfo));
    EXPECT_EQ(arranger_.GetExchangeSpaceSize(), static_cast<uint64_t>(-static_cast<int64_t>(ZBAL_OPERATE_PARAM_SIZE)));

    uintptr_t gvaUninit = 0;
    uintptr_t paramUninit = 0;
    uintptr_t exchUninit = 0;
    EXPECT_NE(arranger_.GetGroupByIndex(ZBAL_UT_NUM_0, gvaUninit, paramUninit, exchUninit), Z_OK);
}

TEST_F(TestZBALGroupMetaEdge, InitializeBoundary)
{
    {
        ZBALInitStateExt ext;
        ext.commMetaSpaceSize = kMetaSpaceSize;
        ext.commGroupCap = ZBAL_UT_NUM_1;
        ext.myCommMetaDeviceGva = reinterpret_cast<void *>(kBaseAddress);
        ext.metaSizeOfDevice = static_cast<uint64_t>(kMetaSpaceSize) * ZBAL_UT_SIZE_1KB;
        EXPECT_EQ(arranger_.Initialize(ext), Z_OK);
        EXPECT_TRUE(arranger_.Initialized());
        arranger_.UnInitialize();
    }
    {
        ZBALInitStateExt ext;
        ext.commMetaSpaceSize = kMetaSpaceSize;
        ext.commGroupCap = kGroupCap;
        ext.myCommMetaDeviceGva = reinterpret_cast<void *>(kBaseAddress);
        uint64_t required = static_cast<uint64_t>(kMetaSpaceSize) * ZBAL_UT_SIZE_1KB * kGroupCap;
        ext.metaSizeOfDevice = required - ZBAL_UT_NUM_1;
        EXPECT_NE(arranger_.Initialize(ext), Z_OK);
        EXPECT_FALSE(arranger_.Initialized());
    }
    {
        ZBALInitStateExt ext;
        ext.commMetaSpaceSize = kMetaSpaceSize;
        ext.commGroupCap = ZBAL_UT_NUM_0;
        ext.myCommMetaDeviceGva = reinterpret_cast<void *>(kBaseAddress);
        ext.metaSizeOfDevice = ZBAL_UT_SIZE_1KB;
        EXPECT_NE(arranger_.Initialize(ext), Z_OK);
        EXPECT_FALSE(arranger_.Initialized());
    }
    {
        ZBALInitStateExt ext;
        ext.commMetaSpaceSize = ZBAL_UT_NUM_0;
        ext.commGroupCap = kGroupCap;
        ext.myCommMetaDeviceGva = reinterpret_cast<void *>(kBaseAddress);
        ext.metaSizeOfDevice = ZBAL_UT_NUM_1;
        EXPECT_EQ(arranger_.Initialize(ext), Z_OK);
        arranger_.UnInitialize();
    }
    {
        ZBALInitStateExt ext;
        ext.commMetaSpaceSize = kMetaSpaceSize;
        ext.commGroupCap = kGroupCap;
        ext.myCommMetaDeviceGva = reinterpret_cast<void *>(kBaseAddress);
        ext.metaSizeOfDevice = static_cast<uint64_t>(kMetaSpaceSize) * ZBAL_UT_SIZE_1KB * kGroupCap + ZBAL_UT_SIZE_1KB;
        EXPECT_EQ(arranger_.Initialize(ext), Z_OK);
        EXPECT_TRUE(arranger_.Initialized());
        arranger_.UnInitialize();
    }
}

TEST_F(TestZBALGroupMetaEdge, GroupIteration)
{
    InitArranger(kGroupCap);
    uint32_t idx;
    uintptr_t gva;
    uintptr_t param;
    uintptr_t exch;

    for (uint32_t i = 0; i < kGroupCap; i++) {
        EXPECT_EQ(arranger_.CurrentGroup(idx, gva, param, exch), Z_OK);
        EXPECT_EQ(idx, i);
        arranger_.Move2NextGroup();
    }
    EXPECT_NE(arranger_.CurrentGroup(idx, gva, param, exch), Z_OK);

    EXPECT_EQ(arranger_.GetGroupByIndex(kGroupCap - ZBAL_UT_NUM_1, gva, param, exch), Z_OK);
    EXPECT_NE(arranger_.GetGroupByIndex(kGroupCap, gva, param, exch), Z_OK);

    arranger_.UnInitialize();
    InitArranger(ZBAL_UT_NUM_1);
    EXPECT_EQ(arranger_.CurrentGroup(idx, gva, param, exch), Z_OK);
    EXPECT_EQ(idx, ZBAL_UT_NUM_0);
    arranger_.Move2NextGroup();
    EXPECT_NE(arranger_.CurrentGroup(idx, gva, param, exch), Z_OK);
}

TEST_F(TestZBALGroupMetaEdge, AddressLayout)
{
    InitArranger(kGroupCap);
    uintptr_t gva0, param0, exch0;
    uintptr_t gva1, param1, exch1;

    ASSERT_EQ(arranger_.GetGroupByIndex(ZBAL_UT_NUM_0, gva0, param0, exch0), Z_OK);
    ASSERT_EQ(arranger_.GetGroupByIndex(ZBAL_UT_NUM_1, gva1, param1, exch1), Z_OK);

    uint64_t stride = arranger_.GetSingleMetaSpaceSize();
    EXPECT_EQ(gva1, gva0 + stride);
    EXPECT_EQ(param1, param0 + stride);
    EXPECT_EQ(exch1, exch0 + stride);

    EXPECT_EQ(param0, gva0 + sizeof(CommGroupInfo));
    EXPECT_EQ(exch0, gva0 + ZBAL_OPERATE_PARAM_SIZE);

    uintptr_t regionEnd = kBaseAddress + stride * kGroupCap;
    for (uint16_t i = 0; i < kGroupCap; i++) {
        ASSERT_EQ(arranger_.GetGroupByIndex(i, gva0, param0, exch0), Z_OK);
        EXPECT_GE(gva0, kBaseAddress);
        EXPECT_LT(gva0, regionEnd);
        EXPECT_GE(param0, kBaseAddress);
        EXPECT_LT(param0, regionEnd);
        EXPECT_GE(exch0, kBaseAddress);
        EXPECT_LT(exch0, regionEnd);
    }
}

TEST_F(TestZBALGroupMetaEdge, SpaceSizeInvariants)
{
    InitArranger(kGroupCap);

    uint64_t single = arranger_.GetSingleMetaSpaceSize();
    uint64_t commInfo = arranger_.GetCommGroupInfoSpaceSize();
    uint64_t param = arranger_.GetParamSpaceSize();
    uint64_t exch = arranger_.GetExchangeSpaceSize();

    EXPECT_EQ(commInfo, sizeof(CommGroupInfo));
    EXPECT_EQ(param, ZBAL_OPERATE_PARAM_SIZE - sizeof(CommGroupInfo));
    EXPECT_EQ(exch, single - ZBAL_OPERATE_PARAM_SIZE);
    EXPECT_EQ(commInfo + param, ZBAL_OPERATE_PARAM_SIZE);
    EXPECT_EQ(commInfo + param + exch, single);
}
