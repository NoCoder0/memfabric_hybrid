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
#ifndef SMEM_SMEM_GROUP_MANAGER_SERVER_H
#define SMEM_SMEM_GROUP_MANAGER_SERVER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "smem_config_store_backend.h"
#include "smem_group_manager_def.h"
#include "smem_message_packer.h"
#include "smem_ref.h"

namespace ock {

// Upper bound for managed ranks; must stay in sync with SMEM_GROUP_MAX in smem_group_manager.h.
inline constexpr uint32_t SMEM_RANK_MAX = 1024U;

/* Link-state query timeout and retry constants.
 * After a server crash Recovery queries each active rank for its link state.
 * • Per-query timeout: 1 s
 * • Max retries:       10 (total wall-clock ≈ 10 s)
 * • After 10 failures the link is assumed IDLE (no connection).
 */
inline constexpr uint32_t LINK_QUERY_TIMEOUT_MS = 20000U;
inline constexpr uint32_t LINK_QUERY_MAX_RETRIES = 10U;

namespace smem {

using StoreBackendPtr = SmRef<ConfigStoreBackend>;

// Control message sequence number shared by server-side senders (defined in
// smem_tcp_config_store_server.cpp). Declared here so .cpp files include the
// header instead of a local extern declaration (G.EXP.05).
extern std::atomic<uint32_t> g_ctrlReqSeq;

/**
 * Rank lifecycle:
 *
 *   IDLE ──(CheckIn)──► CHECKED_IN ──(Connect)──► ACTIVE ──(Disconnect)──► LEAVING
 *     ▲                     │                                           │
 *     └───────(Clear)───────┘                                           │
 *     └───────────────────────────────(Clear)───────────────────────────┘
 *
 * - IDLE:       rank not in the group, no info stored
 * - CHECKED_IN: info stored via CheckIn, not yet accepting connections
 * - ACTIVE:     online and accepting connections
 * - LEAVING:    departing, connections closing
 */
enum RankState : int8_t {
    RANK_IDLE = 0,
    RANK_CHECKED_IN = 1,
    RANK_ACTIVE = 2,
    RANK_LEAVING = 3,
};

/**
 * Directional link state between two ranks (from srcRank to dstRank):
 *
 *   IDLE ──(whitelist add)──► EXCHANGING ──(add response)──► EXCHANGED
 *     ▲                                                          │
 *     │                                          connect request │
 *     │                                                          ▼
 *     │                                                    CONNECTING
 *     │                                                          │
 *     │                                          connect response│
 *     │                                                          ▼
 *     │                                                    CONNECTED
 *     │                                                          │
 *     │                                                disconnect│
 *     │                                                          ▼
 *     │                                                  DISCONNECTING
 *     │                                                          │
 *     │                                       disconnect response│
 *     │                                                          ▼
 *     │                                                  DISCONNECTED
 *     │                                                          │
 *     │                                  whitelist remove request│
 *     │                                                          ▼
 *     └──────────────────────── CLEANING ────────────────────────┘
 *    whitelist remove response
 *
 * - IDLE:           no connection exists
 * - EXCHANGING:     whitelist add request sent, waiting for response
 * - EXCHANGED:      whitelist add response received, info exchanged
 * - CONNECTING:     connect request sent, waiting for response
 * - CONNECTED:      link established and usable
 * - DISCONNECTING:  disconnect request sent, waiting for response
 * - DISCONNECTED:   disconnected, pending resource cleanup
 * - CLEANING:      resources being released, about to return to IDLE
 *
 * Stability:
 *   IDLE and CONNECTED are stable states — no action is triggered proactively
 *   and the link remains in that state indefinitely.
 *   EXCHANGED and DISCONNECTED actively trigger the next event.
 *   EXCHANGING, CONNECTING, DISCONNECTING and CLEANING wait for a response
 *   and retry the event on timeout.
 */
enum LinkState : int8_t {
    LINK_IDLE = 0,
    LINK_EXCHANGING = 1,
    LINK_EXCHANGED = 2,
    LINK_CONNECTING = 3,
    LINK_CONNECTED = 4,
    LINK_DISCONNECTING = 5,
    LINK_DISCONNECTED = 6,
    LINK_CLEANING = 7,
};

/**
 * @brief Manages per-rank state and inter-rank link state.
 *
 * Rank state uses a flat vector of size maxRanks.
 * Link state uses a flat adjacency matrix of size maxRanks x maxRanks
 * stored as a single vector (row-major: index = src * maxRanks + dst).
 *
 * Both vectors grow with maxRanks rather than always allocating
 * SMEM_RANK_MAX entries, so small deployments waste less memory.
 *
 * All public methods are thread-safe (guarded by mutex_).
 */
class SmemGroupManagerServer : public SmReferable {
public:
    explicit SmemGroupManagerServer(SmemGroupCommandSender sender, uint32_t maxRanks = SMEM_RANK_MAX) noexcept;
    ~SmemGroupManagerServer() noexcept override;
    SmemGroupManagerServer(const SmemGroupManagerServer &) = delete;
    SmemGroupManagerServer &operator=(const SmemGroupManagerServer &) = delete;

    void SetSendFunc(SmemGroupCommandSender sender) noexcept
    {
        sender_ = std::move(sender);
    }

    int CheckIn(const RankFullInfo &info, uint64_t reqId = 0) noexcept;
    int ProcessExtendMemory(uint32_t rankId, const MultiBytes &additionalSlices, uint64_t reqId = 0) noexcept;
    int Checkout(uint32_t rankId, uint64_t reqId = 0) noexcept;
    int Clear(uint32_t rankId) noexcept;

    void SetRankState(uint32_t rankId, RankState state) noexcept;
    RankState GetRankState(uint32_t rankId) const noexcept;
    std::vector<uint32_t> GetAliveRanks() const noexcept;

    void SetLinkState(uint32_t srcRank, uint32_t dstRank, LinkState state) noexcept;
    LinkState GetLinkState(uint32_t srcRank, uint32_t dstRank) const noexcept;

    /**
     * @brief Query all active ranks for their link state and update the in-memory matrix.
     *
     * Called during Restore(). Sends a QUERY_LINK_STATE request to each active
     * rank via the TransitionController, waits up to LINK_QUERY_TIMEOUT_MS per
     * attempt and retries up to LINK_QUERY_MAX_RETRIES times. Links for which
     * no response is received are assumed IDLE.
     */
    void QueryLinkStates() noexcept;

    /**
     * @brief Apply a link-state response from a rank.
     *
     * Called when a LINK_STATE_RESPONSE message is received from a rank.
     * Updates the in-memory link-state matrix for the responding rank.
     */
    void OnLinkStateResponse(const RankFullInfo &rankInfo, const std::vector<LinkStateEntry> &entries,
                             uint64_t reqId = 0) noexcept;

    /**
     * @brief Process an ACK from a client for a previous control command.
     *
     * Advances the link state machine based on the confirmed operation:
     *   AddToWhitelist ACK → EXCHANGING → EXCHANGED
     *   EstablishConnection ACK → CONNECTING → CONNECTED
     *   RemoveFromWhitelist ACK → DISCONNECTING → DISCONNECTED
     *   CloseConnection ACK → CLEANING → IDLE
     */
    void OnControlAck(ControlOp ackOp, uint32_t senderRankId, const std::map<uint32_t, int32_t> &targetRankRes,
                      uint64_t requestId = 0) noexcept;

    /**
     * @brief Notify the state machine that a rank's TCP connection has broken.
     *
     * Transitions all non-IDLE links involving this rank to IDLE immediately,
     * bypassing the normal DISCONNECTING→DISCONNECTED→CLEANING→IDLE sequence.
     * If the rank is in LEAVING state, also transitions it to IDLE and clears
     * its stored info, since there is no longer a channel to send control
     * messages to that rank.
     */
    void OnLinkBroken(uint32_t rankId) noexcept;
    void MarkRankReconnected(uint32_t rankId) noexcept;

    // ── 状态访问（供 etcd 持久化使用） ──

    uint32_t GetMaxRanks() const noexcept
    {
        return maxRanks_;
    }
    const std::vector<RankState> &GetStates() const noexcept
    {
        return states_;
    }
    const std::vector<LinkState> &GetLinks() const noexcept
    {
        return links_;
    }
    const Bytes &GetRankBase(uint32_t rankId) const noexcept;
    const std::vector<Bytes> &GetRankExternal(uint32_t rankId) const noexcept;

    // ── 状态恢复（供 EtcdStateStore 使用） ──

    void RestoreState(const std::vector<RankState> &states, const std::vector<LinkState> &links,
                      const std::unordered_set<uint32_t> &alive) noexcept;
    void RestoreState(const std::vector<RankState> &states, const std::unordered_set<uint32_t> &alive) noexcept;
    void SetRankBase(uint32_t rankId, const Bytes &data) noexcept;
    void SetRankExternal(uint32_t rankId, const std::vector<Bytes> &data) noexcept;
    void RebuildActiveLinks() noexcept;

    // ── 状态变更回调（供 AccStoreServer 监听内部状态变化） ──

    using StateChangeCallback = std::function<void()>;
    void SetOnStateChangeCallback(StateChangeCallback cb) noexcept
    {
        onStateChange_ = std::move(cb);
    }

    void Start() noexcept;
    void Stop() noexcept;

private:
    uint32_t LinkIndex(uint32_t src, uint32_t dst) const noexcept;

    struct LinkTransition {
        std::chrono::steady_clock::time_point timestamp{};
        uint32_t retryCount{0};
    };

    static constexpr uint32_t linkTransitionDriverIntervalMs = 10U;
    static constexpr uint32_t sendWorkerPollMs = 100U;

    struct LinkScanJob {
        uint32_t srcRank;
        uint32_t dstRank;
        size_t linkIdx;
    };

    // Shared context for one link scan step; aggregates the per-link parameters
    // passed to HandleLink* handlers so each handler keeps a small signature.
    struct LinkScanContext {
        LinkTransition &tr;
        uint32_t srcRank;
        uint32_t dstRank;
        size_t linkIdx;
        int64_t elapsedMs;
        std::chrono::steady_clock::time_point now;
    };

    static bool IsStableLinkState(LinkState st) noexcept;
    void AddActiveLink(size_t idx) noexcept;
    void RemoveActiveLinksForRank(uint32_t rankId) noexcept;
    void DrivePendingTransitions() noexcept;
    void ResetRankState(uint32_t rankId, std::vector<uint32_t> &connectedPeers) noexcept;

    bool ShouldDegradeLink(LinkTransition &tr, size_t idx) noexcept;
    bool IsTimeout(int64_t elapsed) const noexcept;
    void HandleLinkExchanging(LinkScanContext &ctx, std::vector<LinkScanJob> &addJobs) noexcept;
    void HandleLinkExchanged(LinkScanContext &ctx, std::vector<LinkScanJob> &establishJobs) noexcept;
    void HandleLinkConnecting(LinkScanContext &ctx, std::vector<LinkScanJob> &establishJobs) noexcept;
    void HandleLinkDisconnecting(LinkScanContext &ctx, std::vector<LinkScanJob> &removeJobs) noexcept;
    void HandleLinkDisconnected(LinkScanContext &ctx, std::vector<LinkScanJob> &closeJobs) noexcept;
    void HandleLinkCleaning(LinkScanContext &ctx, std::vector<LinkScanJob> &closeJobs) noexcept;
    struct LinkScanJobs {
        std::vector<LinkScanJob> addJobs;
        std::vector<LinkScanJob> establishJobs;
        std::vector<LinkScanJob> removeJobs;
        std::vector<LinkScanJob> closeJobs;
    };
    void ScanLinkStates(LinkScanContext &ctx, LinkState st, LinkScanJobs &jobs) noexcept;
    void CleanupLeavingRanks(std::vector<uint32_t> &ranksToCleanup) noexcept;
    void PromoteCheckedInRanks(std::vector<uint32_t> &promotedToActive) noexcept;
    bool HasEnoughConnectedLinks(uint32_t r) noexcept;
    void RetryIdleLinks() noexcept;
    void EnqueueLinkJobs(const LinkScanJobs &jobs) noexcept;
    void TransitionDriverTask() noexcept;

    void UpdateLinkTransitionOnSuccess(LinkState st, size_t linkIdx, LinkTransition &tr,
                                       const std::chrono::steady_clock::time_point &now) noexcept;
    void UpdateLinkTransitionOnFailure(LinkState st, size_t linkIdx, LinkTransition &tr,
                                       const std::chrono::steady_clock::time_point &now) noexcept;

    // Apply a per-target ACK to the link state machine (extracted from OnControlAck)
    bool ApplyAckToLink(ControlOp ackOp, uint32_t senderRankId, uint32_t targetRankId,
                        const std::chrono::steady_clock::time_point &now) noexcept;
    // Pack + send one control message (extracted from ExecuteSendOperation)
    int SendPackedMessage(uint32_t srcRank, const SmemMessage &msg) const noexcept;

    enum class SendOp { ADD, REMOVE, ESTABLISH, CLOSE, LEAVE_NOTIFY, QUERY_LINK_STATE };

    struct SendTask {
        SendOp op{SendOp::ADD};
        uint32_t srcRank{};
        uint32_t dstRank{};
        size_t linkIdx{};
        uint32_t retryCount{0};
        uint64_t requestId{0};
    };

    int ExecuteSendOperation(SendOp op, uint32_t srcRank, uint32_t dstRank, uint64_t requestId = 0) const noexcept;
    void ProcessSend(SendTask task) noexcept;
    void ProcessBatch(const std::vector<SendTask> &tasks) noexcept;
    int SendBatchGroup(uint32_t srcRank, SendOp op, const std::vector<const SendTask *> &group) noexcept;
    int SendBatchAddRemove(uint32_t srcRank, SendOp op, const std::vector<const SendTask *> &group,
                           uint64_t reqId) noexcept;
    int SendBatchEstablish(uint32_t srcRank, const std::vector<const SendTask *> &group, uint64_t reqId) noexcept;
    int SendBatchClose(uint32_t srcRank, const std::vector<const SendTask *> &group, uint64_t reqId) noexcept;
    int SendBatchLeaveNotify(const std::vector<const SendTask *> &group) noexcept;
    int SendBatchQueryLinkState(uint32_t srcRank) noexcept;
    void UpdateLinksAfterBatch(const std::vector<const SendTask *> &group, int overallRet) noexcept;
    void EnqueueSend(const SendTask &task) noexcept;
    void EnqueueBatchSend(std::vector<SendTask> &&tasks) noexcept;
    void SendWorkerTask() noexcept;
    uint64_t SendTaskKey(const SendTask &t) const noexcept;

    // Request-id encoding: high 32 bits = node identifier, low 32 bits = sequence number.
    static constexpr uint32_t reqIdShift = 32U;

    mutable std::mutex mutex_;
    SmemGroupCommandSender sender_;
    StateChangeCallback onStateChange_;            // fired after Promote/OnControlAck
    std::vector<RankState> states_;                // index = rankId, fixed size after construction
    std::vector<LinkState> links_;                 // row-major: [src * maxRanks_ + dst]
    std::vector<LinkTransition> transitions_;      // parallel to links_, tracks timestamp & retry
    std::vector<Bytes> rankBase_;                  // base info per rank (e.g. endpoint)
    std::vector<std::vector<Bytes>> rankExternal_; // external addresses per rank
    std::vector<bool> isActiveLink_;               // parallel to links_, marks presence in activeLinks_
    std::vector<size_t> activeLinks_;              // current non-stable link indices for O(1) scan
    std::unordered_set<uint32_t> aliveRanks_;
    uint32_t maxRanks_;
    std::chrono::steady_clock::time_point lastIdleRetryTime_{};
    std::thread driverThread_;
    std::condition_variable driverCond_;
    std::thread sendThread_;
    std::mutex sendMutex_;
    std::condition_variable sendCond_;
    std::vector<SendTask> sendQueue_;
    std::unordered_set<uint64_t> pendingSendKeys_; // dedup keys: (srcRank<<40)|(op<<32)|(linkIdx&0xffffffff)
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> reqSeq_{0};
};

using SmemGroupManagerServerPtr = SmRef<SmemGroupManagerServer>;

} // namespace smem
} // namespace ock

#endif // SMEM_SMEM_GROUP_MANAGER_SERVER_H
