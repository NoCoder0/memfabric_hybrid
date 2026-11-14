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
#include "smem_reconnect_manager.h"

#include "smem_common_includes.h"
#include "smem_tcp_config_store.h"

namespace ock {
namespace smem {
SmemReconnectManager::SmemReconnectManager(RemoveAllImportedHandler removeAllImported)
    : removeAllImported_(std::move(removeAllImported))
{}

void SmemReconnectManager::RegisterDisconnectObserver(TcpConfigStore &store)
{
    // store 可晚于 Entry 释放；迟到观察者只访问 manager 的代次，不调用资源操作。
    disconnectObserver_ = std::make_shared<ConfigStoreClientBrokenHandler>([weak = weak_from_this()] {
        if (auto manager = weak.lock()) {
            manager->disconnectEpoch_.fetch_add(1UL, std::memory_order_acq_rel);
        }
        return SM_OK;
    });
    store.RegisterClientBrokenObserver(disconnectObserver_);
}

void SmemReconnectManager::ReleaseDisconnectObserver() noexcept
{
    disconnectObserver_.reset();
}

int SmemReconnectManager::CleanupAfterReconnect(bool &reset)
{
    reset = false;
    const auto epoch = disconnectEpoch_.load(std::memory_order_acquire);
    if (cleanupAttemptedEpoch_ != epoch) {
        cleanupAttemptedEpoch_ = epoch;
        reset = true;
        auto ret = removeAllImported_();
        if (ret != SM_OK) {
            SM_LOG_WARN("remove all imported peers failed, epoch=" << epoch << " ret=" << ret);
        }
    }
    if (disconnectEpoch_.load(std::memory_order_acquire) != epoch) {
        SM_LOG_ERROR("control link changed before import, epoch=" << epoch);
        return SM_NOT_CONNECTED;
    }
    return SM_OK;
}

int SmemReconnectManager::ImportAfterReconnectCleanup(const std::vector<RankFullInfo> &peers,
                                                      const ImportPeersHandler &importPeers)
{
    if (peers.empty()) {
        return importPeers();
    }
    const auto epoch = disconnectEpoch_.load(std::memory_order_acquire);
    bool reset = false;
    SM_RETURN_IT_IF_NOT_OK(CleanupAfterReconnect(reset));
    SM_RETURN_IT_IF_NOT_OK(importPeers());
    if (disconnectEpoch_.load(std::memory_order_acquire) != epoch) {
        SM_LOG_ERROR("control link changed during import, epoch=" << epoch);
        return SM_NOT_CONNECTED;
    }
    return SM_OK;
}

} // namespace smem
} // namespace ock
