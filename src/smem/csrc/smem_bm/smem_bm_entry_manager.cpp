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
#include <thread>
#include <algorithm>

#include "mf_ipv4_validator.h"
#include "smem_net_common.h"
#include "smem_net_group_engine.h"
#include "smem_store_factory.h"
#include "smem_tcp_config_store.h"
#include "smem_ha_config_store.h"
#include "network_endpoint_util.h"

#include "smem_bm_entry_manager.h"

namespace ock {
namespace smem {

SmemBmEntryManager &SmemBmEntryManager::Instance()
{
    static SmemBmEntryManager instance;
    return instance;
}

SmemBmEntryManager::~SmemBmEntryManager()
{
    // 防止析构函数里面调用Uninitialize
    for (auto &pair : ptr2EntryMap_) {
        pair.second->Uninitialize();
    }
    ptr2EntryMap_.clear();
    for (auto &map : entryIdMap_) {
        map.second->Uninitialize();
    }
    entryIdMap_.clear();
}

Result SmemBmEntryManager::Initialize(const std::string &storeURL, uint32_t worldSize, uint16_t deviceId,
                                      const smem_bm_config_t &config)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    if (inited_) {
        SM_LOG_WARN("smem bm manager has already initialized");
        return SM_OK;
    }

    SM_VALIDATE_RETURN(worldSize != 0, "invalid param, worldSize is 0", SM_INVALID_PARAM);

    storeURL_ = storeURL;
    worldSize_ = worldSize;
    deviceId_ = deviceId;
    config_ = config;

    // Retry loop for server recovery window (~60s). During recovery, new connections
    // (reconnect=0) are rejected with SM_RECONNECT. Retry with 20s interval to cover
    // both auto-ranking and specified-rank paths.  AutoRanking's internal retry is
    // removed — the outer loop handles it uniformly.
    constexpr uint32_t maxRetries = 5;
    constexpr uint32_t retryIntervalSec = 20;
    uint32_t attempt = 0;
    Result ret = SM_OK;
    while (attempt++ < maxRetries) {
        ret = PrepareStore();
        if (ret == SM_OK && config_.autoRanking) {
            ret = AutoRanking();
        }
        if (ret == SM_OK) {
            break;
        }
        SM_LOG_ERROR("[RECOVER] Initialize attempt " << attempt << "/" << maxRetries << " failed, ret=" << ret
                                                     << " deviceId=" << deviceId_);
        if (attempt < maxRetries) {
            confStore_ = nullptr;
            StoreFactory::DestroyStore(storeURL_);
            std::this_thread::sleep_for(std::chrono::seconds(retryIntervalSec));
        }
    }
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "initialize failed: " << ret);

    inited_ = true;
    SM_LOG_INFO("initialize store(" << storeURL << ") world size(" << worldSize << ") device(" << deviceId << ") OK.");
    return SM_OK;
}

int32_t SmemBmEntryManager::PrepareStore()
{
    SM_ASSERT_RETURN(storeUrlExtraction_.ExtractIpPortFromUrl(storeURL_) == SM_OK, SM_INVALID_PARAM);
    StoreFactory::SetTlsInfo(config_.storeTlsConfig);
    if (!config_.autoRanking) {
        SM_ASSERT_RETURN(config_.rankId < worldSize_, SM_INVALID_PARAM);
        uint16_t model = (config_.rankId == 0 && config_.startConfigStoreServer) ? CSM_BOTH : CSM_CLIENT;
        confStore_ = StoreFactory::CreateStoreByUrl(storeURL_, model, worldSize_, static_cast<int>(config_.rankId));
        SM_ASSERT_RETURN(confStore_ != nullptr, StoreFactory::GetFailedReason());
        // In HA (etcd) mode CreateHaStore constructs the clientDelegate with the
        // default rankId=-1, so its localRankId_ is UINT32_MAX until SetRankId is
        // called. Propagate the configured rankId here so the join path (which
        // uses the delegate's localRankId_ via EnableAsyncMode/PackJoin) sends the
        // correct rank. TcpConfigStore already received rankId in the TCP path,
        // so this is a no-op there.
        auto mgr = Convert<ConfigStore, ConfigStoreManager>(confStore_);
        if (mgr.Get() != nullptr) {
            mgr->SetRankId(config_.rankId);
        }
    } else {
        if (config_.startConfigStoreServer) {
            auto ret = RacingForStoreServer();
            SM_ASSERT_RETURN(ret == SM_OK, ret);
        }

        if (confStore_ == nullptr) {
            confStore_ = StoreFactory::CreateStoreByUrl(storeURL_, CSM_CLIENT, worldSize_);
            SM_ASSERT_RETURN(confStore_ != nullptr, StoreFactory::GetFailedReason());
        }
    }
    confStore_ = StoreFactory::PrefixStore(confStore_, "BM_");
    return SM_OK;
}

int32_t SmemBmEntryManager::RacingForStoreServer()
{
    std::string localIp;
    auto success = NetworkEndpointUtil::GetLocalIpWithTarget(storeUrlExtraction_.ip, localIp);
    SM_ASSERT_RETURN(success, SM_ERROR);
    if (localIp != storeUrlExtraction_.ip && !ock::mf::NetValidator::IsZeroIpV4(storeUrlExtraction_.ip)) {
        SM_LOG_INFO("not local ip, skip create store server, ip:" << storeUrlExtraction_.ip);
        return SM_OK;
    }

    confStore_ = StoreFactory::CreateStoreByUrl(storeURL_, CSM_BOTH, worldSize_);
    if (confStore_ != nullptr || StoreFactory::GetFailedReason() == SM_RESOURCE_IN_USE) {
        return SM_OK;
    }

    return StoreFactory::GetFailedReason();
}

int32_t SmemBmEntryManager::AutoRanking()
{
    std::vector<uint8_t> rankIdData;
    auto ret = confStore_->GetCoreStore()->Get(AutoRankingStr, rankIdData, SMEM_DEFAUT_WAIT_TIME * SECOND_TO_MILLSEC);
    if (ret == SM_OK && rankIdData.size() == sizeof(uint32_t)) {
        union Transfer {
            uint32_t rankId;
            uint8_t data[4];
        } trans{};
        std::copy_n(rankIdData.begin(), sizeof(trans.data), trans.data);
        config_.rankId = trans.rankId;
        auto tcpConfigStore = Convert<ConfigStore, ConfigStoreManager>(confStore_);
        tcpConfigStore->SetRankId(config_.rankId);
        SM_LOG_INFO("Success to auto ranking rankId: " << trans.rankId << " deviceId: " << deviceId_);
        return SM_OK;
    }
    SM_LOG_ERROR("AutoRanking failed, deviceId: " << deviceId_ << ", ret: " << ret
                                                  << ", dataSize: " << rankIdData.size());
    return SM_ERROR;
}

Result SmemBmEntryManager::CreateEntryById(uint32_t id, SmemBmEntryPtr &entry /* out */)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the bm entry exists or not with lock */
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    SM_VALIDATE_RETURN(id < HYBM_ENTITY_ID_SEGMENT_SIZE,
                       "invalid id: " << id << ", valid range:  [0, " << HYBM_ENTITY_ID_SEGMENT_SIZE << ")",
                       SM_INVALID_PARAM);
    auto iter = entryIdMap_.find(id);
    if (iter != entryIdMap_.end()) {
        SM_LOG_WARN("unable to create bm entry as already exists, id: " << id);
        return SM_DUPLICATED_OBJECT;
    }

    /* create new bm entry */
    SmemBmEntryOptions opt{id, config_.rankId, config_.dynamicWorldSize, config_.controlOperationTimeout};
    auto store = StoreFactory::PrefixStore(confStore_, std::string("(").append(std::to_string(id)).append(")_"));
    if (store == nullptr) {
        SM_LOG_ERROR("create new prefix store for entity: " << id << " failed");
        return SM_ERROR;
    }

    auto tmpEntry = SmMakeRef<SmemBmEntry>(opt, store);
    SM_ASSERT_RETURN(tmpEntry != nullptr, SM_NEW_OBJECT_FAILED);
    tmpEntry->SetSmemFlags(config_.flags);

    /* add into set and map */
    entryIdMap_.emplace(id, tmpEntry);
    ptr2EntryMap_.emplace(reinterpret_cast<uintptr_t>(tmpEntry.Get()), tmpEntry);

    /* assign out object ptr */
    entry = tmpEntry;
    SM_LOG_DEBUG("create new bm entry success, id: " << id);
    return SM_OK;
}

Result SmemBmEntryManager::GetEntryByPtr(uintptr_t ptr, SmemBmEntryPtr &entry)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the bm entry exists or not with lock */
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    auto iter = ptr2EntryMap_.find(ptr);
    if (iter != ptr2EntryMap_.end()) {
        entry = iter->second;
        return SM_OK;
    }

    SM_LOG_DEBUG("not found bm entry");
    return SM_OBJECT_NOT_EXISTS;
}

Result SmemBmEntryManager::GetEntryById(uint32_t id, SmemBmEntryPtr &entry)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the bm entry exists or not with lock */
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    auto iter = entryIdMap_.find(id);
    if (iter != entryIdMap_.end()) {
        entry = iter->second;
        return SM_OK;
    }

    SM_LOG_DEBUG("not found bm entry with id " << id);
    return SM_OBJECT_NOT_EXISTS;
}

Result SmemBmEntryManager::RemoveEntryByPtr(uintptr_t ptr)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the bm entry exists or not with lock */
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    auto iter = ptr2EntryMap_.find(ptr);
    if (iter == ptr2EntryMap_.end()) {
        SM_LOG_DEBUG("not found bm entry");
        return SM_OBJECT_NOT_EXISTS;
    }

    /* assign to a tmp ptr and remove from map */
    auto entry = iter->second;
    ptr2EntryMap_.erase(iter);

    /* remove from id set */
    SM_ASSERT_RETURN(entry != nullptr, SM_ERROR);
    entryIdMap_.erase(entry->Id());

    SM_LOG_DEBUG("remove bm entry success, id: " << entry->Id());

    return SM_OK;
}

Result SmemBmEntryManager::UpdateStoreUrl(const std::string &storeURL)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    SM_VALIDATE_RETURN(!storeURL.empty(), "invalid param, storeURL is empty", SM_INVALID_PARAM);

    if (storeURL == storeURL_) {
        SM_LOG_INFO("store URL is the same, skip update: " << storeURL);
        return SM_OK;
    }

    SM_LOG_INFO("update store URL from " << storeURL_ << " to " << storeURL);

    // Parse new URL to extract IP and port
    UrlExtraction newExtraction;
    SM_ASSERT_RETURN(newExtraction.ExtractIpPortFromUrl(storeURL) == SM_OK, SM_INVALID_PARAM);

    // Lazy update: only update the underlying TcpConfigStore's server IP/port via SetServerInfo,
    // without destroying/recreating the store. SetServerInfo will close the current connection
    // if IP/port changed, and the existing reconnection mechanism (LocalNonBlockSend -> ReConnectAfterBroken)
    // will automatically reconnect using the new IP/port.
    if (confStore_ == nullptr) {
        SM_LOG_ERROR("confStore_ is null, cannot update store URL");
        return SM_ERROR;
    }

    auto coreStore = confStore_->GetCoreStore();
    auto tcpConfigStore = Convert<ConfigStore, TcpConfigStore>(coreStore);
    if (tcpConfigStore == nullptr) {
        SM_LOG_ERROR("underlying store is not TcpConfigStore, cannot update server info in-place");
        return SM_ERROR;
    }

    tcpConfigStore->SetServerInfo(newExtraction.ip, newExtraction.port);
    SM_LOG_INFO("updated server info to " << newExtraction.ip << ":" << newExtraction.port
                                          << ", reconnect will use new address");

    storeURL_ = storeURL;
    storeUrlExtraction_ = newExtraction;

    SM_LOG_INFO("update store URL success, new URL: " << storeURL_);
    return SM_OK;
}

Result SmemBmEntryManager::UpdateStoreServer(const std::string &newServerIp, uint16_t newServerPort) noexcept
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    if (confStore_ == nullptr) {
        SM_LOG_ERROR("UpdateStoreServer: confStore_ is null, not initialized");
        return SM_ERROR;
    }

    // Unwrap PrefixConfigStore → TcpConfigStore
    auto coreStore = confStore_->GetCoreStore();
    auto tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        SM_LOG_ERROR("UpdateStoreServer: failed to get TcpConfigStore from confStore_");
        return SM_ERROR;
    }

    SM_LOG_INFO("UpdateStoreServer: switching to " << newServerIp << ":" << newServerPort);
    bool wasStarted = tcpStore->SetServerInfo(newServerIp, newServerPort);
    if (wasStarted) {
        // Already started — reconnect to the new address
        auto ret = tcpStore->ReConnectAfterBroken(-1);
        if (ret != SM_OK) {
            SM_LOG_ERROR("UpdateStoreServer: ReConnectAfterBroken failed, ret=" << ret);
            return ret;
        }
        SM_LOG_INFO("UpdateStoreServer: reconnected to " << newServerIp << ":" << newServerPort);
    } else {
        SM_LOG_WARN("UpdateStoreServer: store not yet started, address updated for future ClientStart");
    }

    // Update storeURL_ to reflect the new target
    storeUrlExtraction_.ExtractIpPortFromUrl("tcp://" + newServerIp + ":" + std::to_string(newServerPort));
    return SM_OK;
}

Result SmemBmEntryManager::GetStoreServerInfo(std::string &ip, uint16_t *port) const noexcept
{
    if (confStore_ == nullptr) {
        SM_LOG_ERROR("GetStoreServerInfo: confStore_ is null, not initialized");
        return SM_ERROR;
    }

    auto coreStore = confStore_->GetCoreStore();
    auto *tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        SM_LOG_ERROR("GetStoreServerInfo: underlying store is not TcpConfigStore");
        return SM_ERROR;
    }

    ip = tcpStore->GetServerIp();
    if (ip.empty()) {
        SM_LOG_ERROR("GetStoreServerInfo: server IP is empty");
        return SM_ERROR;
    }
    *port = tcpStore->GetServerPort();

    SM_LOG_INFO("GetStoreServerInfo: ip=" << ip << ", port=" << *port);
    return SM_OK;
}

Result SmemBmEntryManager::GetMetaServiceInfo(std::string &ip, uint16_t *port) const noexcept
{
    if (confStore_ == nullptr) {
        SM_LOG_ERROR("GetMetaServiceInfo: confStore_ is null, not initialized");
        return SM_ERROR;
    }
    auto *haStore = confStore_->AsHaConfigStore();
    if (haStore == nullptr) {
        SM_LOG_ERROR("GetMetaServiceInfo: underlying store is not HaConfigStore");
        return SM_ERROR;
    }
    auto backend = haStore->GetBackend();
    if (backend == nullptr) {
        SM_LOG_ERROR("GetMetaServiceInfo: backend is null");
        return SM_ERROR;
    }
    std::string metaServiceAddr;
    auto ret = backend->Get(KEY_META_SERVICE_ADDR, metaServiceAddr);
    if (ret != StoreErrorCode::SUCCESS) {
        SM_LOG_ERROR("GetMetaServiceInfo: backend Get failed, key: " << KEY_META_SERVICE_ADDR << ", ret: " << ret);
        return SM_ERROR;
    }
    uint16_t parsedPort = 0;
    std::string host;
    if (!NetworkEndpointUtil::ExtractIpAndPort("tcp://" + metaServiceAddr, host, parsedPort)) {
        SM_LOG_ERROR("GetMetaServiceInfo: invalid MetaService address: " << metaServiceAddr);
        return SM_ERROR;
    }
    if (host.empty() || parsedPort == 0) {
        SM_LOG_ERROR("GetMetaServiceInfo: MetaService address is empty, addr: " << metaServiceAddr);
        return SM_ERROR;
    }
    ip = host;
    *port = parsedPort;
    SM_LOG_INFO("GetMetaServiceInfo: ip=" << ip << ", port=" << *port);
    return SM_OK;
}

void SmemBmEntryManager::Destroy()
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    const size_t entryCount = ptr2EntryMap_.size();
    // Uninitialize any entries that were not explicitly destroyed by the caller.
    // This ensures graceful group leave and resource release even if smem_bm_destroy()
    // was not called for every handle before smem_bm_uninit().
    for (auto &pair : ptr2EntryMap_) {
        if (pair.second != nullptr) {
            pair.second->Uninitialize();
        }
    }
    ptr2EntryMap_.clear();
    entryIdMap_.clear();
    inited_ = false;
    confStore_ = nullptr;
    StoreFactory::DestroyStore(storeURL_);
    SM_LOG_INFO("SmemBmEntryManager::Destroy complete, uninitialized entries: " << entryCount);
}

} // namespace smem
} // namespace ock
