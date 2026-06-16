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

#include <cstring>

#include "zbal_test_constants.h"
#include "zbal_common_includes.h"
#include "zbal_init_state.h"

extern "C" {
ZBAL_API int32_t zbal_comm_create(zbal_comm_options_t *options, zbal_comm_t *comm);
ZBAL_API int32_t zbal_comm_get_property(zbal_comm_t comm, zbal_comm_property_t *property);
ZBAL_API zbal_comm_t zbal_comm_get_global();
ZBAL_API zbal_comm_t zbal_comm_get_by_name(const char *name);
ZBAL_API int32_t zbal_comm_destroy(zbal_comm_t comm, uint32_t flags);
ZBAL_API void zbal_comm_destroy_all(uint32_t flags);
ZBAL_API int32_t zbal_all_reduce(const void *send_buff, void *recv_buff, void *buffer, size_t count,
                                 zbal_datatype_t data_type, zbal_reduce_op_t op, zbal_comm_t comm, aclrtStream stream);
ZBAL_API int32_t zbal_reduce_scatter(const void *send_buff, void *recv_buff, size_t recv_count,
                                     zbal_datatype_t data_type, zbal_reduce_op_t op, zbal_comm_t comm,
                                     aclrtStream stream);
ZBAL_API int32_t zbal_all_gather(const void *send_buff, void *recv_buff, size_t send_count, zbal_datatype_t data_type,
                                 zbal_comm_t comm, aclrtStream stream);
ZBAL_API int32_t zbal_barrier(zbal_comm_t comm, aclrtStream stream);
ZBAL_API int32_t zbal_all_to_all_v(const void *sendBuff, void *recvBuff, void *sendCumSum, void *recvSplitCounts,
                                   void *elements, zbal_datatype_t dataType, zbal_comm_t comm, aclrtStream stream);
ZBAL_API int32_t zbal_broadcast(const void *buf, uint64_t data_count, zbal_datatype_t dataType, uint16_t root,
                                zbal_comm_t comm, aclrtStream stream);
ZBAL_API int32_t zbal_scatter(const void *sendBuff, void *recvBuff, uint64_t data_count, zbal_datatype_t dataType,
                              uint16_t root, zbal_comm_t comm, aclrtStream stream);
ZBAL_API int32_t zbal_send(const void *sendBuff, zbal_datatype_t dataType, uint32_t peer, zbal_comm_t comm,
                           aclrtStream stream);
ZBAL_API int32_t zbal_recv(const void *recvBuff, size_t recvCount, zbal_datatype_t dataType, uint32_t peer,
                           zbal_comm_t comm, aclrtStream stream);
ZBAL_API int32_t zbal_dispatch_normal_notify(const zbal_tensor_info_t *sendTokensPerExpert, int64_t sendCount,
                                             int64_t topKNum, const zbal_tensor_info_t *recvBuff,
                                             const zbal_tensor_info_t *totalRecvTokens,
                                             const zbal_tensor_info_t *recvTokensPerExpert,
                                             const zbal_tensor_info_t *pushTargetOffset,
                                             const zbal_tensor_info_t *balanceMatrix, zbal_comm_t comm,
                                             aclrtStream stream, int64_t flags);
ZBAL_API int32_t zbal_dispatch_normal_layout(const zbal_tensor_info_t *topkIndex, int64_t tokens, int64_t expertNum,
                                             int64_t topkNum, const zbal_tensor_info_t *tokensPerRank,
                                             const zbal_tensor_info_t *tokensPerExpert,
                                             const zbal_tensor_info_t *sendTokensIndex,
                                             const zbal_tensor_info_t *notifySendData, zbal_comm_t comm,
                                             aclrtStream stream, int64_t flags);
ZBAL_API int32_t zbal_dispatch_normal(const zbal_tensor_info_t *srcTokens, const zbal_tensor_info_t *topkIndex,
                                      const zbal_tensor_info_t *sendTokensIndex,
                                      const zbal_tensor_info_t *pushTargetOffset,
                                      const zbal_tensor_info_t *balanceMatrix, int64_t expertNum,
                                      zbal_quant_mode_t quantMode, const zbal_tensor_info_t *destTokens,
                                      const zbal_tensor_info_t *destScale, zbal_comm_t comm, aclrtStream stream,
                                      int64_t flags);
ZBAL_API int32_t zbal_combine_normal(const zbal_tensor_info_t *srcTokens, const zbal_tensor_info_t *srcTokensPerEp,
                                     const zbal_tensor_info_t *topKWeight, const zbal_tensor_info_t *topkIndex,
                                     const zbal_tensor_info_t *sendTokensIndex, const zbal_tensor_info_t *balanceMatrix,
                                     uint16_t expertNum, const zbal_tensor_info_t *destTokens, zbal_comm_t comm,
                                     aclrtStream stream, int64_t flags);
ZBAL_API int32_t zbal_dispatch_low_latency(
    const zbal_tensor_info_t *x, const zbal_tensor_info_t *expertIds, int64_t moeExpertNum, int64_t sharedExpertNum,
    int64_t sharedExpertRankNum, int64_t quantMode, int64_t globalBs, int64_t magicVal, int64_t expertTokenNumsType,
    const zbal_tensor_info_t *expandXOut, const zbal_tensor_info_t *dynamicScalesOut,
    const zbal_tensor_info_t *expandIdxOut, const zbal_tensor_info_t *expertTokenNumsOut,
    const zbal_tensor_info_t *epRecvCountsOut, const zbal_tensor_info_t *putOffset,
    const zbal_tensor_info_t *putOffsetStatus, zbal_comm_t comm, aclrtStream stream, int64_t flags);
ZBAL_API int32_t zbal_combine_low_latency(const zbal_tensor_info_t *expandX, const zbal_tensor_info_t *expertIds,
                                          const zbal_tensor_info_t *expertIdx, const zbal_tensor_info_t *epSendCounts,
                                          const zbal_tensor_info_t *expertScales, const zbal_tensor_info_t *xOut,
                                          int64_t moeExpertNum, zbal_comm_t comm, aclrtStream stream, int64_t flags);
}

using namespace zbal;

class TestZBALOperations : public testing::Test {
public:
    void SetUp() override
    {
        ZBALInitState::Instance().Reset();
        ZBLastError::GetAndClear(true);
    }

    void TearDown() override
    {
        ZBALInitState::Instance().Reset();
        ZBLastError::GetAndClear(true);
    }
};
TEST_F(TestZBALOperations, CommCreateNullOptions)
{
    zbal_comm_t comm;
    EXPECT_EQ(zbal_comm_create(nullptr, &comm), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, CommCreateNullComm)
{
    zbal_comm_options_t options;
    std::memset(&options, 0, sizeof(options));
    options.name = const_cast<char *>("test");
    EXPECT_EQ(zbal_comm_create(&options, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, CommCreateNullName)
{
    zbal_comm_options_t options;
    std::memset(&options, 0, sizeof(options));
    options.name = nullptr;
    zbal_comm_t comm;
    EXPECT_EQ(zbal_comm_create(&options, &comm), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, CommCreateEmptyName)
{
    zbal_comm_options_t options;
    std::memset(&options, 0, sizeof(options));
    options.name = const_cast<char *>("");
    zbal_comm_t comm;
    EXPECT_EQ(zbal_comm_create(&options, &comm), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, CommCreateNotBootstrapped)
{
    zbal_comm_options_t options;
    std::memset(&options, 0, sizeof(options));
    options.name = const_cast<char *>("test_group");
    zbal_comm_t comm;
    EXPECT_EQ(zbal_comm_create(&options, &comm), Z_NOT_BOOTSTRAPPED);
}
TEST_F(TestZBALOperations, CommGetPropertyNullComm)
{
    zbal_comm_property_t property;
    EXPECT_EQ(zbal_comm_get_property(nullptr, &property), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, CommGetPropertyNullProperty)
{
    EXPECT_EQ(zbal_comm_get_property(reinterpret_cast<zbal_comm_t>(1), nullptr), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, CommGetGlobalWhenNoComms)
{
    EXPECT_EQ(zbal_comm_get_global(), reinterpret_cast<uintptr_t>(nullptr));
}
TEST_F(TestZBALOperations, CommGetByNameNull)
{
    EXPECT_EQ(zbal_comm_get_by_name(nullptr), nullptr);
}
TEST_F(TestZBALOperations, CommDestroyNull)
{
    EXPECT_EQ(zbal_comm_destroy(nullptr, 0), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, CommDestroyAllNoCrash)
{
    EXPECT_NO_THROW(zbal_comm_destroy_all(0));
}
TEST_F(TestZBALOperations, AllReduceNullComm)
{
    int32_t buf = 0;
    EXPECT_EQ(zbal_all_reduce(&buf, &buf, &buf, 1, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, nullptr, nullptr),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, AllReduceZeroCount)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_reduce(&buf, &buf, &buf, 0, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, fakeComm, nullptr),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, AllReduceInvalidDataType)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_reduce(&buf, &buf, &buf, 1, ZBAL_DATA_TYPE_BUTT, ZBAL_REDUCE_SUM, fakeComm, nullptr),
              Z_INVALID_PARAM);
    EXPECT_EQ(
        zbal_all_reduce(&buf, &buf, &buf, 1, static_cast<zbal_datatype_t>(-1), ZBAL_REDUCE_SUM, fakeComm, nullptr),
        Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, AllReduceInvalidOp)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_reduce(&buf, &buf, &buf, 1, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_BUTT, fakeComm, nullptr),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, AllReduceNullRecvBuff)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_reduce(&buf, nullptr, &buf, 1, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, fakeComm, nullptr),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, AllReduceNullBuffer)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_reduce(&buf, &buf, nullptr, 1, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, fakeComm, nullptr),
              Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, ReduceScatterNullComm)
{
    int32_t buf = 0;
    EXPECT_EQ(zbal_reduce_scatter(&buf, &buf, 1, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, nullptr, nullptr),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, ReduceScatterNullRecvBuff)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_reduce_scatter(&buf, nullptr, 1, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, fakeComm, nullptr),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, ReduceScatterZeroRecvCount)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_reduce_scatter(&buf, &buf, 0, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, fakeComm, nullptr),
              Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, AllGatherNullComm)
{
    int32_t buf = 0;
    EXPECT_EQ(zbal_all_gather(&buf, &buf, 1, ZBAL_DATA_TYPE_INT32, nullptr, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, AllGatherNullRecvBuff)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_gather(&buf, nullptr, 1, ZBAL_DATA_TYPE_INT32, fakeComm, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, AllGatherZeroSendCount)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_gather(&buf, &buf, 0, ZBAL_DATA_TYPE_INT32, fakeComm, nullptr), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, BarrierNullComm)
{
    EXPECT_EQ(zbal_barrier(nullptr, nullptr), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, AlltoAllVNullComm)
{
    int32_t buf = 0;
    EXPECT_EQ(zbal_all_to_all_v(&buf, &buf, &buf, &buf, &buf, ZBAL_DATA_TYPE_INT32, nullptr, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, AlltoAllVNullSendBuff)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_to_all_v(nullptr, &buf, &buf, &buf, &buf, ZBAL_DATA_TYPE_INT32, fakeComm, nullptr),
              Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, BroadcastNullComm)
{
    int32_t buf = 0;
    EXPECT_EQ(zbal_broadcast(&buf, 1, ZBAL_DATA_TYPE_INT32, 0, nullptr, nullptr), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, ScatterNullComm)
{
    int32_t buf = 0;
    EXPECT_EQ(zbal_scatter(&buf, &buf, 1, ZBAL_DATA_TYPE_INT32, 0, nullptr, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, ScatterNullRecvBuff)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_scatter(&buf, nullptr, 1, ZBAL_DATA_TYPE_INT32, 0, fakeComm, nullptr), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, SendNullComm)
{
    int32_t buf = 0;
    EXPECT_EQ(zbal_send(&buf, ZBAL_DATA_TYPE_INT32, 0, nullptr, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, SendNullBuff)
{
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_send(nullptr, ZBAL_DATA_TYPE_INT32, 0, fakeComm, nullptr), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, RecvNullComm)
{
    int32_t buf = 0;
    EXPECT_EQ(zbal_recv(&buf, 1, ZBAL_DATA_TYPE_INT32, 0, nullptr, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, RecvNullBuff)
{
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_recv(nullptr, 1, ZBAL_DATA_TYPE_INT32, 0, fakeComm, nullptr), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, CommCreateNameTooLong)
{
    zbal_comm_options_t options;
    std::memset(&options, 0, sizeof(options));
    options.name = const_cast<char *>(
        "1234567890123456789012345678901234567890123456789012345678901234567890"
        "12345678901234567890123456789012345678901234567890123456789012345678"); // 128 chars, not > MAX
    zbal_comm_t comm;
    EXPECT_EQ(zbal_comm_create(&options, &comm), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, CommCreateWorldSizeMismatch)
{
    auto &state = ZBALInitState::Instance();
    state.Bootstrapped(true);
    state.ext_.worldSize = ZBAL_UT_NUM_8;

    zbal_comm_options_t options;
    std::memset(&options, 0, sizeof(options));
    options.name = const_cast<char *>("test_group");
    options.isWorldGroup = 1;
    options.groupSize = ZBAL_UT_NUM_4; // not equal to worldSize 8
    zbal_comm_t comm;
    EXPECT_EQ(zbal_comm_create(&options, &comm), Z_NOT_BOOTSTRAPPED);
}

TEST_F(TestZBALOperations, CommCreateSubGroupTooLarge)
{
    auto &state = ZBALInitState::Instance();
    state.Bootstrapped(true);
    state.ext_.worldSize = ZBAL_UT_NUM_8;

    zbal_comm_options_t options;
    std::memset(&options, 0, sizeof(options));
    options.name = const_cast<char *>("test_sub");
    options.isWorldGroup = 0;
    options.groupSize = ZBAL_UT_NUM_16; // bigger than worldSize 8
    zbal_comm_t comm;
    EXPECT_EQ(zbal_comm_create(&options, &comm), Z_NOT_BOOTSTRAPPED);
}
TEST_F(TestZBALOperations, AllReduceNullSendBuffReturnsOk)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_reduce(nullptr, &buf, &buf, 1, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, fakeComm, nullptr), Z_OK);
}

TEST_F(TestZBALOperations, ReduceScatterNullSendBuffReturnsOk)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_reduce_scatter(nullptr, &buf, 1, ZBAL_DATA_TYPE_INT32, ZBAL_REDUCE_SUM, fakeComm, nullptr), Z_OK);
}

TEST_F(TestZBALOperations, AllGatherNullSendBuffReturnsOk)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_all_gather(nullptr, &buf, 1, ZBAL_DATA_TYPE_INT32, fakeComm, nullptr), Z_OK);
}

TEST_F(TestZBALOperations, BroadcastNullBufReturnsOk)
{
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_broadcast(nullptr, 1, ZBAL_DATA_TYPE_INT32, 0, fakeComm, nullptr), Z_OK);
}
TEST_F(TestZBALOperations, BroadcastInvalidDataType)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_broadcast(&buf, 1, ZBAL_DATA_TYPE_BUTT, 0, fakeComm, nullptr), Z_INVALID_PARAM);
    EXPECT_EQ(zbal_broadcast(&buf, 1, static_cast<zbal_datatype_t>(-1), 0, fakeComm, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, ScatterInvalidDataType)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_scatter(&buf, &buf, 1, ZBAL_DATA_TYPE_BUTT, 0, fakeComm, nullptr), Z_INVALID_PARAM);
    EXPECT_EQ(zbal_scatter(&buf, &buf, 1, static_cast<zbal_datatype_t>(-1), 0, fakeComm, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, SendInvalidDataType)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_send(&buf, ZBAL_DATA_TYPE_BUTT, 0, fakeComm, nullptr), Z_INVALID_PARAM);
    EXPECT_EQ(zbal_send(&buf, static_cast<zbal_datatype_t>(-1), 0, fakeComm, nullptr), Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, RecvInvalidDataType)
{
    int32_t buf = 0;
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_recv(&buf, 1, ZBAL_DATA_TYPE_BUTT, 0, fakeComm, nullptr), Z_INVALID_PARAM);
    EXPECT_EQ(zbal_recv(&buf, 1, static_cast<zbal_datatype_t>(-1), 0, fakeComm, nullptr), Z_INVALID_PARAM);
}
TEST_F(TestZBALOperations, DispatchNormalNotifyNullSendTokens)
{
    zbal_tensor_info_t info;
    std::memset(&info, 0, sizeof(info));
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_dispatch_normal_notify(nullptr, 1, 1, &info, &info, &info, &info, &info, fakeComm, nullptr, 0),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, DispatchNormalLayoutNullTopkIndex)
{
    zbal_tensor_info_t info;
    std::memset(&info, 0, sizeof(info));
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_dispatch_normal_layout(nullptr, 1, 1, 1, &info, &info, &info, &info, fakeComm, nullptr, 0),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, DispatchNormalNullSrcTokens)
{
    zbal_tensor_info_t info;
    std::memset(&info, 0, sizeof(info));
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(
        zbal_dispatch_normal(nullptr, &info, &info, &info, &info, 1, NO_QUANT, &info, &info, fakeComm, nullptr, 0),
        Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, CombineNormalNullSrcTokens)
{
    zbal_tensor_info_t info;
    std::memset(&info, 0, sizeof(info));
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_combine_normal(nullptr, &info, &info, &info, &info, &info, 1, &info, fakeComm, nullptr, 0),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, DispatchLowLatencyNullX)
{
    zbal_tensor_info_t info;
    std::memset(&info, 0, sizeof(info));
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_dispatch_low_latency(nullptr, &info, 1, 0, 0, 0, 0, 0, 0, &info, &info, &info, &info, &info, &info,
                                        &info, fakeComm, nullptr, 0),
              Z_INVALID_PARAM);
}

TEST_F(TestZBALOperations, CombineLowLatencyNullExpandX)
{
    zbal_tensor_info_t info;
    std::memset(&info, 0, sizeof(info));
    zbal_comm_t fakeComm = reinterpret_cast<zbal_comm_t>(1);
    EXPECT_EQ(zbal_combine_low_latency(nullptr, &info, &info, &info, &info, &info, 1, fakeComm, nullptr, 0),
              Z_INVALID_PARAM);
}