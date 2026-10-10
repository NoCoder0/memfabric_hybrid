/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include "smem_cluster_status.h"

#include "smem_store_factory.h"

namespace ock {
namespace smem {
std::string BuildClusterStatusJson(const std::vector<StorePtr> &stores) noexcept
{
    std::string out = "[";
    std::string sep = "";
    for (const auto &store : stores) {
        if (store == nullptr) {
            continue;
        }
        out.append(sep);
        store->GetClusterStatusJson(out);
        sep = ",";
    }
    out.push_back(']');
    return out;
}

std::string GetClusterStatusJson() noexcept
{
    try {
        return BuildClusterStatusJson(StoreFactory::SnapshotStores());
    } catch (...) {
        return "[]";
    }
}
} // namespace smem
} // namespace ock
