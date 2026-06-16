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

// Need access to private static members to register dummy communicator and mock DlCannApi
#define private public
#include "zbal_communicator.h"
#undef private

#define private public
#include "dl_cann_api.h"
#undef private

#include "zbal_communicator_dummy.h"
#include "zbal_deepep.h"
#include "zbal_deepep_config.h"

// acl stub functions (from acl_stub.cpp)
extern "C" {
int32_t aclrtGetDevice(int32_t *deviceId);
}

using namespace zbal;
using namespace zbal::adaptor::deep_ep;
using namespace zbal::operators;
class TestDeepEPBuffer : public testing::Test {
protected:
    void SetUp() override
    {
        CleanupStaticState();
        RegisterDummyComm();
        // Mock DlCannApi function pointers to use acl stub implementations
        zbal::underapi::DlCannApi::pAclrtGetDevice = aclrtGetDevice;
    }

    void TearDown() override
    {
        CleanupStaticState();
    }

    static void CleanupStaticState()
    {
        Communicator::gCommLookupMap.clear();
        Communicator::gCommLookupMapByName.clear();
        Communicator::gWorldCommunicator = nullptr;
    }

    void RegisterDummyComm()
    {
        CommGroupOptions options;
        options.name = kMoeGroupName;
        options.worldSize = ZBAL_UT_NUM_8;
        options.groupSize = ZBAL_UT_NUM_8;
        options.myWorldRank = 0;
        options.myGroupRank = 0;
        auto dummyComm = CommunicatorPtr(new CommunicatorDummy(options, false, nullptr));
        Communicator::gCommLookupMap.emplace(reinterpret_cast<uintptr_t>(dummyComm.Get()), dummyComm);
        Communicator::gCommLookupMapByName.emplace(kMoeGroupName, dummyComm);
    }

    static constexpr const char *kMoeGroupName = "test_moe_group";
};

TEST_F(TestDeepEPBuffer, ConstructorRankOutOfRange)
{
    EXPECT_THROW(Buffer(-1, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName),
                 std::runtime_error);
    EXPECT_THROW(Buffer(ZBAL_UT_NUM_8, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName),
                 std::runtime_error);
}

TEST_F(TestDeepEPBuffer, ConstructorInvalidGroupName)
{
    EXPECT_THROW(Buffer(ZBAL_UT_DEVICE_ID, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, ""),
                 std::runtime_error);
    std::string longName(ZBAL_UT_NUM_128, 'x');
    EXPECT_THROW(Buffer(ZBAL_UT_DEVICE_ID, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, longName),
                 std::runtime_error);
}

TEST_F(TestDeepEPBuffer, BufferBasicAccessors)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName);
    EXPECT_FALSE(buf.is_available());
    EXPECT_FALSE(buf.is_internode_available());
    EXPECT_GE(buf.get_num_rdma_ranks(), 1);
    EXPECT_GE(buf.get_rdma_rank(), 0);
    buf.clean_low_latency_buffer(ZBAL_UT_NUM_16, ZBAL_UT_NUM_512, ZBAL_UT_NUM_4);

    // low_latency_mode constructor
    Buffer bufLL(0, ZBAL_UT_NUM_8, ZBAL_UT_NUM_512, ZBAL_UT_SIZE_1KB, true, kMoeGroupName);
    EXPECT_FALSE(bufLL.is_available());
}
TEST_F(TestDeepEPBuffer, GetDispatchLayoutInvalidInputs)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName);
    std::optional<EventHandle> prev_event;

    // 1D topk_idx should be 2D
    auto topk_1d = torch::ones({ZBAL_UT_NUM_4}, at::kInt);
    EXPECT_THROW(buf.get_dispatch_layout(topk_1d, ZBAL_UT_NUM_4, prev_event, false, false), std::runtime_error);

    // non-contiguous topk_idx
    auto topk_nc = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt).transpose(0, 1).contiguous().transpose(0, 1);
    EXPECT_THROW(buf.get_dispatch_layout(topk_nc, ZBAL_UT_NUM_4, prev_event, false, false), std::runtime_error);

    // invalid num_experts
    auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt);
    EXPECT_THROW(buf.get_dispatch_layout(topk_idx, 0, prev_event, false, false), std::runtime_error);
    EXPECT_THROW(buf.get_dispatch_layout(topk_idx, -1, prev_event, false, false), std::runtime_error);
}

TEST_F(TestDeepEPBuffer, GetDispatchLayoutSuccess)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName);
    auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt);
    std::optional<EventHandle> prev_event;

    auto [num_per_rank, num_per_rdma, num_per_expert, is_token_in_rank, event] =
        buf.get_dispatch_layout(topk_idx, ZBAL_UT_NUM_4, prev_event, false, false);

    EXPECT_EQ(num_per_rank.size(0), ZBAL_UT_NUM_8);
    EXPECT_EQ(num_per_expert.size(0), ZBAL_UT_NUM_4);
    EXPECT_EQ(is_token_in_rank.size(0), ZBAL_UT_NUM_4);
    EXPECT_EQ(is_token_in_rank.size(1), ZBAL_UT_NUM_8);
    EXPECT_FALSE(event.has_value());
    EXPECT_FALSE(buf.is_internode_available());
}
TEST_F(TestDeepEPBuffer, IntranodeDispatchInvalidInputs)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName);
    Config config(ZBAL_UT_NUM_8, ZBAL_UT_NUM_16, ZBAL_UT_NUM_32, ZBAL_UT_NUM_64, ZBAL_UT_NUM_128);
    std::optional<EventHandle> prev_event;
    std::optional<at::Tensor> x_scales;
    std::optional<at::Tensor> topk_weights;

    // missing num_tokens_per_rank / num_tokens_per_expert
    {
        auto x = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_512}, at::kFloat);
        auto is_token_in_rank = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_8}, at::kInt);
        std::optional<at::Tensor> empty_topk_idx, empty_num_tokens_per_rank, empty_num_tokens_per_expert;
        EXPECT_THROW(buf.intranode_dispatch(x, x_scales, empty_topk_idx, topk_weights, empty_num_tokens_per_rank,
                                            is_token_in_rank, empty_num_tokens_per_expert, ZBAL_UT_NUM_16, config,
                                            prev_event, false, false, false),
                     std::runtime_error);
    }
    // 1D x (should be 2D)
    {
        auto x_1d = torch::ones({ZBAL_UT_NUM_4}, at::kFloat);
        auto is_token_in_rank = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_8}, at::kInt);
        auto num_tokens_per_rank = torch::ones({ZBAL_UT_NUM_8}, at::kInt);
        auto num_tokens_per_expert = torch::ones({ZBAL_UT_NUM_4}, at::kInt);
        auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt);
        EXPECT_THROW(buf.intranode_dispatch(x_1d, x_scales, topk_idx, topk_weights, num_tokens_per_rank,
                                            is_token_in_rank, num_tokens_per_expert, ZBAL_UT_NUM_16, config, prev_event,
                                            false, false, false),
                     std::runtime_error);
    }
    // 2D tokens_per_expert (should be 1D)
    {
        auto x = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_512}, at::kFloat);
        auto is_token_in_rank = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_8}, at::kInt);
        auto num_tokens_per_rank = torch::ones({ZBAL_UT_NUM_8}, at::kInt);
        auto num_tokens_per_expert = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt);
        auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt);
        EXPECT_THROW(buf.intranode_dispatch(x, x_scales, topk_idx, topk_weights, num_tokens_per_rank, is_token_in_rank,
                                            num_tokens_per_expert, ZBAL_UT_NUM_16, config, prev_event, false, false,
                                            false),
                     std::runtime_error);
    }
}

TEST_F(TestDeepEPBuffer, IntranodeDispatchSuccessNoQuant)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName);
    auto x = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_512}, at::kFloat);
    auto is_token_in_rank = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_8}, at::kInt);
    auto num_tokens_per_rank = torch::ones({ZBAL_UT_NUM_8}, at::kInt);
    auto num_tokens_per_expert = torch::ones({ZBAL_UT_NUM_8}, at::kInt);
    auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt);
    std::optional<at::Tensor> x_scales;
    std::optional<at::Tensor> topk_weights;
    std::optional<EventHandle> prev_event;
    Config config(ZBAL_UT_NUM_8, ZBAL_UT_NUM_16, ZBAL_UT_NUM_32, ZBAL_UT_NUM_64, ZBAL_UT_NUM_128);

    // send_token_idx must be initialized via get_dispatch_layout before calling intranode_dispatch
    buf.get_dispatch_layout(topk_idx, ZBAL_UT_NUM_8, prev_event, false, false);

    auto [recv_x, recv_scales, recv_topk_idx, recv_topk_weights, num_recv_per_expert, put_offset, balance_matrix,
          event] =
        buf.intranode_dispatch(x, x_scales, topk_idx, topk_weights, num_tokens_per_rank, is_token_in_rank,
                               num_tokens_per_expert, ZBAL_UT_NUM_16, config, prev_event, false, false, false);

    EXPECT_GT(recv_x.size(0), 0);
    EXPECT_EQ(recv_x.size(1), ZBAL_UT_NUM_512);
    EXPECT_FALSE(event.has_value());
    EXPECT_EQ(put_offset.size(0), ZBAL_UT_NUM_8);
    EXPECT_EQ(put_offset.size(1), ZBAL_UT_NUM_8);
}
TEST_F(TestDeepEPBuffer, IntranodeCombineInvalidInputs)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName);
    auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt);
    auto topk_weights = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kFloat);
    auto put_offset = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_8}, at::kInt);
    auto balance_matrix = torch::ones({ZBAL_UT_NUM_8, ZBAL_UT_NUM_16}, at::kInt);
    std::optional<EventHandle> prev_event;

    // 1D x (should be 2D)
    auto x_1d = torch::ones({ZBAL_UT_NUM_4}, at::kFloat);
    EXPECT_THROW(
        buf.intranode_combine(x_1d, topk_idx, topk_weights, put_offset, balance_matrix, prev_event, false, false),
        std::runtime_error);

    // missing topk_weights
    auto x = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_512}, at::kFloat);
    std::optional<at::Tensor> empty_topk_weights;
    EXPECT_THROW(
        buf.intranode_combine(x, topk_idx, empty_topk_weights, put_offset, balance_matrix, prev_event, false, false),
        std::runtime_error);
}

TEST_F(TestDeepEPBuffer, IntranodeCombineSuccess)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, false, kMoeGroupName);
    auto x = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_512}, at::kFloat);
    auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kInt);
    auto topk_weights = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kFloat);
    auto put_offset = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_8}, at::kInt);
    auto balance_matrix = torch::ones({ZBAL_UT_NUM_8, ZBAL_UT_NUM_16}, at::kInt);
    std::optional<EventHandle> prev_event;

    // send_token_idx must be initialized via get_dispatch_layout before calling intranode_combine
    buf.get_dispatch_layout(topk_idx, ZBAL_UT_NUM_4, prev_event, false, false);

    auto [combined_x, recv_topk_weights, event] =
        buf.intranode_combine(x, topk_idx, topk_weights, put_offset, balance_matrix, prev_event, false, false);

    EXPECT_EQ(combined_x.size(0), ZBAL_UT_NUM_4);
    EXPECT_EQ(combined_x.size(1), ZBAL_UT_NUM_512);
    EXPECT_FALSE(event.has_value());
}
TEST_F(TestDeepEPBuffer, LowLatencyDispatch)
{
    auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kLong);
    std::optional<at::Tensor> cumu_stats;

    // basic success path
    {
        Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, true, kMoeGroupName);
        auto x = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_512}, at::kBFloat16);
        auto [recv_x, recv_scales, recv_count, expand_idx, ep_recv_count, event, recv_hook] = buf.low_latency_dispatch(
            x, topk_idx, cumu_stats, ZBAL_UT_NUM_16, ZBAL_UT_NUM_8, false, false, false, false, false);

        EXPECT_GT(recv_x.size(0), 0);
        EXPECT_EQ(recv_x.size(1), ZBAL_UT_NUM_512);
        EXPECT_GT(recv_count.size(0), 0);
        EXPECT_FALSE(event.has_value());
        EXPECT_TRUE(recv_hook.has_value());
    }
    // FP8 quantization path
    {
        Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, true, kMoeGroupName);
        auto x = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_512}, at::kBFloat16);
        auto [recv_x, recv_scales, recv_count, expand_idx, ep_recv_count, event, recv_hook] = buf.low_latency_dispatch(
            x, topk_idx, cumu_stats, ZBAL_UT_NUM_16, ZBAL_UT_NUM_8, true, false, false, false, false);

        EXPECT_GT(recv_x.size(0), 0);
        EXPECT_EQ(recv_x.scalar_type(), at::kChar);
    }
    // empty input (padding)
    {
        Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, true, kMoeGroupName);
        auto x = torch::empty({0, ZBAL_UT_NUM_512}, at::kBFloat16);
        auto empty_topk = torch::empty({0, ZBAL_UT_NUM_2}, at::kLong);
        auto [recv_x, recv_scales, recv_count, expand_idx, ep_recv_count, event, recv_hook] = buf.low_latency_dispatch(
            x, empty_topk, cumu_stats, ZBAL_UT_NUM_16, ZBAL_UT_NUM_8, false, false, false, false, false);

        EXPECT_GT(recv_x.size(0), 0);
    }
}
TEST_F(TestDeepEPBuffer, LowLatencyCombineInvalidX)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, true, kMoeGroupName);
    auto x = torch::ones({ZBAL_UT_NUM_4}, at::kInt); // not bfloat16 and not 2D
    auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kLong);
    auto topk_weights = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kFloat);
    auto src_info = torch::ones({ZBAL_UT_NUM_4}, at::kInt);
    auto layout_range = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_8}, at::kInt);
    auto packed_recv_count = torch::ones({ZBAL_UT_NUM_4}, at::kLong);
    std::optional<at::Tensor> out;

    EXPECT_THROW(buf.low_latency_combine(x, topk_idx, topk_weights, src_info, layout_range, ZBAL_UT_NUM_16,
                                         ZBAL_UT_NUM_4, packed_recv_count, false, false, false, out),
                 std::runtime_error);
}

TEST_F(TestDeepEPBuffer, LowLatencyCombineSuccess)
{
    Buffer buf(0, ZBAL_UT_NUM_8, ZBAL_UT_SIZE_1KB, ZBAL_UT_SIZE_1KB, true, kMoeGroupName);
    auto x = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_512}, at::kBFloat16);
    auto topk_idx = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kLong);
    auto topk_weights = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_2}, at::kFloat);
    auto src_info = torch::ones({ZBAL_UT_NUM_4}, at::kInt);
    auto layout_range = torch::ones({ZBAL_UT_NUM_4, ZBAL_UT_NUM_8}, at::kInt);
    auto packed_recv_count = torch::ones({ZBAL_UT_NUM_4}, at::kLong);
    std::optional<at::Tensor> out;

    auto [combined_x, event, recv_hook] =
        buf.low_latency_combine(x, topk_idx, topk_weights, src_info, layout_range, ZBAL_UT_NUM_16, ZBAL_UT_NUM_8,
                                packed_recv_count, false, false, false, out);

    EXPECT_EQ(combined_x.size(1), ZBAL_UT_NUM_512);
    EXPECT_FALSE(event.has_value());
    EXPECT_TRUE(recv_hook.has_value());
}