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

#include "urma_topo_common.h"

#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_def.h"
#include "hybm_logger.h"
#include "urma_topo_helper.h"

namespace ock {
namespace mf {
namespace transport {

namespace {

constexpr int EID_FE_BYTE_OFFSET = 6;
constexpr int EID_PORT_DIE_BYTE_OFFSET = 5;
constexpr int EID_PORT_BIT_LEN = 6;
constexpr int EID_FLAG_BYTE_OFFSET = 7;
constexpr int EID_UBOE_FLAG_MASK = 0xc0;
constexpr int EID_UB_RTP_FLAG_VALUE = 0x80;
constexpr int EID_TYPE_FLAG_MASK = 0xc0;
constexpr int PORT_GROUP_PORT_ID = 0x3F;
constexpr int EID_CNA_IP_BYTE_0 = 6;
constexpr int EID_CNA_IP_BYTE_1 = 12;
constexpr int EID_CNA_IP_BYTE_2 = 13;
constexpr int EID_CNA_IP_BYTE_3 = 14;

} // anonymous namespace

Result GetServerId(std::string &serverId)
{
    int sockFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockFd < 0) {
        BM_LOG_ERROR("GetServerId: socket failed");
        return BM_ERROR;
    }
    struct ifreq ifr[MAX_IFREQ_NUM];
    struct ifconf ifc;
    ifc.ifc_len = sizeof(ifr);
    ifc.ifc_buf = reinterpret_cast<char *>(ifr);
    if (ioctl(sockFd, SIOCGIFCONF, &ifc) < 0) {
        BM_LOG_ERROR("GetServerId: ioctl SIOCGIFCONF failed");
        close(sockFd);
        return BM_ERROR;
    }
    int ifCount = ifc.ifc_len / static_cast<int>(sizeof(struct ifreq));
    for (int i = 0; i < ifCount; ++i) {
        if (strcmp(ifr[i].ifr_name, "lo") == 0) {
            continue;
        }
        if (ioctl(sockFd, SIOCGIFHWADDR, &ifr[i]) < 0) {
            continue;
        }
        if (ifr[i].ifr_hwaddr.sa_family != ARPHRD_ETHER) {
            continue;
        }
        auto mac = reinterpret_cast<unsigned char *>(ifr[i].ifr_hwaddr.sa_data);
        char buf[MAX_INSTANCE_ID_LEN] = {0};
        snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        serverId = buf;
        close(sockFd);
        return BM_OK;
    }
    close(sockFd);
    BM_LOG_ERROR("GetServerId: no valid Ethernet interface found");
    return BM_ERROR;
}

void EidToHexStr(const dcmi_urma_eid &eid, std::string &hexStr)
{
    static constexpr int HEX_BUF_LEN = DCMI_URMA_EID_SIZE * 2 + 1;
    char buf[HEX_BUF_LEN] = {0};
    for (int i = 0; i < DCMI_URMA_EID_SIZE; ++i) {
        snprintf(buf + i * 2, HEX_BUF_LEN - i * 2, "%02x", eid.raw[i]);
    }
    hexStr = buf;
}

int32_t GetCardPortId(const dcmi_urma_eid &eid)
{
    uint8_t last = eid.raw[DCMI_URMA_EID_SIZE - 1];
    last = static_cast<uint8_t>(last << 1);
    last = static_cast<uint8_t>(last >> 4);
    return static_cast<int32_t>(last);
}

int32_t GetCardDieId(const dcmi_urma_eid &eid)
{
    uint8_t last = eid.raw[DCMI_URMA_EID_SIZE - 1];
    return (4 & last) == 0 ? 0 : 1;
}

int32_t UrmaEidGetFeId(const dcmi_urma_eid &eid)
{
    uint8_t fe = eid.raw[EID_FE_BYTE_OFFSET];
    return static_cast<int32_t>(fe & 0x7F);
}

int32_t UrmaEidGetPortId(const dcmi_urma_eid &eid)
{
    uint8_t dieAndPort = eid.raw[EID_PORT_DIE_BYTE_OFFSET];
    return static_cast<int32_t>(dieAndPort & 0x3F);
}

int32_t UrmaEidGetDieId(const dcmi_urma_eid &eid)
{
    uint8_t dieAndPort = eid.raw[EID_PORT_DIE_BYTE_OFFSET];
    return static_cast<int32_t>(dieAndPort >> EID_PORT_BIT_LEN);
}

bool UrmaEidIsPortGroup(const dcmi_urma_eid &eid)
{
    return UrmaEidGetPortId(eid) == PORT_GROUP_PORT_ID;
}

bool UrmaEidIsUBOE(const dcmi_urma_eid &eid)
{
    uint8_t flag = eid.raw[EID_FLAG_BYTE_OFFSET];
    return (EID_UBOE_FLAG_MASK & flag) == EID_UBOE_FLAG_MASK;
}

bool UrmaEidIsUbRtp(const dcmi_urma_eid &eid)
{
    uint8_t flag = eid.raw[EID_FLAG_BYTE_OFFSET];
    return (EID_TYPE_FLAG_MASK & flag) == EID_UB_RTP_FLAG_VALUE;
}

void UrmaEid2CNA(const dcmi_urma_eid &eid, std::string &cna)
{
    cna = std::to_string(eid.raw[EID_CNA_IP_BYTE_0]) + "." + std::to_string(eid.raw[EID_CNA_IP_BYTE_1]) + "." +
          std::to_string(eid.raw[EID_CNA_IP_BYTE_2]) + "." + std::to_string(eid.raw[EID_CNA_IP_BYTE_3]);
}

int32_t UrmaEntityGetId(const urmaEntity &ue)
{
    if (ue.eidNum == 0) {
        return -1;
    }
    return UrmaEidGetFeId(ue.eidList[0].eid);
}

int32_t UrmaEntityGetDieId(const urmaEntity &ue)
{
    if (ue.eidNum == 0) {
        return -1;
    }
    return UrmaEidGetDieId(ue.eidList[0].eid);
}

int32_t UrmaEntityGetPortGroupIdx(const urmaEntity &ue)
{
    for (unsigned int i = 0; i < ue.eidNum; ++i) {
        if (UrmaEidIsPortGroup(ue.eidList[i].eid)) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int32_t UBGetMaxEntityId(const urmaEntityList &ueList, int32_t dieId)
{
    int32_t maxId = -1;
    for (unsigned int i = 0; i < ueList.ueNum; ++i) {
        if (ueList.ueList[i].eidNum == 0 || UrmaEntityGetDieId(ueList.ueList[i]) != dieId) {
            continue;
        }
        int32_t id = UrmaEidGetFeId(ueList.ueList[i].eidList[0].eid);
        if (id > maxId) {
            maxId = id;
        }
    }
    return maxId;
}

Result GetSpodInfo(int32_t deviceId, dcmi_spod_info &spodInfo)
{
    spodInfo = {};
    int64_t spodId = 0;
    int64_t serverId = 0;
    int64_t chassisId = 0;
    int64_t spodType = 0;
    auto ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(deviceId), 0, INFO_TYPE_SUPER_POD_ID, &spodId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("GetSpodInfo: get spod_id failed, deviceId=" << deviceId << " ret=" << ret);
        return BM_ERROR;
    }
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(deviceId), 0, INFO_TYPE_SERVER_ID, &serverId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("GetSpodInfo: get server_id failed, deviceId=" << deviceId << " ret=" << ret);
        return BM_ERROR;
    }
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(deviceId), 0, INFO_TYPE_CHASSI_ID, &chassisId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("GetSpodInfo: get chassis_id failed, deviceId=" << deviceId << " ret=" << ret);
        return BM_ERROR;
    }
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(deviceId), 0, INFO_TYPE_SPOD_TYPE, &spodType);
    if (ret != BM_OK) {
        BM_LOG_ERROR("GetSpodInfo: get spod_type failed, deviceId=" << deviceId << " ret=" << ret);
        return BM_ERROR;
    }
    spodInfo.super_pod_id = static_cast<unsigned int>(spodId);
    spodInfo.server_index = static_cast<unsigned int>(serverId);
    spodInfo.chassis_id = static_cast<unsigned int>(chassisId);
    spodInfo.super_pod_type = static_cast<unsigned int>(spodType);
    return BM_OK;
}

Result GetUrmaEntityList(int32_t logicId, urmaEntityList &ueList)
{
    ueList = {};
    unsigned int devCnt = 0;
    auto ret = DlHalApi::DcmiGetUrmaDeviceCnt(logicId, &devCnt);
    if (ret != BM_OK) {
        BM_LOG_ERROR("GetUrmaEntityList: DcmiGetUrmaDeviceCnt failed, logicId=" << logicId << " ret=" << ret);
        return BM_ERROR;
    }
    if (devCnt > MAX_UE_PER_NPU) {
        devCnt = MAX_UE_PER_NPU;
    }
    ueList.ueNum = devCnt;
    for (unsigned int i = 0; i < devCnt; ++i) {
        int eidCnt = MAX_EID_PER_UE;
        ret = DlHalApi::DcmiGetEidListByUrmaDevIndex(logicId, static_cast<int>(i), ueList.ueList[i].eidList, &eidCnt);
        if (ret != BM_OK) {
            ueList.ueList[i].eidNum = 0;
            continue;
        }
        ueList.ueList[i].eidNum = static_cast<unsigned int>(eidCnt);
    }
    return BM_OK;
}

Result GetEidList(int32_t logicId, std::vector<dcmi_urma_eid_info> &eidList)
{
    unsigned int devCnt = 0;
    auto ret = DlHalApi::DcmiGetUrmaDeviceCnt(logicId, &devCnt);
    if (ret != BM_OK) {
        BM_LOG_ERROR("GetEidList: DcmiGetUrmaDeviceCnt failed, logicId=" << logicId << " ret=" << ret);
        return BM_ERROR;
    }
    for (unsigned int i = 0; i < devCnt; ++i) {
        dcmi_urma_eid_info eidArray[MAX_EID_NUM];
        int eidCnt = static_cast<int>(MAX_EID_NUM);
        ret = DlHalApi::DcmiGetEidListByUrmaDevIndex(logicId, static_cast<int>(i), eidArray, &eidCnt);
        if (ret != BM_OK) {
            continue;
        }
        for (int j = 0; j < eidCnt; ++j) {
            eidList.push_back(eidArray[j]);
        }
    }
    return BM_OK;
}

Result ProcessLayerRoce(uint32_t phyId, Level &level)
{
    return BM_ERROR;
}

void HexStrToEid(const std::string &hex, std::array<uint8_t, COMM_ADDR_EID_LEN> &eid)
{
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return c - 'A' + 10;
    };
    eid.fill(0);
    for (size_t k = 0; k < COMM_ADDR_EID_LEN && k * 2 + 1 < hex.size(); ++k) {
        eid[k] = static_cast<uint8_t>(hv(hex[k * 2]) * 16 + hv(hex[k * 2 + 1]));
    }
}

namespace {

std::string JoinStrs(const std::vector<std::string> &items, const std::string &sep)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += sep;
        }
        out += items[i];
    }
    return out;
}

std::string RankAddrToString(const RankAddr &addr)
{
    std::ostringstream os;
    os << "{type:" << addr.addrType << ", addr:" << addr.addr << ", ports:[" << JoinStrs(addr.ports, ",")
       << "], plane:" << addr.planeId << "}";
    return os.str();
}

std::string LevelToString(const Level &level)
{
    std::ostringstream os;
    os << "{layer:" << level.netLayer << ", id:" << level.netInstanceId << ", type:" << level.netType << ", addrs:[";
    for (size_t i = 0; i < level.rankAddrList.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << RankAddrToString(level.rankAddrList[i]);
    }
    os << "]}";
    return os.str();
}

std::string RankToString(const Rank &rank)
{
    std::ostringstream os;
    os << "{dev:" << rank.deviceId << ", local:" << rank.localId << ", levels:[";
    for (size_t i = 0; i < rank.levelList.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << LevelToString(rank.levelList[i]);
    }
    os << "]}";
    return os.str();
}

std::string EdgeToString(const Edge &edge)
{
    std::ostringstream os;
    os << "{layer:" << edge.netLayer << ", link:" << edge.linkType << ", topo:" << edge.topoType
       << ", A:" << edge.localA << "[" << JoinStrs(edge.localAPorts, ",") << "], B:" << edge.localB << "["
       << JoinStrs(edge.localBPorts, ",") << "], proto:[" << JoinStrs(edge.protocols, ",") << "]}";
    return os.str();
}

} // anonymous namespace

std::string RootInfoToString(const RootInfo &rootInfo)
{
    std::ostringstream os;
    os << "RootInfo{ver:" << rootInfo.version << ", topo:" << rootInfo.topoFilePath
       << ", rankCount:" << rootInfo.rankCount << ", ranks:[";
    for (size_t i = 0; i < rootInfo.rankList.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << RankToString(rootInfo.rankList[i]);
    }
    os << "]}";
    return os.str();
}

std::string TopoInfoToString(const TopoInfo &topoInfo)
{
    std::ostringstream os;
    os << "TopoInfo{ver:" << topoInfo.version << ", hw:" << topoInfo.hardwareType
       << ", peerCount:" << topoInfo.peerCount << ", peers:[";
    for (size_t i = 0; i < topoInfo.peerList.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        os << topoInfo.peerList[i];
    }
    os << "], edgeCount:" << topoInfo.edgeCount << ", edges:[";
    for (size_t i = 0; i < topoInfo.edgeList.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << EdgeToString(topoInfo.edgeList[i]);
    }
    os << "]}";
    return os.str();
}

} // namespace transport
} // namespace mf
} // namespace ock
