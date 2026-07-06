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

#include "smem_hybm_helper.h"

using ock::smem::SmemHybmHelper;

TEST(SmemHybmHelperTest, TransHybmDataOpTypeMapsDeviceUboe)
{
    auto r = SmemHybmHelper::TransHybmDataOpType(SMEMB_DATA_OP_DEVICE_UBOE);
    EXPECT_TRUE((static_cast<uint32_t>(r) & HYBM_DOP_TYPE_DEVICE_UBOE) != 0U);
    EXPECT_TRUE((static_cast<uint32_t>(r) & HYBM_DOP_TYPE_DEVICE_URMA) == 0U);
    EXPECT_TRUE((static_cast<uint32_t>(r) & HYBM_DOP_TYPE_DEVICE_RDMA) == 0U);
}

TEST(SmemHybmHelperTest, TransHybmDataOpTypeMapsAllDeviceTypes)
{
    EXPECT_TRUE((static_cast<uint32_t>(SmemHybmHelper::TransHybmDataOpType(SMEMB_DATA_OP_DEVICE_URMA)) &
                HYBM_DOP_TYPE_DEVICE_URMA) != 0U);
    EXPECT_TRUE((static_cast<uint32_t>(SmemHybmHelper::TransHybmDataOpType(SMEMB_DATA_OP_DEVICE_UBOE)) &
                HYBM_DOP_TYPE_DEVICE_UBOE) != 0U);
    EXPECT_TRUE((static_cast<uint32_t>(SmemHybmHelper::TransHybmDataOpType(SMEMB_DATA_OP_DEVICE_RDMA)) &
                HYBM_DOP_TYPE_DEVICE_RDMA) != 0U);
}

TEST(SmemHybmHelperTest, TransHybmDataOpTypeMapsHostTypes)
{
    EXPECT_TRUE((static_cast<uint32_t>(SmemHybmHelper::TransHybmDataOpType(SMEMB_DATA_OP_HOST_URMA)) &
                HYBM_DOP_TYPE_HOST_URMA) != 0U);
    EXPECT_TRUE((static_cast<uint32_t>(SmemHybmHelper::TransHybmDataOpType(SMEMB_DATA_OP_HOST_TCP)) &
                HYBM_DOP_TYPE_HOST_TCP) != 0U);
    EXPECT_TRUE((static_cast<uint32_t>(SmemHybmHelper::TransHybmDataOpType(SMEMB_DATA_OP_HOST_SHM)) &
                HYBM_DOP_TYPE_HOST_SHM) != 0U);
}
