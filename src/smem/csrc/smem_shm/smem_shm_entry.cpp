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

#include <algorithm>
#include <cstring>
#include "smem_common_includes.h"
#include "smem_shm_entry_manager.h"
#include "hybm_big_mem.h"
#include "smem_store_factory.h"
#include "smem_shm_entry.h"

namespace ock {
namespace smem {

SmemShmEntry::SmemShmEntry(uint32_t id) : id_{id}, entity_{nullptr}, gva_{nullptr}
{
    (void)smem_shm_config_init(&extraConfig_);

    auto emptyRollback = []() {};
    initSteps_.emplace_back(ShmEntryInitStep{"01_create_entity", [this]() { return InitStepCreateEntity(); },
                                             [this]() { InitStepDestroyEntity(); }});
    initSteps_.emplace_back(ShmEntryInitStep{"02_reserve_memory", [this]() { return InitStepReserveMemory(); },
                                             [this]() { InitStepUnreserveMemory(); }});
    initSteps_.emplace_back(ShmEntryInitStep{"03_alloc_slice", [this]() { return InitStepAllocSlice(); },
                                             [this]() { InitStepFreeSlice(); }});
    initSteps_.emplace_back(
        ShmEntryInitStep{"04_exchange_slice", [this]() { return InitStepExchangeSlice(); }, emptyRollback});
    initSteps_.emplace_back(
        ShmEntryInitStep{"05_exchange_entity", [this]() { return InitStepExchangeEntity(); }, emptyRollback});
    initSteps_.emplace_back(ShmEntryInitStep{"05_map_memory", [this]() { return InitStepMap(); }, emptyRollback});
}

SmemShmEntry::~SmemShmEntry()
{
    if (globalGroup_ != nullptr) {
        globalGroup_->GroupSnClean();
        globalGroup_ = nullptr;
    }
    uint32_t flags = 0;
    if (entity_ != nullptr && slice_ != nullptr) {
        hybm_free_local_memory(entity_, slice_, 1, flags);
    }

    if (entity_ != nullptr && gva_ != nullptr) {
        hybm_unreserve_mem_space(entity_, 0);
        gva_ = nullptr;
    }

    if (entity_ != nullptr) {
        hybm_destroy_entity(entity_, 0);
        entity_ = nullptr;
    }
}

Result SmemShmEntry::CreateGlobalTeam(uint32_t rankSize, uint32_t rankId)
{
    auto client = SmemShmEntryManager::Instance().GetStoreClient();
    SM_VALIDATE_RETURN(client != nullptr, "GetStoreClient failed (store not initialized), shmId: " << id_,
                       SM_INVALID_PARAM);

    std::string prefix = "SHM_(" + std::to_string(id_) + ")_";
    StorePtr store = StoreFactory::PrefixStore(client, prefix);
    SM_VALIDATE_RETURN(store != nullptr, "PrefixStore failed, prefix: " << prefix << " shmId: " << id_, SM_ERROR);

    SmemGroupOption opt = {rankSize, rankId,  extraConfig_.controlOperationTimeout * SECOND_TO_MILLSEC,
                           false,    nullptr, nullptr,
                           nullptr,  nullptr};
    SmemGroupEnginePtr group = SmemNetGroupEngine::Create(store, opt);
    SM_VALIDATE_RETURN(group != nullptr,
                       "SmemNetGroupEngine::Create failed, shmId: " << id_ << " rankSize: " << rankSize
                                                                    << " rankId: " << rankId,
                       SM_ERROR);

    globalGroup_ = group;
    return globalGroup_->GroupBarrier(); // 保证所有rank都初始化了
}

Result SmemShmEntry::Initialize(hybm_options &options)
{
    localRank_ = options.rankId;
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(CreateGlobalTeam(options.rankCount, options.rankId), "create global team failed");

    options_ = options;
    for (auto it = initSteps_.begin(); it != initSteps_.end(); ++it) {
        SM_LOG_DEBUG("process init step : " << it->name);
        auto stepRet = it->processor();
        if (stepRet != 0) {
            SM_LOG_ERROR("init step(" << it->name << ") process failed: " << stepRet);
            auto fit = it;
            while (fit != initSteps_.begin()) {
                --fit;
                fit->rollback();
            }
            return stepRet;
        }
    }

    inited_ = true;
    return SM_OK;
}

void SmemShmEntry::SetConfig(const smem_shm_config_t &config)
{
    extraConfig_ = config;
    SM_LOG_INFO("shmId: " << id_ << " set_config control_operation_timeout: " << extraConfig_.controlOperationTimeout);
}

Result SmemShmEntry::SetExtraContext(const void *context, uint32_t size)
{
    if (!inited_ || entity_ == nullptr) {
        SM_LOG_ERROR("smem shm entry has not been initialized");
        return SM_ERROR;
    }

    return hybm_set_extra_context(entity_, context, size);
}

void *SmemShmEntry::GetGva() const
{
    return gva_;
}

uint64_t SmemShmEntry::GetHbmMaxSize() const
{
    return options_.maxHBMSize;
}

int32_t SmemShmEntry::InitStepCreateEntity()
{
    auto entityId = id_ + HYBM_ENTITY_ID_SHM_BASE;
    auto entity = hybm_create_entity(entityId, &options_, 0);
    if (entity == nullptr) {
        SM_LOG_ERROR("hybm_create_entity failed, entityId: " << entityId << " rankId: " << options_.rankId);
        return SM_ERROR;
    }

    entity_ = entity;
    return SM_OK;
}

void SmemShmEntry::InitStepDestroyEntity()
{
    hybm_destroy_entity(entity_, 0);
    entity_ = nullptr;
}

int32_t SmemShmEntry::InitStepReserveMemory()
{
    auto ret = hybm_reserve_mem_space(entity_, 0);
    if (ret != 0) {
        SM_LOG_ERROR("reserve mem failed, result: " << ret);
        return SM_ERROR;
    }

    gva_ = hybm_get_memory_ptr(entity_, HYBM_MEM_TYPE_DEVICE);
    return SM_OK;
}

void SmemShmEntry::InitStepUnreserveMemory()
{
    auto ret = hybm_unreserve_mem_space(entity_, 0);
    if (ret != 0) {
        SM_LOG_WARN("unable to unreserve mem space: " << ret);
    }
    gva_ = nullptr;
}

int32_t SmemShmEntry::InitStepAllocSlice()
{
    auto slice = hybm_alloc_local_memory(entity_, HYBM_MEM_TYPE_DEVICE, options_.deviceVASpace, 0);
    if (slice == nullptr) {
        SM_LOG_ERROR("alloc local mem failed, size: " << options_.deviceVASpace);
        return SM_ERROR;
    }

    slice_ = slice;
    return SM_OK;
}

void SmemShmEntry::InitStepFreeSlice()
{
    auto slice = slice_;
    auto ret = hybm_free_local_memory(entity_, slice, 0, 0);
    if (ret != 0) {
        SM_LOG_WARN("unable to free mem slice: " << ret);
    }
    slice_ = nullptr;
}

static int32_t AllGatherExchangeInfos(const SmemGroupEnginePtr &group, const hybm_exchange_info &localInfo,
                                      uint32_t rankCount, std::vector<hybm_exchange_info> &allInfos)
{
    std::vector<uint8_t> localPayload;
    const auto *infoLen = reinterpret_cast<const uint8_t *>(&localInfo.descLen);
    localPayload.insert(localPayload.end(), infoLen, infoLen + sizeof(localInfo.descLen));
    if (localInfo.descLen <= 0 || localInfo.desc == nullptr) {
        SM_LOG_ERROR("allgather exchange localInfo is invalid.");
        return SM_ERROR;
    }
    localPayload.insert(localPayload.end(), localInfo.desc, localInfo.desc + localInfo.descLen);
    uint32_t payloadSize = static_cast<uint32_t>(localPayload.size());

    std::vector<uint32_t> payloadSizes(rankCount, 0);
    auto ret = group->GroupAllGather(reinterpret_cast<const char *>(&payloadSize), sizeof(uint32_t),
                                     reinterpret_cast<char *>(payloadSizes.data()), sizeof(uint32_t) * rankCount);
    if (ret != 0) {
        SM_LOG_ERROR("allgather payload size failed, result: " << ret << ", rankCount: " << rankCount);
        return ret;
    }
    uint32_t maxPayload = *std::max_element(payloadSizes.cbegin(), payloadSizes.cend());
    if (maxPayload == 0) {
        return group->GroupBarrier();
    }

    std::vector<uint8_t> sendBuf(maxPayload, 0);
    std::copy(localPayload.begin(), localPayload.end(), sendBuf.begin());
    std::vector<uint8_t> allPayload(static_cast<size_t>(rankCount) * maxPayload);
    ret = group->GroupAllGather(reinterpret_cast<const char *>(sendBuf.data()), maxPayload,
                                reinterpret_cast<char *>(allPayload.data()), rankCount * maxPayload);
    if (ret != 0) {
        SM_LOG_ERROR("allgather exchange info failed, result: " << ret << ", maxPayload: " << maxPayload);
        return ret;
    }

    for (uint32_t r = 0; r < rankCount; r++) {
        hybm_exchange_info info{};
        const uint8_t *base = allPayload.data() + r * maxPayload;
        std::memcpy(&info.descLen, base, sizeof(uint32_t));
        if (info.descLen > 0) {
            info.desc = new (std::nothrow) uint8_t[info.descLen];
            if (info.desc == nullptr) {
                SM_LOG_ERROR("allgather exchange info alloc failed.");
                for (auto &parsed : allInfos) {
                    hybm_export_info_free(&parsed);
                }
                return SM_ERROR;
            }
            std::memcpy(info.desc, base + sizeof(uint32_t), info.descLen);
            allInfos.push_back(info);
        }
    }
    return SM_OK;
}

int32_t SmemShmEntry::InitStepExchangeSlice()
{
    hybm_exchange_info exInfo{};
    auto ret = hybm_export(entity_, slice_, 0, &exInfo);
    if (ret != 0) {
        SM_LOG_ERROR("hybm export slice failed, result: " << ret);
        return ret;
    }

    std::vector<hybm_exchange_info> allExInfo;
    allExInfo.reserve(options_.rankCount);
    ret = AllGatherExchangeInfos(globalGroup_, exInfo, options_.rankCount, allExInfo);
    hybm_export_info_free(&exInfo);
    if (ret != 0) {
        return ret;
    }

    ret = hybm_import(entity_, allExInfo.data(), options_.rankCount, nullptr, 0);
    for (auto &parsed : allExInfo) {
        hybm_export_info_free(&parsed);
    }
    if (ret != 0) {
        SM_LOG_ERROR("hybm import failed, result: " << ret);
        return ret;
    }

    ret = globalGroup_->GroupBarrier();
    if (ret != 0) {
        SM_LOG_ERROR("hybm barrier for slice failed, result: " << ret);
    }
    return ret;
}

int32_t SmemShmEntry::InitStepExchangeEntity()
{
    hybm_exchange_info exInfo{};
    auto ret = hybm_export(entity_, nullptr, HYBM_FLAG_EXPORT_ENTITY, &exInfo);
    if (ret != 0) {
        SM_LOG_ERROR("hybm export entity failed, result: " << ret);
        return ret;
    }

    if (exInfo.descLen == 0) {
        return SM_OK;
    }

    std::vector<hybm_exchange_info> allExInfo;
    allExInfo.reserve(options_.rankCount);
    ret = AllGatherExchangeInfos(globalGroup_, exInfo, options_.rankCount, allExInfo);
    hybm_export_info_free(&exInfo);
    if (ret != 0) {
        return ret;
    }

    ret = hybm_import(entity_, allExInfo.data(), options_.rankCount, nullptr, HYBM_FLAG_EXPORT_ENTITY);
    for (auto &parsed : allExInfo) {
        hybm_export_info_free(&parsed);
    }
    if (ret != 0) {
        SM_LOG_ERROR("hybm import entity failed, result: " << ret);
        return ret;
    }

    ret = globalGroup_->GroupBarrier();
    if (ret != 0) {
        SM_LOG_ERROR("hybm barrier for entity failed, result: " << ret);
    }
    return ret;
}

int32_t SmemShmEntry::InitStepMap()
{
    auto ret = hybm_mmap(entity_, 0);
    if (ret != 0) {
        SM_LOG_ERROR("hybm mmap failed, result: " << ret);
        return ret;
    }
    return SM_OK;
}

Result SmemShmEntry::GetReachInfo(uint32_t remoteRank, uint32_t &reachInfo) const
{
    if (entity_ == nullptr) {
        SM_LOG_ERROR("entity_ is null, cannot get reach info.");
        return SM_NOT_STARTED;
    }

    hybm_data_op_type reachesTypes;
    auto ret = hybm_entity_reach_types(entity_, remoteRank, reachesTypes, 0);
    if (ret != 0) {
        SM_LOG_ERROR("hybm_entity_reach_types failed, remoteRank: " << remoteRank << " ret: " << ret);
        return SM_ERROR;
    }

    reachInfo = 0U;
    if (reachesTypes & HYBM_DOP_TYPE_MTE) {
        reachInfo |= SMEMS_DATA_OP_MTE;
    }

    if (reachesTypes & HYBM_DOP_TYPE_SDMA) {
        reachInfo |= SMEMS_DATA_OP_SDMA;
    }

    if (reachesTypes & HYBM_DOP_TYPE_AIV_SDMA) {
        reachInfo |= SMEMS_DATA_OP_SDMA;
    }

    if (reachesTypes & HYBM_DOP_TYPE_DEVICE_RDMA) {
        reachInfo |= SMEMS_DATA_OP_RDMA;
    }

    return SM_OK;
}

} // namespace smem
} // namespace ock
