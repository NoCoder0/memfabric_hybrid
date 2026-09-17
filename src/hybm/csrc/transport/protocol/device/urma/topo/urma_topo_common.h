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

#ifndef MF_HYBM_URMA_TOPO_COMMON_H
#define MF_HYBM_URMA_TOPO_COMMON_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dl_hcomm_api.h"
#include "hybm_define.h"
#include "hybm_types.h"
#include "urma_topo_def.h"

#include "dl_hal_api_def.h"

namespace ock {
namespace mf {
namespace transport {

constexpr int32_t MAX_MESH_PORT_ID = 9;
constexpr int32_t CARD_2P_MESH_NUM = 2;
constexpr int32_t ROCE_NET_LAYER = 3;
constexpr size_t MAX_EID_NUM = 32;
constexpr int MAX_IFREQ_NUM = 16;

Result GetServerId(std::string &serverId);

void EidToHexStr(const dcmi_urma_eid &eid, std::string &hexStr);

int32_t GetCardPortId(const dcmi_urma_eid &eid);
int32_t GetCardDieId(const dcmi_urma_eid &eid);

int32_t UrmaEidGetFeId(const dcmi_urma_eid &eid);
int32_t UrmaEidGetPortId(const dcmi_urma_eid &eid);
int32_t UrmaEidGetDieId(const dcmi_urma_eid &eid);
bool UrmaEidIsPortGroup(const dcmi_urma_eid &eid);
bool UrmaEidIsUBOE(const dcmi_urma_eid &eid);
bool UrmaEidIsUbRtp(const dcmi_urma_eid &eid);
void UrmaEid2CNA(const dcmi_urma_eid &eid, std::string &cna);

int32_t UrmaEntityGetId(const urmaEntity &ue);
int32_t UrmaEntityGetDieId(const urmaEntity &ue);
int32_t UrmaEntityGetPortGroupIdx(const urmaEntity &ue);
int32_t UBGetMaxEntityId(const urmaEntityList &ueList, int32_t dieId);

Result GetSpodInfo(int32_t deviceId, dcmi_spod_info &spodInfo);
Result GetUrmaEntityList(int32_t logicId, urmaEntityList &ueList);
Result GetEidList(int32_t logicId, std::vector<dcmi_urma_eid_info> &eidList);

Result ProcessLayerRoce(uint32_t phyId, Level &level);

void HexStrToEid(const std::string &hex, std::array<uint8_t, COMM_ADDR_EID_LEN> &eid);

std::string RootInfoToString(const RootInfo &rootInfo);
std::string TopoInfoToString(const TopoInfo &topoInfo);

} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_URMA_TOPO_COMMON_H
