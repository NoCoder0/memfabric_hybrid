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

#include "hybm_batch_copy_route.h"
#include "hybm_define.h"

using namespace ock::mf;

TEST(HybmBatchCopyRouteTest, AbiOffsetsAndSizesAreStable)
{
    EXPECT_EQ(sizeof(BatchCopyRouteHeader), 0x40U);
    EXPECT_EQ(sizeof(BatchCopyPeerEntry), 0x20U);
    EXPECT_EQ(sizeof(BatchCopyRangeEntry), 0x20U);
    EXPECT_EQ(offsetof(BatchCopyRangeEntry, hcommVaBegin), 0x10U);
    EXPECT_EQ(offsetof(BatchCopyRangeEntry, peerIndex), 0x18U);
    EXPECT_EQ(offsetof(BatchCopyRouteTable, peers), 0x40U);
    EXPECT_EQ(offsetof(BatchCopyRouteTable, ranges), 0x840U);
    EXPECT_EQ(sizeof(BatchCopyRouteTable), 0x8840U);
    EXPECT_EQ(BATCH_COPY_COMPLETION_OFFSET, 0x8840U);
    EXPECT_EQ(BATCH_COPY_CONTROL_USED_SIZE, 0x8A40U);
}

TEST(HybmBatchCopyRouteTest, ControlRegionPrecedesExistingDeviceMetadata)
{
    EXPECT_EQ(HYBM_BATCH_COPY_META_SIZE, HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(HYBM_BATCH_COPY_META_ADDR + HYBM_BATCH_COPY_META_SIZE, HYBM_DEVICE_META_ADDR);
    EXPECT_EQ(HYBM_DEVICE_CONTROL_ADDR, HYBM_BATCH_COPY_META_ADDR);
    EXPECT_EQ(HYBM_DEVICE_CONTROL_SIZE, 34U * MB);
    EXPECT_EQ(HYBM_DEVICE_CONTROL_ADDR + HYBM_DEVICE_CONTROL_SIZE, SVM_END_ADDR);
    EXPECT_LE(BATCH_COPY_CONTROL_USED_SIZE, HYBM_BATCH_COPY_META_SIZE);
}
