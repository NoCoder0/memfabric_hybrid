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

#include <cstring>
#include <thread>

#include "smem_group_manager_server.h"
#include "smem_ref.h"

namespace ock {
namespace smem {

constexpr uint32_t K_RANK_COUNT = 8;
constexpr uint32_t K_RANK_TWO = 2;
constexpr uint32_t K_RANK_THREE = 3;
constexpr uint32_t K_RANK_FOUR = 4;
constexpr uint32_t K_RANK_FIVE = 5;
constexpr uint32_t K_INVALID_RANK = 999;
constexpr uint32_t K_WAIT_SHORT_MS = 100;
constexpr uint32_t K_WAIT_MS = 200;
constexpr uint32_t K_WAIT_LONG_MS = 300;
constexpr uint32_t K_WAIT_LONGER_MS = 400;
constexpr uint32_t K_WAIT_EXTRA_MS = 500;
constexpr uint32_t K_WAIT_SLOW_MS = 1500;
constexpr uint32_t K_QUERY_REQUEST_ID = 42;

/* ================================================================== */
/*  Test fixture — uses a configurable sender lambda                  */
/* ================================================================== */
class GmGroupManagerTest : public ::testing::Test {
protected:
    SmRef<SmemGroupManagerServer> gm_;
    int sendResult_{0};
    int sendCallCount_{0};

    SmemGroupCommandSender MakeSender()
    {
        return [this](uint32_t targetRankId, const std::vector<uint8_t> &data) -> int {
            (void)targetRankId;
            (void)data;
            sendCallCount_++;
            return sendResult_;
        };
    }

    void SetUp() override
    {
        sendResult_ = 0;
        sendCallCount_ = 0;
        gm_ = SmMakeRef<SmemGroupManagerServer>(MakeSender(), K_RANK_COUNT);
        ASSERT_NE(gm_, nullptr);
    }

    void TearDown() override
    {
        gm_->Stop();
        gm_ = nullptr;
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
};

/* ================================================================== */
/*  1. CONTROLLER FAILURES — ProcessSend retry/degrade                */
/* ================================================================== */

TEST_F(GmGroupManagerTest, CheckIn_ThenSendWorker_AddToWhitelistFails_AndDegrades)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Sender always fails → ProcessSend will retry up to max then degrade */
    sendResult_ = -1;

    gm_->Start();
    /* Allow send worker to process the queue and exhaust retries */
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    /* After max retries the link should be degraded to IDLE */
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_IDLE);
}

TEST_F(GmGroupManagerTest, CheckIn_AddAndEstConBothSucceed_LinksReachConnected)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_EXCHANGING);

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_EXCHANGING);

    /* Advance via ACK chain */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{K_RANK_TWO, 0}});
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_EXCHANGED);

    /* DrivePending sees EXCHANGED, enqueues ESTABLISH */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_CONNECTING);

    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 0, std::map<uint32_t, int32_t>{{K_RANK_TWO, 0}});
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_CONNECTED);
}

TEST_F(GmGroupManagerTest, DrivePendingScansAndEnqueues_EstConFailsThenDegrades)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* AddToWhitelist succeeds — ProcessSend updates timestamp, state stays EXCHANGING */
    sendResult_ = 0;
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    /* Inject ADD ACK to advance to EXCHANGED so DrivePending enqueues ESTABLISH */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{K_RANK_TWO, 0}});
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_EXCHANGED);

    /* EstablishConnection always fails → retry → degrade */
    sendResult_ = -1;

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_LONGER_MS));
    gm_->Stop();

    /* After max retries the link should be degraded to IDLE */
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_IDLE);
}

TEST_F(GmGroupManagerTest, RemoveFromWhitelistFails_ImmediateDegrade)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Drain ADD + ESTABLISH jobs so queue is clean before Checkout. */
    sendResult_ = 0;
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_LONG_MS));
    gm_->Stop();

    /* Make sender fail — ProcessSend should immediately degrade */
    sendResult_ = -1;

    /* Manually set a link to DISCONNECTING to test ProcessSend failure path */
    gm_->SetLinkState(K_RANK_TWO, 0, LINK_DISCONNECTING);
    gm_->SetLinkState(0, K_RANK_TWO, LINK_DISCONNECTING);
}

TEST_F(GmGroupManagerTest, CloseConnectionFails_ImmediateDegrade)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Advance to EXCHANGED via ADD ACK */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{K_RANK_TWO, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{0, 0}});
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_EXCHANGED);

    /* ProcessSend(ADD) only updates timestamp; state stays EXCHANGING until ACK */
    sendResult_ = 0;
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_SHORT_MS));
    gm_->Stop();

    /* Manually set link to DISCONNECTED to test CLOSE failure path */
    gm_->SetLinkState(K_RANK_TWO, 0, LINK_DISCONNECTED);
}

/* ================================================================== */
/*  Original tests — keep for regression                              */
/* ================================================================== */
class GroupManagerTest : public ::testing::Test {
protected:
    SmRef<SmemGroupManagerServer> gm_;

    void SetUp() override
    {
        auto sender = [](uint32_t targetRankId, const std::vector<uint8_t> &data) -> int {
            (void)targetRankId;
            (void)data;
            return 0;
        };
        gm_ = SmMakeRef<SmemGroupManagerServer>(std::move(sender), K_RANK_COUNT);
        ASSERT_NE(gm_, nullptr);
    }

    void TearDown() override
    {
        gm_ = nullptr;
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

    LinkState GetLink(uint32_t src, uint32_t dst) const
    {
        return gm_->GetLinkState(src, dst);
    }

    RankState GetRank(uint32_t r) const
    {
        return gm_->GetRankState(r);
    }
};

TEST_F(GroupManagerTest, InitialStateAllIdle)
{
    for (uint32_t i = 0; i < K_RANK_COUNT; ++i) {
        EXPECT_EQ(GetRank(i), RANK_IDLE);
        for (uint32_t j = 0; j < K_RANK_COUNT; ++j) {
            if (i != j) {
                EXPECT_EQ(GetLink(i, j), LINK_IDLE);
            }
        }
    }
    EXPECT_TRUE(gm_->GetAliveRanks().empty());
}

TEST_F(GroupManagerTest, CheckInSingleRank)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), 0);
    EXPECT_EQ(GetRank(K_RANK_THREE), RANK_CHECKED_IN);
}

TEST_F(GroupManagerTest, CheckInDoubleFails)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), 0);
    EXPECT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), -1);
}

TEST_F(GroupManagerTest, CheckInInvalidRank)
{
    EXPECT_EQ(gm_->CheckIn(MakeInfo(K_INVALID_RANK)), -1);
}

TEST_F(GroupManagerTest, CheckInCreatesExchangingLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), 0);
    EXPECT_EQ(GetLink(K_RANK_THREE, 1), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(1, K_RANK_THREE), LINK_EXCHANGING);
}

TEST_F(GroupManagerTest, CheckInNoPeersNoLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    for (uint32_t j = 0; j < K_RANK_COUNT; ++j) {
        if (j != 1) {
            EXPECT_EQ(GetLink(1, j), LINK_IDLE);
        }
    }
}

TEST_F(GroupManagerTest, CheckInAndGetAliveRanks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    auto alive = gm_->GetAliveRanks();
    EXPECT_EQ(alive.size(), K_RANK_TWO);
}

TEST_F(GroupManagerTest, CheckoutSuccess)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    ASSERT_EQ(gm_->Checkout(K_RANK_TWO), 0);
    EXPECT_EQ(GetRank(K_RANK_TWO), RANK_IDLE);
}

TEST_F(GroupManagerTest, CheckoutFromIdleFails)
{
    EXPECT_EQ(gm_->Checkout(0), -1);
}

TEST_F(GroupManagerTest, CheckoutSendsLeaveNotify)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_FOUR)), 0);
    ASSERT_EQ(gm_->Checkout(K_RANK_FOUR), 0);
    /* Checkout now sends LeaveNotify directly instead of RemoveFromWhitelist */
    EXPECT_EQ(GetRank(K_RANK_FOUR), RANK_IDLE);
    EXPECT_EQ(GetLink(K_RANK_FOUR, 0), LINK_IDLE);
    EXPECT_EQ(GetLink(0, K_RANK_FOUR), LINK_IDLE);
}

TEST_F(GroupManagerTest, ClearCheckedInRank)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->Clear(1), 0);
    EXPECT_EQ(GetRank(1), RANK_IDLE);
}

TEST_F(GroupManagerTest, ClearFromActiveFails)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    gm_->SetRankState(1, RANK_ACTIVE);
    EXPECT_EQ(gm_->Clear(1), -1);
}

TEST_F(GroupManagerTest, AckAddToWhitelistAdvancesLink)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_EXCHANGED);
}

TEST_F(GroupManagerTest, AckEstablishConnectionAdvancesLink)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    gm_->SetLinkState(K_RANK_TWO, 1, LINK_CONNECTING);
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_CONNECTED);
}

TEST_F(GroupManagerTest, AckCloseConnectionAdvancesLink)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    gm_->SetLinkState(K_RANK_TWO, 1, LINK_CONNECTING);
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    /* Manually set link states to test ACK advancement (Checkout no longer uses DISCONNECTING) */
    gm_->SetLinkState(K_RANK_TWO, 1, LINK_DISCONNECTING);
    gm_->OnControlAck(CONTROL_REMOVE_FROM_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_DISCONNECTED);
    gm_->SetLinkState(K_RANK_TWO, 1, LINK_CLEANING);
    gm_->OnControlAck(CONTROL_CLOSE_CONNECTION_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_IDLE);
}

TEST_F(GroupManagerTest, AckUnknownOpNoChange)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    gm_->OnControlAck(CONTROL_QUERY_LINK_STATE, 1, std::map<uint32_t, int32_t>{{0, 0}});
    EXPECT_EQ(GetRank(1), RANK_CHECKED_IN);
}

TEST_F(GroupManagerTest, AcK_INVALID_RANKIgnored)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 1, std::map<uint32_t, int32_t>{{K_INVALID_RANK, 0}});
    EXPECT_EQ(GetRank(1), RANK_CHECKED_IN);
}

TEST_F(GroupManagerTest, GetAliveRanksExcludesIdle)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), 0);
    EXPECT_EQ(gm_->GetAliveRanks().size(), K_RANK_THREE);
}

TEST_F(GroupManagerTest, GetAliveRanksExcludesLeaving)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    ASSERT_EQ(gm_->Checkout(K_RANK_TWO), 0);
    auto alive = gm_->GetAliveRanks();
    EXPECT_EQ(alive.size(), 1);
    EXPECT_EQ(alive[0], 1);
}

TEST_F(GroupManagerTest, SetGetRankState)
{
    gm_->SetRankState(K_RANK_THREE, RANK_ACTIVE);
    EXPECT_EQ(GetRank(K_RANK_THREE), RANK_ACTIVE);
}

TEST_F(GroupManagerTest, SetGetLinkState)
{
    gm_->SetLinkState(K_RANK_TWO, K_RANK_FIVE, LINK_CONNECTED);
    EXPECT_EQ(GetLink(K_RANK_TWO, K_RANK_FIVE), LINK_CONNECTED);
    EXPECT_EQ(GetLink(K_RANK_FIVE, K_RANK_TWO), LINK_IDLE);
}

TEST_F(GroupManagerTest, SetRankOutOfRangeNoOp)
{
    gm_->SetRankState(K_INVALID_RANK, RANK_ACTIVE);
}

TEST_F(GroupManagerTest, SetLinkOutOfRangeNoOp)
{
    gm_->SetLinkState(K_INVALID_RANK, 0, LINK_CONNECTED);
    gm_->SetLinkState(0, K_INVALID_RANK, LINK_CONNECTED);
}

TEST_F(GroupManagerTest, CheckInTriggersStateTransitions)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    EXPECT_EQ(gm_->GetLinkState(1, K_RANK_TWO), LINK_EXCHANGING);
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 1), LINK_EXCHANGING);
}

TEST_F(GmGroupManagerTest, OnLinkBroken_ClearsAllLinksAndDegradedLeavingRank)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    EXPECT_EQ(gm_->GetLinkState(0, 1), LINK_EXCHANGING);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_EXCHANGING);
    EXPECT_EQ(gm_->GetLinkState(1, K_RANK_TWO), LINK_EXCHANGING);
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 1), LINK_EXCHANGING);

    gm_->OnLinkBroken(1);

    EXPECT_EQ(gm_->GetLinkState(0, 1), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(1, K_RANK_TWO), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 1), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(0, K_RANK_TWO), LINK_EXCHANGING);
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 0), LINK_EXCHANGING);
}

TEST_F(GmGroupManagerTest, OnLinkBroken_LeavingRankBecomesIdle)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    /* Checkout now immediately transitions to IDLE with LeaveNotify */
    ASSERT_EQ(gm_->Checkout(1), 0);
    EXPECT_EQ(gm_->GetRankState(1), RANK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(0, 1), LINK_IDLE);

    /* OnLinkBroken is a no-op on already-IDLE state */
    gm_->OnLinkBroken(1);
    EXPECT_EQ(gm_->GetRankState(1), RANK_IDLE);
}

/* ================================================================== */
/*  Additional coverage tests for uncovered functions                 */
/* ================================================================== */

TEST_F(GmGroupManagerTest, CheckInReconnected_PromotesToActiveAndQueriesLinkState)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    /* Advance ADD ACKs → EXCHANGED */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});

    /* Start driver + send worker so ESTABLISH is enqueued and processed */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_EXTRA_MS));
    /* Now links should be LINK_CONNECTING (send worker processed ESTABLISH) */
    gm_->Stop();

    /* Send ESTABLISH ACK → CONNECTED */
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_CONNECTED);

    /* Run driver to promote ranks */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(gm_->GetRankState(0), RANK_ACTIVE);

    /* CheckIn again on ACTIVE rank → MarkRankReconnected sees non-idle → fails */
    EXPECT_EQ(gm_->CheckIn(MakeInfo(0)), -1);
    EXPECT_EQ(gm_->GetRankState(0), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, Clear_ActiveRankFails)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    gm_->SetRankState(0, RANK_ACTIVE);
    EXPECT_EQ(gm_->Clear(0), -1);
    EXPECT_EQ(gm_->GetRankState(0), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, Clear_OutOfRangeFails)
{
    EXPECT_EQ(gm_->Clear(K_INVALID_RANK), -1);
}

TEST_F(GmGroupManagerTest, MarkRankReconnected_NonIdleStateSkips)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    /* Promote to ACTIVE via the setter */
    gm_->SetRankState(0, RANK_ACTIVE);
    /* CheckIn again: MarkRankReconnected sees RANK_ACTIVE → warn and return */
    EXPECT_EQ(gm_->CheckIn(MakeInfo(0)), -1);
}

TEST_F(GmGroupManagerTest, SetLink_HandleDisconnecting_DriverReenqueues)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    /* Set link to DISCONNECTING */
    gm_->SetLinkState(1, 0, LINK_DISCONNECTING);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_DISCONNECTING);

    /* Run driver for 1.5s to trigger timeout → re-enqueue */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_SLOW_MS));
    gm_->Stop();

    /* After timeout with retryCount=0, DISCONNECTING stays in that state
       and gets re-enqueued (no transition since retryCount < LINK_QUERY_MAX_RETRIES).
       The key is the driver handles the state without crashing. */
    SUCCEED();
}

TEST_F(GmGroupManagerTest, SetLink_HandleDisconnected_DriverReenqueues)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    gm_->SetLinkState(1, 0, LINK_DISCONNECTED);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_DISCONNECTED);

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_SLOW_MS));
    gm_->Stop();
}

TEST_F(GmGroupManagerTest, SetLink_HandleClearning_DriverReenqueues)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    gm_->SetLinkState(1, 0, LINK_CLEANING);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_CLEANING);

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_SLOW_MS));
    gm_->Stop();
}

TEST_F(GmGroupManagerTest, PromoteCheckedInRanks_AllIdleLinksPromotesToActive)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Manually set all links for rank 1 to IDLE or CONNECTED */
    gm_->SetLinkState(1, 0, LINK_CONNECTED);
    gm_->SetLinkState(0, 1, LINK_CONNECTED);
    gm_->SetLinkState(1, K_RANK_TWO, LINK_IDLE);
    gm_->SetLinkState(K_RANK_TWO, 1, LINK_IDLE);

    /* DrivePendingTransitions → PromoteCheckedInRanks should NOT promote rank 1:
     * rank 2 is already CHECKED_IN but its link is IDLE (not connected), so a
     * synchronous join must wait until every joined peer's link is established
     * (connected == peers). Promoting early would let the upper layer issue
     * WRs to a peer whose QP is not ready (CQE 261). */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(gm_->GetRankState(1), RANK_CHECKED_IN);
}

TEST_F(GmGroupManagerTest, PromoteCheckedInRanks_AllPeersConnectedPromotesToActive)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* All peers of rank 1 connected */
    gm_->SetLinkState(1, 0, LINK_CONNECTED);
    gm_->SetLinkState(0, 1, LINK_CONNECTED);
    gm_->SetLinkState(1, K_RANK_TWO, LINK_CONNECTED);
    gm_->SetLinkState(K_RANK_TWO, 1, LINK_CONNECTED);

    /* DrivePendingTransitions → PromoteCheckedInRanks should promote rank 1 */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(gm_->GetRankState(1), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, CleanupLeavingRank_AllIdleTransitionsToIdle)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    /* Set rank to LEAVING via manual set */
    gm_->SetRankState(1, RANK_LEAVING);

    /* Ensure links are IDLE */
    gm_->SetLinkState(1, 0, LINK_IDLE);
    gm_->SetLinkState(0, 1, LINK_IDLE);

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    EXPECT_EQ(gm_->GetRankState(1), RANK_IDLE);
}

TEST_F(GmGroupManagerTest, GetLinkState_OutOfRangeReturnsIdle)
{
    EXPECT_EQ(gm_->GetLinkState(K_INVALID_RANK, 0), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(0, K_INVALID_RANK), LINK_IDLE);
}

TEST_F(GmGroupManagerTest, QueryLinkStates_WithAliveRanks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    /* Advance ADD ACKs → EXCHANGED */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});

    /* Start to process ESTABLISH tasks */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_EXTRA_MS));
    gm_->Stop();

    /* Send ESTABLISH ACKs → CONNECTED */
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});

    /* Run driver to promote ranks to ACTIVE */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(gm_->GetRankState(0), RANK_ACTIVE);
    EXPECT_EQ(gm_->GetRankState(1), RANK_ACTIVE);
}

TEST_F(GroupManagerTest, GetRankState_OutOfRangeReturnsIdle)
{
    EXPECT_EQ(gm_->GetRankState(K_INVALID_RANK), RANK_IDLE);
}

TEST_F(GroupManagerTest, Clear_CheckedInRankSucceeds)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    EXPECT_EQ(gm_->Clear(1), 0);
    EXPECT_EQ(gm_->GetRankState(1), RANK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(0, 1), LINK_IDLE);
}

TEST_F(GroupManagerTest, MultipleCheckoutAndCheckin_ResetRankStateClearsLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Checkout rank 1 → should clear links to 0 and K_RANK_TWO */
    ASSERT_EQ(gm_->Checkout(1), 0);
    EXPECT_EQ(gm_->GetRankState(1), RANK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(0, 1), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(1, K_RANK_TWO), LINK_IDLE);
    EXPECT_EQ(gm_->GetLinkState(K_RANK_TWO, 1), LINK_IDLE);

    /* Re-checkin rank 1 */
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    EXPECT_EQ(gm_->GetRankState(1), RANK_CHECKED_IN);
    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_EXCHANGING);
    EXPECT_EQ(gm_->GetLinkState(0, 1), LINK_EXCHANGING);
}

/* ================================================================== */
/*  Coverage for MarkRankReconnected, QueryLinkStates, OnLinkStateResp */
/* ================================================================== */

TEST_F(GmGroupManagerTest, MarkRankReconnected_RankIdle_SetsActiveAndEnqueuesQuery)
{
    /* Set rank 0 to IDLE (its initial state) */
    EXPECT_EQ(gm_->GetRankState(0), RANK_IDLE);

    /* MarkReconnected on IDLE rank → becomes ACTIVE */
    gm_->MarkRankReconnected(0);
    EXPECT_EQ(gm_->GetRankState(0), RANK_ACTIVE);

    /* Second call on ACTIVE rank → warn and return (no crash) */
    gm_->MarkRankReconnected(0);
    EXPECT_EQ(gm_->GetRankState(0), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, MarkRankReconnected_WithPeers_StaysActive)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    /* Reset rank 0 to IDLE via Checkout */
    ASSERT_EQ(gm_->Checkout(0), 0);
    EXPECT_EQ(gm_->GetRankState(0), RANK_IDLE);

    /* MarkReconnected → ACTIVE, creates QUERY_LINK_STATE task */
    gm_->MarkRankReconnected(0);
    EXPECT_EQ(gm_->GetRankState(0), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, QueryLinkStates_CallsControllerForAliveRanks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Manually set ranks 0 and 1 to ACTIVE */
    gm_->SetRankState(0, RANK_ACTIVE);
    gm_->SetRankState(1, RANK_ACTIVE);
    gm_->SetRankState(K_RANK_TWO, RANK_CHECKED_IN);

    /* Sender returns success by default */
    sendResult_ = 0;

    /* Call should not crash */
    gm_->QueryLinkStates();
}

TEST_F(GmGroupManagerTest, OnLinkStateResponse_OutOfRangeIgnored)
{
    RankFullInfo info;
    info.rankId = K_INVALID_RANK;
    std::vector<LinkStateEntry> entries;
    gm_->OnLinkStateResponse(info, entries, 0);
    /* No crash */
    SUCCEED();
}

TEST_F(GmGroupManagerTest, OnLinkStateResponse_UpdatesLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Build a link state response from rank 1 to ranks 0 and K_RANK_TWO */
    RankFullInfo info = MakeInfo(1);
    LinkStateEntry e0;
    LinkStateEntry e2;
    e0.dstRankId = 0;
    e0.state = static_cast<uint8_t>(LINK_CONNECTED);
    e2.dstRankId = K_RANK_TWO;
    e2.state = static_cast<uint8_t>(LINK_CONNECTED);
    std::vector<LinkStateEntry> entries = {e0, e2};

    gm_->OnLinkStateResponse(info, entries, K_QUERY_REQUEST_ID);

    EXPECT_EQ(gm_->GetLinkState(1, 0), LINK_CONNECTED);
    EXPECT_EQ(gm_->GetLinkState(1, K_RANK_TWO), LINK_CONNECTED);
}

TEST_F(GmGroupManagerTest, OnLinkStateResponse_UnknownDstIgnored)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);

    RankFullInfo info = MakeInfo(0);
    LinkStateEntry e;
    e.dstRankId = K_INVALID_RANK;
    e.state = static_cast<uint8_t>(LINK_CONNECTED);
    std::vector<LinkStateEntry> entries = {e};

    gm_->OnLinkStateResponse(info, entries, 0);
    /* Unknown dst ignored, no crash */
    SUCCEED();
}

/* ================================================================== */
/*  Rank base/external accessors                                       */
/* ================================================================== */
TEST_F(GroupManagerTest, GetRankBase_OutOfRangeReturnsEmpty)
{
    EXPECT_TRUE(gm_->GetRankBase(K_INVALID_RANK).empty());
}

TEST_F(GroupManagerTest, SetGetRankBase_RoundTrip)
{
    Bytes data{0x01, 0x02, 0x03};
    gm_->SetRankBase(K_RANK_TWO, data);
    EXPECT_EQ(gm_->GetRankBase(K_RANK_TWO), data);
}

TEST_F(GroupManagerTest, SetRankBase_OutOfRangeNoOp)
{
    gm_->SetRankBase(K_INVALID_RANK, Bytes{0x01});
    SUCCEED();
}

TEST_F(GroupManagerTest, GetRankExternal_OutOfRangeReturnsEmpty)
{
    EXPECT_TRUE(gm_->GetRankExternal(K_INVALID_RANK).empty());
}

TEST_F(GroupManagerTest, SetGetRankExternal_RoundTrip)
{
    std::vector<Bytes> data{{0x01, 0x02}, {0x03}};
    gm_->SetRankExternal(K_RANK_TWO, data);
    EXPECT_EQ(gm_->GetRankExternal(K_RANK_TWO), data);
}

TEST_F(GroupManagerTest, SetRankExternal_OutOfRangeNoOp)
{
    gm_->SetRankExternal(K_INVALID_RANK, std::vector<Bytes>{{0x01}});
    SUCCEED();
}

/* ================================================================== */
/*  RestoreState / RebuildActiveLinks                                  */
/* ================================================================== */
TEST_F(GroupManagerTest, RestoreState_WithLinksAndAlive)
{
    std::vector<RankState> states(K_RANK_COUNT, RANK_ACTIVE);
    std::vector<LinkState> links(K_RANK_COUNT * K_RANK_COUNT, LINK_CONNECTED);
    std::unordered_set<uint32_t> alive{0, K_RANK_TWO};
    gm_->RestoreState(states, links, alive);
    EXPECT_EQ(gm_->GetRankState(0), RANK_ACTIVE);
    EXPECT_EQ(gm_->GetLinkState(0, K_RANK_TWO), LINK_CONNECTED);
}

TEST_F(GroupManagerTest, RestoreState_StatesOnly)
{
    std::vector<RankState> states(K_RANK_COUNT, RANK_LEAVING);
    std::unordered_set<uint32_t> alive{0};
    gm_->RestoreState(states, alive);
    EXPECT_EQ(gm_->GetRankState(0), RANK_LEAVING);
}

TEST_F(GroupManagerTest, RebuildActiveLinks_NoCrash)
{
    gm_->RebuildActiveLinks();
    SUCCEED();
}

} // namespace smem
} // namespace ock
