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

#ifndef MEMFABRIC_HYBRID_URMA_TOPO_PRODUCT_POD_H
#define MEMFABRIC_HYBRID_URMA_TOPO_PRODUCT_POD_H
#include "urma_topo_product.h"

namespace ock {
namespace mf {
namespace transport {
class UrmaTopoProductPod : public UrmaTopoProduct {
public:
    explicit UrmaTopoProductPod(int32_t deviceId, int32_t logicId, int32_t phyId, uint32_t mainboardId)
        : UrmaTopoProduct(deviceId, logicId, phyId, mainboardId)
    {}
    ~UrmaTopoProductPod() override = default;
    Result GetLocalId(int32_t &localId) override;
    Result GetRootInfo(RootInfo &rootInfo) override;
};
} // namespace transport
} // namespace mf
} // namespace ock

#endif //MEMFABRIC_HYBRID_URMA_TOPO_PRODUCT_POD_H
