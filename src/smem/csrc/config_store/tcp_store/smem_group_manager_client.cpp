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

#include "smem_group_manager_client.h"
#include "smem_config_store_logger.h"

namespace ock {
namespace smem {

int SmemGroupManagerClient::AddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId)
{
    lastAckRes_.assign(others.size(), 0);
    if (onAddToWhitelist_) {
        return onAddToWhitelist_(rankId, others, reqId);
    }
    STORE_LOG_DEBUG("[GM][Client][Recv] AddToWhitelist rankId: " << rankId << " count: " << others.size()
                                                                 << " reqId: " << reqId);
    return 0;
}

int SmemGroupManagerClient::RemoveFromWhitelist(uint32_t rankId, const std::vector<RankBaseInfo> &others,
                                                uint64_t reqId)
{
    lastAckRes_.assign(others.size(), 0);
    if (onRemoveFromWhitelist_) {
        std::vector<RankFullInfo> fullInfos;
        fullInfos.reserve(others.size());
        for (const auto &base : others) {
            RankFullInfo full(base.rankId);
            full.baseInfo = base.baseInfo;
            fullInfos.push_back(full);
        }
        return onRemoveFromWhitelist_(rankId, fullInfos, reqId);
    }
    STORE_LOG_DEBUG("[GM][Client][Recv] RemoveFromWhitelist rankId: " << rankId << " count: " << others.size()
                                                                      << " reqId: " << reqId);
    return 0;
}

int SmemGroupManagerClient::EstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &others,
                                                uint64_t reqId)
{
    lastAckRes_.assign(others.size(), 0);
    if (onEstablishConnection_) {
        return onEstablishConnection_(rankId, others, reqId);
    }
    STORE_LOG_DEBUG("[GM][Client][Recv] EstablishConnection rankId: " << rankId << " count: " << others.size()
                                                                      << " reqId: " << reqId);
    return 0;
}

int SmemGroupManagerClient::CloseConnection(uint32_t rankId, const std::vector<uint32_t> &others, uint64_t reqId)
{
    lastAckRes_.assign(others.size(), 0);
    if (onCloseConnection_) {
        return onCloseConnection_(rankId, others, reqId);
    }
    STORE_LOG_INFO("[GM][Client][Recv] CloseConnection rankId: " << rankId << " count: " << others.size()
                                                                 << " reqId: " << reqId);
    return 0;
}

int SmemGroupManagerClient::PromoteToActive(uint32_t rankId)
{
    if (onPromoteToActive_) {
        return onPromoteToActive_(rankId);
    }
    STORE_LOG_INFO("[GM][Client][Recv] PromoteToActive rankId=" << rankId);
    return 0;
}

int SmemGroupManagerClient::QueryLinkState(uint32_t rankId)
{
    if (onQueryLinkState_) {
        lastQueryLinkStateEntries_ = onQueryLinkState_();
        return 0;
    }
    STORE_LOG_INFO("[GM][Client][Recv] QueryLinkState rankId: " << rankId);
    return 0;
}

int SmemGroupManagerClient::LeaveNotify(uint32_t targetRankId, uint32_t leavingRankId)
{
    (void)targetRankId;
    if (onLeaveNotify_) {
        return onLeaveNotify_(leavingRankId);
    }
    STORE_LOG_INFO("[GM][Client][Recv] LeaveNotify leavingRankId: " << leavingRankId);
    return 0;
}

int SmemGroupManagerClient::AddSlices(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId)
{
    if (onAddSlices_) {
        return onAddSlices_(extendingRankId, newSlices, reqId);
    }
    STORE_LOG_INFO("[GM][Client][Recv] AddSlices extendingRankId: " << extendingRankId
                                                                    << " sliceCount: " << newSlices.size());
    return 0;
}

} // namespace smem
} // namespace ock
