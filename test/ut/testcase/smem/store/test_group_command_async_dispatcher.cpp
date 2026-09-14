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

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#define private public // NOLINT(google-runtime-int) test hack: expose private members
#include "smem_group_command_async_dispatcher.h"
#undef private

namespace ock {
namespace smem {

namespace {

constexpr uint32_t K_RANK_TWO = 2;
constexpr uint32_t K_RANK_THREE = 3;
constexpr uint32_t K_RANK_FOUR = 4;
constexpr uint32_t K_RANK_FIVE = 5;
constexpr uint32_t K_EXPECTED_ACK_COUNT = 3;

constexpr uint64_t K_REQ_ID_ADD_WHITELIST = 100;
constexpr uint64_t K_REQ_ID_REMOVE_WHITELIST = 101;
constexpr uint64_t K_REQ_ID_ESTABLISH_CONNECTION = 102;
constexpr uint64_t K_REQ_ID_QUERY_LINK_STATE = 104;
constexpr uint64_t K_REQ_ID_ADD_SLICES = 105;
constexpr uint64_t K_REQ_ID_ADD_WHITELIST_FIRST = 200;
constexpr uint64_t K_REQ_ID_ADD_WHITELIST_SECOND = 201;
constexpr uint64_t K_REQ_ID_QUERY_FIRST = 300;
constexpr uint64_t K_REQ_ID_QUERY_SECOND = 301;

constexpr uint32_t K_DRAIN_TIMEOUT_SEC = 5;
constexpr uint32_t K_POLL_INTERVAL_MS = 10;

constexpr uint8_t K_SLICE_BYTE_ONE = 0x01;
constexpr uint8_t K_SLICE_BYTE_TWO = 0x02;

class FakeGroupManager : public SmemGroupManager {
public:
    int Join(const RankFullInfo &info) noexcept override
    {
        (void)info;
        return 0;
    }

    int Leave() noexcept override
    {
        return 0;
    }

    int ExtendMemory(const MultiBytes &additionalSlices) noexcept override
    {
        (void)additionalSlices;
        return 0;
    }
};

RankFullInfo MakeRankInfo(uint32_t rankId)
{
    RankFullInfo info;
    info.rankId = rankId;
    std::string base = "host" + std::to_string(rankId);
    info.baseInfo.assign(base.begin(), base.end());
    return info;
}

} // namespace

class AsyncDispatcherTest : public ::testing::Test {
protected:
    struct CallbackCounters {
        std::atomic<int> add{0};
        std::atomic<int> remove{0};
        std::atomic<int> connection{0};
        std::atomic<int> query{0};
        std::atomic<int> slices{0};
        std::atomic<int> ack{0};
    };

    void SetUp() override
    {
        groupMgr_ = SmMakeRef<FakeGroupManager>();
        dispatcher_ = std::make_unique<SmemGroupCommandAsyncDispatcher>(groupMgr_.Get());
    }

    void TearDown() override
    {
        dispatcher_->Stop();
        dispatcher_.reset();
        groupMgr_ = nullptr;
    }

    void RegisterCountingCallbacks(CallbackCounters &counters)
    {
        dispatcher_->SetWhitelistCallback([&counters](uint32_t, const std::vector<RankFullInfo> &, uint64_t) {
            counters.add++;
            return 0;
        });
        dispatcher_->SetRemoveWhitelistCallback([&counters](uint32_t, const std::vector<RankFullInfo> &, uint64_t) {
            counters.remove++;
            return 0;
        });
        dispatcher_->SetConnectionCallback([&counters](uint32_t, const std::vector<RankFullInfo> &, uint64_t) {
            counters.connection++;
            return 0;
        });
        dispatcher_->SetLinkStateQueryCallback([&counters]() {
            counters.query++;
            return std::vector<LinkStateEntry>{};
        });
        dispatcher_->SetAddSlicesCallback([&counters](uint32_t, const MultiBytes &, uint64_t) {
            counters.slices++;
            return 0;
        });
        dispatcher_->SetSendAckBatchFunc([&counters](ControlOp, uint32_t, const std::vector<uint32_t> &,
                                                     const std::vector<int32_t> &, uint64_t) { counters.ack++; });
    }

    void EnqueueAllOps()
    {
        std::vector<RankFullInfo> others{MakeRankInfo(K_RANK_TWO), MakeRankInfo(K_RANK_THREE)};
        dispatcher_->EnqueueAddToWhitelist(1, std::move(others), K_REQ_ID_ADD_WHITELIST);
        std::vector<RankFullInfo> removeOthers{MakeRankInfo(K_RANK_TWO)};
        dispatcher_->EnqueueRemoveFromWhitelist(1, std::move(removeOthers), K_REQ_ID_REMOVE_WHITELIST);
        std::vector<RankFullInfo> peers{MakeRankInfo(K_RANK_TWO)};
        dispatcher_->EnqueueEstablishConnection(1, std::move(peers), K_REQ_ID_ESTABLISH_CONNECTION);
        dispatcher_->EnqueueQueryLinkState(K_REQ_ID_QUERY_LINK_STATE);
        MultiBytes slices;
        slices.push_back(Bytes{K_SLICE_BYTE_ONE, K_SLICE_BYTE_TWO});
        dispatcher_->EnqueueAddSlices(K_RANK_THREE, std::move(slices), K_REQ_ID_ADD_SLICES);
    }

    SmRef<FakeGroupManager> groupMgr_;
    std::unique_ptr<SmemGroupCommandAsyncDispatcher> dispatcher_;
};

/* ================================================================== */
/*  Start / Stop                                                      */
/* ================================================================== */

TEST_F(AsyncDispatcherTest, StartWithNullGroupManagerFails)
{
    auto dispatcher = std::make_unique<SmemGroupCommandAsyncDispatcher>(nullptr);
    EXPECT_FALSE(dispatcher->Start());
}

TEST_F(AsyncDispatcherTest, StartTwiceReturnsTrue)
{
    EXPECT_TRUE(dispatcher_->Start());
    EXPECT_TRUE(dispatcher_->Start());
    dispatcher_->Stop();
}

TEST_F(AsyncDispatcherTest, StopWithoutStartIsSafe)
{
    dispatcher_->Stop();
    dispatcher_->Stop();
}

TEST_F(AsyncDispatcherTest, EnqueueBeforeStartThenStartProcessesQueue)
{
    CallbackCounters counters;

    dispatcher_->SetLocalRankId(1);
    RegisterCountingCallbacks(counters);
    EnqueueAllOps();

    EXPECT_TRUE(dispatcher_->Start());

    // Wait for the background thread to drain all queues.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(K_DRAIN_TIMEOUT_SEC);
    while (counters.add.load() == 0 || counters.remove.load() == 0 || counters.connection.load() == 0 ||
           counters.query.load() == 0 || counters.slices.load() == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(K_POLL_INTERVAL_MS));
    }

    dispatcher_->Stop();

    EXPECT_EQ(counters.add.load(), 1);
    EXPECT_EQ(counters.remove.load(), 1);
    EXPECT_EQ(counters.connection.load(), 1);
    EXPECT_EQ(counters.query.load(), 1);
    EXPECT_EQ(counters.slices.load(), 1);
    // add/remove/connection each send an ack; query does not.
    EXPECT_GE(counters.ack.load(), K_EXPECTED_ACK_COUNT);
}

/* ================================================================== */
/*  Background handlers (direct invocation)                           */
/* ================================================================== */

TEST_F(AsyncDispatcherTest, BackgroundAddWhitelistEmptyOthersSkips)
{
    std::atomic<int> ackCalls{0};
    std::atomic<int> callbackCalls{0};
    dispatcher_->SetWhitelistCallback([&](uint32_t, const std::vector<RankFullInfo> &, uint64_t) {
        callbackCalls++;
        return 0;
    });
    dispatcher_->SetSendAckBatchFunc([&](ControlOp, uint32_t, const std::vector<uint32_t> &,
                                         const std::vector<int32_t> &, uint64_t) { ackCalls++; });

    SmemGroupCommandAsyncDispatcher::WhitelistRequest req{1U, {}};
    dispatcher_->BackgroundAddWhiteList(req, 1);
    EXPECT_EQ(callbackCalls.load(), 0);
    EXPECT_EQ(ackCalls.load(), 0);
}

TEST_F(AsyncDispatcherTest, BackgroundAddWhitelistWithoutCallbackReportsFailure)
{
    std::atomic<int> ackCalls{0};
    std::vector<int32_t> lastResults;
    dispatcher_->SetSendAckBatchFunc(
        [&](ControlOp, uint32_t, const std::vector<uint32_t> &, const std::vector<int32_t> &results, uint64_t) {
            ackCalls++;
            lastResults = results;
        });

    std::vector<RankFullInfo> others{MakeRankInfo(K_RANK_TWO)};
    SmemGroupCommandAsyncDispatcher::WhitelistRequest req{1U, others};
    dispatcher_->BackgroundAddWhiteList(req, 1);
    EXPECT_EQ(ackCalls.load(), 1);
    ASSERT_EQ(lastResults.size(), 1U);
    EXPECT_EQ(lastResults[0], -1);
}

TEST_F(AsyncDispatcherTest, BackgroundRemoveWhitelistEmptyOthersSkips)
{
    std::atomic<int> ackCalls{0};
    dispatcher_->SetSendAckBatchFunc([&](ControlOp, uint32_t, const std::vector<uint32_t> &,
                                         const std::vector<int32_t> &, uint64_t) { ackCalls++; });

    SmemGroupCommandAsyncDispatcher::WhitelistRequest req{1U, {}};
    dispatcher_->BackgroundRmvWhiteList(req, 1);
    EXPECT_EQ(ackCalls.load(), 0);
}

TEST_F(AsyncDispatcherTest, BackgroundEstablishConnectionAcksPeers)
{
    std::atomic<int> ackCalls{0};
    std::vector<uint32_t> lastTargets;
    dispatcher_->SetConnectionCallback([&](uint32_t, const std::vector<RankFullInfo> &, uint64_t) { return 0; });
    dispatcher_->SetSendAckBatchFunc(
        [&](ControlOp, uint32_t, const std::vector<uint32_t> &targets, const std::vector<int32_t> &, uint64_t) {
            ackCalls++;
            lastTargets = targets;
        });

    std::vector<RankFullInfo> peers{MakeRankInfo(K_RANK_TWO), MakeRankInfo(K_RANK_THREE)};
    SmemGroupCommandAsyncDispatcher::ConnEstablishRequest req{1U, peers};
    dispatcher_->BackgroundEstablishConnection(req, K_RANK_TWO);
    EXPECT_EQ(ackCalls.load(), 1);
    ASSERT_EQ(lastTargets.size(), K_RANK_TWO);
    EXPECT_EQ(lastTargets[0], K_RANK_TWO);
    EXPECT_EQ(lastTargets[1], K_RANK_THREE);
}

TEST_F(AsyncDispatcherTest, BackgroundQueryLinkStateInvokesCallback)
{
    std::atomic<int> queryCalls{0};
    dispatcher_->SetLinkStateQueryCallback([&]() {
        queryCalls++;
        return std::vector<LinkStateEntry>{};
    });
    dispatcher_->BackgroundQueryLinkState(1);
    EXPECT_EQ(queryCalls.load(), 1);
}

TEST_F(AsyncDispatcherTest, BackgroundAddSlicesInvokesCallback)
{
    std::atomic<int> slicesCalls{0};
    dispatcher_->SetAddSlicesCallback([&](uint32_t rankId, const MultiBytes &, uint64_t) {
        EXPECT_EQ(rankId, K_RANK_FOUR);
        slicesCalls++;
        return 0;
    });
    MultiBytes slices;
    slices.push_back(Bytes{K_SLICE_BYTE_ONE});
    SmemGroupCommandAsyncDispatcher::AddSlicesRequest req{K_RANK_FOUR, slices};
    dispatcher_->BackgroundAddSlices(req, 1);
    EXPECT_EQ(slicesCalls.load(), 1);
}

/* ================================================================== */
/*  Enqueue request-id bookkeeping                                    */
/* ================================================================== */

TEST_F(AsyncDispatcherTest, EnqueueKeepsReqIdPerRequest)
{
    std::vector<RankFullInfo> others{MakeRankInfo(K_RANK_TWO)};
    dispatcher_->EnqueueAddToWhitelist(1, std::move(others), K_REQ_ID_ADD_WHITELIST_FIRST);
    std::vector<RankFullInfo> secondOthers{MakeRankInfo(K_RANK_TWO)};
    dispatcher_->EnqueueAddToWhitelist(1, std::move(secondOthers), K_REQ_ID_ADD_WHITELIST_SECOND);

    // Each queued request carries its own reqId.
    EXPECT_EQ(dispatcher_->addWhitelistQueue_[0].reqId, K_REQ_ID_ADD_WHITELIST_FIRST);
    EXPECT_EQ(dispatcher_->addWhitelistQueue_[1].reqId, K_REQ_ID_ADD_WHITELIST_SECOND);
    EXPECT_EQ(dispatcher_->addWhitelistQueue_.size(), K_RANK_TWO);

    dispatcher_->EnqueueQueryLinkState(K_REQ_ID_QUERY_FIRST);
    dispatcher_->EnqueueQueryLinkState(K_REQ_ID_QUERY_SECOND);
    EXPECT_TRUE(dispatcher_->hasQueryLinkState_);
    EXPECT_EQ(dispatcher_->queryLinkStateReqId_, K_REQ_ID_QUERY_FIRST);
}

} // namespace smem
} // namespace ock
