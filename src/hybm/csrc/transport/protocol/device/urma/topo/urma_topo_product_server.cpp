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

#include "urma_topo_product_server.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hybm_def.h"
#include "hybm_logger.h"
#include "urma_topo_common.h"
#include "urma_topo_def.h"

namespace ock {
namespace mf {
namespace transport {

namespace {
constexpr int32_t MESH_ENTITY_WILDCARD_ID = 99;
constexpr int32_t SERVER_NPU_NUM = 8;
constexpr int32_t NPU_NUM_PER_BOARD = 8;
constexpr int32_t LINK_LOCAL_IP_PREFIX_LEN = 3;

const char *UBOE_CLUSTER_PLANE_ID = "plane_uboe";
const char *UB_RTP_CLUSTER_PLANE_ID = "plane_ub_rtp";
const char *LINK_LOCAL_IP_PREFIX = "254";

enum class UrmaEntityType : int32_t {
    MESH = 0,
    CLOS = 1,
    UBOE = 2,
    UB_RTP = 3,
    CLOS_PORTS = 4,
};

enum class InstanceIdType : int32_t {
    OS,
    POD,
    POD_FLEX,
    SUPER_POD,
    CLUSTER,
};

struct UEInfo {
    int32_t dieId;
    int32_t feId;
    UrmaEntityType type;
    std::string ports;
};

struct LevelInfo {
    int32_t level;
    std::string netType;
    std::vector<UEInfo> ueList;
    InstanceIdType instanceIdType;
};

struct NetInfo {
    std::vector<uint32_t> mainBoardIds;
    uint32_t spodType;
    std::vector<LevelInfo> levelInfos;
};

void GetNetInstanceId(InstanceIdType type, int32_t npuId, const dcmi_spod_info &spod, std::string &out)
{
    switch (type) {
        case InstanceIdType::OS:
            GetServerId(out);
            break;
        case InstanceIdType::POD:
            out = "sp_" + std::to_string(spod.super_pod_id) + "_srv_" + std::to_string(spod.server_index);
            break;
        case InstanceIdType::POD_FLEX:
            out = "sp_" + std::to_string(spod.super_pod_id) + "_srv_" + std::to_string(spod.server_index) + "_board" +
                  std::to_string(npuId / NPU_NUM_PER_BOARD);
            break;
        case InstanceIdType::SUPER_POD:
            out = "sp_" + std::to_string(spod.super_pod_id);
            break;
        case InstanceIdType::CLUSTER:
            out = "cluster";
            break;
    }
}

const std::vector<NetInfo> &GetServerTopologyRules()
{
    static const std::vector<NetInfo> list = {
        {
            {
                MAIN_BOARD_ID_SERVER_350L,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0,
                 NET_TYPE_TOPO_FILE_DESC,
                 {{1, 3, UrmaEntityType::MESH, ""},
                  {1, 2, UrmaEntityType::CLOS_PORTS, ""},
                  {1, 2, UrmaEntityType::CLOS, ""}},
                 InstanceIdType::OS},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_SERVER_8PMESH,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0,
                 NET_TYPE_TOPO_FILE_DESC,
                 {{1, MESH_ENTITY_WILDCARD_ID, UrmaEntityType::MESH, ""}},
                 InstanceIdType::POD},
                {1, NET_TYPE_CLOS, {{0, 3, UrmaEntityType::CLOS, ""}}, InstanceIdType::SUPER_POD},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_SERVER_8PMESH_UBOE,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0,
                 NET_TYPE_TOPO_FILE_DESC,
                 {{1, MESH_ENTITY_WILDCARD_ID, UrmaEntityType::MESH, ""}},
                 InstanceIdType::POD},
                {1, NET_TYPE_CLOS, {{0, 3, UrmaEntityType::CLOS, ""}}, InstanceIdType::SUPER_POD},
                {2, NET_TYPE_CLOS, {{0, 0, UrmaEntityType::UBOE, "1/8"}}, InstanceIdType::CLUSTER},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_SERVER_8PMESH_UBOE,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_SERVER_16FM,
            {{
                {0,
                 NET_TYPE_TOPO_FILE_DESC,
                 {{0, MESH_ENTITY_WILDCARD_ID, UrmaEntityType::MESH, ""},
                  {1, MESH_ENTITY_WILDCARD_ID, UrmaEntityType::MESH, ""}},
                 InstanceIdType::POD},
                {2, NET_TYPE_CLOS, {{0, 0, UrmaEntityType::UBOE, "1/8"}}, InstanceIdType::CLUSTER},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_SERVER_8PMESH_NOSP_UBOE,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0,
                 NET_TYPE_TOPO_FILE_DESC,
                 {{1, MESH_ENTITY_WILDCARD_ID, UrmaEntityType::MESH, ""}},
                 InstanceIdType::OS},
                {2, NET_TYPE_CLOS, {{0, 0, UrmaEntityType::UBOE, "1/8"}}, InstanceIdType::CLUSTER},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_SERVER_8PMESH_NOSP,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0,
                 NET_TYPE_TOPO_FILE_DESC,
                 {{1, MESH_ENTITY_WILDCARD_ID, UrmaEntityType::MESH, ""}},
                 InstanceIdType::OS},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_SERVER_TYPE1,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0,
                 NET_TYPE_TOPO_FILE_DESC,
                 {{1, MESH_ENTITY_WILDCARD_ID, UrmaEntityType::MESH, ""}},
                 InstanceIdType::OS},
                {1,
                 NET_TYPE_CLOS,
                 {{0, 3, UrmaEntityType::CLOS, ""}, {1, 2, UrmaEntityType::CLOS, ""}},
                 InstanceIdType::SUPER_POD},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_POD_FLEX,
                MAIN_BOARD_ID_POD_FLEX_RTP,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0,
                 NET_TYPE_TOPO_FILE_DESC,
                 {{0, MESH_ENTITY_WILDCARD_ID, UrmaEntityType::MESH, ""}},
                 InstanceIdType::POD_FLEX},
                {1, NET_TYPE_CLOS, {{1, 2, UrmaEntityType::CLOS, ""}}, InstanceIdType::SUPER_POD},
                {2, NET_TYPE_CLOS, {{0, 0, UrmaEntityType::UB_RTP, "0/8"}}, InstanceIdType::CLUSTER},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_SERVER_550EL_100,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0, NET_TYPE_CLOS, {{1, 2, UrmaEntityType::CLOS, ""}}, InstanceIdType::POD},
                {1, NET_TYPE_CLOS, {{1, 2, UrmaEntityType::CLOS, ""}}, InstanceIdType::SUPER_POD},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
        {
            {
                MAIN_BOARD_ID_SERVER_550EL_200,
                MAIN_BOARD_INVALID,
            },
            TOPO_TYPE_IGNORE,
            {{
                {0,
                 NET_TYPE_CLOS,
                 {{0, 2, UrmaEntityType::CLOS, ""}, {1, 2, UrmaEntityType::CLOS, ""}},
                 InstanceIdType::POD},
                {1,
                 NET_TYPE_CLOS,
                 {{0, 2, UrmaEntityType::CLOS, ""}, {1, 2, UrmaEntityType::CLOS, ""}},
                 InstanceIdType::SUPER_POD},
                {ROCE_NET_LAYER, NET_TYPE_CLOS, {}, InstanceIdType::CLUSTER},
            }},
        },
    };
    return list;
}

const NetInfo *FindTopologyRule(uint32_t mainboardId, uint32_t spodType)
{
    const auto &list = GetServerTopologyRules();
    for (const auto &info : list) {
        bool matchBoard = false;
        for (uint32_t id : info.mainBoardIds) {
            if (id == MAIN_BOARD_INVALID) {
                break;
            }
            if (id == mainboardId) {
                matchBoard = true;
                break;
            }
        }
        if (matchBoard && info.spodType == spodType) {
            return &info;
        }
    }
    for (const auto &info : list) {
        for (uint32_t id : info.mainBoardIds) {
            if (id == MAIN_BOARD_INVALID) {
                break;
            }
            if (id == mainboardId) {
                return &info;
            }
        }
    }
    return nullptr;
}

const urmaEntity *FindUrmaEntityByType(const urmaEntityList &ueList, UrmaEntityType type)
{
    for (unsigned int i = 0; i < ueList.ueNum; ++i) {
        if (ueList.ueList[i].eidNum == 0) {
            continue;
        }
        if (type == UrmaEntityType::UBOE && UrmaEidIsUBOE(ueList.ueList[i].eidList[0].eid)) {
            return &ueList.ueList[i];
        }
        if (type == UrmaEntityType::UB_RTP && UrmaEidIsUbRtp(ueList.ueList[i].eidList[0].eid)) {
            return &ueList.ueList[i];
        }
    }
    return nullptr;
}

const urmaEntity *FindUrmaEntity(const urmaEntityList &ueList, int32_t dieId, int32_t ueId, UrmaEntityType type)
{
    if (type == UrmaEntityType::UBOE || type == UrmaEntityType::UB_RTP) {
        return FindUrmaEntityByType(ueList, type);
    }
    int32_t maxFe = 0;
    const urmaEntity *maxFeEntity = nullptr;
    for (unsigned int i = 0; i < ueList.ueNum; ++i) {
        if (ueList.ueList[i].eidNum == 0) {
            continue;
        }
        if (UrmaEidIsUBOE(ueList.ueList[i].eidList[0].eid)) {
            continue;
        }
        int32_t die = UrmaEidGetDieId(ueList.ueList[i].eidList[0].eid);
        int32_t fe = UrmaEntityGetId(ueList.ueList[i]);
        if (die == dieId && fe == ueId) {
            return &ueList.ueList[i];
        }
        if (die == dieId && fe > maxFe) {
            maxFe = fe;
            maxFeEntity = &ueList.ueList[i];
        }
    }
    if (ueId == MESH_ENTITY_WILDCARD_ID) {
        return maxFeEntity;
    }
    return nullptr;
}

void AddMeshAddrsToLevel(const urmaEntity &ue, Level &level, UrmaEntityType type)
{
    for (unsigned int j = 0; j < ue.eidNum; ++j) {
        if (UrmaEidIsPortGroup(ue.eidList[j].eid)) {
            continue;
        }
        RankAddr addr;
        addr.addrType = "EID";
        EidToHexStr(ue.eidList[j].eid, addr.addr);
        int32_t portId = UrmaEidGetPortId(ue.eidList[j].eid);
        int32_t dieId = UrmaEidGetDieId(ue.eidList[j].eid);
        addr.ports.push_back(std::to_string(dieId) + "/" + std::to_string(portId));
        if (type == UrmaEntityType::MESH) {
            addr.planeId = "plane_" + std::to_string(dieId);
        } else {
            addr.planeId = "plane_clos_" + std::to_string(dieId) + "_" + std::to_string(portId);
        }
        level.rankAddrList.push_back(std::move(addr));
    }
}

Result AddClosAddrToLevel(const urmaEntity &ue, Level &level)
{
    int32_t portGroupIdx = UrmaEntityGetPortGroupIdx(ue);
    if (portGroupIdx < 0) {
        return BM_ERROR;
    }
    int32_t dieId = UrmaEidGetDieId(ue.eidList[portGroupIdx].eid);
    RankAddr addr;
    addr.addrType = "EID";
    EidToHexStr(ue.eidList[portGroupIdx].eid, addr.addr);
    for (unsigned int i = 0; i < ue.eidNum; ++i) {
        if (UrmaEidIsPortGroup(ue.eidList[i].eid)) {
            continue;
        }
        int32_t portId = UrmaEidGetPortId(ue.eidList[i].eid);
        addr.ports.push_back(std::to_string(dieId) + "/" + std::to_string(portId));
    }
    addr.planeId = "plane_clos_" + std::to_string(dieId);
    level.rankAddrList.push_back(std::move(addr));
    return BM_OK;
}

Result AddUboeAddrToLevel(const urmaEntity &ue, const UEInfo &ueInfo, Level &level)
{
    bool found = false;
    RankAddr addr;
    for (unsigned int i = 0; i < ue.eidNum; ++i) {
        std::string cna;
        UrmaEid2CNA(ue.eidList[i].eid, cna);
        if (strncmp(cna.c_str(), LINK_LOCAL_IP_PREFIX, LINK_LOCAL_IP_PREFIX_LEN) == 0) {
            continue;
        }
        addr.addrType = "IPV4";
        addr.addr = cna;
        addr.ports.push_back(ueInfo.ports);
        found = true;
    }
    if (!found) {
        BM_LOG_ERROR("AddUboeAddrToLevel: no valid UBOE address, eidNum=" << ue.eidNum);
        return BM_ERROR;
    }
    addr.planeId = UBOE_CLUSTER_PLANE_ID;
    level.rankAddrList.push_back(std::move(addr));
    return BM_OK;
}

Result AddUbRtpAddrToLevel(const urmaEntity &ue, const UEInfo &ueInfo, Level &level)
{
    bool found = false;
    RankAddr addr;
    addr.addrType = "EID";
    for (unsigned int i = 0; i < ue.eidNum; ++i) {
        EidToHexStr(ue.eidList[i].eid, addr.addr);
        addr.ports.push_back(ueInfo.ports);
        found = true;
    }
    if (!found) {
        BM_LOG_ERROR("AddUbRtpAddrToLevel: no UB RTP EID, eidNum=" << ue.eidNum);
        return BM_ERROR;
    }
    addr.planeId = UB_RTP_CLUSTER_PLANE_ID;
    level.rankAddrList.push_back(std::move(addr));
    return BM_OK;
}

Result BuildLevel(int32_t npuId, Level &level, const urmaEntityList &ueList, const LevelInfo &levelInfo,
                  const dcmi_spod_info &spod)
{
    if (levelInfo.level == ROCE_NET_LAYER) {
        return ProcessLayerRoce(static_cast<uint32_t>(npuId), level);
    }
    std::string netInstanceId;
    GetNetInstanceId(levelInfo.instanceIdType, npuId, spod, netInstanceId);
    level.netLayer = levelInfo.level;
    level.netInstanceId = netInstanceId;
    level.netType = levelInfo.netType;
    Result result = BM_ERROR;
    for (const auto &ueInfo : levelInfo.ueList) {
        const urmaEntity *ue = FindUrmaEntity(ueList, ueInfo.dieId, ueInfo.feId, ueInfo.type);
        if (ue == nullptr || ue->eidNum == 0) {
            continue;
        }
        if (ueInfo.type == UrmaEntityType::MESH || ueInfo.type == UrmaEntityType::CLOS_PORTS) {
            AddMeshAddrsToLevel(*ue, level, ueInfo.type);
            result = BM_OK;
        } else if (ueInfo.type == UrmaEntityType::CLOS) {
            result = AddClosAddrToLevel(*ue, level);
        } else if (ueInfo.type == UrmaEntityType::UBOE) {
            result = AddUboeAddrToLevel(*ue, ueInfo, level);
        } else if (ueInfo.type == UrmaEntityType::UB_RTP) {
            result = AddUbRtpAddrToLevel(*ue, ueInfo, level);
        }
        if (result != BM_OK) {
            break;
        }
    }
    return result;
}

} // anonymous namespace

Result UrmaTopoProductServer::GetRootInfo(RootInfo &rootInfo)
{
    rootInfo = {};
    rootInfo.version = "2.0";

    dcmi_spod_info spod{};
    if (GetSpodInfo(deviceId_, spod) != BM_OK) {
        BM_LOG_ERROR("GetRootInfo: GetSpodInfo failed, deviceId=" << deviceId_ << " phyId=" << phyId_);
        return BM_ERROR;
    }

    auto ret = GetTopoFilePath(mainboardId_, spod.super_pod_type, rootInfo.topoFilePath);
    if (ret != BM_OK) {
        BM_LOG_ERROR("GetRootInfo: GetTopoFilePath failed, mainboardId=" << mainboardId_ << " phyId=" << phyId_);
        return ret;
    }

    urmaEntityList ueList{};
    if (GetUrmaEntityList(logicId_, ueList) != BM_OK) {
        BM_LOG_WARN("GetRootInfo: GetUrmaEntityList failed, logicId=" << logicId_ << " phyId=" << phyId_);
    }

    const NetInfo *netInfo = FindTopologyRule(mainboardId_, spod.super_pod_type);
    if (netInfo == nullptr) {
        BM_LOG_ERROR("GetRootInfo: FindTopologyRule failed, mainboardId="
                     << mainboardId_ << " spodType=" << spod.super_pod_type << " phyId=" << phyId_);
        return BM_INVALID_PARAM;
    }

    Rank rank;
    rank.deviceId = phyId_;
    rank.localId = phyId_;
    if (spod.super_pod_type == TOPO_TYPE_SERVER_16FM) {
        rank.localId = phyId_ + static_cast<int32_t>(spod.server_index * SERVER_NPU_NUM);
    }

    for (const auto &levelInfo : netInfo->levelInfos) {
        Level level;
        if (BuildLevel(phyId_, level, ueList, levelInfo, spod) == BM_OK) {
            rank.levelList.push_back(std::move(level));
        }
    }

    rootInfo.rankList.push_back(std::move(rank));
    rootInfo.rankCount = static_cast<int>(rootInfo.rankList.size());
    return BM_OK;
}

Result UrmaTopoProductServer::GetLocalId(int32_t &localId)
{
    dcmi_spod_info spod{};
    if (GetSpodInfo(deviceId_, spod) != BM_OK) {
        BM_LOG_ERROR("GetRootInfo: GetSpodInfo failed, deviceId=" << deviceId_ << " phyId=" << phyId_);
        return BM_ERROR;
    }
    localId = phyId_;
    if (spod.super_pod_type == TOPO_TYPE_SERVER_16FM) {
        localId = phyId_ + static_cast<int32_t>(spod.server_index * SERVER_NPU_NUM);
    }
    return BM_OK;
}

} // namespace transport
} // namespace mf
} // namespace ock
