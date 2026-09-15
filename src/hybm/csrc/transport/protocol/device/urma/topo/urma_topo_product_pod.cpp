/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "urma_topo_product_pod.h"

#include <cstdint>
#include <string>

#include "hybm_def.h"
#include "hybm_logger.h"
#include "urma_topo_common.h"
#include "urma_topo_def.h"

namespace ock {
namespace mf {
namespace transport {

namespace {
constexpr int32_t POD_MESH_NET_LAYER = 0;
constexpr int32_t POD_CLOS_NET_LAYER = 1;
constexpr int32_t NPU_NUM = 8;
constexpr int32_t PRIMARY_PORT_THRESHOLD = 6;

struct UrmaEntityRule {
    uint32_t mainboardId;
    int32_t level;
    int32_t dieId;
    int32_t ueId;
    std::vector<int32_t> ports;
    std::vector<int32_t> npus;
    std::string planeId;
};

const std::vector<UrmaEntityRule> &GetPodClosRules()
{
    static const std::vector<UrmaEntityRule> rules = {
        {MAIN_BOARD_ID_POD, POD_CLOS_NET_LAYER, 1, 2, {0, 1, 2, 3, 5, 6}, {0, 1, 2, 3}, "plane_pg_0"},
        {MAIN_BOARD_ID_POD, POD_CLOS_NET_LAYER, 0, 2, {1, 2}, {0, 1, 2, 3}, "plane_pg_1"},
        {MAIN_BOARD_ID_POD, POD_CLOS_NET_LAYER, 0, 2, {0, 1, 2, 3, 4, 5}, {4, 5, 6, 7}, "plane_pg_0"},
        {MAIN_BOARD_ID_POD, POD_CLOS_NET_LAYER, 1, 2, {1, 2}, {4, 5, 6, 7}, "plane_pg_1"},
        {MAIN_BOARD_ID_POD_2D, POD_CLOS_NET_LAYER, 1, 2, {0, 1, 2, 3, 5, 6}, {0, 1, 2, 3}, "plane_pg_0"},
        {MAIN_BOARD_ID_POD_2D, POD_CLOS_NET_LAYER, 0, 2, {1, 2}, {0, 1, 2, 3}, "plane_pg_1"},
        {MAIN_BOARD_ID_POD_2D, POD_CLOS_NET_LAYER, 0, 2, {0, 1, 2, 3, 4, 5}, {4, 5, 6, 7}, "plane_pg_0"},
        {MAIN_BOARD_ID_POD_2D, POD_CLOS_NET_LAYER, 1, 2, {1, 2}, {4, 5, 6, 7}, "plane_pg_1"},
    };
    return rules;
}

const UrmaEntityRule *FindPodClosRule(int32_t npuId, int32_t level, uint32_t mainboardId, int32_t die, int32_t feId)
{
    for (const auto &rule : GetPodClosRules()) {
        if (rule.level != level || rule.mainboardId != mainboardId || rule.dieId != die || rule.ueId != feId) {
            continue;
        }
        for (int32_t n : rule.npus) {
            if (n == npuId) {
                return &rule;
            }
        }
    }
    return nullptr;
}

Result BuildPodMeshLevel(int32_t npuId, Level &level, const urmaEntityList &ueList, const dcmi_spod_info &spod)
{
    std::string netInstanceId = "sp" + std::to_string(spod.super_pod_id) + "_srv" + std::to_string(spod.server_index);
    level.netLayer = POD_MESH_NET_LAYER;
    level.netInstanceId = netInstanceId;
    level.netType = NET_TYPE_MESH;

    int32_t meshDieId = (npuId % NPU_NUM < 4) ? 0 : 1;
    int32_t meshEntityId = UBGetMaxEntityId(ueList, meshDieId);
    for (unsigned int i = 0; i < ueList.ueNum; ++i) {
        int32_t fe = UrmaEntityGetId(ueList.ueList[i]);
        int32_t dieId = UrmaEntityGetDieId(ueList.ueList[i]);
        if (fe != meshEntityId || dieId != meshDieId) {
            continue;
        }
        for (unsigned int j = 0; j < ueList.ueList[i].eidNum; ++j) {
            int32_t portId = UrmaEidGetPortId(ueList.ueList[i].eidList[j].eid);
            if (portId > MAX_MESH_PORT_ID) {
                continue;
            }
            RankAddr addr;
            addr.addrType = "EID";
            EidToHexStr(ueList.ueList[i].eidList[j].eid, addr.addr);
            addr.ports.push_back(std::to_string(dieId) + "/" + std::to_string(portId));
            addr.planeId = "plane_0";
            level.rankAddrList.push_back(std::move(addr));
        }
    }
    return BM_OK;
}

Result BuildPodClosLevel(int32_t npuId, uint32_t mainboardId, Level &level, const urmaEntityList &ueList,
                         const dcmi_spod_info &spod)
{
    std::string netInstanceId = "superpod_" + std::to_string(spod.super_pod_id);
    level.netLayer = POD_CLOS_NET_LAYER;
    level.netInstanceId = netInstanceId;
    level.netType = NET_TYPE_CLOS;

    int32_t localNpuId = npuId % NPU_NUM;
    for (unsigned int i = 0; i < ueList.ueNum; ++i) {
        int32_t fe = UrmaEntityGetId(ueList.ueList[i]);
        int32_t portGroupIdx = UrmaEntityGetPortGroupIdx(ueList.ueList[i]);
        if (portGroupIdx < 0) {
            continue;
        }
        int32_t die = UrmaEidGetDieId(ueList.ueList[i].eidList[portGroupIdx].eid);
        const UrmaEntityRule *rule = FindPodClosRule(localNpuId, POD_CLOS_NET_LAYER, mainboardId, die, fe);
        if (rule == nullptr) {
            continue;
        }
        RankAddr addr;
        addr.addrType = "EID";
        EidToHexStr(ueList.ueList[i].eidList[portGroupIdx].eid, addr.addr);

        int32_t portNum = 0;
        for (unsigned int j = 0; j < ueList.ueList[i].eidNum; ++j) {
            if (UrmaEidIsPortGroup(ueList.ueList[i].eidList[j].eid)) {
                continue;
            }
            int32_t portId = UrmaEidGetPortId(ueList.ueList[i].eidList[j].eid);
            addr.ports.push_back(std::to_string(die) + "/" + std::to_string(portId));
            ++portNum;
        }
        if (portNum >= PRIMARY_PORT_THRESHOLD) {
            addr.planeId = "plane_0";
            level.rankAddrList.insert(level.rankAddrList.begin(), std::move(addr));
        } else {
            addr.planeId = "plane_1";
            auto secondaryPos = level.rankAddrList.begin();
            if (secondaryPos != level.rankAddrList.end()) {
                ++secondaryPos;
            }
            level.rankAddrList.insert(secondaryPos, std::move(addr));
        }
    }
    return BM_OK;
}

} // anonymous namespace

Result UrmaTopoProductPod::GetLocalId(int32_t &localId)
{
    dcmi_spod_info spod{};
    if (GetSpodInfo(logicId_, spod) != BM_OK) {
        BM_LOG_ERROR("GetRootInfo: GetSpodInfo failed, logicId=" << logicId_ << " phyId=" << phyId_);
        return BM_ERROR;
    }

    localId = (phyId_ % NPU_NUM) + static_cast<int32_t>((spod.server_index % NPU_NUM) * NPU_NUM);
    return BM_OK;
}

Result UrmaTopoProductPod::GetRootInfo(RootInfo &rootInfo)
{
    rootInfo = {};
    rootInfo.version = "2.0";

    auto ret = GetTopoFilePath(mainboardId_, TOPO_TYPE_IGNORE, rootInfo.topoFilePath);
    if (ret != BM_OK) {
        BM_LOG_ERROR("GetRootInfo: GetTopoFilePath failed, mainboardId=" << mainboardId_ << " phyId=" << phyId_);
        return ret;
    }

    dcmi_spod_info spod{};
    if (GetSpodInfo(logicId_, spod) != BM_OK) {
        BM_LOG_ERROR("GetRootInfo: GetSpodInfo failed, logicId=" << logicId_ << " phyId=" << phyId_);
        return BM_ERROR;
    }

    urmaEntityList ueList{};
    if (GetUrmaEntityList(logicId_, ueList) != BM_OK) {
        BM_LOG_WARN("GetRootInfo: GetUrmaEntityList failed, logicId=" << logicId_ << " phyId=" << phyId_);
    }

    int32_t localId = (phyId_ % NPU_NUM) + static_cast<int32_t>((spod.server_index % NPU_NUM) * NPU_NUM);
    Rank rank;
    rank.deviceId = phyId_;
    rank.localId = localId;

    Level meshLevel;
    if (BuildPodMeshLevel(phyId_, meshLevel, ueList, spod) == BM_OK) {
        rank.levelList.push_back(std::move(meshLevel));
    }

    Level closLevel;
    if (BuildPodClosLevel(phyId_, mainboardId_, closLevel, ueList, spod) == BM_OK) {
        rank.levelList.push_back(std::move(closLevel));
    }

    Level roceLevel;
    if (ProcessLayerRoce(static_cast<uint32_t>(phyId_), roceLevel) == BM_OK) {
        rank.levelList.push_back(std::move(roceLevel));
    }

    rootInfo.rankList.push_back(std::move(rank));
    rootInfo.rankCount = static_cast<int>(rootInfo.rankList.size());
    return BM_OK;
}

} // namespace transport
} // namespace mf
} // namespace ock
