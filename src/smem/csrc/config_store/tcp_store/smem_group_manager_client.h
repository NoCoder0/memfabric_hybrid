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

#ifndef SMEM_GROUP_MANAGER_CLIENT_H
#define SMEM_GROUP_MANAGER_CLIENT_H

#include <functional>
#include <vector>

#include "smem_group_manager_def.h"
#include "smem_ref.h"

namespace ock {
namespace smem {

/**
 * @brief Client-side group manager that executes concrete actions
 *        when receiving CONTROL commands from the server.
 *
 * Each method corresponds to a server-pushed CONTROL message.
 * The default implementation logs the call and returns success (0).
 * Override or inject callbacks to perform actual whitelist/connection operations.
 *
 * A LinkStateQueryCallback can be set so that the client can respond to
 * QueryLinkState requests from the server with its current link states.
 */
class SmemGroupManagerClient : public SmReferable {
public:
    using WhitelistCallback =
        std::function<int(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId)>;
    using ConnectionCallback =
        std::function<int(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId)>;
    using CloseConnectionCallback =
        std::function<int(uint32_t rankId, const std::vector<uint32_t> &others, uint64_t reqId)>;
    using LinkStateQueryCallback = std::function<std::vector<LinkStateEntry>()>;
    using LeaveNotifyCallback = std::function<int(uint32_t leavingRankId)>;
    using PromoteToActiveCallback = std::function<int(uint32_t rankId)>;
    using AddSlicesCallback = std::function<int(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId)>;

    SmemGroupManagerClient() = default;
    ~SmemGroupManagerClient() noexcept = default;

    SmemGroupManagerClient(const SmemGroupManagerClient &) = delete;
    SmemGroupManagerClient &operator=(const SmemGroupManagerClient &) = delete;

    int AddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId);
    int RemoveFromWhitelist(uint32_t rankId, const std::vector<RankBaseInfo> &others, uint64_t reqId);
    int EstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId);
    int CloseConnection(uint32_t rankId, const std::vector<uint32_t> &others, uint64_t reqId);
    int PromoteToActive(uint32_t rankId);
    int QueryLinkState(uint32_t rankId);
    int LeaveNotify(uint32_t targetRankId, uint32_t leavingRankId);
    int AddSlices(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId);

    const std::vector<LinkStateEntry> &GetLastQueryLinkStateEntries() const
    {
        return lastQueryLinkStateEntries_;
    }

    /**
     * @brief Returns the result codes from the most recent callback invocation.
     *
     * Each callback (AddToWhitelist, RemoveFromWhitelist, EstablishConnection,
     * CloseConnection) initialises lastAckRes_ with zeroes before calling the
     * injected function. The caller may inspect this after HandleControlMessage
     * to determine per-target success/failure.
     */
    const std::vector<int> &GetLastAckResults() const
    {
        return lastAckRes_;
    }

    WhitelistCallback onAddToWhitelist_;
    WhitelistCallback onRemoveFromWhitelist_;
    ConnectionCallback onEstablishConnection_;
    CloseConnectionCallback onCloseConnection_;
    LinkStateQueryCallback onQueryLinkState_;
    LeaveNotifyCallback onLeaveNotify_;
    PromoteToActiveCallback onPromoteToActive_;
    AddSlicesCallback onAddSlices_;

private:
    std::vector<LinkStateEntry> lastQueryLinkStateEntries_; //< Cached entries from the most recent QueryLinkState call
    std::vector<int> lastAckRes_; //< Per-target result codes from the most recent callback invocation
};

using SmemGroupManagerClientPtr = SmRef<SmemGroupManagerClient>;

} // namespace smem
} // namespace ock

#endif // SMEM_GROUP_MANAGER_CLIENT_H
