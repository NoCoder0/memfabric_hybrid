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

#ifndef MEMFABRIC_HYBRID_SMEM_NET_ASYNC_GROUP_MANAGER_H
#define MEMFABRIC_HYBRID_SMEM_NET_ASYNC_GROUP_MANAGER_H

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>

#include "smem_group_manager.h"

namespace ock::smem {

class SmemGroupCommandAsyncDispatcher {
public:
    using WhitelistCallback =
        std::function<int(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId)>;
    using ConnectionCallback =
        std::function<int(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId)>;
    using CloseConnectionCallback =
        std::function<int(uint32_t rankId, const std::vector<uint32_t> &others, uint64_t reqId)>;
    using LinkStateQueryCallback = std::function<std::vector<LinkStateEntry>()>;
    using LeaveNotifyCallback = std::function<int(uint32_t leavingRankId)>;
    using AddSlicesCallback = std::function<int(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId)>;
    using SendAckBatchFunc =
        std::function<void(ControlOp ackOp, uint32_t senderRankId, const std::vector<uint32_t> &targetRankIds,
                           const std::vector<int32_t> &results, uint64_t requestId)>;

    explicit SmemGroupCommandAsyncDispatcher(SmemGroupManager *groupMgr) noexcept;
    virtual ~SmemGroupCommandAsyncDispatcher() noexcept;

    SmemGroupCommandAsyncDispatcher(const SmemGroupCommandAsyncDispatcher &) = delete;
    SmemGroupCommandAsyncDispatcher &operator=(const SmemGroupCommandAsyncDispatcher &) = delete;

    bool Start() noexcept;
    void Stop() noexcept;

    void SetLocalRankId(uint32_t rankId) noexcept
    {
        localRankId_ = rankId;
    }

    void SetSendAckBatchFunc(SendAckBatchFunc func) noexcept
    {
        sendAckBatch_ = std::move(func);
    }

    void SetWhitelistCallback(WhitelistCallback cb) noexcept
    {
        onAddToWhitelist_ = std::move(cb);
    }

    void SetRemoveWhitelistCallback(WhitelistCallback cb) noexcept
    {
        onRemoveFromWhitelist_ = std::move(cb);
    }

    void SetConnectionCallback(ConnectionCallback cb) noexcept
    {
        onEstablishConnection_ = std::move(cb);
    }

    void SetCloseConnectionCallback(CloseConnectionCallback cb) noexcept
    {
        onCloseConnection_ = std::move(cb);
    }

    void SetLinkStateQueryCallback(LinkStateQueryCallback cb) noexcept
    {
        onQueryLinkState_ = std::move(cb);
    }

    void SetLeaveNotifyCallback(LeaveNotifyCallback cb) noexcept
    {
        onLeaveNotify_ = std::move(cb);
    }

    void SetAddSlicesCallback(AddSlicesCallback cb) noexcept
    {
        onAddSlices_ = std::move(cb);
    }

    void EnqueueAddToWhitelist(uint32_t rankId, std::vector<RankFullInfo> &&others, uint64_t reqId) noexcept;
    void EnqueueRemoveFromWhitelist(uint32_t rankId, std::vector<RankFullInfo> &&others, uint64_t reqId) noexcept;
    void EnqueueEstablishConnection(uint32_t rankId, std::vector<RankFullInfo> &&peers, uint64_t reqId) noexcept;
    void EnqueueCloseConnection(uint32_t rankId, std::vector<uint32_t> &&peers, uint64_t reqId) noexcept;
    void EnqueueCloseConnection(uint32_t rankId, std::vector<RankFullInfo> &&peers, uint64_t reqId) noexcept;
    void EnqueueLeaveNotify(uint32_t leavingRankId) noexcept;
    void EnqueueQueryLinkState(uint64_t reqId) noexcept;
    void EnqueueAddSlices(uint32_t extendingRankId, MultiBytes &&newSlices, uint64_t reqId) noexcept;

    uint32_t GetLocalRankId() const noexcept
    {
        return localRankId_;
    }

private:
    struct WhitelistRequest {
        uint32_t rankId;
        std::vector<RankFullInfo> others;
        uint64_t reqId;
    };

    struct ConnEstablishRequest {
        uint32_t rankId;
        std::vector<RankFullInfo> peers;
        uint64_t reqId;
    };

    struct ConnCloseRequest {
        uint32_t rankId;
        std::vector<uint32_t> peers;
        uint64_t reqId;
    };

    struct AddSlicesRequest {
        uint32_t extendingRankId;
        MultiBytes newSlices;
        uint64_t reqId;
    };

    // Jobs drained from the incoming queues in one background-loop iteration.
    struct DrainedJobs {
        std::vector<WhitelistRequest> addJobs;
        std::vector<WhitelistRequest> rmvJobs;
        std::vector<ConnEstablishRequest> connJobs;
        std::vector<ConnCloseRequest> disconnJobs;
        std::vector<uint32_t> leaveJobs;
        std::vector<AddSlicesRequest> slicesJobs;
        uint64_t queryReqId = 0;
        bool doQuery = false;
    };

    void BackgroundThread() noexcept;
    void DrainQueues(DrainedJobs &jobs) noexcept;
    void ProcessDrainedJobs(const DrainedJobs &jobs) noexcept;
    void BackgroundAddWhiteList(const WhitelistRequest &req, uint64_t reqId) noexcept;
    void BackgroundRmvWhiteList(const WhitelistRequest &req, uint64_t reqId) noexcept;
    void BackgroundEstablishConnection(const ConnEstablishRequest &req, uint64_t reqId) noexcept;
    void BackgroundCloseConnection(const ConnCloseRequest &req, uint64_t reqId) noexcept;
    void BackgroundLeaveNotify(uint32_t leavingRankId) noexcept;
    void BackgroundQueryLinkState(uint64_t reqId) noexcept;
    void BackgroundAddSlices(const AddSlicesRequest &req, uint64_t reqId) noexcept;

    void SendAck(ControlOp ackOp, const std::vector<uint32_t> &targetRankIds, const std::vector<int32_t> &results,
                 uint64_t reqId) noexcept;

    SmemGroupManager *groupManager_;
    uint32_t localRankId_{0};
    std::atomic<bool> started_{false};
    std::thread backgroundThread_;
    std::mutex mutex_;
    std::condition_variable cond_;

    std::vector<WhitelistRequest> addWhitelistQueue_;
    std::vector<WhitelistRequest> removeWhitelistQueue_;
    std::vector<ConnEstablishRequest> connectionQueue_;
    std::vector<ConnCloseRequest> disconnectionQueue_;
    std::vector<uint32_t> leaveNotifyQueue_;
    std::vector<AddSlicesRequest> addSlicesQueue_;

    uint64_t queryLinkStateReqId_{0};
    bool hasQueryLinkState_{false};

    WhitelistCallback onAddToWhitelist_;
    WhitelistCallback onRemoveFromWhitelist_;
    ConnectionCallback onEstablishConnection_;
    CloseConnectionCallback onCloseConnection_;
    LinkStateQueryCallback onQueryLinkState_;
    LeaveNotifyCallback onLeaveNotify_;
    AddSlicesCallback onAddSlices_;
    SendAckBatchFunc sendAckBatch_;
};

} // namespace ock::smem

#endif // MEMFABRIC_HYBRID_SMEM_NET_ASYNC_GROUP_MANAGER_H
