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

#include <sstream>
#include <string>
#include <vector>

#include "zbal_defines.h"
#include "zbal_test_constants.h"
#include "zbal_pytorch_util.h"

using namespace zbal;
using namespace zbal::adaptor::pytorch_npu;

TEST(TestZBALUtil, GlobalsAndDataTypes)
{
    EXPECT_TRUE(ZBALFormatErrorCode(Z_OK).find("[ERROR] CODE0") != std::string::npos);
    EXPECT_TRUE(ZBALFormatErrorCode(-1).find("[ERROR] CODE-1") != std::string::npos);
    EXPECT_FALSE(OptionsManager::IsHcclZeroCopyEnable);
    EXPECT_FALSE(OptionsManager::CheckForceUncached);

    struct Case {
        at::ScalarType in;
        zbal_datatype_t out;
    };
    Case cases[] = {
        {at::kByte, ZBAL_DATA_TYPE_UINT8},     {at::kChar, ZBAL_DATA_TYPE_INT8},   {at::kShort, ZBAL_DATA_TYPE_INT16},
        {at::kInt, ZBAL_DATA_TYPE_INT32},      {at::kLong, ZBAL_DATA_TYPE_INT64},  {at::kHalf, ZBAL_DATA_TYPE_FP16},
        {at::kFloat, ZBAL_DATA_TYPE_FP32},     {at::kDouble, ZBAL_DATA_TYPE_FP64}, {at::kBool, ZBAL_DATA_TYPE_UINT8},
        {at::kBFloat16, ZBAL_DATA_TYPE_BFP16},
    };
    for (auto &c : cases) {
        EXPECT_EQ(GetZbalDataType(c.in), c.out);
    }
    EXPECT_THROW(GetZbalDataType(at::kComplexHalf), std::runtime_error);

    EXPECT_EQ(GetZbalReduceOp(c10d::ReduceOp::MIN), ZBAL_REDUCE_MIN);
    EXPECT_EQ(GetZbalReduceOp(c10d::ReduceOp::MAX), ZBAL_REDUCE_MAX);
    EXPECT_EQ(GetZbalReduceOp(c10d::ReduceOp::SUM), ZBAL_REDUCE_SUM);
    EXPECT_EQ(GetZbalReduceOp(c10d::ReduceOp::PRODUCT), ZBAL_REDUCE_PROD);
    EXPECT_THROW(GetZbalReduceOp(c10d::ReduceOp::AVG), std::runtime_error);

    at::ScalarType supported[] = {at::kChar, at::kShort, at::kInt, at::kHalf, at::kFloat, at::kBFloat16};
    for (auto t : supported) {
        EXPECT_TRUE(ZbalReduceSupportDataType(t));
    }
    at::ScalarType unsupported[] = {at::kDouble, at::kByte, at::kBool, at::kLong, at::kComplexFloat};
    for (auto t : unsupported) {
        EXPECT_FALSE(ZbalReduceSupportDataType(t));
    }
}

TEST(TestZBALUtil, KeyDeviceAndCheckHelpers)
{
    EXPECT_TRUE(GetKeyFromDevices({}).empty());
    EXPECT_EQ(GetKeyFromDevices({at::Device(at::kCPU, 0)}), "0");
    EXPECT_EQ(GetKeyFromDevices({at::Device(at::kPrivateUse1, 0), at::Device(at::kPrivateUse1, 1),
                                 at::Device(at::kPrivateUse1, ZBAL_UT_NUM_2)}),
              "0,1,2");

    EXPECT_EQ(GetNumelForZBAL(at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3, ZBAL_UT_NUM_4}, at::kFloat)), 24ULL);
    EXPECT_EQ(GetNumelForZBAL(at::empty({0}, at::kFloat)), 0ULL);
    EXPECT_TRUE(GetDeviceList({}).empty());
    EXPECT_EQ(GetDeviceList({at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
                             at::zeros({ZBAL_UT_NUM_3, ZBAL_UT_NUM_4}, at::kFloat)})
                  .size(),
              2u);
    CheckTensors({at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)});
}

TEST(TestZBALUtil, CheckSameSize)
{
    EXPECT_TRUE(CheckSameSize({}));
    EXPECT_TRUE(CheckSameSize(
        {at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat), at::zeros({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}));
    EXPECT_FALSE(CheckSameSize(
        {at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat), at::zeros({ZBAL_UT_NUM_3, ZBAL_UT_NUM_2}, at::kFloat)}));
}

TEST(TestZBALUtil, CheckSplitSize)
{
    auto t = at::ones({ZBAL_UT_NUM_10}, at::kFloat);
    EXPECT_THROW(CheckSplitSize({}, t, 0), std::runtime_error);
    EXPECT_THROW(CheckSplitSize({ZBAL_UT_NUM_3, ZBAL_UT_NUM_3, ZBAL_UT_NUM_4}, t, ZBAL_UT_NUM_2), std::runtime_error);
    EXPECT_THROW(CheckSplitSize({ZBAL_UT_NUM_3, ZBAL_UT_NUM_3, ZBAL_UT_NUM_3}, t, ZBAL_UT_NUM_3), std::runtime_error);
    EXPECT_THROW(CheckSplitSize({ZBAL_UT_NUM_3, ZBAL_UT_NUM_12, -ZBAL_UT_NUM_5}, t, ZBAL_UT_NUM_3), std::runtime_error);
    CheckSplitSize({ZBAL_UT_NUM_3, ZBAL_UT_NUM_3, ZBAL_UT_NUM_4}, t, ZBAL_UT_NUM_3);
    SUCCEED();
}

TEST(TestZBALUtil, CheckSingleTensor)
{
    EXPECT_THROW(CheckSingleTensor(at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()),
                 std::runtime_error);
    EXPECT_THROW(CheckSingleTensor(at::ones({ZBAL_UT_NUM_3, ZBAL_UT_NUM_4}, at::kFloat).t()), std::runtime_error);
    CheckSingleTensor(at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat));
    CheckSingleTensor(at::zeros({ZBAL_UT_NUM_4, ZBAL_UT_NUM_5, ZBAL_UT_NUM_6}, at::kInt));
    SUCCEED();
}

TEST(TestZBALUtil, CheckNpuTensorsDifferentDevices)
{
    EXPECT_EQ(CheckNpuTensorsDifferentDevices({}), Z_INVALID_PARAM);
    EXPECT_EQ(CheckNpuTensorsDifferentDevices({at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
                                               at::zeros({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}),
              Z_INVALID_PARAM);
    EXPECT_EQ(CheckNpuTensorsDifferentDevices({at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()}),
              Z_INVALID_PARAM);
    EXPECT_EQ(CheckNpuTensorsDifferentDevices({at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).t()}),
              Z_INVALID_PARAM);
    EXPECT_EQ(CheckNpuTensorsDifferentDevices({at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}), Z_OK);
    EXPECT_EQ(CheckNpuTensorsDifferentDevices({at::zeros({ZBAL_UT_NUM_5}, at::kDouble)}), Z_OK);
}

TEST(TestZBALUtil, CheckNpuTensorsSameDevice)
{
    EXPECT_THROW(CheckNpuTensorsSameDevice({}), std::runtime_error);
    EXPECT_THROW(CheckNpuTensorsSameDevice({at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat).to_sparse()}),
                 std::runtime_error);
    EXPECT_THROW(CheckNpuTensorsSameDevice({at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat),
                                            at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kInt)}),
                 std::runtime_error);
    auto t1 = at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
    EXPECT_THROW(
        CheckNpuTensorsSameDevice({t1, at::ones({ZBAL_UT_NUM_2}, at::kFloat).expand({ZBAL_UT_NUM_2, ZBAL_UT_NUM_2})}),
        std::runtime_error);
    CheckNpuTensorsSameDevice(
        {at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat), at::zeros({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)});
    CheckNpuTensorsSameDevice({at::ones({ZBAL_UT_NUM_4}, at::kInt), at::zeros({ZBAL_UT_NUM_4}, at::kInt)});
    SUCCEED();
}

TEST(TestZBALUtil, ZbalNewLikeFlat)
{
    std::vector<std::vector<at::Tensor>> empty;
    EXPECT_THROW(ZbalNewLikeFlat(empty, 0), std::runtime_error);

    std::vector<std::vector<at::Tensor>> tensors = {{at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
    EXPECT_THROW(ZbalNewLikeFlat(tensors, 1), std::runtime_error);

    auto result = ZbalNewLikeFlat(tensors, 0);
    EXPECT_EQ(result.sizes(), (std::vector<int64_t>{1, ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}));
    EXPECT_EQ(result.strides(), (std::vector<int64_t>{ZBAL_UT_NUM_6, ZBAL_UT_NUM_3, 1}));

    std::vector<std::vector<at::Tensor>> multiTensors = {
        {at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat), at::zeros({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat)}};
    auto result2 = ZbalNewLikeFlat(multiTensors, 0);
    EXPECT_EQ(result2.sizes(), (std::vector<int64_t>{ZBAL_UT_NUM_2, ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}));
    EXPECT_EQ(result2.strides(), (std::vector<int64_t>{ZBAL_UT_NUM_6, ZBAL_UT_NUM_3, 1}));
}

TEST(TestZBALUtil, FlattenForScatterGather)
{
    {
        std::vector<std::vector<at::Tensor>> tensorLists = {{}};
        std::vector<at::Tensor> other;
        EXPECT_THROW(FlattenForScatterGather(tensorLists, other, ZBAL_UT_NUM_2), std::runtime_error);
    }
    {
        auto t = at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
        std::vector<std::vector<at::Tensor>> tensorLists = {{t}};
        std::vector<at::Tensor> other = {t};
        EXPECT_THROW(FlattenForScatterGather(tensorLists, other, ZBAL_UT_NUM_2), std::runtime_error);
    }
    {
        auto t1 = at::ones({ZBAL_UT_NUM_2, ZBAL_UT_NUM_3}, at::kFloat);
        auto t2 = at::ones({ZBAL_UT_NUM_3, ZBAL_UT_NUM_2}, at::kFloat);
        std::vector<std::vector<at::Tensor>> tensorLists = {{t1, t2}};
        std::vector<at::Tensor> other = {t1};
        EXPECT_THROW(FlattenForScatterGather(tensorLists, other, 1), std::runtime_error);
    }
}
