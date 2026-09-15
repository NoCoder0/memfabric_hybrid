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

#ifndef URMA_TOPO_DEF_H
#define URMA_TOPO_DEF_H

#include "hybm_define.h"
#include "dl_hal_api_def.h"

#include <string>
#include <vector>

namespace ock {
namespace mf {
namespace transport {

#define MAX_TOPO_PATH_LEN   (256)
#define MAX_ADDR_LEN        (64)
#define MAX_INSTANCE_ID_LEN (64)
#define MAX_PLANE_ID_LEN    (64)
#define MAX_PORT_LEN        (16)
#define MAX_PORT_NUM        (32)
#define MAX_NET_TYPE_LEN    (16)
#define MAX_NET_LEVEL_NUM   (8) // 最大网络层级数

#define MAX_ADDR_TYPE_LEN (8) // 网络地址类型最大长度，例如：EID, IPV4
#define MAX_RANK_NUM      (8)
#define MAX_VERSION_LEN   (32)
#define MAX_STATUS_LEN    (32)
#define MAX_ADDR_NUM      (32) // 最大地址个数
#define MAX_NET_ADDR_LEN  (33) // 保留结束符

#define MAIN_BOARD_INVALID                    (0)
#define MAIN_BOARD_ID_CARD_NOMESH             (0x68)
#define MAIN_BOARD_ID_CARD_2PMESH             (0x6a)
#define MAIN_BOARD_ID_CARD_4PMESH             (0x6c)
#define MAIN_BOARD_ID_SERVER_TYPE1            (0x23)
#define MAIN_BOARD_ID_SERVER_8PMESH           (0x25)
#define MAIN_BOARD_ID_SERVER_8PMESH_UBOE      (0x27)
#define MAIN_BOARD_ID_SERVER_8PMESH_NOSP      (0x29)
#define MAIN_BOARD_ID_SERVER_8PMESH_NOSP_UBOE (0x2B)
#define MAIN_BOARD_ID_SERVER_350L             (0x44)
#define MAIN_BOARD_ID_SERVER_550EL_100        (0x46)
#define MAIN_BOARD_ID_SERVER_550EL_200        (0x48)
#define MAIN_BOARD_ID_POD                     (0x07)
#define MAIN_BOARD_ID_POD_2D                  (0x03)
#define MAIN_BOARD_ID_POD_FLEX                (0x2D)
#define MAIN_BOARD_ID_POD_FLEX_RTP            (0x2F)

#define TOPO_TYPE_IGNORE      (99) // 忽略该拓扑类型
#define TOPO_TYPE_SERVER_8P   (0)  // 普通服务
#define TOPO_TYPE_SERVER_16FM (3)  // 两个服务器组16p fullmesh

#define NET_TYPE_TOPO_FILE_DESC "TOPO_FILE_DESC"
#define NET_TYPE_MESH           "TOPO_FILE_DESC"
#define NET_TYPE_CLOS           "CLOS"

#define ADDR_TYPE_EID  "EID"
#define MAX_UE_PER_NPU (8)
#define MAX_EID_PER_UE (32)

static constexpr const char *ROOTINFO_PATH = "/etc/hccl_rootinfo.json";

struct RankAddr {
    std::string addrType;
    std::string addr;
    std::vector<std::string> ports;
    std::string planeId;
};

struct Level {
    int netLayer;
    std::string netInstanceId;
    std::string netType;
    std::string netAttr;
    std::vector<RankAddr> rankAddrList;
};

struct Rank {
    int deviceId;
    int localId;
    std::vector<Level> levelList;
};

struct RootInfo {
    std::string version;
    std::string topoFilePath;
    int rankCount;
    std::vector<Rank> rankList;
};

struct Edge {
    int netLayer;
    std::string linkType;
    std::string topoType;
    int topoInstanceId;
    std::string topoAttr;
    int localA;
    std::vector<std::string> localAPorts;
    int localB;
    std::vector<std::string> localBPorts;
    std::vector<std::string> protocols;
    std::string position;
};

struct TopoInfo {
    std::string version;
    std::string hardwareType;
    int peerCount;
    std::vector<int> peerList;
    int edgeCount;
    std::vector<Edge> edgeList;
};

struct urmaEntity {
    dcmi_urma_eid_info eidList[MAX_EID_PER_UE];
    unsigned int eidNum;
};

struct urmaEntityList {
    urmaEntity ueList[MAX_UE_PER_NPU];
    unsigned int ueNum;
};

} // namespace transport
} // namespace mf
} // namespace ock

#endif //URMA_TOPO_DEF_H
