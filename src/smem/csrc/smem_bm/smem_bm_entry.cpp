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
#include "smem_bm_entry.h"

#include <chrono>

#include "hybm_def.h"
#include "hybm_big_mem.h"
#include "hybm_data_op.h"
#include "mf_env_define.h"
#include "hybm_ptracer.h"
#include "mf_env_util.h"
#include "smem_store_factory.h"
#include "mf_fault_injection_point.h"
#include "mf_num_util.h"
#include "smem_tcp_config_store.h"
#include "smem_store_factory.h"

namespace ock {
namespace smem {

// Max payload carried in hybm_exchange_info.desc[1280]; keep headroom for framework metadata.
constexpr size_t EXCHANGE_INFO_PAYLOAD_MAX = 1152;
// LinkState wire values (keep in sync with LinkState in smem_group_manager_server.h)
constexpr int8_t BM_LINK_IDLE = 0;
constexpr int8_t BM_LINK_CONNECTED = 4;

Result SmemBmEntry::AllocDramMemBySlice(hybm_entity_t entity, uint64_t totalSize, uint32_t flags)
{
    constexpr uint64_t dramSliceSize = 32ULL * 1024ULL * 1024ULL * 1024ULL;
    SM_LOG_INFO("alloc dram mem by 32GB slice start, totalSize: " << totalSize);
    uint64_t remaining = totalSize;
    uint64_t allocated = 0;
    while (remaining > 0) {
        uint64_t sliceSize = (remaining >= dramSliceSize) ? dramSliceSize : remaining;
        SM_LOG_INFO("alloc dram slice progress: " << allocated << "/" << totalSize << ", sliceSize: " << sliceSize);
        auto memSlice = hybm_alloc_local_memory(entity, HYBM_MEM_TYPE_HOST, sliceSize, flags);
        if (memSlice == nullptr) {
            SM_LOG_ERROR("alloc host mem slice failed, allocated: " << allocated << " sliceSize: " << sliceSize
                                                                    << " totalSize: " << totalSize);
            return SM_ERROR;
        }
        slices_.push_back(memSlice);

        hybm_exchange_info sliceInfo{};
        auto ret = hybm_export(entity, memSlice, flags, &sliceInfo);
        if (ret != 0) {
            SM_LOG_ERROR("hybm export host slice failed, allocated: " << allocated << " sliceSize: " << sliceSize
                                                                      << " result: " << ret);
            return SM_ERROR;
        }
        sliceInfos_.push_back(sliceInfo);
        remaining -= sliceSize;
        allocated += sliceSize;
    }
    realDRAMSize_ = allocated;
    SM_LOG_INFO("alloc dram mem by 32GB slice done, totalSize: " << totalSize);
    return SM_OK;
}

Result SmemBmEntry::AllocDramMemBestEffort(hybm_entity_t entity, uint64_t totalSize, uint32_t flags)
{
    constexpr uint64_t dramSliceSize = 32ULL * 1024ULL * 1024ULL * 1024ULL;
    SM_LOG_INFO("alloc dram mem best effort start, totalSize: " << totalSize);
    uint64_t allocated = 0;
    uint64_t sliceIdx = 0;
    while (allocated < totalSize) {
        uint64_t sliceSize = dramSliceSize;
        if (allocated + sliceSize > totalSize) {
            sliceSize = totalSize - allocated;
        }
        SM_LOG_INFO("alloc dram slice progress: " << allocated << "/" << totalSize << ", sliceSize: " << sliceSize);
        auto memSlice = hybm_alloc_local_memory(entity, HYBM_MEM_TYPE_HOST, sliceSize, flags);
        if (memSlice == nullptr) {
            SM_LOG_INFO("alloc dram mem best effort stopped, allocated: " << allocated << " sliceCount: " << sliceIdx);
            break;
        }
        slices_.push_back(memSlice);

        hybm_exchange_info sliceInfo{};
        auto ret = hybm_export(entity, memSlice, flags, &sliceInfo);
        if (ret != 0) {
            SM_LOG_ERROR("hybm export host slice failed at slice: " << sliceIdx << " result: " << ret);
            return SM_ERROR;
        }
        sliceInfos_.push_back(sliceInfo);
        allocated += sliceSize;
        sliceIdx++;
    }
    realDRAMSize_ = allocated;
    SM_LOG_INFO("alloc dram mem best effort done, " << allocated << "/" << totalSize);
    return SM_OK;
}

Result SmemBmEntry::AllocDramMem(hybm_entity_t entity, const hybm_options &options, uint32_t flags)
{
    if (options.maxDRAMSize == 0) {
        return SM_OK;
    }
    if (options.flags & SMEM_BM_FLAG_DRAM_BEST_EFFORT) {
        return AllocDramMemBestEffort(entity, options.hostVASpace, flags);
    }
    if (options.flags & SMEM_BM_FLAG_DRAM_MAP_HOST_VA) {
        return AllocDramMemBySlice(entity, options.hostVASpace, flags);
    }
    auto slice = hybm_alloc_local_memory(entity, HYBM_MEM_TYPE_HOST, options.hostVASpace, flags);
    if (slice == nullptr) {
        SM_LOG_ERROR("alloc local host mem failed, size: " << options.hostVASpace);
        return SM_ERROR;
    }

    slices_.push_back(slice);

    hybm_exchange_info dramSliceInfo{};
    auto ret = hybm_export(entity, slice, flags, &dramSliceInfo);
    if (ret != 0) {
        SM_LOG_ERROR("hybm export host slice failed, result: " << ret);
        return SM_ERROR;
    }
    sliceInfos_.push_back(dramSliceInfo);
    realDRAMSize_ = options.hostVASpace;
    return SM_OK;
}
int32_t SmemBmEntry::Initialize(const hybm_options &options)
{
    if (inited_) {
        return SM_OK;
    }
    uint32_t flags = 0;
    hybm_entity_t entity = nullptr;
    hybm_mem_slice_t slice = nullptr;
    Result ret = SM_ERROR;

    SM_VALIDATE_RETURN(CheckRankConfigConsistency(options), "check rank config consistency failed", SM_INVALID_PARAM);
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(SetupGroupManagerCallbacks(), "setup group manager callbacks failed");
    if (!executorService_.Start()) {
        SM_LOG_ERROR("executor service start failed");
        executorService_.Stop();
        return SM_ERROR;
    }
    executorService_.SetThreadName("batch-copy");

    do {
        auto entityId = Id() + HYBM_ENTITY_ID_BM_BASE;
        entity = hybm_create_entity(entityId, &options, flags);
        if (entity == nullptr) {
            SM_LOG_ERROR("hybm_create_entity failed, entityId: " << entityId << " rankId: " << options.rankId
                                                                 << " flags: " << flags);
            ret = SM_ERROR;
            break;
        }

        ret = hybm_reserve_mem_space(entity, flags);
        if (ret != 0) {
            SM_LOG_ERROR("hybm_reserve_mem_space failed, entityId: " << entityId << " ret: " << ret);
            hybm_destroy_entity(entity, flags);
            ret = SM_ERROR;
            break;
        }
        entity_ = entity;

        hybm_exchange_info hbmSliceInfo{};
        if (options.maxHBMSize > 0) {
            slice = hybm_alloc_local_memory(entity, HYBM_MEM_TYPE_DEVICE, options.deviceVASpace, flags);
            if (slice == nullptr) {
                SM_LOG_ERROR("alloc local device mem failed, size: " << options.deviceVASpace);
                ret = SM_ERROR;
                break;
            }
            slices_.push_back(slice);

            ret = hybm_export(entity, slice, flags, &hbmSliceInfo);
            if (ret != 0) {
                SM_LOG_ERROR("hybm export device slice failed, result: " << ret);
                break;
            }
            sliceInfos_.push_back(hbmSliceInfo);
            realHBMSize_ = options.deviceVASpace;
        }

        ret = AllocDramMem(entity, options, flags);
        if (ret != SM_OK) {
            SM_LOG_ERROR("alloc dram mem failed, result: " << ret);
            break;
        }

        bzero(&entityInfo_, sizeof(hybm_exchange_info));
        ret = hybm_export(entity, nullptr, HYBM_FLAG_EXPORT_ENTITY, &entityInfo_);
        if (ret != 0) {
            SM_LOG_ERROR("hybm entity export failed, result: " << ret);
            break;
        }
    } while (0);

    if (ret != 0) {
        inited_ = true; // ensure Uninitialize will execute
        Uninitialize();
        return ret;
    }

    coreOptions_ = options;
    hostGva_ = hybm_get_memory_ptr(entity, HYBM_MEM_TYPE_HOST);
    deviceGva_ = hybm_get_memory_ptr(entity, HYBM_MEM_TYPE_DEVICE);
    inited_ = true;
    return 0;
}

void SmemBmEntry::Uninitialize()
{
    executorService_.Stop();
    if (!inited_) {
        return;
    }
    if (entity_ == nullptr) {
        return;
    }
    SM_LOG_INFO("SmemBmEntry::Uninitialize begin, rank: " << options_.rank);
    // Perform a graceful group leave so that peer ranks can synchronously clean up
    // their imported state. This must happen before the local entity is destroyed
    // because the leave callback on the peer side still references entity_.
    if (joined_) {
        auto coreStore = _configStore->GetCoreStore();
        auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
        if (groupMgr != nullptr) {
            SM_LOG_DEBUG("SmemBmEntry::Uninitialize sending group leave, rank: " << options_.rank);
            groupMgr->Leave();
        }
        joined_ = false;
    }

    uint32_t flags = 0;
    for (auto slice : slices_) {
        hybm_free_local_memory(entity_, slice, 1, flags);
    }
    slices_.clear();
    sliceInfos_.clear();
    realDRAMSize_ = 0;
    realHBMSize_ = 0;
    for (auto &pair : registedSlice_) {
        hybm_free_local_memory(entity_, pair.second.second, 1, flags);
    }
    registedSlice_.clear();
    hybm_unreserve_mem_space(entity_, flags);
    hybm_destroy_entity(entity_, flags);
    entity_ = nullptr;
    inited_ = false;
    SM_LOG_INFO("SmemBmEntry::Uninitialize complete, rank: " << options_.rank);
}

Result SmemBmEntry::Join(uint32_t flags)
{
    (void)flags;
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    TP_TRACE_BEGIN(TP_SMEM_GROUP_JOIN_RANK);

    joinComplete_ = std::make_shared<std::promise<void>>();
    auto coreStore = _configStore->GetCoreStore();
    auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
    if (groupMgr == nullptr) {
        SM_LOG_ERROR("SmemBmEntry::Join failed, group manager is nullptr");
        TP_TRACE_END(TP_SMEM_GROUP_JOIN_RANK, 1);
        return SM_ERROR;
    }

    RankFullInfo info{groupMgr->GetLocalRankId()};
    info.baseInfo.insert(info.baseInfo.end(), &entityInfo_.desc[0], &entityInfo_.desc[entityInfo_.descLen]);
    for (auto &[data, len] : sliceInfos_) {
        Bytes bytes(&data[0], &data[len]);
        info.externalInfo.emplace_back(std::move(bytes));
    }
    if (auto joinRet = groupMgr->Join(info); joinRet != SM_OK) {
        SM_LOG_ERROR("SmemBmEntry::Join failed, result: " << joinRet);
        TP_TRACE_END(TP_SMEM_GROUP_JOIN_RANK, 1);
        return SM_ERROR;
    }

    // Wait for server to promote rank to ACTIVE (all links established)
    if (joinComplete_ == nullptr) {
        joinComplete_ = std::make_shared<std::promise<void>>();
    }
    auto future = joinComplete_->get_future();
    if (future.wait_for(std::chrono::seconds(MF_GROUP_JOIN_DEFAULT_TIMEOUT)) != std::future_status::ready) {
        SM_LOG_ERROR("SmemBmEntry::Join timeout waiting for PROMOTE_TO_ACTIVE, rank=" << options_.rank);
        // Clean up server-side state so the rank can retry Join cleanly
        SM_LOG_DEBUG("SmemBmEntry::Join timeout, sending group leave for cleanup, rank: " << options_.rank);
        groupMgr->Leave();
        TP_TRACE_END(TP_SMEM_GROUP_JOIN_RANK, 1);
        return SM_ERROR;
    }

    SM_LOG_DEBUG("join success. rank: " << options_.rank);
    TP_TRACE_END(TP_SMEM_GROUP_JOIN_RANK, 0);
    return SM_OK;
}

Result SmemBmEntry::Update(uint32_t flags)
{
    (void)flags;
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_LOG_WARN("SmemBmEntry::Update is deprecated — "
                "dynamic memory extension must use ExtendLocalMem which "
                "propagates via the server. Calling Update directly is a no-op. "
                "rank="
                << options_.rank);
    return SM_OK;
}

Result SmemBmEntry::Leave(uint32_t flags)
{
    (void)flags;
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    TP_TRACE_BEGIN(TP_SMEM_GROUP_LEAVE_RANK);

    auto coreStore = _configStore->GetCoreStore();
    auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
    if (groupMgr == nullptr) {
        SM_LOG_ERROR("SmemBmEntry::Leave failed, group manager is nullptr");
        TP_TRACE_END(TP_SMEM_GROUP_LEAVE_RANK, 1);
        return SM_ERROR;
    }

    auto tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        SM_LOG_ERROR("SmemBmEntry::Leave failed, tcpStore is nullptr");
        TP_TRACE_END(TP_SMEM_GROUP_LEAVE_RANK, 1);
        return SM_ERROR;
    }

    if (tcpStore->GetConnectStatus()) {
        if (auto leaveRet = groupMgr->Leave(); leaveRet != SM_OK) {
            SM_LOG_ERROR("rankId:" << groupMgr->GetLocalRankId() << " SmemBmEntry::Leave failed, result: " << leaveRet);
            TP_TRACE_END(TP_SMEM_GROUP_LEAVE_RANK, 1);
        } else {
            SM_LOG_INFO("rankId:" << groupMgr->GetLocalRankId() << " SmemBmEntry::Leave success.");
            // 出组成功后清 joined_，否则 Uninitialize 会基于 joined_ 再次 Leave，
            // 导致 server 端 Checkout 找不到 rank 报 "CONTROL Leave: Checkout failed"
            joined_ = false;
            TP_TRACE_END(TP_SMEM_GROUP_LEAVE_RANK, 0);
        }
    } else {
        SM_LOG_INFO("Store not connected, skipping Leave, rank " << groupMgr->GetLocalRankId());
        TP_TRACE_END(TP_SMEM_GROUP_LEAVE_RANK, 0);
    }
    joined_ = false;
    return SM_OK;
}

Result SmemBmEntry::ExtendLocalMem(smem_bm_mem_type memType, uint64_t size)
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_ASSERT_RETURN(memType == SMEM_MEM_TYPE_DEVICE || memType == SMEM_MEM_TYPE_HOST, SM_INVALID_PARAM);
    SM_ASSERT_RETURN(size > 0, SM_INVALID_PARAM);
    std::lock_guard<std::mutex> lock(mutex_);
    // 1.alloc slice
    auto hybmMemType = memType == SMEM_MEM_TYPE_DEVICE ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    auto slice = hybm_alloc_local_memory(entity_, hybmMemType, size, 0);
    if (slice == nullptr) {
        SM_LOG_ERROR("Failed to alloc memory, memType:" << memType << " size:" << size);
        return SM_ERROR;
    }
    // 2.export slice
    hybm_exchange_info info{};
    auto ret = hybm_export(entity_, slice, 0, &info);
    if (ret != 0) {
        SM_LOG_ERROR("Failed to export slice:" << slice << " memType:" << memType << " size:" << size);
        hybm_free_local_memory(entity_, slice, 1, 0);
        return ret;
    }
    slices_.push_back(slice);
    sliceInfos_.push_back(info);

    // Send new slice to server for propagation to all peers
    auto coreStore = _configStore->GetCoreStore();
    auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
    if (groupMgr == nullptr) {
        SM_LOG_ERROR("ExtendLocalMem: group manager is nullptr");
        slices_.pop_back();
        sliceInfos_.pop_back();
        hybm_free_local_memory(entity_, slice, 1, 0);
        return SM_ERROR;
    }

    MultiBytes newSlices;
    Bytes sliceBytes(&info.desc[0], &info.desc[info.descLen]);
    newSlices.push_back(std::move(sliceBytes));

    extendComplete_ = std::make_shared<std::promise<void>>();
    if (auto ret2 = groupMgr->ExtendMemory(newSlices); ret2 != SM_OK) {
        SM_LOG_ERROR("ExtendLocalMem: ExtendMemory failed, ret=" << ret2);
        slices_.pop_back();
        sliceInfos_.pop_back();
        hybm_free_local_memory(entity_, slice, 1, 0);
        return SM_ERROR;
    }

    // Server responds synchronously; peers receive ADD_SLICES asynchronously
    extendComplete_->set_value();

    if (memType == SMEM_MEM_TYPE_DEVICE) {
        realHBMSize_ += size;
    } else {
        realDRAMSize_ += size;
    }
    return SM_OK;
}

static hybm_data_copy_direction directMap[SMEM_MEM_TYPE_BUTT + 1][SMEM_MEM_TYPE_BUTT + 1] = {
    {HYBM_DATA_COPY_DIRECTION_BUTT, HYBM_DATA_COPY_DIRECTION_BUTT, HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE,
     HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST, HYBM_DATA_COPY_DIRECTION_BUTT},
    {HYBM_DATA_COPY_DIRECTION_BUTT, HYBM_DATA_COPY_DIRECTION_BUTT, HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE,
     HYBM_LOCAL_HOST_TO_GLOBAL_HOST, HYBM_DATA_COPY_DIRECTION_BUTT},
    {HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST, HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE,
     HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST, HYBM_DATA_COPY_DIRECTION_BUTT},
    {HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE, HYBM_GLOBAL_HOST_TO_LOCAL_HOST, HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE,
     HYBM_GLOBAL_HOST_TO_GLOBAL_HOST, HYBM_DATA_COPY_DIRECTION_BUTT},
    {HYBM_DATA_COPY_DIRECTION_BUTT, HYBM_DATA_COPY_DIRECTION_BUTT, HYBM_DATA_COPY_DIRECTION_BUTT,
     HYBM_DATA_COPY_DIRECTION_BUTT, HYBM_DATA_COPY_DIRECTION_BUTT},
};

smem_bm_mem_type SmemBmEntry::GetHybmMemTypeFromGva(const void *addr, uint64_t size)
{
    if (AddrInHostGva(addr, size)) {
        return SMEM_MEM_TYPE_HOST;
    }
    if (AddrInDeviceGva(addr, size)) {
        return SMEM_MEM_TYPE_DEVICE;
    }
    return SMEM_MEM_TYPE_BUTT;
}

Result SmemBmEntry::CheckJoined() const
{
    SM_VALIDATE_RETURN(joined_, "not joined the net group yet", SM_NOT_STARTED);
    return SM_OK;
}

hybm_data_copy_direction SmemBmEntry::TransToHybmDirection(const smem_bm_copy_type &smemDirect, const void *src,
                                                           uint64_t srcSize, const void *dest, uint64_t destSize)
{
    smem_bm_mem_type srcMemType = GetHybmMemTypeFromGva(src, srcSize);
    smem_bm_mem_type destMemType = GetHybmMemTypeFromGva(dest, destSize);
    switch (smemDirect) {
        case SMEMB_COPY_L2G:
            srcMemType = SMEM_MEM_TYPE_LOCAL_DEVICE;
            break;
        case SMEMB_COPY_G2L:
            destMemType = SMEM_MEM_TYPE_LOCAL_DEVICE;
            break;
        case SMEMB_COPY_G2H:
            destMemType = SMEM_MEM_TYPE_LOCAL_HOST;
            break;
        case SMEMB_COPY_H2G:
            srcMemType = SMEM_MEM_TYPE_LOCAL_HOST;
            break;
        case SMEMB_COPY_L2GH:
            srcMemType = SMEM_MEM_TYPE_LOCAL_DEVICE;
            // dest is already determined by GetHybmMemTypeFromGva (global host)
            break;
        case SMEMB_COPY_GH2L:
            // src is already determined by GetHybmMemTypeFromGva (global host)
            destMemType = SMEM_MEM_TYPE_LOCAL_DEVICE;
            break;
        case SMEMB_COPY_GH2H:
            destMemType = SMEM_MEM_TYPE_LOCAL_HOST;
            break;
        case SMEMB_COPY_H2GH:
            srcMemType = SMEM_MEM_TYPE_LOCAL_HOST;
            // dest is already determined by GetHybmMemTypeFromGva (global host)
            break;
        case SMEMB_COPY_G2G:
        default:
            break;
    }

    return directMap[srcMemType][destMemType];
}

Result SmemBmEntry::DataCopy(const void *src, void *dest, uint64_t size, smem_bm_copy_type t, void *stream,
                             uint32_t flags)
{
    SM_VALIDATE_RETURN(src != nullptr, "invalid param, src is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(dest != nullptr, "invalid param, dest is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(size != 0, "invalid param, size is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(t < SMEMB_COPY_BUTT, "invalid param, type invalid: " << t, SM_INVALID_PARAM);
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_RETURN_IT_IF_NOT_OK(CheckJoined());

    if (!(flags & SMEM_BM_FLAG_USE_EXTERNAL_STREAM)) {
        stream = nullptr;
    } else {
        flags &= ~(SMEM_BM_FLAG_USE_EXTERNAL_STREAM);
    }

    hybm_data_copy_direction direct =
        (t == SMEMB_COPY_AUTO) ? HYBM_DATA_COPY_DIRECTION_AUTO : TransToHybmDirection(t, src, size, dest, size);
    if (direct == HYBM_DATA_COPY_DIRECTION_BUTT) {
        SM_LOG_ERROR("Failed to trans to hybm direct, smem direct: " << t << " src: " << src << " dest: " << dest);
        return SM_INVALID_PARAM;
    }

    hybm_copy_params copyParams = {const_cast<void *>(src), dest, size};
    auto ret = hybm_data_copy(entity_, &copyParams, direct, stream, flags);
    return ret == BM_NOT_CONNECTED ? SMEM_NOT_CONNECTED : ret;
}

Result SmemBmEntry::Wait()
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    return hybm_wait(entity_);
}

uint32_t SmemBmEntry::GetRankIdByGva(void *gva)
{
    if (AddrInHostGva(gva, 1UL)) {
        return ((uint64_t)gva - (uint64_t)hostGva_) / coreOptions_.maxDRAMSize;
    }

    if (AddrInDeviceGva(gva, 1UL)) {
        return ((uint64_t)gva - (uint64_t)deviceGva_) / coreOptions_.maxHBMSize;
    }
    return UINT32_MAX;
}

Result SmemBmEntry::RegisterMem(uint64_t addr, uint64_t size)
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = registedSlice_.find(addr);
    if (iter != registedSlice_.end()) {
        if (iter->second.first != size) {
            SM_LOG_ERROR("RegisterMem size_mismatch: addr=0x" << std::hex << addr << std::dec << " new_size=" << size
                                                              << " existing_size=" << iter->second.first);
            return SM_ERROR;
        }
        SM_LOG_WARN("RegisterMem skip_dup: addr=0x" << std::hex << addr << std::dec << " size=" << size);
        return SM_OK;
    }
    auto slice = hybm_register_local_memory(entity_, reinterpret_cast<void *>(addr), size, 0);
    if (slice != nullptr) {
        registedSlice_.emplace(addr, std::make_pair(size, slice));
        SM_LOG_DEBUG("RegisterMem ok: addr=0x" << std::hex << addr << std::dec << " size=" << size);
        return SM_OK;
    }
    SM_LOG_ERROR("RegisterMem fail: addr=0x" << std::hex << addr << std::dec << " size=" << size);
    return SM_ERROR;
}

Result SmemBmEntry::UnRegisterMem(uint64_t addr)
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = registedSlice_.find(addr);
    if (iter == registedSlice_.end()) {
        SM_LOG_WARN("UnRegisterMem skip_notfound: addr=0x" << std::hex << addr);
        return SM_OK;
    }
    auto sz = iter->second.first;
    auto ret = hybm_free_local_memory(entity_, iter->second.second, 1, 0);
    if (ret != 0) {
        SM_LOG_ERROR("UnRegisterMem free_fail: addr=0x" << std::hex << addr << std::dec << " size=" << sz
                                                        << " ret=" << ret);
        return SM_ERROR;
    }
    registedSlice_.erase(iter);
    SM_LOG_DEBUG("UnRegisterMem ok: addr=0x" << std::hex << addr << std::dec << " size=" << sz);
    return SM_OK;
}

Result SmemBmEntry::DataCopyBatch(smem_batch_copy_params *params, smem_bm_copy_type t, uint32_t flags)
{
    SM_VALIDATE_RETURN(params->sources != nullptr, "invalid param, src is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(params->destinations != nullptr, "invalid param, dest is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(params->batchSize != 0, "invalid param, size is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(params->dataSizes != nullptr, "invalid param, size is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(t < SMEMB_COPY_BUTT, "invalid param, type invalid: " << t, SM_INVALID_PARAM);
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_RETURN_IT_IF_NOT_OK(CheckJoined());

    if (!(flags & SMEM_BM_FLAG_USE_EXTERNAL_STREAM)) {
        params->stream = nullptr;
    } else {
        flags &= ~(SMEM_BM_FLAG_USE_EXTERNAL_STREAM);
    }

    hybm_data_copy_direction direct = (t == SMEMB_COPY_AUTO)
                                          ? HYBM_DATA_COPY_DIRECTION_AUTO
                                          : TransToHybmDirection(t, params->sources[0], params->dataSizes[0],
                                                                 params->destinations[0], params->dataSizes[0]);
    if (direct == HYBM_DATA_COPY_DIRECTION_BUTT) {
        SM_LOG_ERROR("Failed to trans to hybm direct, smem direct: " << t << " src: " << params->sources[0]
                                                                     << " dest: " << params->destinations[0]);
        return SM_INVALID_PARAM;
    }
    hybm_batch_copy_params copyParams = {params->sources, params->destinations, params->dataSizes, params->batchSize};
    return hybm_data_batch_copy(entity_, &copyParams, direct, params->stream, flags);
}

Result SmemBmEntry::DataCopyBatchConcurrent(smem_batch_copy_params *params, smem_bm_copy_type t, uint32_t flags,
                                            smem_batch_copy_result *results)
{
    SM_VALIDATE_RETURN(params->sources != nullptr, "invalid param, src is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(params->destinations != nullptr, "invalid param, dest is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(params->batchSize != 0, "invalid param, size is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(results != nullptr, "results is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(results->results, "results inner pointer is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(results->batchSize == params->batchSize, "result batch size invalid", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(t < SMEMB_COPY_BUTT, "invalid param, type invalid: " << t, SM_INVALID_PARAM);
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_RETURN_IT_IF_NOT_OK(CheckJoined());

    std::mutex finishMutex;
    std::condition_variable finishCond;
    uint32_t finishedCount = 0;
    for (auto i = 0U; i < params->batchSize; i++) {
        auto submitSuccess =
            executorService_.Execute([this, &finishedCount, &finishMutex, &finishCond, i, t, params, flags, results]() {
                hybm_copy_params singleParam{};
                singleParam.src = params->sources[i];
                singleParam.dest = params->destinations[i];
                singleParam.dataSize = params->dataSizes[i];
                auto direct = (t == SMEMB_COPY_AUTO)
                                  ? HYBM_DATA_COPY_DIRECTION_AUTO
                                  : TransToHybmDirection(t, params->sources[i], params->dataSizes[i],
                                                         params->destinations[i], params->dataSizes[i]);
                auto ret = hybm_data_copy(entity_, &singleParam, direct, params->stream, flags);
                SM_LOG_DEBUG("copy index: " << i << ", result:" << ret);
                results->results[i] = (ret == BM_NOT_CONNECTED) ? SMEM_NOT_CONNECTED : ret;

                std::unique_lock<std::mutex> locker{finishMutex};
                if (++finishedCount >= params->batchSize) {
                    locker.unlock();
                    finishCond.notify_one();
                }
                SM_LOG_DEBUG("copy index: " << i << ", run exit:");
            });
        if (!submitSuccess) {
            std::unique_lock<std::mutex> locker{finishMutex};
            ++finishedCount;
            results->results[i] = SM_ERROR;
        }
    }

    std::unique_lock<std::mutex> locker{finishMutex};
    finishCond.wait(locker, [&]() { return finishedCount >= params->batchSize; });
    locker.unlock();
    auto hasSuccess =
        std::any_of(results->results, results->results + results->batchSize, [](int r) { return r == 0; });
    auto hasFail = std::any_of(results->results, results->results + results->batchSize, [](int r) { return r != 0; });
    SM_LOG_DEBUG("has success = " << hasSuccess << ", has failed = " << hasFail);
    if (!hasFail) {
        return SM_OK;
    }

    if (!hasSuccess) {
        return SM_ERROR;
    }

    return SM_PARTIAL_FAILED;
}

bool SmemBmEntry::AddrInHostGva(const void *address, uint64_t size)
{
    if (hostGva_ == nullptr) {
        return false;
    }

    auto totalSize = coreOptions_.maxDRAMSize * coreOptions_.rankCount;
    if ((const uint8_t *)address + size > (const uint8_t *)hostGva_ + totalSize) {
        return false;
    }

    if ((const uint8_t *)address < (const uint8_t *)hostGva_) {
        return false;
    }

    return true;
}

bool SmemBmEntry::AddrInDeviceGva(const void *address, uint64_t size)
{
    if (deviceGva_ == nullptr) {
        return false;
    }

    auto totalSize = coreOptions_.maxHBMSize * coreOptions_.rankCount;
    if ((const uint8_t *)address + size > (const uint8_t *)deviceGva_ + totalSize) {
        return false;
    }

    if ((const uint8_t *)address < (const uint8_t *)deviceGva_) {
        return false;
    }

    return true;
}

bool SmemBmEntry::CheckRankConfigConsistency(const hybm_options &options) const
{
    SmemBmConsistencyConfig localConfig{options};
    const auto localConfigAddr = static_cast<const uint8_t *>(static_cast<const void *>(&localConfig));
    const std::string key = "check_rank_config_consistency";

    std::vector<uint8_t> expectData;
    std::vector<uint8_t> existData;
    std::vector<uint8_t> localConfData;
    localConfData.insert(localConfData.end(), localConfigAddr, localConfigAddr + sizeof(localConfig));

    auto ret = _configStore->Cas(key, expectData, localConfData, existData);
    if (ret == SUCCESS) {
        SM_LOG_DEBUG("first set for key: " << key << ", config: " << localConfig.ToStr());
        return true;
    }

    if (ret != RESTORE) {
        SM_LOG_ERROR("CAS for key: " << key << " failed: " << ret);
        return false;
    }

    // CAS return RESTORE
    if (existData.size() != sizeof(localConfig)) {
        SM_LOG_ERROR("CAS for key: " << key << "Expected size of localConfig(" << sizeof(localConfig) << ") but got "
                                     << existData.size());
        return false;
    }

    auto existConfig = static_cast<const SmemBmConsistencyConfig *>(static_cast<const void *>(existData.data()));
    if (existConfig->maxDRAMSize != localConfig.maxDRAMSize) {
        SM_LOG_ERROR("exist Config maxDRAMSize:" << existConfig->maxDRAMSize << " != " << localConfig.maxDRAMSize);
        return false;
    }

    if (existConfig->maxHBMSize != localConfig.maxHBMSize) {
        SM_LOG_ERROR("exist Config maxHBMSize:" << existConfig->maxHBMSize << " != " << localConfig.maxHBMSize);
        return false;
    }

    if (existConfig->enable56BitsGva != localConfig.enable56BitsGva) {
        SM_LOG_ERROR("exist Config enable56BitsGva:" << existConfig->enable56BitsGva
                                                     << " != " << localConfig.enable56BitsGva);
        return false;
    }

    SM_LOG_DEBUG("compare for key: " << key << ", config: " << localConfig.ToStr() << " matches.");
    return true;
}

int SmemBmEntry::SetupGroupManagerCallbacks() noexcept
{
    // Unwrap PrefixConfigStore to get the real TcpConfigStore
    auto coreStore = _configStore->GetCoreStore();
    auto tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        SM_LOG_ERROR("SmemBmEntry::SetupGroupManagerCallbacks failed, tcp store is nullptr");
        return SM_ERROR;
    }

    auto *asyncMgr = tcpStore->GetAsyncDispatcher();
    if (asyncMgr == nullptr) {
        SM_LOG_ERROR("SmemBmEntry::SetupGroupManagerCallbacks: async dispatcher is null");
        return SM_ERROR;
    }

    if (auto ret = RegisterAsyncCallbacks(asyncMgr); ret != SMEM_OK) {
        return ret;
    }

    auto executor = SmMakeRef<SmemGroupManagerClient>();
    if (auto ret = RegisterExecutorCallbacks(executor.Get(), asyncMgr); ret != SMEM_OK) {
        return ret;
    }

    auto *groupMgr = static_cast<SmemGroupManager *>(tcpStore);
    groupMgr->SetExecutor(executor.Get());
    SM_LOG_INFO("SmemBmEntry callbacks setup, rank " << groupMgr->GetLocalRankId());
    return SMEM_OK;
}

int SmemBmEntry::RegisterAsyncCallbacks(SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept
{
    asyncMgr->SetWhitelistCallback([this](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
        return OnAddToWhitelist(rankId, others, reqId);
    });
    asyncMgr->SetRemoveWhitelistCallback(
        [this](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
            return OnRemoveFromWhitelist(rankId, others, reqId);
        });
    asyncMgr->SetConnectionCallback([this](uint32_t rankId, const std::vector<RankFullInfo> &peers, uint64_t reqId) {
        return OnEstablishConnection(rankId, peers, reqId);
    });
    asyncMgr->SetCloseConnectionCallback([this](uint32_t rankId, const std::vector<uint32_t> &peers, uint64_t reqId) {
        return OnCloseConnection(rankId, peers, reqId);
    });
    asyncMgr->SetLinkStateQueryCallback([this]() { return OnQueryLinkState(); });
    asyncMgr->SetLeaveNotifyCallback([this](uint32_t leavingRankId) { return OnLeaveNotify(leavingRankId); });
    asyncMgr->SetAddSlicesCallback([this](uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) {
        return OnAddSlices(extendingRankId, newSlices, reqId);
    });
    return SMEM_OK;
}

int SmemBmEntry::RegisterExecutorCallbacks(SmemGroupManagerClient *executor,
                                           SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept
{
    executor->onAddToWhitelist_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
        std::vector<RankFullInfo> copy(others);
        asyncMgr->EnqueueAddToWhitelist(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onRemoveFromWhitelist_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &others,
                                                  uint64_t reqId) {
        std::vector<RankFullInfo> copy(others);
        asyncMgr->EnqueueRemoveFromWhitelist(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onEstablishConnection_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &peers,
                                                  uint64_t reqId) {
        std::vector<RankFullInfo> copy(peers);
        asyncMgr->EnqueueEstablishConnection(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onCloseConnection_ = [asyncMgr](uint32_t rankId, const std::vector<uint32_t> &peers, uint64_t reqId) {
        std::vector<uint32_t> copy(peers);
        asyncMgr->EnqueueCloseConnection(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onQueryLinkState_ = [asyncMgr]() {
        asyncMgr->EnqueueQueryLinkState(0);
        return std::vector<LinkStateEntry>{};
    };
    executor->onLeaveNotify_ = [asyncMgr](uint32_t leavingRankId) {
        asyncMgr->EnqueueLeaveNotify(leavingRankId);
        return 0;
    };
    executor->onPromoteToActive_ = [this](uint32_t rankId) {
        joined_ = true;
        SM_LOG_INFO("SmemBmEntry::onPromoteToActive rankId=" << rankId);
        try {
            joinComplete_->set_value();
        } catch (const std::future_error &e) {
            SM_LOG_WARN("onPromoteToActive: promise already satisfied: " << e.what());
        }
        return 0;
    };
    executor->onAddSlices_ = [asyncMgr](uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) {
        MultiBytes copy(newSlices);
        asyncMgr->EnqueueAddSlices(extendingRankId, std::move(copy), reqId);
        return 0;
    };
    return SMEM_OK;
}

int SmemBmEntry::OnAddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) noexcept
{
    if (others.empty()) {
        SM_LOG_ERROR("rankId: " << rankId << "OnAddToWhitelist reqId: " << reqId << " others is empty.");
        return BM_INVALID_PARAM;
    }

    if (auto ret = ValidatePeerPayloads(rankId, others); ret != SMEM_OK) {
        return ret;
    }

    TP_TRACE_BEGIN(TP_SMEM_GROUP_ADD_TO_WHITELIST);
    auto importRet = ImportPeerEntities(others);
    TP_TRACE_END(TP_SMEM_GROUP_ADD_TO_WHITELIST, importRet == SMEM_OK ? 0 : 1);
    if (importRet != SMEM_OK) {
        return importRet;
    }

    TP_TRACE_BEGIN(TP_SMEM_GROUP_EXPORT_ENTITY);
    auto sliceRet = ImportPeerSlices(others);
    TP_TRACE_END(TP_SMEM_GROUP_EXPORT_ENTITY, sliceRet == SMEM_OK ? 0 : 1);
    if (sliceRet != SMEM_OK) {
        return sliceRet;
    }

    SM_LOG_DEBUG("OnAddToWhitelist done, rank=" << rankId << " peers=" << others.size());
    return SMEM_OK;
}

int SmemBmEntry::ValidatePeerPayloads(uint32_t rankId, const std::vector<RankFullInfo> &others) noexcept
{
    for (auto &info : others) {
        if (info.baseInfo.size() > EXCHANGE_INFO_PAYLOAD_MAX) {
            SM_LOG_ERROR("rankId: " << rankId << " other rank: " << info.rankId
                                    << " entity too large: " << info.baseInfo.size());
            return BM_INVALID_PARAM;
        }

        for (auto &slice : info.externalInfo) {
            if (slice.size() > EXCHANGE_INFO_PAYLOAD_MAX) {
                SM_LOG_ERROR("rankId: " << rankId << " other rank: " << info.rankId
                                        << " slice too large: " << slice.size());
                return BM_INVALID_PARAM;
            }
        }
    }
    return SMEM_OK;
}

int SmemBmEntry::ImportPeerEntities(const std::vector<RankFullInfo> &others) noexcept
{
    std::vector<hybm_exchange_info> entities(others.size());
    for (auto i = 0U; i < others.size(); ++i) {
        std::copy(others[i].baseInfo.begin(), others[i].baseInfo.end(), entities[i].desc);
        entities[i].descLen = others[i].baseInfo.size();
    }

    if (auto ret = hybm_import(entity_, entities.data(), entities.size(), nullptr, HYBM_FLAG_EXPORT_ENTITY);
        ret != BM_OK) {
        SM_LOG_ERROR("failed to import entity: " << ret);
        return SMEM_ERROR;
    }
    return SMEM_OK;
}

int SmemBmEntry::ImportPeerSlices(const std::vector<RankFullInfo> &others) noexcept
{
    uint32_t totalSliceCount = 0;
    std::for_each(others.begin(), others.end(),
                  [&totalSliceCount](const RankFullInfo &info) { totalSliceCount += info.externalInfo.size(); });
    if (totalSliceCount == 0) {
        return SMEM_OK;
    }
    uint32_t i = 0;
    std::vector<hybm_exchange_info> slices(totalSliceCount);
    for (auto &info : others) {
        for (auto &slice : info.externalInfo) {
            std::copy(slice.begin(), slice.end(), slices[i].desc);
            slices[i++].descLen = slice.size();
        }
    }
    if (auto ret = hybm_import(entity_, slices.data(), slices.size(), nullptr, 0); ret != BM_OK) {
        SM_LOG_ERROR("hybm import slice failed, result: " << ret << " local_rank:" << options_.rank);
        return SMEM_ERROR;
    }
    return SMEM_OK;
}

int SmemBmEntry::OnRemoveFromWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others,
                                       uint64_t reqId) noexcept
{
    (void)rankId;
    (void)reqId;
    for (const auto &other : others) {
        auto ret = hybm_remove_imported(entity_, other.rankId, 0);
        if (ret != SMEM_OK) {
            SM_LOG_WARN("SmemBmEntry::OnRemoveFromWhitelist remove rank " << other.rankId << " failed: " << ret);
        }
    }
    SM_LOG_DEBUG("OnRemoveFromWhitelist done, rank=" << rankId << " peers=" << others.size());
    return SMEM_OK;
}

int SmemBmEntry::OnEstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &peers, uint64_t reqId) noexcept
{
    (void)rankId;
    (void)reqId;
    if (peers.empty()) {
        SM_LOG_DEBUG("OnEstablishConnection rankId=" << rankId << " peers empty, skip");
        return SMEM_OK;
    }
    for (auto &p : peers) {
        if (p.externalInfo.empty())
            continue;
        std::vector<hybm_exchange_info> slices(p.externalInfo.size());
        for (size_t i = 0; i < p.externalInfo.size(); ++i) {
            std::copy(p.externalInfo[i].begin(), p.externalInfo[i].end(), slices[i].desc);
            slices[i].descLen = p.externalInfo[i].size();
        }
        if (auto ret = hybm_import(entity_, slices.data(), slices.size(), nullptr, 0); ret != BM_OK) {
            SM_LOG_ERROR("import slice from ESTABLISH failed, rank=" << p.rankId << " ret=" << ret);
        }
    }
    hybm_mmap(entity_, 0);
    TP_TRACE_BEGIN(TP_SMEM_GROUP_ESTABLISH_CONNECTION);
    std::vector<uint32_t> rankIds;
    rankIds.reserve(peers.size());
    for (auto &p : peers)
        rankIds.push_back(p.rankId);
    auto ret = hybm_transport_connect(entity_, rankIds.data(), static_cast<uint32_t>(rankIds.size()), 0);
    if (ret != SMEM_OK) {
        SM_LOG_ERROR("OnEstablishConnection rankId=" << rankId << " hybm_transport_connect failed: " << ret);
        TP_TRACE_END(TP_SMEM_GROUP_ESTABLISH_CONNECTION, 1);
        return ret;
    }
    TP_TRACE_END(TP_SMEM_GROUP_ESTABLISH_CONNECTION, 0);
    SM_LOG_INFO("OnEstablishConnection rankId=" << rankId << " peers.size=" << peers.size());
    return SMEM_OK;
}

int SmemBmEntry::OnCloseConnection(uint32_t rankId, const std::vector<uint32_t> &peers, uint64_t reqId) noexcept
{
    (void)rankId;
    (void)reqId;
    for (auto peer : peers) {
        auto ret = hybm_unmap_rank(entity_, peer);
        if (ret != SMEM_OK) {
            SM_LOG_WARN("OnCloseConnection unmap rank " << peer << " failed: " << ret);
        }
    }
    SM_LOG_DEBUG("OnCloseConnection rankId=" << rankId << " peers.size=" << peers.size());
    return SMEM_OK;
}

std::vector<ock::smem::LinkStateEntry> SmemBmEntry::OnQueryLinkState() noexcept
{
    std::vector<ock::smem::LinkStateEntry> entries;
    if (entity_ == nullptr) {
        return entries;
    }
    for (uint32_t peerRank = 0; peerRank < options_.rankSize; ++peerRank) {
        if (peerRank == options_.rank) {
            continue;
        }
        hybm_data_op_type reachTypes = static_cast<hybm_data_op_type>(0);
        auto ret = hybm_entity_reach_types(entity_, peerRank, reachTypes, 0);
        LinkStateEntry entry;
        entry.dstRankId = peerRank;
        entry.state = (ret == 0 && reachTypes != 0) ? BM_LINK_CONNECTED : BM_LINK_IDLE;
        entries.push_back(entry);
    }
    SM_LOG_DEBUG("OnQueryLinkState: " << entries.size() << " peers");
    return entries;
}

int SmemBmEntry::OnLeaveNotify(uint32_t leavingRankId) noexcept
{
    (void)leavingRankId;
    SM_LOG_DEBUG("OnLeaveNotify leavingRankId=" << leavingRankId);
    return SMEM_OK;
}

int SmemBmEntry::OnAddSlices(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) noexcept
{
    (void)reqId;
    if (newSlices.empty()) {
        return SM_OK;
    }
    constexpr size_t kInfoSize = sizeof(hybm_exchange_info);
    std::vector<hybm_exchange_info> infos;
    infos.reserve(newSlices.size());
    for (const auto &slice : newSlices) {
        hybm_exchange_info info{};
        auto copyLen = std::min(slice.size(), kInfoSize);
        std::copy(slice.begin(), slice.begin() + copyLen, reinterpret_cast<uint8_t *>(&info));
        infos.push_back(info);
    }
    if (auto ret = hybm_import(entity_, infos.data(), infos.size(), nullptr, 0); ret != BM_OK) {
        SM_LOG_ERROR("OnAddSlices: hybm_import failed, extendingRank=" << extendingRankId << " ret=" << ret);
        return SM_ERROR;
    }
    hybm_mmap(entity_, 0);
    SM_LOG_INFO("OnAddSlices extendingRank=" << extendingRankId << " slices=" << newSlices.size());
    return SM_OK;
}
} // namespace smem
} // namespace ock
