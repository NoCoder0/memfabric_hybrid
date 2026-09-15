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

#ifndef URMA_TOPO_BUILDER_H
#define URMA_TOPO_BUILDER_H

#include <memory>

#include "hybm_types.h"
#include "urma_topo_def.h"

namespace ock {
namespace mf {
namespace transport {
class UrmaTopoProduct;
using UrmaTopoProductPtr = std::shared_ptr<UrmaTopoProduct>;
class UrmaTopoProduct {
public:
    static UrmaTopoProductPtr GetUrmaTopoProduct(int32_t deviceId, int32_t logicId, int32_t phyId,
                                                 uint32_t mainboardId);

    static Result GetTopoFilePath(uint32_t mainboardId, uint32_t spodType, std::string &topoFilePath);

    explicit UrmaTopoProduct(int32_t deviceId, int32_t logicId, int32_t phyId, uint32_t mainboardId)
        : deviceId_(deviceId), logicId_(logicId), phyId_(phyId), mainboardId_(mainboardId)
    {}
    virtual ~UrmaTopoProduct() = default;
    virtual Result GetRootInfo(RootInfo &rootInfo) = 0;
    virtual Result GetLocalId(int32_t &localId) = 0;

protected:
    int32_t deviceId_{-1};
    int32_t logicId_{-1};
    int32_t phyId_{-1};
    int32_t rankId_{-1};
    uint32_t mainboardId_{0};
    uint32_t sopType_{0};
    RootInfo rootInfo_{};
};

} // namespace transport
} // namespace mf
} // namespace ock

#endif //URMA_TOPO_BUILDER_H
