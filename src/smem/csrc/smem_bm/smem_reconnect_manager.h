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
#ifndef MEMFABRIC_HYBRID_SMEM_RECONNECT_MANAGER_H
#define MEMFABRIC_HYBRID_SMEM_RECONNECT_MANAGER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "smem_group_manager_def.h"

namespace ock {
namespace smem {
class TcpConfigStore;

class SmemReconnectManager : public std::enable_shared_from_this<SmemReconnectManager> {
public:
    using ImportPeersHandler = std::function<int()>;
    using RemoveAllImportedHandler = std::function<int()>;

    explicit SmemReconnectManager(RemoveAllImportedHandler removeAllImported);
    void RegisterDisconnectObserver(TcpConfigStore &store);
    void ReleaseDisconnectObserver() noexcept;
    int CleanupAfterReconnect(bool &reset);
    int ImportAfterReconnectCleanup(const std::vector<RankFullInfo> &peers, const ImportPeersHandler &importPeers);

private:
    RemoveAllImportedHandler removeAllImported_;
    std::shared_ptr<std::function<int()>> disconnectObserver_;
    std::atomic<uint64_t> disconnectEpoch_{0UL};
    uint64_t cleanupAttemptedEpoch_{0UL};
};

} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_RECONNECT_MANAGER_H
