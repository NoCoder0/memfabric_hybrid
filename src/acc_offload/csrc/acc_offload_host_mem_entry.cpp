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
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <vector>
#include "hybm_big_mem.h"
#include "hybm_define.h"
#include "smem_net_common.h"
#include "smem_store_factory.h"
#include "mf_env_define.h"
#include "dl_acl_api.h"
#include "acc_offload_define.h"
#include "acc_offload_launch.h"
#include "acc_offload_mem_manager.h"
#include "acc_offload_pool_fingerprint.h"
#include "acc_offload_host_mem_entry.h"

namespace ock {
namespace offload {

using namespace ock::smem;

namespace {
constexpr uint64_t GB = 1024ULL * 1024ULL * 1024ULL;

uint64_t AlignUp(uint64_t value, uint64_t align) noexcept
{
    return (value + align - 1) & ~(align - 1);
}

/* host-mem instances may share one store URL; the factory entry may be erased only after the
 * last local user is gone, otherwise a new instance on the URL cannot rebind the port */
std::mutex gStoreUrlMutex;
std::map<std::string, uint64_t> gStoreUrlUsers;

void AcquireStoreUrl(const std::string &storeUrl)
{
    std::lock_guard<std::mutex> lock(gStoreUrlMutex);
    ++gStoreUrlUsers[storeUrl];
}

bool ReleaseStoreUrl(const std::string &storeUrl)
{
    std::lock_guard<std::mutex> lock(gStoreUrlMutex);
    auto it = gStoreUrlUsers.find(storeUrl);
    if (it == gStoreUrlUsers.end()) {
        OFFLOAD_LOG_ERROR("store url released without acquire, storeUrl: " << storeUrl);
        return false;
    }
    if (--it->second != 0) {
        return false;
    }
    gStoreUrlUsers.erase(it);
    return true;
}

/* Wire format: [descLen (4B)][desc (descLen)]; desc is process-local, never gather it as-is. */
int32_t SerializeExchangeInfos(const std::vector<hybm_exchange_info> &infos, uint32_t selfRank,
                               std::vector<uint8_t> &payload)
{
    for (const auto &info : infos) {
        if (info.desc == nullptr || info.descLen == 0) {
            OFFLOAD_LOG_ERROR("exchange info invalid, descLen: " << info.descLen << ", rankId: " << selfRank);
            return OFFLOAD_ERROR;
        }
        const auto *lenBytes = reinterpret_cast<const uint8_t *>(&info.descLen);
        payload.insert(payload.end(), lenBytes, lenBytes + sizeof(info.descLen));
        payload.insert(payload.end(), info.desc, info.desc + info.descLen);
    }
    return OFFLOAD_OK;
}

/* Rebuild a peer's infos from its chunk; desc is new[]-owned, freed by hybm_export_info_free. */
int32_t ParseExchangeInfos(const uint8_t *chunk, uint32_t chunkLen, uint32_t peerRank, uint32_t selfRank,
                           std::vector<hybm_exchange_info> &out)
{
    uint32_t offset = 0;
    while (offset + sizeof(uint32_t) <= chunkLen) {
        uint32_t descLen = 0;
        memcpy(&descLen, chunk + offset, sizeof(descLen));
        offset += sizeof(descLen);
        if (descLen == 0 || descLen > chunkLen - offset) {
            OFFLOAD_LOG_ERROR("exchange payload corrupt, peerRank: " << peerRank << ", offset: " << offset
                                                                     << ", descLen: " << descLen << ", chunkLen: "
                                                                     << chunkLen << ", rankId: " << selfRank);
            return OFFLOAD_ERROR;
        }
        auto *desc = new (std::nothrow) uint8_t[descLen];
        if (desc == nullptr) {
            OFFLOAD_LOG_ERROR("alloc exchange desc failed, descLen: " << descLen << ", peerRank: " << peerRank
                                                                      << ", rankId: " << selfRank);
            return OFFLOAD_ERROR;
        }
        memcpy(desc, chunk + offset, descLen);
        offset += descLen;
        out.push_back({desc, descLen});
    }
    return OFFLOAD_OK;
}

void FreeExchangeInfos(std::vector<hybm_exchange_info> &infos)
{
    for (auto &info : infos) {
        hybm_export_info_free(&info);
    }
    infos.clear();
}

/* size allgather -> payload allgather -> import -> barrier; one call per host_register round */
int32_t AllGatherAndImportPeers(const SmemGroupEnginePtr &group, const std::vector<hybm_exchange_info> &localInfos,
                                hybm_entity_t entity, uint32_t importFlags, uint32_t rankCount, uint32_t selfRank)
{
    std::vector<uint8_t> localPayload;
    auto ret = SerializeExchangeInfos(localInfos, selfRank, localPayload);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    uint32_t payloadSize = static_cast<uint32_t>(localPayload.size());
    std::vector<uint32_t> payloadSizes(rankCount, 0);
    ret = group->GroupAllGather(reinterpret_cast<const char *>(&payloadSize), sizeof(uint32_t),
                                reinterpret_cast<char *>(payloadSizes.data()), rankCount * sizeof(uint32_t));
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("exchange size allgather failed, result: " << ret << ", rankId: " << selfRank
                                                                     << ", rankCount: " << rankCount);
        return ret;
    }
    uint32_t maxPayload = *std::max_element(payloadSizes.cbegin(), payloadSizes.cend());
    if (maxPayload == 0) {
        return group->GroupBarrier();
    }
    localPayload.resize(maxPayload, 0);
    std::vector<uint8_t> allPayloads(static_cast<size_t>(rankCount) * maxPayload, 0);
    ret = group->GroupAllGather(reinterpret_cast<const char *>(localPayload.data()), maxPayload,
                                reinterpret_cast<char *>(allPayloads.data()), rankCount * maxPayload);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("exchange payload allgather failed, result: " << ret << ", rankId: " << selfRank
                                                                        << ", maxPayload: " << maxPayload);
        return ret;
    }

    std::vector<hybm_exchange_info> peerInfos;
    for (uint32_t r = 0; r < rankCount; r++) {
        if (r == selfRank) {
            continue;
        }
        const uint8_t *chunk = allPayloads.data() + static_cast<size_t>(r) * maxPayload;
        if (ParseExchangeInfos(chunk, payloadSizes[r], r, selfRank, peerInfos) != OFFLOAD_OK) {
            FreeExchangeInfos(peerInfos);
            return OFFLOAD_ERROR;
        }
    }
    if (!peerInfos.empty()) {
        size_t peerCount = peerInfos.size();
        ret = hybm_import(entity, peerInfos.data(), peerCount, nullptr, importFlags);
        FreeExchangeInfos(peerInfos);
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_ERROR("hybm import peer infos failed, result: " << ret << ", count: " << peerCount
                                                                        << ", rankId: " << selfRank);
            return ret;
        }
    }
    return group->GroupBarrier();
}
} // namespace

int32_t AccOffloadHostMemEntry::ValidateConfig(const offload_config_t &config)
{
    if (config.reserveSize == 0) {
        OFFLOAD_LOG_ERROR(
            "host mem entry requires reserveSize (slot stride) > 0, scene: " << static_cast<uint32_t>(config.scene));
        return OFFLOAD_ERROR;
    }
    if ((config.flags & OFFLOAD_FLAG_GIANT_PAGE) != 0U && config.scene == OFFLOAD_SCENE_SHARED) {
        OFFLOAD_LOG_ERROR("SHARED is always vmm on the host mem entry, GIANT_PAGE flag is LOCAL only");
        return OFFLOAD_ERROR;
    }
    if (config.scene == OFFLOAD_SCENE_SHARED) {
        if (config.worldSize == 0 || config.rankId >= config.worldSize) {
            OFFLOAD_LOG_ERROR("invalid shared geometry, rankId: " << config.rankId
                                                                  << ", worldSize: " << config.worldSize);
            return OFFLOAD_ERROR;
        }
        if (config.localWorldSize != 0 &&
            (config.localWorldSize > config.worldSize || config.worldSize % config.localWorldSize != 0)) {
            OFFLOAD_LOG_ERROR("invalid localWorldSize, localWorldSize: " << config.localWorldSize
                                                                         << ", worldSize: " << config.worldSize);
            return OFFLOAD_ERROR;
        }
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadHostMemEntry::CreateEntity(const offload_config_t &config)
{
    options_ = {};
    options_.bmType = HYBM_TYPE_HOST_INITIATE;
    options_.memType = HYBM_MEM_TYPE_HOST;
    options_.bmDataOpType = HYBM_DOP_TYPE_MTE;
    options_.rankCount = (config.scene == OFFLOAD_SCENE_SHARED) ? config.worldSize : 1;
    options_.rankId = (config.scene == OFFLOAD_SCENE_SHARED) ? config.rankId : 0;
    options_.devId = config.deviceId;
    options_.maxDRAMSize = AlignUp(config.reserveSize, GB);
    options_.hostVASpace = 0; /* geometry only at init; the pool slice is backed as a whole right after */
    options_.role = HYBM_ROLE_PEER;
    options_.scene = HYBM_SCENE_OFFLOAD;
    options_.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
    options_.dramShmFd = -1;
    options_.enable56BitsGva = false;
    bzero(options_.transUrl, sizeof(options_.transUrl));
    if (config.scene == OFFLOAD_SCENE_SHARED) {
        if (storeUrl_.size() >= sizeof(options_.transUrl)) {
            OFFLOAD_LOG_ERROR("store url too long, storeUrl: " << storeUrl_ << ", rankId: " << options_.rankId);
            return OFFLOAD_ERROR;
        }
        std::copy_n(storeUrl_.c_str(), storeUrl_.size(), options_.transUrl);
    }
    /* A5: LOCAL + GIANT_PAGE routes to the D2RH conn form, other forms stay on the default
     * vmm path — never set HYBM_FLAG_UNRESTRICTED_MEM (it breaks the A3 dual-machine SHARED
     * pool). A3: the default vmm gives 1G giant pages + NUMA. The pool itself is one slice
     * of maxDRAMSize (GB-aligned), so the driver's rejection of sub-GB HalMemMap on
     * giant-page handles never bites. */
    if (ock::mf::DlAclApi::GetAscendSocType() == ock::mf::AscendSocType::ASCEND_950) {
        if (config.scene == OFFLOAD_SCENE_LOCAL && (config.flags & OFFLOAD_FLAG_GIANT_PAGE) != 0U) {
            options_.flags |= HYBM_FLAG_OFFLOAD_GIANT_PAGE;
        }
    }

    entity_ = hybm_create_entity(entityId_, &options_, 0);
    if (entity_ == nullptr) {
        OFFLOAD_LOG_ERROR("create entity failed, entityId: " << entityId_
                                                             << ", scene: " << static_cast<uint32_t>(config.scene));
        return OFFLOAD_ERROR;
    }
    auto ret = hybm_reserve_mem_space(entity_, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("reserve mem space failed, result: " << ret << ", entityId: " << entityId_);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadHostMemEntry::AllocatePool()
{
    poolSlice_ = hybm_alloc_local_memory(entity_, HYBM_MEM_TYPE_HOST, options_.maxDRAMSize, 0);
    if (poolSlice_ == nullptr) {
        OFFLOAD_LOG_ERROR("alloc host pool failed, poolSize: " << options_.maxDRAMSize
                                                               << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    poolBase_ = hybm_get_slice_va(entity_, poolSlice_, 0);
    if (poolBase_ == nullptr) {
        OFFLOAD_LOG_ERROR("get host pool va failed, poolSize: " << options_.maxDRAMSize
                                                                << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    memManager_ = std::make_unique<AccOffloadMemManager>(poolBase_, options_.maxDRAMSize);
    return OFFLOAD_OK;
}

/* store url order: config.storeUrl > ASCEND_MF_STORE_URL > derived loopback port (single machine). */
int32_t AccOffloadHostMemEntry::ResolveStoreUrl(const offload_config_t &config)
{
    if (config.storeUrl[0] != '\0') {
        storeUrl_ = config.storeUrl;
        return OFFLOAD_OK;
    }
    const std::string &envUrl = mf::env::MF_CONFIG_STORE_URL;
    if (!envUrl.empty()) {
        storeUrl_ = envUrl;
        return OFFLOAD_OK;
    }
    uint32_t localWorld = NormalizeLocalWorldSize(config.worldSize, config.localWorldSize);
    if (localWorld != config.worldSize) {
        OFFLOAD_LOG_ERROR("multi-machine pool needs an explicit store (config.storeUrl or ASCEND_MF_STORE_URL), "
                          "rankId: "
                          << config.rankId << ", worldSize: " << config.worldSize
                          << ", localWorldSize: " << config.localWorldSize);
        return OFFLOAD_ERROR;
    }
    constexpr int portBase = 8500;
    storeUrl_ = "tcp://127.0.0.1:" + std::to_string(portBase + config.deviceId / localWorld);
    return OFFLOAD_OK;
}

int32_t AccOffloadHostMemEntry::CreateGroupStore(const offload_config_t &config)
{
    UrlExtraction extraction;
    if (extraction.ExtractIpPortFromUrl(storeUrl_) != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("extract ip port from url failed, storeUrl: " << storeUrl_ << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }

    smem_tls_config tlsCfg{};
    StoreFactory::SetTlsInfo(tlsCfg);
    bool startServer = (config.rankId == 0);
    uint16_t model = startServer ? CSM_BOTH : CSM_CLIENT;
    auto baseStore = StoreFactory::CreateStoreByUrl(storeUrl_, model, config.worldSize, config.rankId);
    if (baseStore == nullptr) {
        OFFLOAD_LOG_ERROR("create store failed, storeUrl: " << storeUrl_ << ", rankId: " << config.rankId
                                                            << ", worldSize: " << config.worldSize);
        return OFFLOAD_ERROR;
    }
    AcquireStoreUrl(storeUrl_);
    storeUrlHeld_ = true;
    /* entity id in the key prefix: keeps this pool's store namespace distinct from the legacy one */
    auto prefix = "(" + std::to_string(entityId_) + ")_";
    auto offloadStore = StoreFactory::PrefixStore(baseStore, "OFFLOAD_");
    entryStore_ = StoreFactory::PrefixStore(offloadStore, prefix);
    if (entryStore_ == nullptr) {
        OFFLOAD_LOG_ERROR("create prefix store failed, storeUrl: " << storeUrl_);
        return OFFLOAD_ERROR;
    }

    SmemGroupOption groupOpt = {
        config.worldSize, config.rankId, SMEM_DEFAUT_WAIT_TIME * SECOND_TO_MILLSEC, false, nullptr, nullptr,
        nullptr,          nullptr};
    group_ = SmemNetGroupEngine::Create(entryStore_, groupOpt);
    if (group_ == nullptr) {
        OFFLOAD_LOG_ERROR("create net group failed, rankId: " << config.rankId << ", worldSize: " << config.worldSize
                                                              << ", storeUrl: " << storeUrl_);
        return OFFLOAD_ERROR;
    }
    auto ret = group_->GroupBarrier();
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("group barrier after create failed, result: " << ret << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadHostMemEntry::ExchangeAndValidatePoolFingerprint(const offload_config_t &config)
{
    ock::mf::AscendSocType socType = ock::mf::DlAclApi::GetAscendSocType();
    PoolFingerprint self =
        MakePoolFingerprint(reinterpret_cast<uint64_t>(poolGva_), options_.maxDRAMSize, options_.hostVASpace,
                            config.worldSize, NormalizeLocalWorldSize(config.worldSize, config.localWorldSize),
                            config.rankId, static_cast<uint32_t>(socType));
    std::vector<PoolFingerprint> all(config.worldSize);
    auto ret = group_->GroupAllGather(reinterpret_cast<const char *>(&self), sizeof(PoolFingerprint),
                                      reinterpret_cast<char *>(all.data()), sizeof(PoolFingerprint) * config.worldSize);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("pool fingerprint allgather failed, result: " << ret << ", rankId: " << config.rankId
                                                                        << ", worldSize: " << config.worldSize
                                                                        << ", storeUrl: " << storeUrl_);
        return OFFLOAD_ERROR;
    }

    int32_t localRet = ValidatePoolFingerprints(all.data(), static_cast<uint32_t>(all.size()));
    std::vector<std::pair<int, int>> errList;
    ret = group_->GroupGatherResult(localRet, errList);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("pool fingerprint gather result failed, result: " << ret << ", rankId: " << config.rankId
                                                                            << ", storeUrl: " << storeUrl_);
        return OFFLOAD_ERROR;
    }
    if (!errList.empty()) {
        OFFLOAD_LOG_ERROR("pool fingerprint check failed on remote ranks, failedCount: "
                          << errList.size() << ", firstFailedRank: " << errList.front().first << ", errCode: "
                          << errList.front().second << ", rankId: " << config.rankId << ", poolGva: " << self.poolGva
                          << ", worldSize: " << self.worldSize << ", storeUrl: " << storeUrl_);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadHostMemEntry::Initialize(const offload_config_t &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        OFFLOAD_LOG_ERROR(
            "host mem entry already initialized, call offload_uninit first, scene: " << static_cast<uint32_t>(scene_));
        return OFFLOAD_ERROR;
    }

    int32_t ret = ValidateConfig(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    scene_ = config.scene;

    ret = hybm_init(config.deviceId, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("hybm_init failed, result: " << ret << ", deviceId: " << config.deviceId);
        return OFFLOAD_ERROR;
    }
    hybmInited_ = true;
    do {
        if (scene_ == OFFLOAD_SCENE_SHARED) {
            ret = ResolveStoreUrl(config);
            if (ret != OFFLOAD_OK) {
                break;
            }
        }
        ret = CreateEntity(config);
        if (ret != OFFLOAD_OK) {
            break;
        }
        ret = AllocatePool();
        if (ret != OFFLOAD_OK) {
            break;
        }
        ret = AccOffloadLaunchApi::TryLoadLibrary();
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_ERROR("offload launch load library failed, deviceId: " << config.deviceId);
            break;
        }
        launchLoaded_ = true;
        if (scene_ == OFFLOAD_SCENE_SHARED) {
            ret = CreateGroupStore(config);
            if (ret != OFFLOAD_OK) {
                break;
            }
            poolGva_ = hybm_get_memory_ptr(entity_, HYBM_MEM_TYPE_HOST);
            if (poolGva_ == nullptr) {
                OFFLOAD_LOG_ERROR("get pool gva failed, rankId: " << config.rankId);
                ret = OFFLOAD_ERROR;
                break;
            }
            ret = ExchangeAndValidatePoolFingerprint(config);
            if (ret != OFFLOAD_OK) {
                break;
            }
            ret = RegisterPoolShared();
            if (ret != OFFLOAD_OK) {
                break;
            }
        }
    } while (0);

    if (ret != OFFLOAD_OK) {
        UnInitialize();
        return ret;
    }
    inited_ = true;
    OFFLOAD_LOG_INFO("host mem entry initialized, scene: "
                     << static_cast<uint32_t>(scene_) << ", deviceId: " << config.deviceId << ", poolSize: "
                     << options_.maxDRAMSize << ", poolBase: " << reinterpret_cast<uint64_t>(poolBase_)
                     << ", poolGva: " << reinterpret_cast<uint64_t>(poolGva_) << ", rankId: " << options_.rankId
                     << ", worldSize: " << options_.rankCount << ", storeUrl: " << storeUrl_
                     << ", giantPage: " << ((options_.flags & HYBM_FLAG_OFFLOAD_GIANT_PAGE) != 0));
    return OFFLOAD_OK;
}

void AccOffloadHostMemEntry::UnInitialize()
{
    /* lock-free: Initialize's failure path calls this under mutex_ */
    if (!inited_ && entity_ == nullptr && !hybmInited_ && !launchLoaded_) {
        return;
    }

    uint32_t flags = 0;
    memManager_.reset();
    if (poolSlice_ != nullptr) {
        auto ret = hybm_free_local_memory(entity_, poolSlice_, 1, flags);
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_WARN("free host pool failed during uninit, result: " << ret << ", poolBase: "
                                                                             << reinterpret_cast<uint64_t>(poolBase_)
                                                                             << ", poolSize: " << options_.maxDRAMSize);
        }
        poolSlice_ = nullptr;
    }
    poolBase_ = nullptr;
    if (group_ != nullptr) {
        group_->GroupSnClean();
        group_ = nullptr;
    }
    if (entity_ != nullptr) {
        hybm_unreserve_mem_space(entity_, flags);
        hybm_destroy_entity(entity_, flags);
        entity_ = nullptr;
    }
    entryStore_ = nullptr;
    if (storeUrlHeld_ && ReleaseStoreUrl(storeUrl_)) {
        StoreFactory::DestroyStore(storeUrl_);
    }
    storeUrlHeld_ = false;
    storeUrl_.clear();
    if (launchLoaded_) {
        AccOffloadLaunchApi::CleanupLibrary();
        launchLoaded_ = false;
    }
    if (hybmInited_) {
        hybm_uninit();
        hybmInited_ = false;
    }

    poolGva_ = nullptr;
    options_ = {};
    inited_ = false;
    OFFLOAD_LOG_INFO("host mem entry uninitialized");
}

bool AccOffloadHostMemEntry::InPool(uint64_t hostPtr) const
{
    auto base = reinterpret_cast<uint64_t>(poolBase_);
    return poolBase_ != nullptr && hostPtr >= base && hostPtr < base + options_.maxDRAMSize;
}

uint64_t AccOffloadHostMemEntry::HostMalloc(uint64_t size, uint64_t offloadFlags)
{
    (void)offloadFlags;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        OFFLOAD_LOG_ERROR("entry not initialized, host malloc failed, size: " << size);
        return 0;
    }
    if (size == 0) {
        OFFLOAD_LOG_ERROR("host malloc size is 0");
        return 0;
    }
    /* in-pool split (16B-aligned best-fit); the whole reserve was backed at initialize */
    auto *ptr = memManager_->Allocate(size);
    if (ptr == nullptr) {
        OFFLOAD_LOG_ERROR("host malloc failed, size: " << size << ", poolSize: " << options_.maxDRAMSize
                                                       << ", rankId: " << options_.rankId);
        return 0;
    }
    OFFLOAD_LOG_INFO("host malloc success, hostPtr: " << ptr << ", size: " << size);
    return reinterpret_cast<uint64_t>(ptr);
}

int32_t AccOffloadHostMemEntry::RegisterPoolShared()
{
    hybm_exchange_info sliceInfo{};
    auto ret = hybm_export(entity_, poolSlice_, 0, &sliceInfo);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("export host pool failed, result: " << ret << ", poolSize: " << options_.maxDRAMSize
                                                              << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    std::vector<hybm_exchange_info> sliceInfos{sliceInfo};
    ret = AllGatherAndImportPeers(group_, sliceInfos, entity_, 0, options_.rankCount, options_.rankId);
    hybm_export_info_free(&sliceInfo);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("allgather/import host pool failed, result: " << ret << ", poolSize: " << options_.maxDRAMSize
                                                                        << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    ret = hybm_mmap(entity_, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("hybm mmap failed, result: " << ret << ", poolSize: " << options_.maxDRAMSize
                                                       << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadHostMemEntry::HostRegister(uint64_t hostPtr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        OFFLOAD_LOG_ERROR("entry not initialized, host register failed, hostPtr: " << hostPtr);
        return OFFLOAD_ERROR;
    }
    if (!InPool(hostPtr)) {
        OFFLOAD_LOG_ERROR("host ptr is outside the host mem pool, hostPtr: "
                          << hostPtr << ", poolBase: " << reinterpret_cast<uint64_t>(poolBase_)
                          << ", poolSize: " << options_.maxDRAMSize << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    /* the pool was registered up-front at initialize (once, whole-pool in SHARED);
     * repeated calls stay idempotent for caller-side per-block sequences */
    OFFLOAD_LOG_INFO("host register ok on the pre-registered pool, hostPtr: " << hostPtr);
    return OFFLOAD_OK;
}

int32_t AccOffloadHostMemEntry::HostGetDevicePtr(uint64_t hostPtr, uint64_t *dvaPtr)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_ || dvaPtr == nullptr) {
        OFFLOAD_LOG_ERROR("entry not initialized or dvaPtr is null, inited: " << inited_ << ", hostPtr: " << hostPtr);
        return OFFLOAD_ERROR;
    }
    if (!InPool(hostPtr)) {
        OFFLOAD_LOG_ERROR("host ptr is outside the host mem pool, hostPtr: "
                          << hostPtr << ", poolBase: " << reinterpret_cast<uint64_t>(poolBase_)
                          << ", poolSize: " << options_.maxDRAMSize << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    auto ret = hybm_gva_to_va(hostPtr, HYBM_MEM_TYPE_DEVICE, dvaPtr);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("gva to dva failed, result: " << ret << ", hostPtr: " << std::hex << hostPtr);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadHostMemEntry::HostFree(uint64_t hostPtr, uint64_t offloadFlags)
{
    (void)offloadFlags;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        OFFLOAD_LOG_ERROR("entry not initialized, host free failed, hostPtr: " << hostPtr);
        return OFFLOAD_ERROR;
    }
    auto ret = memManager_->Release(reinterpret_cast<void *>(hostPtr));
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("free host block failed, hostPtr: "
                          << hostPtr << ", poolBase: " << reinterpret_cast<uint64_t>(poolBase_)
                          << ", poolSize: " << options_.maxDRAMSize << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

void *AccOffloadHostMemEntry::MallocHost(size_t size)
{
    /* legacy offload_malloc is rejected here; stub satisfies the base-class contract */
    OFFLOAD_LOG_ERROR("legacy malloc (offload_malloc) is not supported on the host mem entry, size: " << size);
    return nullptr;
}

void AccOffloadHostMemEntry::FreeHost(void *ptr)
{
    /* nothing to release; log so the legacy call site is traceable */
    OFFLOAD_LOG_ERROR(
        "legacy free (offload_free) is not supported on the host mem entry, ptr: " << reinterpret_cast<uint64_t>(ptr));
}

} // namespace offload
} // namespace ock
