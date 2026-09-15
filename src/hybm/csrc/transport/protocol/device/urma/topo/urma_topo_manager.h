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

#ifndef MEMFABRIC_HYBRID_ROOTINFO_MANAGER_H
#define MEMFABRIC_HYBRID_ROOTINFO_MANAGER_H

#include "hybm_types.h"
#include "dl_hcomm_api.h"
#include "urma_topo_def.h"
#include "urma_topo_product.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ock {
namespace mf {
namespace transport {

class UrmaTopoManager {
public:
    UrmaTopoManager(const UrmaTopoManager &) = delete;
    UrmaTopoManager &operator=(const UrmaTopoManager &) = delete;

    static UrmaTopoManager &GetInstance()
    {
        static UrmaTopoManager instance;
        return instance;
    }

    void ParseRankAddrObj(const std::string &objJson, RankAddr &addr);
    Result ParseRankAddrList(const std::string &json, std::vector<RankAddr> &addrList);
    void ParseLevelObj(const std::string &objJson, Level &level);
    Result ParseLevelList(const std::string &json, std::vector<Level> &levelList);
    void ParseRankObj(const std::string &objJson, Rank &rank);
    Result ParseRankListFull(const std::string &json, std::vector<Rank> &rankList);
    Result ParsePeerList(const std::string &json, std::vector<int> &peerList);
    Result ParseEdgeList(const std::string &json, std::vector<Edge> &edgeList);
    Result ParseTopoInfo(const std::string &json, TopoInfo &topoInfo);
    Result Initialize(int32_t deviceId, uint32_t rankId);
    void Uninitialize();

    Result GetPeer2PeerEid(int32_t dstPhyId, std::array<uint8_t, COMM_ADDR_EID_LEN> &eidData);
    Result GetPeer2NetEid(std::array<uint8_t, COMM_ADDR_EID_LEN> &eidData);

private:
    UrmaTopoManager() = default;
    ~UrmaTopoManager() = default;

    Result SetDeviceBaseInfo();
    Result BuildRootInfo();
    Result BuildRootInfoFromFile();
    Result BuildTopoInfo();
    Result BuildPeer2PeerMap();

private:
    int32_t userDeviceId_{-1};
    int32_t logicDeviceId_{-1};
    int32_t phyDeviceId_{-1};
    uint32_t rankId_{0};
    uint32_t mainboardId_{0};
    int32_t localId_{-1};
    UrmaTopoProductPtr topoProductPtr_{nullptr};
    RootInfo rootInfo_{};
    TopoInfo topoInfo_{};
    std::unordered_map<int32_t, std::array<uint8_t, COMM_ADDR_EID_LEN>> peer2PeerMap_;
};

} // namespace transport
} // namespace mf
} // namespace ock

#endif // MEMFABRIC_HYBRID_ROOTINFO_MANAGER_H
