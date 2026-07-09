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

#include <gtest/gtest.h>

#define private public
#include "hybm_mem_slice.h"
#undef private

using namespace ock::mf;

class MemSliceUnitTest : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MemSliceUnitTest, ConvertToIdAndGetIndex)
{
    MemSlice slice(42, HYBM_MEM_TYPE_HOST, MemPageTblType::MEM_PT_TYPE_SVM, // 42
                   0x1000, 0x2000, 4096);                                   // 4096
    hybm_mem_slice_t id = slice.ConvertToId();
    EXPECT_NE(id, nullptr);
    EXPECT_TRUE(slice.ValidateId(id));
    EXPECT_EQ(MemSlice::GetIndexFrom(id), 42U);
}

TEST_F(MemSliceUnitTest, ValidateId_InvalidSlice)
{
    MemSlice slice(3, HYBM_MEM_TYPE_HOST, MemPageTblType::MEM_PT_TYPE_HYM, // 3
                   0x1000, 0x2000, 4096);                                  // 4096
    hybm_mem_slice_t id = slice.ConvertToId();
    hybm_mem_slice_t badId = reinterpret_cast<hybm_mem_slice_t>(reinterpret_cast<uint64_t>(id) ^ 0x1);
    EXPECT_FALSE(slice.ValidateId(badId));
    EXPECT_FALSE(slice.ValidateId(nullptr));
}

TEST_F(MemSliceUnitTest, ConvertToId_MaxIndex)
{
    uint32_t maxIndex = (1U << 31) - 1;
    MemSlice slice(maxIndex, HYBM_MEM_TYPE_DEVICE, MemPageTblType::MEM_PT_TYPE_GVM, 0x3000, 0x4000, 1048576); // 1048576
    hybm_mem_slice_t id = slice.ConvertToId();
    EXPECT_TRUE(slice.ValidateId(id));
    EXPECT_EQ(MemSlice::GetIndexFrom(id), static_cast<uint64_t>(maxIndex));
}
