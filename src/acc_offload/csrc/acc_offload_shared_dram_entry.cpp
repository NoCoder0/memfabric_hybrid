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
#include <new>
#include "hybm_big_mem.h"
#include "smem_net_common.h"
#include "smem_store_factory.h"
#include "mf_env_define.h"
#include "dl_acl_api.h"
#include "acc_offload_launch.h"
#include "acc_offload_pool_fingerprint.h"
#include "acc_offload_shared_dram_entry.h"

namespace ock {
namespace offload {

using namespace ock::smem;

constexpr uint64_t GB = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t HOST_MEM_SLICE_SIZE = 32ULL * GB;

static uint64_t AlignUp(uint64_t value, uint64_t align) noexcept
{
    return (value + align - 1) & ~(align - 1);
}

/* Wire format of one serialized exchange info: [descLen (4B)][desc (descLen)].
 * desc is a process-local pointer and must never be gathered as-is — the old
 * struct-array gather shipped raw pointers and mis-parsed every rank past 0. */
static int32_t SerializeExchangeInfos(const std::vector<hybm_exchange_info> &infos, uint32_t selfRank,
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

/* Rebuild one peer rank's infos from its gathered byte chunk. desc buffers are
 * new[]-allocated so the later hybm_export_info_free is the matching free. */
static int32_t ParseExchangeInfos(const uint8_t *chunk, uint32_t chunkLen, uint32_t peerRank, uint32_t selfRank,
                                  std::vector<hybm_exchange_info> &out)
{
    uint32_t offset = 0;
    while (offset + sizeof(uint32_t) <= chunkLen) {
        uint32_t descLen = 0;
        memcpy(&descLen, chunk + offset, sizeof(descLen));
        offset += sizeof(descLen);
        if (descLen == 0 || offset + descLen > chunkLen) {
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

static void FreeExchangeInfos(std::vector<hybm_exchange_info> &infos)
{
    for (auto &info : infos) {
        hybm_export_info_free(&info);
    }
    infos.clear();
}

static int32_t AllGatherAndImportPeers(const SmemGroupEnginePtr &group,
                                       const std::vector<hybm_exchange_info> &localInfos, hybm_entity_t entity,
                                       uint32_t importFlags, uint32_t rankCount, uint32_t selfRank)
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

int32_t AccOffloadSharedDramEntry::AllocAndExportHostSlices()
{
    const uint64_t totalSize = options_.hostVASpace;
    uint64_t remaining = totalSize;
    do {
        uint64_t sliceSize = (remaining >= HOST_MEM_SLICE_SIZE) ? HOST_MEM_SLICE_SIZE : remaining;
        uint64_t allocated = totalSize - remaining;
        OFFLOAD_LOG_INFO("alloc host slice progress: " << allocated << "/" << totalSize << ", sliceSize: " << sliceSize
                                                       << ", rankId: " << options_.rankId);
        auto memSlice = hybm_alloc_local_memory(entity_, HYBM_MEM_TYPE_HOST, sliceSize, 0);
        if (memSlice == nullptr) {
            OFFLOAD_LOG_ERROR("alloc host slice failed, allocated: " << allocated << ", sliceSize: " << sliceSize
                                                                     << ", totalSize: " << totalSize
                                                                     << ", rankId: " << options_.rankId);
            return OFFLOAD_ERROR;
        }
        hybm_exchange_info sliceInfo{};
        auto ret = hybm_export(entity_, memSlice, 0, &sliceInfo);
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_ERROR("export host slice failed, result: " << ret << ", sliceSize: " << sliceSize
                                                                   << ", rankId: " << options_.rankId);
            hybm_export_info_free(&sliceInfo);
            return ret;
        }
        slices_.push_back(memSlice);
        sliceInfos_.push_back(sliceInfo);
        remaining -= sliceSize;
    } while (remaining > 0);
    OFFLOAD_LOG_INFO("alloc and export host slices done, sliceCount: " << sliceInfos_.size() << ", totalSize: "
                                                                       << totalSize << ", rankId: " << options_.rankId);
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramEntry::ValidateSharedConfig(const offload_config_t &config)
{
    if (config.worldSize == 0) {
        OFFLOAD_LOG_ERROR("shared dram world size is 0, rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    if (config.rankId >= config.worldSize) {
        OFFLOAD_LOG_ERROR("rankId out of range, rankId: " << config.rankId << ", worldSize: " << config.worldSize);
        return OFFLOAD_ERROR;
    }
    /* machines carry a uniform rank count: machineRank = rankId / localWorldSize */
    if (config.localWorldSize != 0 &&
        (config.localWorldSize > config.worldSize || config.worldSize % config.localWorldSize != 0)) {
        OFFLOAD_LOG_ERROR("invalid localWorldSize, localWorldSize: " << config.localWorldSize
                                                                     << ", worldSize: " << config.worldSize
                                                                     << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    /* the rank's physical DRAM must fit its own virtual slot (stride = aligned
     * reserveSize): a bigger allocSize would spill into the neighbor slot */
    if (AlignUp(config.allocSize, GB) > AlignUp(config.reserveSize, GB)) {
        OFFLOAD_LOG_ERROR("allocSize exceeds reserveSize, allocSize: " << config.allocSize
                                                                       << ", reserveSize: " << config.reserveSize
                                                                       << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

/* Store rendezvous url resolution order: config.storeUrl > ASCEND_MF_STORE_URL
 * env > derived loopback port (single machine only; a multi-machine pool with
 * neither explicit url nor env would silently split into per-machine groups). */
int32_t AccOffloadSharedDramEntry::ResolveStoreUrl(const offload_config_t &config)
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

int32_t AccOffloadSharedDramEntry::CreateGroupStore(const offload_config_t &config)
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

    /* One pool = one store: coexisting pools (e.g. PD-disaggregated P/D) deploy
     * with independent store urls, so the fixed entity-id key prefix needs no
     * extra namespace. */
    auto prefix = "(" + std::to_string(HYBM_ENTITY_ID_OFFLOAD_BASE) + ")_";
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

int32_t AccOffloadSharedDramEntry::CreateEntityAndPool(const offload_config_t &config)
{
    options_ = {};
    options_.bmType = HYBM_TYPE_HOST_INITIATE;
    options_.memType = HYBM_MEM_TYPE_HOST;
    options_.bmDataOpType = HYBM_DOP_TYPE_MTE;
    options_.rankCount = config.worldSize;
    options_.rankId = config.rankId;
    options_.devId = config.deviceId;
    options_.maxDRAMSize = AlignUp(config.reserveSize, GB);
    options_.hostVASpace = AlignUp(config.allocSize, GB);
    options_.role = HYBM_ROLE_PEER;
    options_.scene = HYBM_SCENE_DEFAULT;
    options_.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
    if (!multiNode_) {
        options_.flags |= HYBM_FLAG_UNRESTRICTED_MEM;
    }
    options_.dramShmFd = -1;
    options_.enable56BitsGva = false;
    bzero(options_.transUrl, sizeof(options_.transUrl));
    if (storeUrl_.size() >= sizeof(options_.transUrl)) {
        OFFLOAD_LOG_ERROR("store url too long, storeUrl: " << storeUrl_ << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    std::copy_n(storeUrl_.c_str(), storeUrl_.size(), options_.transUrl);

    entity_ = hybm_create_entity(HYBM_ENTITY_ID_OFFLOAD_BASE, &options_, 0);
    if (entity_ == nullptr) {
        OFFLOAD_LOG_ERROR("create entity failed, rankId: " << config.rankId << ", worldSize: " << config.worldSize);
        return OFFLOAD_ERROR;
    }
    auto ret = hybm_reserve_mem_space(entity_, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("reserve mem failed, result: " << ret << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    ret = AllocAndExportHostSlices();
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("alloc and export host slices failed, hostVASpace: " << options_.hostVASpace
                                                                               << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    ret = AllGatherAndImportPeers(group_, sliceInfos_, entity_, 0, options_.rankCount, options_.rankId);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("allgather/import slice info failed, result: " << ret << ", rankId: " << config.rankId
                                                                         << ", worldSize: " << config.worldSize);
        return OFFLOAD_ERROR;
    }
    return ExportAndImportEntityInfo();
}

int32_t AccOffloadSharedDramEntry::ExportAndImportEntityInfo()
{
    hybm_exchange_info entityInfo{};
    auto ret = hybm_export(entity_, nullptr, HYBM_FLAG_EXPORT_ENTITY, &entityInfo);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("export entity failed, result: " << ret << ", rankId: " << options_.rankId);
        hybm_export_info_free(&entityInfo);
        return OFFLOAD_ERROR;
    }
    if (entityInfo.descLen == 0) {
        return OFFLOAD_OK;
    }
    std::vector<hybm_exchange_info> entityInfos{entityInfo};
    ret = AllGatherAndImportPeers(group_, entityInfos, entity_, HYBM_FLAG_EXPORT_ENTITY, options_.rankCount,
                                  options_.rankId);
    hybm_export_info_free(&entityInfo);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("allgather/import entity info failed, result: " << ret << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramEntry::MapPool(const offload_config_t &config)
{
    auto ret = hybm_mmap(entity_, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("hybm mmap failed, result: " << ret << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    hostGva_ = hybm_get_memory_ptr(entity_, HYBM_MEM_TYPE_HOST);
    if (hostGva_ == nullptr) {
        OFFLOAD_LOG_ERROR("get host gva failed, rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    /* rankId is the GLOBAL rank: this rank's own slot sits rankId slots deep in
     * the whole-pool GVA, and hostGva_ must be the global rank0 slot start on
     * every machine (asserted next by the pool fingerprint exchange). */
    base_ = reinterpret_cast<uint8_t *>(hostGva_) + options_.maxDRAMSize * options_.rankId;
    /* the slot is maxDRAMSize (aligned reserveSize) of virtual address space, but
     * only hostVASpace (aligned allocSize, per-rank and allowed to differ across
     * ranks) is physically backed by this rank's slices — malloc/DVA must stay
     * inside the backed span */
    size_ = options_.hostVASpace;

    memMng_ = std::make_shared<AccOffloadMemManager>(base_, size_);
    if (memMng_ == nullptr) {
        OFFLOAD_LOG_ERROR("create mem manager failed, rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

/* Whole-pool consistency protocol (see acc_offload_pool_fingerprint.h): one
 * AllGather of the pool fingerprint, local validation, then GroupGatherResult
 * so every rank reaches the same verdict and fails together. */
int32_t AccOffloadSharedDramEntry::ExchangeAndValidatePoolFingerprint(const offload_config_t &config)
{
    ock::mf::AscendSocType socType = ock::mf::DlAclApi::GetAscendSocType();
    PoolFingerprint self =
        MakePoolFingerprint(reinterpret_cast<uint64_t>(hostGva_), options_.maxDRAMSize, options_.hostVASpace,
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

int32_t AccOffloadSharedDramEntry::InitSharedPool(const offload_config_t &config)
{
    auto ret = AccOffloadLaunchApi::TryLoadLibrary();
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("offload launch load library failed, deviceId: " << config.deviceId);
        return OFFLOAD_ERROR;
    }
    ret = ResolveStoreUrl(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = CreateGroupStore(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = CreateEntityAndPool(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = MapPool(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    return ExchangeAndValidatePoolFingerprint(config);
}

int32_t AccOffloadSharedDramEntry::Initialize(const offload_config_t &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        return OFFLOAD_OK;
    }

    int32_t ret = ValidateSharedConfig(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    uint32_t localWorld = NormalizeLocalWorldSize(config.worldSize, config.localWorldSize);
    multiNode_ = localWorld != config.worldSize;

    ret = hybm_init(config.deviceId, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("hybm_init failed, result: " << ret << ", deviceId: " << config.deviceId);
    } else {
        ret = InitSharedPool(config);
    }

    inited_ = true;
    if (ret != OFFLOAD_OK) {
        UnInitialize();
        return ret;
    }

    OFFLOAD_LOG_INFO("shared dram entry initialized, rankId(global): "
                     << config.rankId << ", machineRank: " << config.rankId / localWorld
                     << ", localRank: " << config.rankId % localWorld << ", worldSize: " << config.worldSize
                     << ", localWorldSize: " << localWorld << ", storeUrl: " << storeUrl_
                     << ", poolGva: " << reinterpret_cast<uint64_t>(hostGva_) << ", deviceId: " << config.deviceId
                     << ", base: " << reinterpret_cast<void *>(base_) << ", slotStride: " << options_.maxDRAMSize
                     << ", localAllocSize: " << options_.hostVASpace);
    return OFFLOAD_OK;
}

void AccOffloadSharedDramEntry::UnInitialize()
{
    entryTable_ = {};
    entryTableRegistered_ = false;
    if (!inited_) {
        return;
    }

    uint32_t flags = 0;
    memMng_.reset();
    if (group_ != nullptr) {
        group_->GroupSnClean();
        group_ = nullptr;
    }
    if (entity_ != nullptr) {
        for (auto slice : slices_) {
            hybm_free_local_memory(entity_, slice, 1, flags);
        }
        slices_.clear();
        for (auto &info : sliceInfos_) {
            hybm_export_info_free(&info);
        }
        sliceInfos_.clear();
        hybm_unreserve_mem_space(entity_, flags);
        hybm_destroy_entity(entity_, flags);
        entity_ = nullptr;
    }
    entryStore_ = nullptr;
    if (!storeUrl_.empty()) {
        StoreFactory::DestroyStore(storeUrl_);
        storeUrl_.clear();
    }
    AccOffloadLaunchApi::CleanupLibrary();
    hybm_uninit();

    base_ = nullptr;
    hostGva_ = nullptr;
    size_ = 0;
    inited_ = false;
}

void *AccOffloadSharedDramEntry::MallocHost(size_t size)
{
    if (memMng_ == nullptr) {
        OFFLOAD_LOG_ERROR("mem manager is nullptr, malloc failed");
        return nullptr;
    }

    OFFLOAD_LOG_DEBUG("shared malloc host size: " << size);
    return memMng_->Allocate(size);
}

void AccOffloadSharedDramEntry::FreeHost(void *ptr)
{
    if (memMng_ == nullptr || ptr == nullptr) {
        OFFLOAD_LOG_ERROR("mem manager is nullptr or ptr is nullptr, free failed");
        return;
    }

    OFFLOAD_LOG_DEBUG("shared free host ptr: " << reinterpret_cast<uint64_t>(ptr));
    memMng_->Release(ptr);
}

int32_t AccOffloadSharedDramEntry::GetDva(uint64_t hostPtr, uint64_t *dvaPtr)
{
    if (!inited_) {
        OFFLOAD_LOG_ERROR("entry not initialized, get dva failed");
        return OFFLOAD_ERROR;
    }
    if (dvaPtr == nullptr || hostPtr == 0) {
        OFFLOAD_LOG_ERROR("invalid input, hostPtr is null: " << (hostPtr == 0)
                                                             << ", dvaPtr is null: " << (dvaPtr == nullptr));
        return OFFLOAD_ERROR;
    }
    if (reinterpret_cast<uint64_t>(base_) > hostPtr || hostPtr >= reinterpret_cast<uint64_t>(base_) + size_) {
        OFFLOAD_LOG_ERROR("hostPtr out of pool range, pool size: " << size_ << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }

    /* vmm unified mapping: dva == hva == gva (see EntryGather, where
     * hostGva_ is passed to the kernel as the pool GVA directly), so the
     * conversion is the identity in this scene. The range check above keeps
     * the identity honest; a conn-based shared pool would need to resolve
     * through its registered dva instead. */
    *dvaPtr = hostPtr;
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramEntry::SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                              uint32_t *sizePtr, uint8_t devIdx)
{
    OFFLOAD_LOG_DEBUG("shared sparse copy, src: " << reinterpret_cast<uint64_t>(srcPtrs)
                                                  << ", dst: " << reinterpret_cast<uint64_t>(dstPtrs) << ", len: "
                                                  << reinterpret_cast<uint64_t>(lenPtrs) << ", devIdx: " << devIdx);

    return AccOffloadLaunchApi::AccOffloadSparseCopy(srcPtrs, dstPtrs, lenPtrs, sizePtr, devIdx);
}

int32_t AccOffloadSharedDramEntry::GroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                                 uint32_t *numLocalExpertPtr, int64_t *groupList,
                                                 int64_t *packedGroupList, uint8_t devIdx)
{
    return AccOffloadLaunchApi::AccOffloadGroupPackCopy(srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtr, groupList,
                                                        packedGroupList, devIdx);
}

int32_t AccOffloadSharedDramEntry::KvExchangeCopy(uint64_t *metaPtr, uint8_t devIdx)
{
    return AccOffloadLaunchApi::AccOffloadKvExchange(metaPtr, devIdx);
}

int32_t AccOffloadSharedDramEntry::RegisterEntryTable(uint32_t entryBytes, uint32_t rowsPerSlot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        OFFLOAD_LOG_ERROR("entry not initialized, register entry table failed, rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    if (entryTableRegistered_) {
        OFFLOAD_LOG_ERROR("entry table already registered (one uniform grid per pool lifetime; the registry is "
                          "cleared by uninit), rankId: "
                          << options_.rankId);
        return OFFLOAD_ERROR;
    }
    if (entryBytes == 0 || entryBytes > OFFLOAD_ENTRY_GATHER_MAX_ENTRY_BYTES) {
        OFFLOAD_LOG_ERROR("invalid entryBytes " << entryBytes << ", must be in (0, "
                                                << OFFLOAD_ENTRY_GATHER_MAX_ENTRY_BYTES
                                                << "], rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    if (rowsPerSlot == 0) {
        OFFLOAD_LOG_ERROR("invalid rowsPerSlot 0, rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    /* the row grid must live inside this rank's physically backed span (size_,
     * the aligned allocSize) — rows beyond it have no DRAM behind them even
     * though the slot's virtual size (the stride below) is larger */
    if (static_cast<uint64_t>(rowsPerSlot) * entryBytes > size_) {
        OFFLOAD_LOG_ERROR("row grid " << rowsPerSlot << " rows x " << entryBytes << "B does not fit the locally "
                                      << "allocated " << size_ << "B span, rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    /* slotStride is the whole-pool slot layout (aligned reserveSize, fingerprint-
     * checked identical on every rank), NOT this rank's allocSize: the kernel
     * resolves a global row id against one uniform stride on every rank */
    entryTable_ = AccOffloadEntryGatherLayout{0, options_.maxDRAMSize, entryBytes, rowsPerSlot};
    entryTableRegistered_ = true;
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramEntry::EntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint8_t devIdx)
{
    if (!inited_ || !entryTableRegistered_) {
        OFFLOAD_LOG_ERROR("entry not initialized or no table registered"
                          << ", inited: " << inited_ << ", tableRegistered: " << entryTableRegistered_
                          << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }
    /* dva == hva == gva in the shared pool's unified mapping. hostGva_ is the
     * whole-pool GVA base (rank 0 slot start) the kernel maps row ids against;
     * the layout comes from the single registry entry (slotStride is this
     * pool's slot size, stored at registration). countPtr is a device address
     * of the uint32 entry count; the kernel reads it, nothing is
     * dereferenced here. */
    AccOffloadEntryGatherLayout layout = entryTable_;
    layout.poolGva = reinterpret_cast<uint64_t>(hostGva_);
    return AccOffloadLaunchApi::AccOffloadEntryGather(dstPtr, idsPtr, countPtr, layout, devIdx);
}

} // namespace offload
} // namespace ock
