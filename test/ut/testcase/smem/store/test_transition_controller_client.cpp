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

#include "smem_group_manager_client.h"
#include "smem_group_manager_server.h"

namespace ock {
namespace smem {

constexpr uint32_t K_RANK_TWO = 2;
constexpr uint32_t K_RANK_THREE = 3;
constexpr uint32_t K_RANK_FIVE = 5;

class TestTransitionControllerClient : public ::testing::Test {
protected:
    SmemGroupManagerClientPtr ctrl_;

    void SetUp() override
    {
        ctrl_ = SmMakeRef<SmemGroupManagerClient>();
    }

    static RankBaseInfo MakeBaseInfo(uint32_t rankId)
    {
        RankBaseInfo info;
        info.rankId = rankId;
        std::string base = "host" + std::to_string(rankId);
        info.baseInfo.assign(base.begin(), base.end());
        return info;
    }

    static RankFullInfo MakeFullInfo(uint32_t rankId)
    {
        RankFullInfo info;
        info.rankId = rankId;
        std::string base = "host" + std::to_string(rankId);
        info.baseInfo.assign(base.begin(), base.end());
        info.externalInfo.push_back(Bytes{static_cast<uint8_t>(rankId)});
        return info;
    }
};

TEST_F(TestTransitionControllerClient, AddToWhitelist_DefaultReturnsSuccess)
{
    std::vector<RankFullInfo> others{MakeFullInfo(1)};
    EXPECT_EQ(ctrl_->AddToWhitelist(0, others, 0), 0);
    auto &res = ctrl_->GetLastAckResults();
    EXPECT_EQ(res.size(), 1);
    EXPECT_EQ(res[0], 0);
}

TEST_F(TestTransitionControllerClient, AddToWhitelist_CallbackInvoked)
{
    int callbackRankId = -1;
    ctrl_->onAddToWhitelist_ = [&](uint32_t rankId, const std::vector<RankFullInfo> &, uint64_t) -> int {
        callbackRankId = static_cast<int>(rankId);
        return 0;
    };
    std::vector<RankFullInfo> others{MakeFullInfo(K_RANK_FIVE)};
    EXPECT_EQ(ctrl_->AddToWhitelist(K_RANK_FIVE, others, 0), 0);
    EXPECT_EQ(callbackRankId, static_cast<int>(K_RANK_FIVE));
}

TEST_F(TestTransitionControllerClient, AddToWhitelist_CallbackReturnsError)
{
    ctrl_->onAddToWhitelist_ = [](uint32_t, const std::vector<RankFullInfo> &, uint64_t) -> int { return -1; };
    std::vector<RankFullInfo> others{MakeFullInfo(1)};
    EXPECT_EQ(ctrl_->AddToWhitelist(0, others, 0), -1);
}

TEST_F(TestTransitionControllerClient, RemoveFromWhitelist_DefaultReturnsSuccess)
{
    std::vector<RankBaseInfo> others{MakeBaseInfo(K_RANK_TWO)};
    EXPECT_EQ(ctrl_->RemoveFromWhitelist(0, others, 0), 0);
    auto &res = ctrl_->GetLastAckResults();
    EXPECT_EQ(res.size(), 1);
    EXPECT_EQ(res[0], 0);
}

TEST_F(TestTransitionControllerClient, RemoveFromWhitelist_CallbackInvoked)
{
    int callbackRankId = -1;
    ctrl_->onRemoveFromWhitelist_ = [&](uint32_t rankId, const std::vector<RankFullInfo> &, uint64_t) -> int {
        callbackRankId = static_cast<int>(rankId);
        return 0;
    };
    std::vector<RankBaseInfo> others{MakeBaseInfo(K_RANK_THREE)};
    EXPECT_EQ(ctrl_->RemoveFromWhitelist(K_RANK_THREE, others, 0), 0);
    EXPECT_EQ(callbackRankId, static_cast<int>(K_RANK_THREE));
}

TEST_F(TestTransitionControllerClient, EstablishConnection_DefaultReturnsSuccess)
{
    std::vector<RankFullInfo> others{MakeFullInfo(1)};
    EXPECT_EQ(ctrl_->EstablishConnection(0, others, 0), 0);
    auto &res = ctrl_->GetLastAckResults();
    EXPECT_EQ(res.size(), 1);
    EXPECT_EQ(res[0], 0);
}

TEST_F(TestTransitionControllerClient, EstablishConnection_CallbackInvoked)
{
    int callbackRankId = -1;
    ctrl_->onEstablishConnection_ = [&](uint32_t rankId, const std::vector<RankFullInfo> &, uint64_t) -> int {
        callbackRankId = static_cast<int>(rankId);
        return 0;
    };
    std::vector<RankFullInfo> others{MakeFullInfo(K_RANK_TWO)};
    EXPECT_EQ(ctrl_->EstablishConnection(K_RANK_TWO, others, 0), 0);
    EXPECT_EQ(callbackRankId, static_cast<int>(K_RANK_TWO));
}

TEST_F(TestTransitionControllerClient, CloseConnection_DefaultReturnsSuccess)
{
    std::vector<uint32_t> ids{1, K_RANK_TWO};
    EXPECT_EQ(ctrl_->CloseConnection(0, ids, 0), 0);
    auto &res = ctrl_->GetLastAckResults();
    EXPECT_EQ(res.size(), K_RANK_TWO);
    EXPECT_EQ(res[0], 0);
    EXPECT_EQ(res[1], 0);
}

TEST_F(TestTransitionControllerClient, CloseConnection_CallbackInvoked)
{
    uint32_t callbackRankId = 0;
    ctrl_->onCloseConnection_ = [&](uint32_t rankId, const std::vector<uint32_t> &, uint64_t) -> int {
        callbackRankId = rankId;
        return 0;
    };
    std::vector<uint32_t> ids{1};
    EXPECT_EQ(ctrl_->CloseConnection(K_RANK_FIVE, ids, 0), 0);
    EXPECT_EQ(callbackRankId, static_cast<int>(K_RANK_FIVE));
}

TEST_F(TestTransitionControllerClient, QueryLinkState_DefaultReturnsSuccess)
{
    EXPECT_EQ(ctrl_->QueryLinkState(0), 0);
    EXPECT_TRUE(ctrl_->GetLastQueryLinkStateEntries().empty());
}

TEST_F(TestTransitionControllerClient, QueryLinkState_CallbackStoresEntries)
{
    ctrl_->onQueryLinkState_ = []() -> std::vector<LinkStateEntry> {
        std::vector<LinkStateEntry> entries;
        entries.push_back({1, static_cast<int8_t>(LINK_CONNECTED)});
        entries.push_back({K_RANK_TWO, static_cast<int8_t>(LINK_IDLE)});
        return entries;
    };
    EXPECT_EQ(ctrl_->QueryLinkState(0), 0);
    auto &entries = ctrl_->GetLastQueryLinkStateEntries();
    EXPECT_EQ(entries.size(), K_RANK_TWO);
    EXPECT_EQ(entries[0].dstRankId, 1);
    EXPECT_EQ(entries[0].state, static_cast<int8_t>(LINK_CONNECTED));
    EXPECT_EQ(entries[1].dstRankId, K_RANK_TWO);
    EXPECT_EQ(entries[1].state, static_cast<int8_t>(LINK_IDLE));
}

TEST_F(TestTransitionControllerClient, LeaveNotify_DefaultReturnsSuccess)
{
    EXPECT_EQ(ctrl_->LeaveNotify(0, 1), 0);
}

TEST_F(TestTransitionControllerClient, LeaveNotify_CallbackInvoked)
{
    uint32_t callbackLeavingRank = 0;
    ctrl_->onLeaveNotify_ = [&](uint32_t leavingRankId) -> int {
        callbackLeavingRank = leavingRankId;
        return 0;
    };
    EXPECT_EQ(ctrl_->LeaveNotify(K_RANK_FIVE, K_RANK_THREE), 0);
    EXPECT_EQ(callbackLeavingRank, K_RANK_THREE);
}

TEST_F(TestTransitionControllerClient, LeaveNotify_CallbackReturnsError)
{
    ctrl_->onLeaveNotify_ = [](uint32_t) -> int { return -1; };
    EXPECT_EQ(ctrl_->LeaveNotify(0, 1), -1);
}

TEST_F(TestTransitionControllerClient, AckResultsResetOnEachCall)
{
    std::vector<RankFullInfo> others3{MakeFullInfo(1), MakeFullInfo(K_RANK_TWO), MakeFullInfo(K_RANK_THREE)};
    ctrl_->AddToWhitelist(0, others3, 0);
    EXPECT_EQ(ctrl_->GetLastAckResults().size(), K_RANK_THREE);

    std::vector<RankFullInfo> others1{MakeFullInfo(K_RANK_FIVE)};
    ctrl_->AddToWhitelist(0, others1, 0);
    EXPECT_EQ(ctrl_->GetLastAckResults().size(), 1);
}

TEST_F(TestTransitionControllerClient, EmptyOthersListZeroAckResults)
{
    std::vector<RankFullInfo> empty;
    ctrl_->AddToWhitelist(0, empty, 0);
    EXPECT_EQ(ctrl_->GetLastAckResults().size(), 0);
}

} // namespace smem
} // namespace ock
