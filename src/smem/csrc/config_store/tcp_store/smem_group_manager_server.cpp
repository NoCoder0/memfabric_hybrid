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

#include "smem_group_manager_server.h"

#include <pthread.h>
#include <unordered_map>

#include "hybm_ptracer.h"
#include "smem_config_store_logger.h"
#include "smem_message_packer.h"

namespace ock::smem {

// Each alive rank needs two ADD tasks (src->newRank and newRank->src) to build bidirectional links.
constexpr size_t K_ADD_DIRECTIONS = 2;

SmemGroupManagerServer::SmemGroupManagerServer(SmemGroupCommandSender sender, const uint32_t maxRanks) noexcept
    : sender_(std::move(sender)), maxRanks_(maxRanks > SMEM_RANK_MAX ? SMEM_RANK_MAX : maxRanks)
{
    /*
     * Vectors are sized once on construction and never resized.
     * Using vector instead of raw array or std::array so that
     * when maxRanks is small (e.g. 4 on a single-node test),
     * we don't pay for 1024 entries.
     *
     * links_ is a flat adjacency matrix: index = src * maxRanks_ + dst.
     * For 1024 ranks this is ~1 MiB (1024 * 1024 * 1 byte).
     * transitions_ parallels links_, each cell tracks the timestamp
     * and retry count for the corresponding link state.
     */
    states_.assign(maxRanks_, RANK_IDLE);
    links_.assign(maxRanks_ * maxRanks_, LINK_IDLE);
    transitions_.assign(maxRanks_ * maxRanks_, LinkTransition{});
    rankBase_.resize(maxRanks_);
    rankExternal_.resize(maxRanks_);
    isActiveLink_.assign(maxRanks_ * maxRanks_, false);
}

/*
 * Stop driver and send worker threads.
 */
SmemGroupManagerServer::~SmemGroupManagerServer() noexcept
{
    Stop();
}

/*
 * Set the state of a single rank. No-op if rankId is out of range.
 */
void SmemGroupManagerServer::SetRankState(uint32_t rankId, RankState state) noexcept
{
    if (rankId >= maxRanks_) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    states_[rankId] = state;
}

/*
 * Get the current state of a single rank. Returns RANK_IDLE for out-of-range rankId.
 */
RankState SmemGroupManagerServer::GetRankState(uint32_t rankId) const noexcept
{
    if (rankId >= maxRanks_) {
        return RANK_IDLE;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return states_[rankId];
}

/*
 * Returns ranks that are ACTIVE or CHECKED_IN.
 * LEAVING ranks are excluded because they are in teardown.
 * IDLE ranks have never connected.
 */
std::vector<uint32_t> SmemGroupManagerServer::GetAliveRanks() const noexcept
{
    std::vector<uint32_t> alive;
    std::unique_lock<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < maxRanks_; ++i) {
        if (states_[i] == RANK_ACTIVE || states_[i] == RANK_CHECKED_IN) {
            alive.push_back(i);
        }
    }
    return alive;
}

/*
 * Register a rank into the group. Transitions IDLE → CHECKED_IN.
 * Stores rank metadata in-memory and enqueues AddToWhitelist tasks
 * for all existing active ranks.
 * Returns 0 on success, -1 on invalid rankId or duplicate CheckIn.
 */
int SmemGroupManagerServer::CheckIn(const RankFullInfo &info, uint64_t reqId) noexcept
{
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << info.rankId << " type=JOINREQ reqId=" << reqId);
    TP_TRACE_BEGIN(TP_SMEM_GROUP_SERVER_CHECKIN);
    if (info.rankId >= maxRanks_) {
        TP_TRACE_END(TP_SMEM_GROUP_SERVER_CHECKIN, 1);
        return -1;
    }

    std::vector<RankBaseInfo> aliveBases;
    auto now = std::chrono::steady_clock::now();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (states_[info.rankId] != RANK_IDLE) {
            STORE_LOG_INFO("[GM][Server][Recv] JOINREQ state=" << static_cast<int>(states_[info.rankId]));
            TP_TRACE_END(TP_SMEM_GROUP_SERVER_CHECKIN, 1);
            return -1;
        }

        rankBase_[info.rankId] = info.baseInfo;
        rankExternal_[info.rankId] = info.externalInfo;

        states_[info.rankId] = RANK_CHECKED_IN;
        aliveRanks_.insert(info.rankId);

        for (uint32_t i = 0; i < maxRanks_; ++i) {
            if ((states_[i] == RANK_ACTIVE || states_[i] == RANK_CHECKED_IN) && i != info.rankId) {
                uint32_t fwdIdx = LinkIndex(info.rankId, i);
                uint32_t revIdx = LinkIndex(i, info.rankId);
                links_[fwdIdx] = LINK_EXCHANGING;
                links_[revIdx] = LINK_EXCHANGING;
                transitions_[fwdIdx] = {now, 0};
                transitions_[revIdx] = {now, 0};
                AddActiveLink(fwdIdx);
                AddActiveLink(revIdx);

                RankBaseInfo base;
                base.rankId = i;
                base.baseInfo = rankBase_[i];
                aliveBases.push_back(base);
            }
        }
    }

    if (sender_ && !aliveBases.empty()) {
        std::vector<SendTask> addTasks;
        addTasks.reserve(aliveBases.size() * K_ADD_DIRECTIONS);
        for (auto &[rankId, baseInfo] : aliveBases) {
            addTasks.push_back({SendOp::ADD, rankId, info.rankId, LinkIndex(rankId, info.rankId)});
            addTasks.push_back({SendOp::ADD, info.rankId, rankId, LinkIndex(info.rankId, rankId)});
        }
        EnqueueBatchSend(std::move(addTasks));
    }

    TP_TRACE_END(TP_SMEM_GROUP_SERVER_CHECKIN, 0);
    return 0;
}

int SmemGroupManagerServer::ProcessExtendMemory(uint32_t rankId, const MultiBytes &additionalSlices,
                                                uint64_t reqId) noexcept
{
    (void)reqId;
    std::unique_lock<std::mutex> lock(mutex_);
    if (rankId >= maxRanks_) {
        STORE_LOG_ERROR("ProcessExtendMemory: invalid rankId=" << rankId);
        return -1;
    }
    if (states_[rankId] != RANK_ACTIVE) {
        STORE_LOG_ERROR("ProcessExtendMemory: rankId=" << rankId << " not ACTIVE, state=" << states_[rankId]);
        return -1;
    }
    rankExternal_[rankId].insert(rankExternal_[rankId].end(), additionalSlices.begin(), additionalSlices.end());
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << rankId << " type=EXTMEM slices=" << additionalSlices.size());

    // Broadcast ADD_SLICES to all active peers via sender callback
    if (sender_) {
        SmemMessage addMsg = SmemMessage::PackAddSlices(rankId, additionalSlices);
        auto packed = SmemMessagePacker::Pack(addMsg);
        for (uint32_t peerId : aliveRanks_) {
            if (peerId != rankId) {
                sender_(peerId, packed);
            }
        }
        std::vector<uint32_t> aliveVec(aliveRanks_.begin(), aliveRanks_.end());
        STORE_LOG_INFO("[GM][Server][Send] type=ADD_SLICES extendingRank=" << rankId << " peers=" << aliveVec.size());
    }

    return 0;
}

void SmemGroupManagerServer::MarkRankReconnected(uint32_t rankId) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (states_[rankId] != RANK_IDLE) {
        lock.unlock();
        STORE_LOG_WARN("mark rank:" << rankId << " reconnected, but state not idle: " << states_[rankId]);
        return;
    }

    states_[rankId] = RANK_ACTIVE;
    aliveRanks_.insert(rankId);

    if (sender_) {
        lock.unlock();
        STORE_LOG_INFO("[GM][Server][Send] rank=" << rankId << " type=QUERYREQ");
        EnqueueSend({SendOp::QUERY_LINK_STATE, rankId, 0, 0, 0, 0});
    }
}

/*
 * Deregister a rank from the group.
 * Immediately transitions all links to IDLE, broadcasts LeaveNotify to peers,
 * and clears in-memory rank metadata.
 */
void SmemGroupManagerServer::ResetRankState(uint32_t rankId, std::vector<uint32_t> &connectedPeers) noexcept
{
    states_[rankId] = RANK_IDLE;
    rankBase_[rankId].clear();
    rankExternal_[rankId].clear();

    for (uint32_t i = 0; i < maxRanks_; ++i) {
        if (i == rankId) {
            continue;
        }
        if (states_[i] != RANK_ACTIVE && states_[i] != RANK_CHECKED_IN) {
            continue;
        }

        uint32_t fwdIdx = LinkIndex(rankId, i);
        uint32_t revIdx = LinkIndex(i, rankId);
        if (links_[fwdIdx] != LINK_IDLE || links_[revIdx] != LINK_IDLE) {
            connectedPeers.push_back(i);
        }
        links_[fwdIdx] = LINK_IDLE;
        transitions_[fwdIdx] = {};
        links_[revIdx] = LINK_IDLE;
        transitions_[revIdx] = {};
    }
}

int SmemGroupManagerServer::Checkout(uint32_t rankId, uint64_t reqId) noexcept
{
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << rankId << " type=LEAVREQ reqId=" << reqId);
    if (rankId >= maxRanks_) {
        return -1;
    }

    std::vector<uint32_t> connectedPeers;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (states_[rankId] != RANK_CHECKED_IN && states_[rankId] != RANK_ACTIVE) {
            return -1;
        }
        ResetRankState(rankId, connectedPeers);
        aliveRanks_.erase(rankId);

        if (sender_ && !connectedPeers.empty()) {
            for (uint32_t peerId : connectedPeers) {
                EnqueueSend({SendOp::REMOVE, peerId, rankId, LinkIndex(peerId, rankId)});
                EnqueueSend({SendOp::CLOSE, peerId, rankId, LinkIndex(peerId, rankId)});
            }
            // LEAVE_NOTIFY is the lightweight notification; REMOVE+CLOSE trigger actual cleanup
            for (uint32_t peerId : connectedPeers) {
                EnqueueSend({SendOp::LEAVE_NOTIFY, peerId, rankId, 0});
            }
        }
    }

    STORE_LOG_INFO("rank=" << rankId << " LEAVE done, notify peers=" << connectedPeers.size());
    return 0;
}

/*
 * Forcefully clear a CHECKED_IN rank's state.
 * Resets all links, clears in-memory rank metadata, and enqueues
 * RemoveFromWhitelist for affected peers. Returns 0 on success.
 */
int SmemGroupManagerServer::Clear(uint32_t rankId) noexcept
{
    if (rankId >= maxRanks_) {
        return -1;
    }

    std::vector<uint32_t> affectedPeers;
    {
        RankBaseInfo departingBase;
        std::unique_lock<std::mutex> lock(mutex_);
        if (states_[rankId] != RANK_CHECKED_IN) {
            return -1;
        }
        STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << rankId << " type=CLEAR");

        departingBase.rankId = rankId;
        departingBase.baseInfo = rankBase_[rankId];

        for (uint32_t i = 0; i < maxRanks_; ++i) {
            if (i == rankId) {
                continue;
            }
            if (states_[i] != RANK_ACTIVE && states_[i] != RANK_CHECKED_IN) {
                continue;
            }

            uint32_t fwdIdx = LinkIndex(rankId, i);
            uint32_t revIdx = LinkIndex(i, rankId);
            if (links_[fwdIdx] != LINK_IDLE || links_[revIdx] != LINK_IDLE) {
                affectedPeers.push_back(i);
            }
            links_[fwdIdx] = LINK_IDLE;
            links_[revIdx] = LINK_IDLE;
            transitions_[fwdIdx] = {};
            transitions_[revIdx] = {};
        }

        rankBase_[rankId].clear();
        rankExternal_[rankId].clear();
        states_[rankId] = RANK_IDLE;
        aliveRanks_.erase(rankId);
    }

    if (sender_) {
        for (uint32_t peerId : affectedPeers) {
            EnqueueSend({SendOp::REMOVE, peerId, rankId, LinkIndex(peerId, rankId)});
        }
    }

    return 0;
}

/*
 * Compute the row-major index for the directional link src→dst.
 */
uint32_t SmemGroupManagerServer::LinkIndex(uint32_t src, uint32_t dst) const noexcept
{
    return src * maxRanks_ + dst;
}

/*
 * Set the state of a single directional link. No-op if either rankId is out of range.
 */
void SmemGroupManagerServer::SetLinkState(uint32_t srcRank, uint32_t dstRank, LinkState state) noexcept
{
    if (srcRank >= maxRanks_ || dstRank >= maxRanks_) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    auto idx = LinkIndex(srcRank, dstRank);
    links_[idx] = state;
    if (!IsStableLinkState(state)) {
        AddActiveLink(idx);
    }
}

/*
 * Get the state of a single directional link. Returns LINK_IDLE for out-of-range rankId.
 */
LinkState SmemGroupManagerServer::GetLinkState(uint32_t srcRank, uint32_t dstRank) const noexcept
{
    if (srcRank >= maxRanks_ || dstRank >= maxRanks_) {
        return LINK_IDLE;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return links_[LinkIndex(srcRank, dstRank)];
}

void SmemGroupManagerServer::QueryLinkStates() noexcept
{
    std::vector<uint32_t> alive;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < maxRanks_; ++i) {
            if (states_[i] == RANK_ACTIVE || states_[i] == RANK_CHECKED_IN) {
                alive.push_back(i);
            }
        }
    }

    if (sender_) {
        for (auto rankId : alive) {
            auto msg = SmemMessage::PackQueryLinkState(rankId);
            auto packed = SmemMessagePacker::Pack(msg);
            int qRet = -1;
            if (!packed.empty()) {
                qRet = sender_(rankId, packed);
            }
            if (qRet != 0) {
                STORE_LOG_WARN("[GM][Server][Send] QueryLinkStates: failed for rank " << rankId << ": " << qRet);
            }
        }
    }
}

void SmemGroupManagerServer::OnLinkStateResponse(const RankFullInfo &rankInfo,
                                                 const std::vector<LinkStateEntry> &entries, uint64_t reqId) noexcept
{
    const uint32_t rankId = rankInfo.rankId;
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << rankId << " type=LNKSRSP reqId=" << reqId);
    if (rankId >= maxRanks_) {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    rankBase_[rankId] = rankInfo.baseInfo;
    rankExternal_[rankId] = rankInfo.externalInfo;
    aliveRanks_.insert(rankId);
    for (const auto &entry : entries) {
        if (entry.dstRankId >= maxRanks_ || aliveRanks_.find(entry.dstRankId) == aliveRanks_.end()) {
            continue;
        }
        auto idx = LinkIndex(rankId, entry.dstRankId);
        auto newSt = static_cast<LinkState>(entry.state);
        links_[idx] = newSt;
        if (!IsStableLinkState(newSt)) {
            AddActiveLink(idx);
        }
    }
}

bool SmemGroupManagerServer::ShouldDegradeLink(LinkTransition &tr, size_t idx) noexcept
{
    if (tr.retryCount >= LINK_QUERY_MAX_RETRIES) {
        links_[idx] = LINK_IDLE;
        tr = {};
        return true;
    }
    return false;
}

bool SmemGroupManagerServer::IsTimeout(int64_t elapsed) const noexcept
{
    return elapsed >= static_cast<int64_t>(LINK_QUERY_TIMEOUT_MS);
}

void SmemGroupManagerServer::HandleLinkExchanging(LinkScanContext &ctx, std::vector<LinkScanJob> &addJobs) noexcept
{
    if (ShouldDegradeLink(ctx.tr, ctx.linkIdx)) {
        return;
    }
    if (IsTimeout(ctx.elapsedMs)) {
        ctx.tr.retryCount++;
        addJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::HandleLinkExchanged(LinkScanContext &ctx, std::vector<LinkScanJob> &establishJobs) noexcept
{
    if (ShouldDegradeLink(ctx.tr, ctx.linkIdx)) {
        return;
    }
    if (ctx.tr.retryCount == 0 || IsTimeout(ctx.elapsedMs)) {
        ctx.tr.retryCount++;
        establishJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::HandleLinkConnecting(LinkScanContext &ctx,
                                                  std::vector<LinkScanJob> &establishJobs) noexcept
{
    if (ShouldDegradeLink(ctx.tr, ctx.linkIdx)) {
        return;
    }
    if (IsTimeout(ctx.elapsedMs)) {
        establishJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::HandleLinkDisconnecting(LinkScanContext &ctx,
                                                     std::vector<LinkScanJob> &removeJobs) noexcept
{
    if (ctx.tr.retryCount >= LINK_QUERY_MAX_RETRIES) {
        links_[ctx.linkIdx] = LINK_DISCONNECTED;
        ctx.tr = {ctx.now, 0};
        return;
    }
    if (IsTimeout(ctx.elapsedMs)) {
        ctx.tr.retryCount++;
        removeJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::HandleLinkDisconnected(LinkScanContext &ctx, std::vector<LinkScanJob> &closeJobs) noexcept
{
    if (ShouldDegradeLink(ctx.tr, ctx.linkIdx)) {
        return;
    }
    if (ctx.tr.retryCount == 0 || IsTimeout(ctx.elapsedMs)) {
        ctx.tr.retryCount++;
        closeJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::HandleLinkCleaning(LinkScanContext &ctx, std::vector<LinkScanJob> &closeJobs) noexcept
{
    if (ShouldDegradeLink(ctx.tr, ctx.linkIdx)) {
        return;
    }
    if (IsTimeout(ctx.elapsedMs)) {
        ctx.tr.retryCount++;
        closeJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::ScanLinkStates(LinkScanContext &ctx, LinkState st, LinkScanJobs &jobs) noexcept
{
    switch (st) {
        case LINK_EXCHANGING:
            HandleLinkExchanging(ctx, jobs.addJobs);
            break;
        case LINK_EXCHANGED:
            HandleLinkExchanged(ctx, jobs.establishJobs);
            break;
        case LINK_CONNECTING:
            HandleLinkConnecting(ctx, jobs.establishJobs);
            break;
        case LINK_DISCONNECTING:
            HandleLinkDisconnecting(ctx, jobs.removeJobs);
            break;
        case LINK_DISCONNECTED:
            HandleLinkDisconnected(ctx, jobs.closeJobs);
            break;
        case LINK_CLEANING:
            HandleLinkCleaning(ctx, jobs.closeJobs);
            break;
        default:
            break;
    }
}

/*
 * Transition LEAVING ranks to IDLE once all their links are IDLE.
 * Populates ranksToCleanup for caller to persist.
 */
void SmemGroupManagerServer::CleanupLeavingRanks(std::vector<uint32_t> &ranksToCleanup) noexcept
{
    for (uint32_t r = 0; r < maxRanks_; ++r) {
        if (states_[r] != RANK_LEAVING) {
            continue;
        }
        bool allIdle = true;
        for (uint32_t j = 0; j < maxRanks_; ++j) {
            if (j == r) {
                continue;
            }
            if (links_[LinkIndex(r, j)] != LINK_IDLE || links_[LinkIndex(j, r)] != LINK_IDLE) {
                allIdle = false;
                break;
            }
        }
        if (allIdle) {
            states_[r] = RANK_IDLE;
            rankBase_[r].clear();
            rankExternal_[r].clear();
            ranksToCleanup.push_back(r);
            STORE_LOG_INFO("rank=" << r << " state=IDLE, leave cleanup done");
        }
    }
}

/*
 * Promote CHECKED_IN ranks to ACTIVE once all their links reach stable states
 * and at least half of the peer links are CONNECTED.
 * Populates promotedToActive for caller to persist state changes.
 */
void SmemGroupManagerServer::PromoteCheckedInRanks(std::vector<uint32_t> &promotedToActive) noexcept
{
    TP_TRACE_BEGIN(TP_SMEM_GROUP_SERVER_PROMOTE);
    for (uint32_t r = 0; r < maxRanks_; ++r) {
        if (states_[r] != RANK_CHECKED_IN) {
            continue;
        }
        bool allStable = true;
        for (uint32_t j = 0; j < maxRanks_; ++j) {
            if (j == r) {
                continue;
            }
            LinkState ls = links_[LinkIndex(r, j)];
            if (ls != LINK_IDLE && ls != LINK_CONNECTED) {
                allStable = false;
                break;
            }
        }
        if (allStable && HasEnoughConnectedLinks(r)) {
            states_[r] = RANK_ACTIVE;
            promotedToActive.push_back(r);
            STORE_LOG_INFO("rank=" << r << " state=ACTIVE (all links stable, enough connected)");
        }
    }
    TP_TRACE_END(TP_SMEM_GROUP_SERVER_PROMOTE, 0);
}

bool SmemGroupManagerServer::HasEnoughConnectedLinks(uint32_t r) noexcept
{
    uint32_t connected = 0;
    uint32_t peers = 0;
    for (uint32_t j = 0; j < maxRanks_; ++j) {
        if (j == r) {
            continue;
        }
        if (states_[j] != RANK_ACTIVE && states_[j] != RANK_CHECKED_IN) {
            continue;
        }
        peers++;
        if (links_[LinkIndex(r, j)] == LINK_CONNECTED) {
            connected++;
        }
    }
    if (peers == 0) {
        return true;
    }
    return connected == peers;
}

void SmemGroupManagerServer::RetryIdleLinks() noexcept
{
    constexpr int64_t kIdleLinkRetryIntervalMs = 30000;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastIdleRetryTime_).count();
    if (elapsed < kIdleLinkRetryIntervalMs) {
        return;
    }
    lastIdleRetryTime_ = now;

    for (uint32_t r = 0; r < maxRanks_; ++r) {
        if (states_[r] != RANK_ACTIVE && states_[r] != RANK_CHECKED_IN) {
            continue;
        }
        for (uint32_t j = 0; j < maxRanks_; ++j) {
            if (j == r) {
                continue;
            }
            if (states_[j] != RANK_ACTIVE && states_[j] != RANK_CHECKED_IN) {
                continue;
            }
            uint32_t idx = LinkIndex(r, j);
            if (links_[idx] == LINK_IDLE) {
                links_[idx] = LINK_EXCHANGING;
                transitions_[idx] = {now, 0};
                AddActiveLink(idx);
            }
        }
    }
}

/*
 * Convert ScanLinkStates results into EnqueueSend calls for the send worker.
 */
void SmemGroupManagerServer::EnqueueLinkJobs(const LinkScanJobs &jobs) noexcept
{
    auto count = jobs.addJobs.size() + jobs.establishJobs.size() + jobs.removeJobs.size() + jobs.closeJobs.size();
    if (count == 0) {
        return;
    }

    std::vector<SendTask> batch;
    batch.reserve(count);

    auto append = [&batch](const auto &jobs, SendOp op) {
        for (auto &job : jobs) {
            batch.push_back({op, job.srcRank, job.dstRank, job.linkIdx});
        }
    };
    append(jobs.addJobs, SendOp::ADD);
    append(jobs.establishJobs, SendOp::ESTABLISH);
    append(jobs.removeJobs, SendOp::REMOVE);
    append(jobs.closeJobs, SendOp::CLOSE);

    EnqueueBatchSend(std::move(batch));
}

bool SmemGroupManagerServer::IsStableLinkState(LinkState st) noexcept
{
    return st == LINK_IDLE || st == LINK_CONNECTED;
}

void SmemGroupManagerServer::AddActiveLink(size_t idx) noexcept
{
    if (isActiveLink_[idx]) {
        return;
    }
    if (IsStableLinkState(links_[idx])) {
        return;
    }
    isActiveLink_[idx] = true;
    activeLinks_.push_back(idx);
}

/*
 * Main driver: scan all non-stable links, enqueue actions for expired
 * states, promote CHECKED_IN ranks, and cleanup LEAVING ranks.
 * Called periodically by TransitionDriverTask.
 */
void SmemGroupManagerServer::DrivePendingTransitions() noexcept
{
    TP_TRACE_BEGIN(TP_SMEM_GROUP_SERVER_DRIVE_TRANSITIONS);
    LinkScanJobs jobs;
    auto now = std::chrono::steady_clock::now();

    std::vector<uint32_t> promotedToActive;
    {
        std::vector<uint32_t> ranksToCleanup;
        std::unique_lock<std::mutex> lock(mutex_);
        for (size_t i = 0; i < activeLinks_.size();) {
            size_t idx = activeLinks_[i];
            LinkState st = links_[idx];
            if (IsStableLinkState(st)) {
                isActiveLink_[idx] = false;
                activeLinks_[i] = activeLinks_.back();
                activeLinks_.pop_back();
                continue;
            }
            auto src = static_cast<uint32_t>(idx / maxRanks_);
            auto dst = static_cast<uint32_t>(idx % maxRanks_);
            LinkTransition &tr = transitions_[idx];
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - tr.timestamp).count();
            LinkScanContext ctx{tr, src, dst, idx, elapsed, now};
            ScanLinkStates(ctx, st, jobs);
            ++i;
        }
        CleanupLeavingRanks(ranksToCleanup);
        PromoteCheckedInRanks(promotedToActive);
        RetryIdleLinks();
    }

    // Notify newly promoted ranks (outside lock — sender_ may block)
    for (auto r : promotedToActive) {
        auto msg = SmemMessage::PackPromoteToActive(r);
        auto packed = SmemMessagePacker::Pack(msg);
        if (!packed.empty()) {
            sender_(r, packed);
            STORE_LOG_INFO("[GM][Server][Send] rank=" << r << " type=PROMOTE_TO_ACTIVE");
        }
    }

    if (!promotedToActive.empty() && onStateChange_) {
        onStateChange_();
    }

    EnqueueLinkJobs(jobs);
    TP_TRACE_END(TP_SMEM_GROUP_SERVER_DRIVE_TRANSITIONS, 0);
}

/*
 * Advance the link state machine on receipt of a client ACK.
 * Only advances if the link is in the expected state for that ACK type.
 * Stale / duplicate ACKs are safely ignored.
 */
void SmemGroupManagerServer::OnControlAck(ControlOp ackOp, uint32_t senderRankId,
                                          const std::map<uint32_t, int32_t> &targetRankRes, uint64_t requestId) noexcept
{
    static const char *ackTag[] = {"ADDWACK", "REMWACK", "ESTCACK", "CLSCACK"};
    auto tag = (ackOp >= CONTROL_ADD_TO_WHITELIST_ACK && ackOp <= CONTROL_CLOSE_CONNECTION_ACK)
                   ? ackTag[ackOp - CONTROL_ADD_TO_WHITELIST_ACK]
                   : "??????";
    std::string ok;
    std::string nok;
    for (auto &kv : targetRankRes) {
        std::string &list = (kv.second == 0) ? ok : nok;
        if (!list.empty()) {
            list += ",";
        }
        list += std::to_string(kv.first);
    }
    std::string extra;
    if (!ok.empty()) {
        extra += " ok=(" + ok + ")";
    }
    if (!nok.empty()) {
        extra += " nok=(" + nok + ")";
    }
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << senderRankId << " type=" << tag << " rid=" << requestId << extra);

    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    bool linksChanged = false;
    for (auto &kv : targetRankRes) {
        uint32_t targetRankId = kv.first;
        if (kv.second != 0) {
            STORE_LOG_INFO("[GM][Server][Event] ackOp=" << tag << " src=" << senderRankId << " tgt=" << targetRankId);
            continue;
        }
        if (ApplyAckToLink(ackOp, senderRankId, targetRankId, now)) {
            linksChanged = true;
        }
    }
    if (linksChanged && onStateChange_) {
        onStateChange_();
    }
}

bool SmemGroupManagerServer::ApplyAckToLink(ControlOp ackOp, uint32_t senderRankId, uint32_t targetRankId,
                                            const std::chrono::steady_clock::time_point &now) noexcept
{
    if (senderRankId >= maxRanks_ || targetRankId >= maxRanks_) {
        return false;
    }
    size_t idx = LinkIndex(senderRankId, targetRankId);
    LinkState &st = links_[idx];
    LinkTransition &tr = transitions_[idx];

    switch (ackOp) {
        case CONTROL_ADD_TO_WHITELIST_ACK: {
            TP_TRACE_BEGIN(TP_SMEM_GROUP_ON_ACK_ADD);
            bool advanced = (st == LINK_EXCHANGING);
            if (advanced) {
                st = LINK_EXCHANGED;
                tr = {now, 0};
            }
            TP_TRACE_END(TP_SMEM_GROUP_ON_ACK_ADD, 0);
            return advanced;
        }
        case CONTROL_ESTABLISH_CONNECTION_ACK: {
            TP_TRACE_BEGIN(TP_SMEM_GROUP_ON_ACK_ESTABLISH);
            bool advanced = (st == LINK_CONNECTING);
            if (advanced) {
                st = LINK_CONNECTED;
                tr = {};
                STORE_LOG_INFO("link CONNECTED sender=" << senderRankId << " target=" << targetRankId);
            }
            TP_TRACE_END(TP_SMEM_GROUP_ON_ACK_ESTABLISH, 0);
            return advanced;
        }
        case CONTROL_REMOVE_FROM_WHITELIST_ACK: {
            TP_TRACE_BEGIN(TP_SMEM_GROUP_ON_ACK_LEAVE);
            bool advanced = (st == LINK_DISCONNECTING);
            if (advanced) {
                st = LINK_DISCONNECTED;
                tr = {now, 0};
            }
            TP_TRACE_END(TP_SMEM_GROUP_ON_ACK_LEAVE, 0);
            return advanced;
        }
        case CONTROL_CLOSE_CONNECTION_ACK: {
            TP_TRACE_BEGIN(TP_SMEM_GROUP_ON_ACK_CLOSE);
            bool advanced = (st == LINK_CLEANING);
            if (advanced) {
                st = LINK_IDLE;
                tr = {};
            }
            TP_TRACE_END(TP_SMEM_GROUP_ON_ACK_CLOSE, 0);
            return advanced;
        }
        default:
            break;
    }
    return false;
}

void SmemGroupManagerServer::OnLinkBroken(uint32_t rankId) noexcept
{
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << rankId << " type=LEAVREQ reason=LinkBroken");
    if (rankId >= maxRanks_) {
        return;
    }

    std::vector<uint32_t> connectedPeers;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (states_[rankId] != RANK_CHECKED_IN && states_[rankId] != RANK_ACTIVE && states_[rankId] != RANK_LEAVING) {
            return;
        }
        ResetRankState(rankId, connectedPeers);
        aliveRanks_.erase(rankId);
    }

    STORE_LOG_INFO("rank=" << rankId << " linkBroken connectedPeers=" << connectedPeers.size());
    if (sender_ && !connectedPeers.empty()) {
        for (uint32_t peerId : connectedPeers) {
            EnqueueSend({SendOp::REMOVE, peerId, rankId, LinkIndex(peerId, rankId)});
            EnqueueSend({SendOp::CLOSE, peerId, rankId, LinkIndex(peerId, rankId)});
        }
        for (uint32_t peerId : connectedPeers) {
            EnqueueSend({SendOp::LEAVE_NOTIFY, peerId, rankId, 0});
        }
    }
}

/*
 * Execute a batch of send tasks grouped by (srcRank, op).
 * Groups tasks and delegates to SendBatchGroup + UpdateLinksAfterBatch.
 */
void SmemGroupManagerServer::ProcessBatch(const std::vector<SendTask> &tasks) noexcept
{
    if (!sender_) {
        return;
    }

    std::unordered_map<uint64_t, std::vector<const SendTask *>> groups;
    groups.reserve(tasks.size());
    for (auto &t : tasks) {
        uint64_t key = (static_cast<uint64_t>(t.srcRank) << reqIdShift) | static_cast<uint32_t>(t.op);
        groups[key].push_back(&t);
    }

    for (auto &kv : groups) {
        auto &g = kv.second;
        int ret = SendBatchGroup(g[0]->srcRank, g[0]->op, g);
        UpdateLinksAfterBatch(g, ret);
    }
}

int SmemGroupManagerServer::SendBatchAddRemove(uint32_t srcRank, SendOp op, const std::vector<const SendTask *> &group,
                                               uint64_t reqId) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    if (op == SendOp::ADD) {
        std::vector<RankFullInfo> bases;
        bases.reserve(group.size());
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto *t : group) {
                RankFullInfo base;
                base.rankId = t->dstRank;
                base.baseInfo = rankBase_[t->dstRank];
                base.externalInfo = rankExternal_[t->dstRank];
                bases.push_back(base);
            }
        }
        msg = SmemMessage::PackAddToWhitelist(srcRank, bases);
    } else {
        std::vector<RankBaseInfo> bases;
        bases.reserve(group.size());
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto *t : group) {
                RankBaseInfo base;
                base.rankId = t->dstRank;
                base.baseInfo = rankBase_[t->dstRank];
                bases.push_back(base);
            }
        }
        msg = SmemMessage::PackRemoveFromWhitelist(srcRank, bases);
    }
    msg.requestId = reqId;
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

int SmemGroupManagerServer::SendBatchEstablish(uint32_t srcRank, const std::vector<const SendTask *> &group,
                                               uint64_t reqId) noexcept
{
    std::vector<RankFullInfo> fulls;
    fulls.reserve(group.size());
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto *t : group) {
            RankFullInfo full;
            full.rankId = t->dstRank;
            full.baseInfo = rankBase_[t->dstRank];
            full.externalInfo = rankExternal_[t->dstRank];
            fulls.push_back(full);
        }
    }
    auto msg = SmemMessage::PackEstablishConnection(srcRank, fulls);
    msg.requestId = reqId;
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

int SmemGroupManagerServer::SendBatchClose(uint32_t srcRank, const std::vector<const SendTask *> &group,
                                           uint64_t reqId) noexcept
{
    std::vector<uint32_t> dsts;
    dsts.reserve(group.size());
    for (auto *t : group) {
        dsts.push_back(t->dstRank);
    }
    auto msg = SmemMessage::PackCloseConnection(srcRank, dsts);
    msg.requestId = reqId;
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

int SmemGroupManagerServer::SendBatchLeaveNotify(const std::vector<const SendTask *> &group) noexcept
{
    for (auto *t : group) {
        auto msg = SmemMessage::PackLeaveNotify(t->dstRank);
        auto packed = SmemMessagePacker::Pack(msg);
        if (!packed.empty()) {
            sender_(t->srcRank, packed);
        }
    }
    if (!group.empty()) {
        STORE_LOG_INFO("[GM][Server][Send] type=LEAV_NY rank=" << group.front()->dstRank);
    }
    return 0;
}

int SmemGroupManagerServer::SendBatchQueryLinkState(uint32_t srcRank) noexcept
{
    auto msg = SmemMessage::PackQueryLinkState(srcRank);
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

int SmemGroupManagerServer::SendBatchGroup(uint32_t srcRank, SendOp op,
                                           const std::vector<const SendTask *> &group) noexcept
{
    static const char *opTag[] = {"ADDWLST", "REMWLST", "ESTCONN", "CLSCONN", "LEAV_NY", "QRY_LST"};
    const char *tag = (static_cast<size_t>(op) < std::size(opTag)) ? opTag[static_cast<size_t>(op)] : "??????";
    uint64_t reqId = group.empty() ? 0 : group.front()->requestId;
    std::string tgt;
    for (size_t i = 0; i < group.size(); ++i) {
        if (i > 0) {
            tgt += ",";
        }
        tgt += std::to_string(group[i]->dstRank);
    }
    STORE_LOG_INFO("[GM][Server][Send] rank=" << srcRank << " type=" << tag << " tgt=(" << tgt << ")");
    switch (op) {
        case SendOp::ADD:
        case SendOp::REMOVE:
            return SendBatchAddRemove(srcRank, op, group, reqId);
        case SendOp::ESTABLISH:
            return SendBatchEstablish(srcRank, group, reqId);
        case SendOp::CLOSE:
            return SendBatchClose(srcRank, group, reqId);
        case SendOp::LEAVE_NOTIFY:
            return SendBatchLeaveNotify(group);
        case SendOp::QUERY_LINK_STATE:
            return SendBatchQueryLinkState(srcRank);
    }
    return -1;
}

void SmemGroupManagerServer::UpdateLinkTransitionOnSuccess(LinkState st, size_t linkIdx, LinkTransition &tr,
                                                           const std::chrono::steady_clock::time_point &now) noexcept
{
    switch (st) {
        case LINK_EXCHANGING:
        case LINK_CONNECTING:
        case LINK_DISCONNECTING:
        case LINK_CLEANING:
            tr.timestamp = now;
            break;
        case LINK_EXCHANGED:
            links_[linkIdx] = LINK_CONNECTING;
            tr = {now, 0};
            break;
        case LINK_DISCONNECTED:
            links_[linkIdx] = LINK_CLEANING;
            tr = {now, 0};
            break;
        default:
            break;
    }
}

void SmemGroupManagerServer::UpdateLinkTransitionOnFailure(LinkState st, size_t linkIdx, LinkTransition &tr,
                                                           const std::chrono::steady_clock::time_point &now) noexcept
{
    switch (st) {
        case LINK_EXCHANGING:
        case LINK_EXCHANGED:
        case LINK_CONNECTING:
            links_[linkIdx] = LINK_IDLE;
            tr = {};
            break;
        case LINK_DISCONNECTING:
            links_[linkIdx] = LINK_DISCONNECTED;
            tr = {now, 0};
            break;
        case LINK_DISCONNECTED:
        case LINK_CLEANING:
            links_[linkIdx] = LINK_IDLE;
            tr = {};
            break;
        default:
            break;
    }
}

void SmemGroupManagerServer::UpdateLinksAfterBatch(const std::vector<const SendTask *> &group, int overallRet) noexcept
{
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto t : group) {
        if (t->op == SendOp::QUERY_LINK_STATE) {
            continue;
        }
        LinkState st = links_[t->linkIdx];
        LinkTransition &tr = transitions_[t->linkIdx];
        if (st == LINK_IDLE) {
            continue;
        }

        if (overallRet == 0) {
            UpdateLinkTransitionOnSuccess(st, t->linkIdx, tr, now);
        } else {
            UpdateLinkTransitionOnFailure(st, t->linkIdx, tr, now);
        }
    }
}

/*
 * Push a send task to the queue and wake up the send worker thread.
 */
void SmemGroupManagerServer::EnqueueSend(const SendTask &task) noexcept
{
    SendTask t = task;
    t.requestId = (static_cast<uint64_t>(maxRanks_ + 1) << reqIdShift) | g_ctrlReqSeq.fetch_add(1);
    std::lock_guard<std::mutex> lk(sendMutex_);
    sendQueue_.push_back(t);
    sendCond_.notify_one();
}

void SmemGroupManagerServer::EnqueueBatchSend(std::vector<SendTask> &&tasks) noexcept
{
    if (tasks.empty()) {
        return;
    }
    auto base = static_cast<uint64_t>(maxRanks_ + 1) << reqIdShift;
    for (auto &t : tasks) {
        t.requestId = base | g_ctrlReqSeq.fetch_add(1);
    }
    std::lock_guard<std::mutex> lk(sendMutex_);
    sendQueue_.insert(sendQueue_.end(), std::make_move_iterator(tasks.begin()), std::make_move_iterator(tasks.end()));
    sendCond_.notify_one();
}

/*
 * Execute a send task via the TransitionController.
 * On success, advances send-driven states (EXCHANGED→CONNECTING, DISCONNECTED→CLEANING)
 * or updates the timestamp for ACK-waiting states (EXCHANGING, CONNECTING, etc.).
 * On failure, increments retryCount and re-enqueues; degrades the link on max retries.
 */
int SmemGroupManagerServer::ExecuteSendOperation(SendOp op, uint32_t srcRank, uint32_t dstRank,
                                                 uint64_t requestId) const noexcept
{
    switch (op) {
        case SendOp::ADD: {
            RankFullInfo base;
            base.rankId = dstRank;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                base.baseInfo = rankBase_[dstRank];
                base.externalInfo = rankExternal_[dstRank];
            }
            auto msg = SmemMessage::PackAddToWhitelist(srcRank, {base});
            msg.requestId = requestId;
            return SendPackedMessage(srcRank, msg);
        }
        case SendOp::REMOVE: {
            RankBaseInfo base;
            base.rankId = dstRank;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                base.baseInfo = rankBase_[dstRank];
            }
            auto msg = SmemMessage::PackRemoveFromWhitelist(srcRank, {base});
            msg.requestId = requestId;
            return SendPackedMessage(srcRank, msg);
        }
        case SendOp::ESTABLISH: {
            RankFullInfo full;
            full.rankId = dstRank;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                full.baseInfo = rankBase_[dstRank];
                full.externalInfo = rankExternal_[dstRank];
            }
            auto msg = SmemMessage::PackEstablishConnection(srcRank, {full});
            msg.requestId = requestId;
            return SendPackedMessage(srcRank, msg);
        }
        case SendOp::CLOSE: {
            auto msg = SmemMessage::PackCloseConnection(srcRank, {dstRank});
            msg.requestId = requestId;
            return SendPackedMessage(srcRank, msg);
        }
        case SendOp::QUERY_LINK_STATE: {
            auto msg = SmemMessage::PackQueryLinkState(srcRank);
            return SendPackedMessage(srcRank, msg);
        }
        default:
            return -1;
    }
}

int SmemGroupManagerServer::SendPackedMessage(uint32_t srcRank, const SmemMessage &msg) const noexcept
{
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

void SmemGroupManagerServer::ProcessSend(SendTask task) noexcept
{
    if (!sender_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    int ret = ExecuteSendOperation(task.op, task.srcRank, task.dstRank, task.requestId);

    std::lock_guard<std::mutex> lk(mutex_);
    LinkState st = links_[task.linkIdx];
    LinkTransition &tr = transitions_[task.linkIdx];

    if (st == LINK_IDLE) {
        return;
    }

    if (ret == 0) {
        UpdateLinkTransitionOnSuccess(st, task.linkIdx, tr, now);
    } else {
        UpdateLinkTransitionOnFailure(st, task.linkIdx, tr, now);
    }
}

/*
 * Background thread: dequeue ALL pending send tasks, batch them by
 * (srcRank, op), and send each group as a single packed message.
 * Wakes on new tasks or every 100 ms (poll timeout).
 */
void SmemGroupManagerServer::SendWorkerTask() noexcept
{
    pthread_setname_np(pthread_self(), "group_send");
    while (running_) {
        std::vector<SendTask> batch;
        {
            std::unique_lock<std::mutex> lk(sendMutex_);
            sendCond_.wait_for(lk, std::chrono::milliseconds(sendWorkerPollMs),
                               [this] { return !sendQueue_.empty() || !running_; });
            if (!running_ && sendQueue_.empty()) {
                break;
            }
            if (sendQueue_.empty()) {
                continue;
            }
            batch.swap(sendQueue_);
        }
        ProcessBatch(batch);
    }
}

/*
 * Start the driver and send worker background threads.
 * Safe to call multiple times (no-op if already running).
 */
void SmemGroupManagerServer::Start() noexcept
{
    if (running_) {
        return;
    }
    running_ = true;
    driverThread_ = std::thread(&SmemGroupManagerServer::TransitionDriverTask, this);
    sendThread_ = std::thread(&SmemGroupManagerServer::SendWorkerTask, this);
}

/*
 * Signal threads to stop and wait for them to join.
 * Safe to call multiple times (no-op if not running).
 */
void SmemGroupManagerServer::Stop() noexcept
{
    if (!running_) {
        return;
    }
    running_ = false;
    sendCond_.notify_all();
    driverCond_.notify_all();
    if (sendThread_.joinable()) {
        sendThread_.join();
    }
    if (driverThread_.joinable()) {
        driverThread_.join();
    }
}

/*
 * Background thread: periodically call DrivePendingTransitions to
 * scan and advance the link state machine.
 */
void SmemGroupManagerServer::TransitionDriverTask() noexcept
{
    pthread_setname_np(pthread_self(), "group_mgr_drv");
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        lock.unlock();
        DrivePendingTransitions();
        lock.lock();
        if (!running_) {
            break;
        }
        driverCond_.wait_for(lock, std::chrono::milliseconds(linkTransitionDriverIntervalMs),
                             [this] { return !running_; });
    }
}

// ── 状态访问 ──

const Bytes &SmemGroupManagerServer::GetRankBase(uint32_t rankId) const noexcept
{
    if (rankId >= maxRanks_) {
        static const Bytes empty;
        return empty;
    }
    return rankBase_[rankId];
}

const std::vector<Bytes> &SmemGroupManagerServer::GetRankExternal(uint32_t rankId) const noexcept
{
    if (rankId >= maxRanks_) {
        static const std::vector<Bytes> empty;
        return empty;
    }
    return rankExternal_[rankId];
}

// ── 状态恢复 ──

void SmemGroupManagerServer::RestoreState(const std::vector<RankState> &states, const std::vector<LinkState> &links,
                                          const std::unordered_set<uint32_t> &alive) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    uint32_t n = std::min(static_cast<uint32_t>(states.size()), maxRanks_);
    for (uint32_t i = 0; i < n; ++i) {
        states_[i] = states[i];
    }
    uint32_t linkCount = std::min(static_cast<uint32_t>(links.size()), maxRanks_ * maxRanks_);
    for (uint32_t i = 0; i < linkCount; ++i) {
        links_[i] = links[i];
    }
    aliveRanks_ = alive;
    STORE_LOG_INFO("RestoreState: " << n << " ranks, " << linkCount << " links, " << alive.size() << " alive");
}

void SmemGroupManagerServer::RestoreState(const std::vector<RankState> &states,
                                          const std::unordered_set<uint32_t> &alive) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    uint32_t n = std::min(static_cast<uint32_t>(states.size()), maxRanks_);
    for (uint32_t i = 0; i < n; ++i) {
        states_[i] = states[i];
    }
    aliveRanks_ = alive;
    STORE_LOG_INFO("RestoreState (states only): " << n << " ranks, " << alive.size() << " alive");
}

void SmemGroupManagerServer::SetRankBase(uint32_t rankId, const Bytes &data) noexcept
{
    if (rankId >= maxRanks_) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    rankBase_[rankId] = data;
}

void SmemGroupManagerServer::SetRankExternal(uint32_t rankId, const std::vector<Bytes> &data) noexcept
{
    if (rankId >= maxRanks_) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    rankExternal_[rankId] = data;
}

void SmemGroupManagerServer::RebuildActiveLinks() noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    isActiveLink_.assign(maxRanks_ * maxRanks_, false);
    activeLinks_.clear();
    for (uint32_t i = 0; i < maxRanks_ * maxRanks_; ++i) {
        if (!IsStableLinkState(links_[i])) {
            isActiveLink_[i] = true;
            activeLinks_.push_back(i);
        }
    }
    STORE_LOG_INFO("RebuildActiveLinks: " << activeLinks_.size() << " non-stable links");
}

} // namespace ock::smem
