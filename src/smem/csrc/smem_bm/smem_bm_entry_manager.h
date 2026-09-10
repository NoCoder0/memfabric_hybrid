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
#ifndef MEMFABRIC_HYBRID_SMEM_BM_ENTRY_MANAGER_H
#define MEMFABRIC_HYBRID_SMEM_BM_ENTRY_MANAGER_H

#include <string>
#include "smem_net_common.h"
#include "smem_bm.h"
#include "smem_bm_entry.h"
#include "smem_config_store.h"

namespace ock {
namespace smem {

class SmemBmEntryManager {
public:
    static SmemBmEntryManager &Instance();

    SmemBmEntryManager() = default;
    ~SmemBmEntryManager();

    SmemBmEntryManager(const SmemBmEntryManager &) = delete;
    SmemBmEntryManager(SmemBmEntryManager &&) = delete;
    SmemBmEntryManager &operator=(const SmemBmEntryManager &other) = delete;
    SmemBmEntryManager &operator=(SmemBmEntryManager &&) = delete;

    Result Initialize(const std::string &storeURL, uint32_t worldSize, uint16_t deviceId,
                      const smem_bm_config_t &config);

    Result CreateEntryById(uint32_t id, SmemBmEntryPtr &entry);
    Result GetEntryByPtr(uintptr_t ptr, SmemBmEntryPtr &entry);
    Result GetEntryById(uint32_t id, SmemBmEntryPtr &entry);
    Result RemoveEntryByPtr(uintptr_t ptr);

    void Destroy();

    /**
     * @brief Update the config store URL lazily, used when MetaService restarts with a new IP.
     * This only updates the underlying TcpConfigStore's server IP/port via SetServerInfo.
     * The actual reconnection is handled by the existing reconnection mechanism (ReConnectAfterBroken),
     * which will use the new IP/port automatically when the connection is broken.
     * @param storeURL        [in] new store URL
     * @return SM_OK if successful, SM_ERROR if the underlying store is not TcpConfigStore or confStore_ is null
     */
    Result UpdateStoreUrl(const std::string &storeURL);

    /**
     * @brief Update the config store server address and reconnect.
     *
     * Called when the leader has changed. Unwraps the PrefixConfigStore to reach the underlying TcpConfigStore,
     * calls SetServerInfo + ReConnectAfterBroken to switch to the new leader.
     *
     * @param newServerIp    New leader's IP
     * @param newServerPort  New leader's config store port
     * @return SM_OK on success
     */
    Result UpdateStoreServer(const std::string &newServerIp, uint16_t newServerPort) noexcept;

    Result GetStoreServerInfo(std::string &ip, uint16_t *port) const noexcept;

    Result GetMetaServiceInfo(std::string &ip, uint16_t *port) const noexcept;

    /**
     * @brief Register a callback invoked when the HA leader changes.
     *
     * Pure leader-change notification: libsmem only tells the upper layer "the leader
     * has changed"; what to do next (e.g. re-discover the MetaService address via
     * GetMetaServiceInfo / smem_bm_get_meta_service_info and switch connections) is
     * entirely up to the caller. Bridges HaConfigStore's leader-change notification
     * across the C API boundary, covering SIGSTOP-like failures where the old leader
     * stays ESTABLISHED and no TCP link-broken event is delivered.
     *
     * Notes:
     * - Runs on the HA re-election thread inside libsmem; implementations should return
     *   quickly or offload heavy work to their own thread.
     * - Not fired on the leader node for its own leadership transitions (isLeader
     *   short-circuits); a client process never becomes leader, so for callers the
     *   semantics is simply "the leader I should talk to has changed".
     * - No payload is passed: the caller fetches whatever it needs at notify time.
     *
     * @param callback    [in] C callback: void (*)(void *userData); NULL to unregister
     * @param userData    [in] opaque user data passed back to the callback
     * @return SM_OK on success; SM_ERROR when not initialized or when the store is
     *         not HaConfigStore (non-etcd mode, caller should skip registration)
     */
    using LeaderChangedFunc = void (*)(void *userData);
    Result RegisterLeaderChangeCallback(LeaderChangedFunc callback, void *userData) noexcept;

    inline uint32_t GetRankId() const
    {
        return config_.rankId;
    }

    inline uint32_t GetWorldSize() const
    {
        return worldSize_;
    }

    inline uint16_t GetDeviceId() const
    {
        return deviceId_;
    }

    inline std::string GetHcomUrl() const
    {
        return config_.hcomUrl;
    }

    inline smem_tls_config GetHcomTlsOption() const
    {
        return config_.hcomTlsConfig;
    }

    inline const smem_bm_config_t &GetConfig() const
    {
        return config_;
    }

private:
    int32_t PrepareStore();
    int32_t RacingForStoreServer();
    int32_t AutoRanking();

private:
    std::mutex entryMutex_;
    std::map<uintptr_t, SmemBmEntryPtr> ptr2EntryMap_; /* lookup entry by ptr */
    std::map<uint32_t, SmemBmEntryPtr> entryIdMap_;    /* deduplicate entry by id */
    smem_bm_config_t config_{};
    std::string storeURL_;
    uint32_t worldSize_{0};
    uint16_t deviceId_{0};
    bool inited_ = false;
    UrlExtraction storeUrlExtraction_;
    StorePtr confStore_ = nullptr;
};

} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_BM_ENTRY_MANAGER_H
