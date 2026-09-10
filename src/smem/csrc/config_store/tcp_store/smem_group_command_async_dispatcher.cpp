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
#include "smem_logger.h"
#include "smem_group_command_async_dispatcher.h"

namespace ock::smem {

static const char *ControlOpName(ControlOp op)
{
    switch (op) {
        case CONTROL_ADD_TO_WHITELIST:
            return "ADD_TO_WHITELIST";
        case CONTROL_REMOVE_FROM_WHITELIST:
            return "REMOVE_FROM_WHITELIST";
        case CONTROL_ESTABLISH_CONNECTION:
            return "ESTABLISH_CONNECTION";
        case CONTROL_CLOSE_CONNECTION:
            return "CLOSE_CONNECTION";
        case CONTROL_JOIN:
            return "JOIN";
        case CONTROL_LEAVE:
            return "LEAVE";
        case CONTROL_QUERY_LINK_STATE:
            return "QUERY_LINK_STATE";
        case CONTROL_LINK_STATE_RESPONSE:
            return "LINK_STATE_RESPONSE";
        case CONTROL_ADD_TO_WHITELIST_ACK:
            return "ADD_TO_WHITELIST_ACK";
        case CONTROL_REMOVE_FROM_WHITELIST_ACK:
            return "REMOVE_FROM_WHITELIST_ACK";
        case CONTROL_ESTABLISH_CONNECTION_ACK:
            return "ESTABLISH_CONNECTION_ACK";
        case CONTROL_CLOSE_CONNECTION_ACK:
            return "CLOSE_CONNECTION_ACK";
        case CONTROL_LEAVE_NOTIFY:
            return "LEAVE_NOTIFY";
        default:
            return "UNKNOWN";
    }
}

SmemGroupCommandAsyncDispatcher::SmemGroupCommandAsyncDispatcher(SmemGroupManager *groupMgr) noexcept
    : groupManager_{groupMgr}
{}

SmemGroupCommandAsyncDispatcher::~SmemGroupCommandAsyncDispatcher() noexcept
{
    Stop();
}

bool SmemGroupCommandAsyncDispatcher::Start() noexcept
{
    if (started_) {
        return true;
    }

    if (groupManager_ == nullptr) {
        SM_LOG_ERROR("groupManager is nullptr, cannot start!");
        return false;
    }

    started_ = true;
    backgroundThread_ = std::thread(&SmemGroupCommandAsyncDispatcher::BackgroundThread, this);
    SM_LOG_INFO("SmemGroupCommandAsyncDispatcher Started, localRankId=" << localRankId_);
    return true;
}

void SmemGroupCommandAsyncDispatcher::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> locker{mutex_};
        started_ = false;
    }
    cond_.notify_all();
    if (backgroundThread_.joinable()) {
        SM_LOG_INFO("SmemGroupCommandAsyncDispatcher joining background thread...");
        backgroundThread_.join();
    }
    SM_LOG_INFO("SmemGroupCommandAsyncDispatcher Stopped.");
}

// ── Enqueue methods ────────────────────────────────────────────────

void SmemGroupCommandAsyncDispatcher::EnqueueAddToWhitelist(uint32_t rankId, std::vector<RankFullInfo> &&others,
                                                            uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    addWhitelistQueue_.push_back({rankId, std::move(others), reqId});
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueRemoveFromWhitelist(uint32_t rankId, std::vector<RankFullInfo> &&others,
                                                                 uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    removeWhitelistQueue_.push_back({rankId, std::move(others), reqId});
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueEstablishConnection(uint32_t rankId, std::vector<RankFullInfo> &&peers,
                                                                 uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    connectionQueue_.push_back({rankId, std::move(peers), reqId});
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueCloseConnection(uint32_t rankId, std::vector<uint32_t> &&peers,
                                                             uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    disconnectionQueue_.push_back({rankId, std::move(peers), reqId});
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueLeaveNotify(uint32_t leavingRankId) noexcept
{
    std::lock_guard locker{mutex_};
    leaveNotifyQueue_.push_back(leavingRankId);
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueQueryLinkState(uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    if (!hasQueryLinkState_) {
        queryLinkStateReqId_ = reqId;
        hasQueryLinkState_ = true;
    }
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueAddSlices(uint32_t extendingRankId, MultiBytes &&newSlices,
                                                       uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    addSlicesQueue_.push_back({extendingRankId, std::move(newSlices), reqId});
    cond_.notify_one();
}

// ── Ack sending ────────────────────────────────────────────────────

void SmemGroupCommandAsyncDispatcher::SendAck(ControlOp ackOp, const std::vector<uint32_t> &targetRankIds,
                                              const std::vector<int32_t> &results, uint64_t reqId) noexcept
{
    auto tag = AckTagName(ackOp);
    if (sendAckBatch_) {
        sendAckBatch_(ackOp, localRankId_, targetRankIds, results, reqId);
    } else {
        SM_LOG_WARN("[GM][Client][Ack] skip rank=" << localRankId_ << " type=" << tag << " no sendAckBatch_");
    }
}

// ── Background thread ──────────────────────────────────────────────

void SmemGroupCommandAsyncDispatcher::BackgroundThread() noexcept
{
    pthread_setname_np(pthread_self(), "NetAsyncGrpMgr");
    SM_LOG_INFO("SmemGroupCommandAsyncDispatcher::BackgroundThread enter.");

    while (started_) {
        DrainedJobs jobs;
        DrainQueues(jobs);
        if (!started_) {
            break;
        }
        ProcessDrainedJobs(jobs);
    }

    SM_LOG_INFO("SmemGroupCommandAsyncDispatcher::BackgroundThread exit.");
}

void SmemGroupCommandAsyncDispatcher::DrainQueues(DrainedJobs &jobs) noexcept
{
    std::unique_lock locker{mutex_};
    cond_.wait(locker, [this] {
        return !started_.load() || !addWhitelistQueue_.empty() || !removeWhitelistQueue_.empty() ||
               !connectionQueue_.empty() || !disconnectionQueue_.empty() || !leaveNotifyQueue_.empty() ||
               hasQueryLinkState_ || !addSlicesQueue_.empty();
    });

    jobs.addJobs.swap(addWhitelistQueue_);
    jobs.rmvJobs.swap(removeWhitelistQueue_);
    jobs.connJobs.swap(connectionQueue_);
    jobs.disconnJobs.swap(disconnectionQueue_);
    jobs.leaveJobs.swap(leaveNotifyQueue_);
    jobs.slicesJobs.swap(addSlicesQueue_);
    if (hasQueryLinkState_) {
        jobs.doQuery = true;
        jobs.queryReqId = queryLinkStateReqId_;
        hasQueryLinkState_ = false;
    }
}

void SmemGroupCommandAsyncDispatcher::ProcessDrainedJobs(const DrainedJobs &jobs) noexcept
{
    for (auto &req : jobs.addJobs) {
        BackgroundAddWhiteList(req, req.reqId);
    }

    for (auto &req : jobs.rmvJobs) {
        BackgroundRmvWhiteList(req, req.reqId);
    }

    for (auto &req : jobs.connJobs) {
        BackgroundEstablishConnection(req, req.reqId);
    }

    for (auto &req : jobs.disconnJobs) {
        BackgroundCloseConnection(req, req.reqId);
    }

    for (auto leavingRankId : jobs.leaveJobs) {
        BackgroundLeaveNotify(leavingRankId);
    }

    if (jobs.doQuery) {
        BackgroundQueryLinkState(jobs.queryReqId);
    }

    for (auto &req : jobs.slicesJobs) {
        BackgroundAddSlices(req, req.reqId);
    }
}

// ── Background handlers ────────────────────────────────────────────

static int32_t DefaultResult(int ret)
{
    return (ret == 0) ? 0 : -1;
}

static int CallbackOrError(const char *name, const std::function<int()> &cb)
{
    if (!cb) {
        SM_LOG_ERROR(name << ": callback not registered, reporting failure");
        return -1;
    }
    return cb();
}

void SmemGroupCommandAsyncDispatcher::BackgroundAddWhiteList(const WhitelistRequest &req, uint64_t reqId) noexcept
{
    if (req.others.empty()) {
        SM_LOG_WARN("[GM][Client][Recv] AddWhiteList: empty others, reqId=" << reqId);
        return;
    }

    int overallRet = CallbackOrError("onAddToWhitelist_", [this, &req, reqId]() {
        if (!onAddToWhitelist_) {
            return -1;
        }
        return onAddToWhitelist_(req.rankId, req.others, reqId);
    });
    {
        const std::string peers = JoinRankIds(req.others);
        if (overallRet != 0) {
            SM_LOG_ERROR("[GM][Client][Recv] WhiteList FAIL r=" << req.rankId << " ret=" << overallRet);
        } else {
            SM_LOG_INFO("[GM][Client][Recv] WhiteList r=" << req.rankId << " peers=[" << peers << "]");
        }
    }

    std::vector<uint32_t> targets;
    std::vector<int32_t> results;
    targets.reserve(req.others.size());
    results.reserve(req.others.size());
    for (const auto &other : req.others) {
        targets.push_back(other.rankId);
        results.push_back(DefaultResult(overallRet));
    }
    SendAck(CONTROL_ADD_TO_WHITELIST_ACK, targets, results, reqId);
}

void SmemGroupCommandAsyncDispatcher::BackgroundRmvWhiteList(const WhitelistRequest &req, uint64_t reqId) noexcept
{
    if (req.others.empty()) {
        SM_LOG_WARN("[GM][Client][Recv] RmvWhiteList: empty others, reqId=" << reqId);
        return;
    }

    int overallRet = CallbackOrError("onRemoveFromWhitelist_", [this, &req, reqId]() {
        if (!onRemoveFromWhitelist_) {
            return -1;
        }
        return onRemoveFromWhitelist_(req.rankId, req.others, reqId);
    });
    if (overallRet != 0) {
        SM_LOG_ERROR("[GM][Client][Recv] RmvWhiteList FAIL r=" << req.rankId << " ret=" << overallRet);
    } else {
        SM_LOG_INFO("[GM][Client][Recv] RmvWhiteList r=" << req.rankId << " peers=[" << JoinRankIds(req.others) << "]");
    }

    std::vector<uint32_t> targets;
    std::vector<int32_t> results;
    targets.reserve(req.others.size());
    results.reserve(req.others.size());
    for (const auto &other : req.others) {
        targets.push_back(other.rankId);
        results.push_back(DefaultResult(overallRet));
    }
    SendAck(CONTROL_REMOVE_FROM_WHITELIST_ACK, targets, results, reqId);
}

void SmemGroupCommandAsyncDispatcher::BackgroundEstablishConnection(const ConnEstablishRequest &req,
                                                                    uint64_t reqId) noexcept
{
    int overallRet = CallbackOrError("onEstablishConnection_", [this, &req, reqId]() {
        if (!onEstablishConnection_) {
            return -1;
        }
        return onEstablishConnection_(req.rankId, req.peers, reqId);
    });
    {
        const std::string peers = JoinRankIds(req.peers);
        if (overallRet != 0) {
            SM_LOG_ERROR("[GM][Client][Recv] EstConn FAIL r=" << req.rankId << " ret=" << overallRet);
        } else {
            SM_LOG_INFO("[GM][Client][Recv] EstConn r=" << req.rankId << " peers=[" << peers << "]");
        }
    }

    std::vector<uint32_t> targetIds;
    std::vector<int32_t> results;
    targetIds.reserve(req.peers.size());
    results.reserve(req.peers.size());
    for (const auto &p : req.peers) {
        targetIds.push_back(p.rankId);
        results.push_back(DefaultResult(overallRet));
    }
    SendAck(CONTROL_ESTABLISH_CONNECTION_ACK, targetIds, results, reqId);
}

void SmemGroupCommandAsyncDispatcher::BackgroundCloseConnection(const ConnCloseRequest &req, uint64_t reqId) noexcept
{
    int overallRet = CallbackOrError("onCloseConnection_", [this, &req, reqId]() {
        if (!onCloseConnection_) {
            return -1;
        }
        return onCloseConnection_(req.rankId, req.peers, reqId);
    });
    if (overallRet != 0) {
        SM_LOG_ERROR("[GM][Client][Recv] CloseConn FAIL r=" << req.rankId << " rid=" << reqId << " ret=" << overallRet);
    } else {
        SM_LOG_INFO("[GM][Client][Recv] CloseConn r=" << req.rankId << " peers=[" << JoinRankIds(req.peers) << "]");
    }

    std::vector<int32_t> results;
    results.reserve(req.peers.size());
    for (size_t i = 0; i < req.peers.size(); ++i) {
        results.push_back(DefaultResult(overallRet));
    }
    SendAck(CONTROL_CLOSE_CONNECTION_ACK, req.peers, results, reqId);
}

void SmemGroupCommandAsyncDispatcher::BackgroundLeaveNotify(uint32_t leavingRankId) noexcept
{
    if (onLeaveNotify_) {
        int ret = onLeaveNotify_(leavingRankId);
        if (ret != 0) {
            SM_LOG_ERROR("[GM][Client][Recv] LeaveNotify FAIL lr=" << leavingRankId << " ret=" << ret);
        } else {
            SM_LOG_INFO("[GM][Client][Recv] LeaveNotify lr=" << leavingRankId);
        }
    } else {
        SM_LOG_WARN("[GM][Client][Recv] LeaveNotify lr=" << leavingRankId << " no callback registered");
    }
}

void SmemGroupCommandAsyncDispatcher::BackgroundQueryLinkState(uint64_t reqId) noexcept
{
    std::vector<LinkStateEntry> entries;
    if (onQueryLinkState_) {
        entries = onQueryLinkState_();
    }
    SM_LOG_INFO("[GM][Client][Send] QueryLinkState reqId=" << reqId << " entries=" << entries.size());
}

void SmemGroupCommandAsyncDispatcher::BackgroundAddSlices(const AddSlicesRequest &req, uint64_t reqId) noexcept
{
    if (onAddSlices_) {
        int ret = onAddSlices_(req.extendingRankId, req.newSlices, reqId);
        if (ret != 0) {
            SM_LOG_ERROR("[GM][Client][Recv] AddSlices FAIL ext=" << req.extendingRankId << " ret=" << ret);
        }
    }
    SM_LOG_INFO("[GM][Client][Recv] AddSlices ext=" << req.extendingRankId << " slices=" << req.newSlices.size());
}

} // namespace ock::smem
