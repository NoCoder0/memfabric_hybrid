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
#ifndef SMEM_CLUSTER_STATUS_H
#define SMEM_CLUSTER_STATUS_H

#include <chrono>
#include <string>
#include <vector>

#include "smem_config_store.h"

namespace ock {
namespace smem {
/**
 * @brief Aggregate cluster status JSON of all config stores in this process.
 *
 * Each store contributes one self-described JSON object (see
 * ConfigStore::GetClusterStatusJson); the result is a JSON array of objects.
 * Callers can publish this to the metrics service (memcache) which surfaces
 * it as the cluster view: server state machine, world size, alive ranks,
 * heartbeat set and group-manager rank/link states per store.
 *
 * SnapshotStores copies the store set under the factory mutex; per-store
 * summaries are composed from thread-safe getters only.
 * @param stores store set to summarise
 * @return JSON array string, e.g. "[{"type":"tcp",...},{"type":"ha",...}]"
 */
std::string BuildClusterStatusJson(const std::vector<StorePtr> &stores) noexcept;

/**
 * @brief Convenience wrapper: snapshot stores from StoreFactory and build JSON.
 * @return JSON array string of all stores in this process
 */
std::string GetClusterStatusJson() noexcept;
} // namespace smem
} // namespace ock
#endif // SMEM_CLUSTER_STATUS_H
