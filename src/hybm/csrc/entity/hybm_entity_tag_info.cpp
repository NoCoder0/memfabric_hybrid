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
#include "hybm_entity_tag_info.h"

#include <algorithm>
#include <array>

namespace ock {
namespace mf {

static const std::map<std::string, hybm_data_op_type> str2OpTypeMap = {
    {"DEVICE_SDMA", HYBM_DOP_TYPE_SDMA},        {"device_sdma", HYBM_DOP_TYPE_SDMA},
    {"DEVICE_RDMA", HYBM_DOP_TYPE_DEVICE_RDMA}, {"device_rdma", HYBM_DOP_TYPE_DEVICE_RDMA},
    {"DEVICE_URMA", HYBM_DOP_TYPE_DEVICE_URMA}, {"device_urma", HYBM_DOP_TYPE_DEVICE_URMA},
    {"HOST_RDMA", HYBM_DOP_TYPE_HOST_RDMA},     {"host_rdma", HYBM_DOP_TYPE_HOST_RDMA},
    {"HOST_TCP", HYBM_DOP_TYPE_HOST_TCP},       {"host_tcp", HYBM_DOP_TYPE_HOST_TCP},
    {"HOST_URMA", HYBM_DOP_TYPE_HOST_URMA},     {"host_urma", HYBM_DOP_TYPE_HOST_URMA},
    {"HOST_SHM", HYBM_DOP_TYPE_HOST_SHM},       {"host_shm", HYBM_DOP_TYPE_HOST_SHM},
    {"DEVICE_MTE", HYBM_DOP_TYPE_MTE},          {"device_mte", HYBM_DOP_TYPE_MTE},
    {"AIV_SDMA", HYBM_DOP_TYPE_AIV_SDMA},       {"aiv_sdma", HYBM_DOP_TYPE_AIV_SDMA},
};

static std::vector<std::string> SplitLayerProtocols(const std::string &protocol)
{
    std::vector<std::string> segments;
    size_t start = 0;
    while (true) {
        const size_t end = protocol.find(':', start);
        segments.emplace_back(protocol, start, end == std::string::npos ? std::string::npos : end - start);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return segments;
}

Result HybmEntityTagInfo::TagInfoInit(const hybm_options &option)
{
    const std::string protocol = option.tagOpInfo;
    if (protocol.empty()) {
        legacyOpType_ = static_cast<uint32_t>(option.bmDataOpType);
        BM_LOG_WARN("Protocol layer config is empty, fall back to legacy op selection, rankId:" << option.rankId);
        return BM_OK;
    }
    std::vector<hybm_data_op_type> layers;
    auto layerRet = ParseLayerProtocol(protocol, layers);
    if (layerRet != BM_OK) {
        return layerRet;
    }
    return AddRankLayers(option.rankId, layers);
}

Result HybmEntityTagInfo::ParseLayerProtocol(const std::string &protocol, std::vector<hybm_data_op_type> &parsed)
{
    const auto segments = SplitLayerProtocols(protocol);
    parsed.reserve(segments.size());
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].empty()) {
            BM_LOG_DEBUG("Empty protocol layer, index:" << i << " protocol:" << protocol);
            parsed.push_back(HYBM_DOP_TYPE_DEFAULT);
            continue;
        }
        hybm_data_op_type opType;
        auto it = str2OpTypeMap.find(segments[i]);
        if (it == str2OpTypeMap.end()) {
            BM_LOG_ERROR("Invalid protocol layer, index:" << i << " layer:" << segments[i] << " protocol:" << protocol);
            return BM_INVALID_PARAM;
        }
        opType = it->second;
        parsed.push_back(opType);
    }
    const bool hasEnabledLayer = std::any_of(parsed.begin(), parsed.end(),
                                             [](hybm_data_op_type opType) { return opType != HYBM_DOP_TYPE_DEFAULT; });
    if (!hasEnabledLayer) {
        BM_LOG_ERROR("Protocol layer config has no enabled layer, protocol:" << protocol);
        parsed.clear();
        return BM_INVALID_PARAM;
    }
    BM_LOG_DEBUG("Success to parse protocol layers:" << protocol << " layerCount:" << parsed.size());
    return BM_OK;
}

Result HybmEntityTagInfo::RemoveRankInfo(uint32_t rankId)
{
    std::unique_lock lock(mutex_);
    rankLayers_.erase(rankId);
    return BM_OK;
}

bool HybmEntityTagInfo::HasLayerProtocols(uint32_t rankId) const
{
    std::shared_lock lock(mutex_);
    auto it = rankLayers_.find(rankId);
    return it != rankLayers_.end() && !it->second.empty();
}

std::vector<hybm_data_op_type> HybmEntityTagInfo::GetLayersForRank(uint32_t rankId) const
{
    std::shared_lock lock(mutex_);
    auto it = rankLayers_.find(rankId);
    if (it == rankLayers_.end()) {
        return {};
    }
    return it->second;
}

Result HybmEntityTagInfo::AddRankLayers(uint32_t rankId, const std::vector<hybm_data_op_type> &layers)
{
    std::unique_lock lock(mutex_);
    for (const auto &item : rankLayers_) {
        if (item.first != rankId && !IsLayerLayoutCompatible(item.second, layers)) {
            BM_LOG_ERROR("Layer template mismatch, existing rank:" << item.first << " new rank:" << rankId
                                                                   << " existingCount:" << item.second.size()
                                                                   << " newCount:" << layers.size());
            return BM_INVALID_PARAM;
        }
    }
    rankLayers_[rankId] = layers;
    BM_LOG_DEBUG("Success to add rank layers, rankId:" << rankId << " layerCount:" << layers.size());
    return BM_OK;
}

uint32_t HybmEntityTagInfo::GetRank2RankOpType(uint32_t rankId1, uint32_t rankId2)
{
    const auto matched = GetRank2RankLayers(rankId1, rankId2);
    uint32_t ops = 0;
    if (!matched.empty()) {
        for (auto layer : matched) {
            ops |= static_cast<uint32_t>(layer);
        }
        BM_LOG_DEBUG("Layer prefix ops:" << ops << " rank1:" << rankId1 << " rank2:" << rankId2);
    } else {
        ops = legacyOpType_;
    }
    return ops;
}

std::vector<hybm_data_op_type> HybmEntityTagInfo::GetRank2RankLayers(uint32_t rankId1, uint32_t rankId2) const
{
    const auto layers1 = GetLayersForRank(rankId1);
    const auto layers2 = GetLayersForRank(rankId2);
    std::vector<hybm_data_op_type> matched;
    if (layers1.empty() || layers2.empty() || layers1.size() != layers2.size()) {
        return matched;
    }
    matched.reserve(layers1.size());
    for (size_t i = 0; i < layers1.size(); ++i) {
        if (layers1[i] == HYBM_DOP_TYPE_DEFAULT || layers2[i] == HYBM_DOP_TYPE_DEFAULT || layers1[i] != layers2[i]) {
            continue;
        }
        matched.push_back(layers1[i]);
    }
    return matched;
}

void HybmEntityTagInfo::SetRankTopology(uint32_t rankId, uint32_t superPodId, uint32_t serverId, int32_t devicePhyId)
{
    std::unique_lock lock(mutex_);
    auto &topology = rankTopologies_[rankId];
    topology.superPodId = superPodId;
    topology.serverId = serverId;
    topology.devicePhyId = devicePhyId;
    topology.valid = superPodId != 0xFFFFFFFFU && serverId != 0x3FFU;
}

bool HybmEntityTagInfo::GetRankTopology(uint32_t rankId, RankTopology &topology) const
{
    std::shared_lock lock(mutex_);
    const auto it = rankTopologies_.find(rankId);
    if (it == rankTopologies_.end()) {
        return false;
    }
    topology = it->second;
    return true;
}

bool HybmEntityTagInfo::IsSameNode(uint32_t rankId1, uint32_t rankId2) const
{
    if (rankId1 == rankId2) {
        return true;
    }

    std::shared_lock lock(mutex_);
    const auto left = rankTopologies_.find(rankId1);
    const auto right = rankTopologies_.find(rankId2);
    if (left == rankTopologies_.end() || right == rankTopologies_.end() || !left->second.valid ||
        !right->second.valid) {
        return false;
    }
    return left->second.superPodId == right->second.superPodId && left->second.serverId == right->second.serverId;
}

bool HybmEntityTagInfo::IsSameSuperPod(uint32_t localRank, uint32_t remoteRank) const
{
    if (localRank == remoteRank) {
        return false;
    }
    std::shared_lock lock(mutex_);
    const auto left = rankTopologies_.find(localRank);
    const auto right = rankTopologies_.find(remoteRank);
    if (left == rankTopologies_.end() || right == rankTopologies_.end() || !left->second.valid ||
        !right->second.valid) {
        return false;
    }
    return left->second.superPodId == right->second.superPodId;
}

void HybmEntityTagInfo::GetTopologyPair(uint32_t localRank, uint32_t remoteRank, bool &topologyKnown,
                                        bool &sameNode) const
{
    topologyKnown = false;
    sameNode = false;
    if (localRank == remoteRank) {
        // self-pair 的地址域是平凡的，且该结果不被上层消费，按无效处理走兜底。
        return;
    }
    std::shared_lock lock(mutex_);
    const auto left = rankTopologies_.find(localRank);
    const auto right = rankTopologies_.find(remoteRank);
    topologyKnown =
        left != rankTopologies_.end() && right != rankTopologies_.end() && left->second.valid && right->second.valid;
    sameNode = topologyKnown && left->second.superPodId == right->second.superPodId &&
               left->second.serverId == right->second.serverId;
}

std::vector<hybm_data_op_type> HybmEntityTagInfo::ResolvePeerProtocols(const PeerProtocolInfo &peerInfo) const
{
    static constexpr std::array<hybm_data_op_type, 2> kSameNodeProtocols = {
        HYBM_DOP_TYPE_SDMA,
        HYBM_DOP_TYPE_HOST_SHM,
    };
    static constexpr std::array<hybm_data_op_type, 7> kFallbackProtocols = {
        HYBM_DOP_TYPE_SDMA,      HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_DEVICE_URMA, HYBM_DOP_TYPE_HOST_RDMA,
        HYBM_DOP_TYPE_HOST_URMA, HYBM_DOP_TYPE_HOST_TCP,    HYBM_DOP_TYPE_HOST_SHM,
    };
    static constexpr std::array<hybm_data_op_type, 5> kRemoteNodeProtocols = {
        HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_DEVICE_URMA, HYBM_DOP_TYPE_HOST_RDMA,
        HYBM_DOP_TYPE_HOST_URMA,   HYBM_DOP_TYPE_HOST_TCP,
    };

    const auto matched = GetRank2RankLayers(peerInfo.localRank, peerInfo.remoteRank);
    std::vector<hybm_data_op_type> protocols;
    if (matched.empty()) {
        // 对端未声明分层协议（A3 独立部署不配置 tagOpInfo、老版本对端）时不做同机/远端裁剪，
        // 与历史语义一致：直接用 legacy op 全集，可达性由运行期 CanSdmaReaches / 段映射判定。
        for (auto layer : kFallbackProtocols) {
            if ((legacyOpType_ & static_cast<uint32_t>(layer)) != 0U) {
                protocols.emplace_back(layer);
            }
        }
        return protocols;
    }

    bool topologyKnown = false;
    bool sameNode = false;
    GetTopologyPair(peerInfo.localRank, peerInfo.remoteRank, topologyKnown, sameNode);
    const bool sameSuperPod = IsSameSuperPod(peerInfo.localRank, peerInfo.remoteRank);

    for (auto layer : matched) {
        const bool inSameNodeProtocols = std::any_of(kSameNodeProtocols.begin(), kSameNodeProtocols.end(),
                                                     [layer](auto protocol) { return protocol == layer; });
        const bool inRemoteNodeProtocols = std::any_of(kRemoteNodeProtocols.begin(), kRemoteNodeProtocols.end(),
                                                       [layer](auto protocol) { return protocol == layer; });
        // SDMA/AIV_SDMA/MTE 走的是 SDMA 引擎，可达域由 MemSegment::CanSdmaReaches 决定：同 server 或同
        // superPod。跨 server 但同 superPod 时不能按“同机层”提前滤掉，否则超节点内跨机 SDMA 会退化成
        // 无算子可用；跨 superPod 时仍按不可达处理。
        const bool sdmaEngineLayer =
            layer == HYBM_DOP_TYPE_SDMA || layer == HYBM_DOP_TYPE_AIV_SDMA || layer == HYBM_DOP_TYPE_MTE;
        if (topologyKnown && sameNode && !inSameNodeProtocols && !sdmaEngineLayer) {
            continue;
        }
        if (topologyKnown && !sameNode && !inRemoteNodeProtocols && !(sdmaEngineLayer && sameSuperPod)) {
            continue;
        }
        if (std::find(protocols.begin(), protocols.end(), layer) == protocols.end()) {
            protocols.emplace_back(layer);
        }
    }
    return protocols;
}

bool HybmEntityTagInfo::IsLayerLayoutCompatible(const std::vector<hybm_data_op_type> &left,
                                                const std::vector<hybm_data_op_type> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    return true;
}

std::string HybmEntityTagInfo::GetOpTypeStr(hybm_data_op_type opType)
{
    static const std::map<hybm_data_op_type, std::string> opType2StrMap = {
        {HYBM_DOP_TYPE_SDMA, "DEVICE_SDMA"},        {HYBM_DOP_TYPE_DEVICE_RDMA, "DEVICE_RDMA"},
        {HYBM_DOP_TYPE_DEVICE_URMA, "DEVICE_URMA"}, {HYBM_DOP_TYPE_HOST_RDMA, "HOST_RDMA"},
        {HYBM_DOP_TYPE_HOST_TCP, "HOST_TCP"},       {HYBM_DOP_TYPE_HOST_URMA, "HOST_URMA"},
        {HYBM_DOP_TYPE_HOST_SHM, "HOST_SHM"},       {HYBM_DOP_TYPE_MTE, "DEVICE_MTE"},
        {HYBM_DOP_TYPE_AIV_SDMA, "AIV_SDMA"},
    };
    auto it = opType2StrMap.find(opType);
    if (it != opType2StrMap.end()) {
        return it->second;
    }
    return "OP_TYPE_BUTT";
}

} // namespace mf
} // namespace ock
