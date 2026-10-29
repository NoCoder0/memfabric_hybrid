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
#ifndef MEM_FABRIC_HYBRID_HYBM_ENTITY_TAG_INFO_H
#define MEM_FABRIC_HYBRID_HYBM_ENTITY_TAG_INFO_H

#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <vector>

#include "hybm_common_include.h"

namespace ock {
namespace mf {

class HybmEntityTagInfo {
public:
    Result TagInfoInit(const hybm_options &option);
    Result RemoveRankInfo(uint32_t rankId);
    uint32_t GetRank2RankOpType(uint32_t rankId1, uint32_t rankId2);
    std::vector<hybm_data_op_type> GetRank2RankLayers(uint32_t rankId1, uint32_t rankId2) const;
    Result AddRankLayers(uint32_t rankId, const std::vector<hybm_data_op_type> &layers);
    bool HasLayerProtocols(uint32_t rankId) const;
    std::vector<hybm_data_op_type> GetLayersForRank(uint32_t rankId) const;
    static std::string GetOpTypeStr(hybm_data_op_type opType);

private:
    Result ParseLayerProtocol(const std::string &protocol, std::vector<hybm_data_op_type> &layers);
    static bool IsLayerLayoutCompatible(const std::vector<hybm_data_op_type> &left,
                                        const std::vector<hybm_data_op_type> &right);

private:
    std::shared_mutex mutex_;
    uint32_t legacyOpType_{0};
    std::unordered_map<uint32_t, std::vector<hybm_data_op_type>> rankLayers_;
};

using HybmEntityTagInfoPtr = std::shared_ptr<HybmEntityTagInfo>;
} // namespace mf
} // namespace ock
#endif // MEM_FABRIC_HYBRID_HYBM_ENTITY_TAG_INFO_H
