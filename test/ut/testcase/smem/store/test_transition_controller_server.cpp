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

#include <vector>
#include <functional>

#include "smem_group_manager_server.h"
#include "smem_message_packer.h"

namespace ock {
namespace smem {

constexpr uint32_t K_RANK_TWO = 2;
constexpr uint32_t K_RANK_FOUR = 4;
constexpr uint32_t K_MAX_RANKS = 8;
constexpr uint32_t K_DRAIN_WAIT_MS = 300;
constexpr uint32_t K_ESTABLISH_WAIT_MS = 500;
constexpr uint32_t K_ACK_WAIT_MS = 200;

class TestTransitionControllerServer : public ::testing::Test {
protected:
    uint32_t lastTargetRank_{0};
    std::vector<uint8_t> lastSentData_;
    int sendResult_{0};

    SmemGroupCommandSender MakeSender()
    {
        return [this](uint32_t targetRankId, const std::vector<uint8_t> &data) -> int {
            lastTargetRank_ = targetRankId;
            lastSentData_ = data;
            return sendResult_;
        };
    }

    SmemGroupCommandSender MakeNullSender()
    {
        return SmemGroupCommandSender{};
    }

    SmRef<SmemGroupManagerServer> CreateServer(SmemGroupCommandSender sender)
    {
        return SmMakeRef<SmemGroupManagerServer>(std::move(sender), K_MAX_RANKS);
    }

    static RankFullInfo MakeInfo(uint32_t rankId)
    {
        RankFullInfo info;
        info.rankId = rankId;
        std::string base = "host" + std::to_string(rankId);
        info.baseInfo.assign(base.begin(), base.end());
        info.externalInfo.push_back(Bytes{static_cast<uint8_t>(rankId)});
        return info;
    }

    void SetUp() override
    {
        lastTargetRank_ = 0;
        lastSentData_.clear();
        sendResult_ = 0;
    }
};

TEST_F(TestTransitionControllerServer, CheckIn_TriggersAddToWhitelistSend)
{
    auto server = CreateServer(MakeSender());
    ASSERT_EQ(server->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(server->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Start the driver/send worker to process the queue */
    server->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_DRAIN_WAIT_MS));
    server->Stop();

    /* The sender should have been called (target could be 0 or 2) */
    EXPECT_FALSE(lastSentData_.empty());

    /* Verify the packed message */
    SmemMessage unpacked;
    auto size = SmemMessagePacker::Unpack(lastSentData_.data(), lastSentData_.size(), unpacked);
    EXPECT_GT(size, 0);
    EXPECT_EQ(unpacked.mt, MessageType::CONTROL);
    int8_t controlOp = SmemMessage::GetControlOp(unpacked);
    EXPECT_EQ(controlOp, CONTROL_ADD_TO_WHITELIST);
}

TEST_F(TestTransitionControllerServer, Checkout_TriggersRemoveFromWhitelistSend)
{
    auto server = CreateServer(MakeSender());
    ASSERT_EQ(server->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(server->CheckIn(MakeInfo(K_RANK_FOUR)), 0);

    /* Let the queue drain */
    server->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_DRAIN_WAIT_MS));
    server->Stop();

    lastTargetRank_ = 0;
    lastSentData_.clear();

    /* Checkout notifies peers via RemoveFromWhitelist (LEAVE_NOTIFY was removed) */
    ASSERT_EQ(server->Checkout(K_RANK_FOUR), 0);
    EXPECT_EQ(server->GetStates()[K_RANK_FOUR], RANK_IDLE);
}

TEST_F(TestTransitionControllerServer, NullSender_ServerStillOperates)
{
    /* Null sender should not crash, operations still work */
    auto server = CreateServer(MakeNullSender());
    ASSERT_EQ(server->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(server->CheckIn(MakeInfo(1)), 0);
    EXPECT_EQ(server->GetStates()[0], RANK_CHECKED_IN);
}

TEST_F(TestTransitionControllerServer, QueryLinkState_WithAliveRanks)
{
    auto server = CreateServer(MakeSender());
    ASSERT_EQ(server->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(server->CheckIn(MakeInfo(1)), 0);

    server->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    server->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});

    server->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_ESTABLISH_WAIT_MS));
    server->Stop();

    server->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    server->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});

    server->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_ACK_WAIT_MS));
    server->Stop();
    EXPECT_EQ(server->GetStates()[0], RANK_ACTIVE);
    EXPECT_EQ(server->GetStates()[1], RANK_ACTIVE);
}

} // namespace smem
} // namespace ock
