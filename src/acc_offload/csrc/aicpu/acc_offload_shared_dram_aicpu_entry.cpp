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
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hybm_big_mem.h"
#include "hybm_device_copy.h"
#include "dl_acl_api.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "acc_offload_pool_fingerprint.h"
#include "acc_offload_shared_dram_aicpu_entry.h"

namespace ock::offload {

int32_t AccOffloadSharedDramAicpuEntry::ValidateBackendConfig(const offload_config_t &config) const
{
    const auto soc = mf::DlAclApi::GetAscendSocType();
    const auto localWorld = NormalizeLocalWorldSize(config.worldSize, config.localWorldSize);
    if (soc != mf::ASCEND_910B || localWorld < 2U) {
        OFFLOAD_LOG_ERROR("shared AICPU requires Ascend910B and at least two local NPUs, soc="
                          << static_cast<uint32_t>(soc) << " localWorldSize=" << localWorld);
        return OFFLOAD_ERROR;
    }
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramAicpuEntry::InitializePool(const offload_config_t &config)
{
    auto ret = ValidateBackendConfig(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = ResolveStoreUrl(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = CreateGroupStore(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    constexpr uint64_t gibMask = (1ULL << 30) - 1;
    options_ = {};
    options_.bmType = HYBM_TYPE_HOST_INITIATE;
    options_.memType = HYBM_MEM_TYPE_HOST;
    options_.bmDataOpType = HYBM_DOP_TYPE_DEVICE_RDMA;
    options_.rankCount = config.worldSize;
    options_.rankId = config.rankId;
    options_.devId = config.deviceId;
    options_.maxDRAMSize = (config.reserveSize + gibMask) & ~gibMask;
    options_.hostVASpace = (config.allocSize + gibMask) & ~gibMask;
    options_.role = HYBM_ROLE_PEER;
    options_.scene = HYBM_SCENE_OFFLOAD; // offload 独立 VA 区段(常规 GVM 区最前2T)，与 BM 物理隔离
    options_.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
    options_.dramShmFd = -1;
    localWorldSize_ = NormalizeLocalWorldSize(config.worldSize, config.localWorldSize);
    if (options_.hostVASpace != 0) {
        const auto enable4K = mf::MfEnvUtil::GetOptionalUintOrDefault(mf::env::MF_HYBM_ENABLE_4K_PAGE, 0U) != 0U;
        backingFd_ = memfd_create("mf-offload", MFD_CLOEXEC | (enable4K ? 0U : MFD_HUGETLB));
        if (backingFd_ < 0) {
            OFFLOAD_LOG_ERROR("create shared DRAM backing failed, rank=" << config.rankId << " errno=" << errno);
            return OFFLOAD_ERROR;
        }
        options_.dramShmFd = backingFd_;
        options_.flags |= HYBM_FLAG_CREATE_WITH_SHM;
    }
    options_.enable56BitsGva = false;
    if (storeUrl_.size() >= sizeof(options_.transUrl)) {
        OFFLOAD_LOG_ERROR("store url too long, storeUrl: " << storeUrl_ << ", rankId: " << config.rankId);
        return OFFLOAD_ERROR;
    }
    std::copy_n(storeUrl_.c_str(), storeUrl_.size(), options_.transUrl);

    ret = CreateEntity();
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = ReserveSharedPool();
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
    ret = RegisterPeerPool();
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = ExportAndImportEntityInfo();
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = ExchangeSliceInfos();
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("allgather/import slice info failed, result: " << ret << ", rankId: " << config.rankId
                                                                         << ", worldSize: " << config.worldSize);
        return OFFLOAD_ERROR;
    }
    for (uint32_t rank = 0; rank < options_.rankCount; ++rank) {
        if (rank == options_.rankId) {
            continue;
        }
        ret = hybm_transport_connect(entity_, &rank, 1, 0);
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_ERROR("offload transport connect failed, rank=" << rank << " ret=" << ret);
            return ret;
        }
    }
    ret = MapPool(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = ExchangeAndValidatePoolFingerprint(config);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    return PrepareSharedCopy();
}

int32_t AccOffloadSharedDramAicpuEntry::RegisterPeerPool()
{
    struct PoolBacking {
        uint64_t size;
        uint64_t device;
        uint64_t inode;
        int32_t pid;
        int32_t fd;
    };
    struct stat status {};
    if (options_.dramShmFd >= 0 && fstat(options_.dramShmFd, &status) != 0) {
        OFFLOAD_LOG_ERROR("stat shared DRAM failed, rank=" << options_.rankId << " errno=" << errno);
        return OFFLOAD_ERROR;
    }
    PoolBacking self{options_.hostVASpace, status.st_dev, status.st_ino, getpid(), options_.dramShmFd};
    std::vector<PoolBacking> pools(options_.rankCount);
    auto ret = group_->GroupAllGather(reinterpret_cast<const char *>(&self), sizeof(self),
                                      reinterpret_cast<char *>(pools.data()), pools.size() * sizeof(self));
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("shared DRAM backing exchange failed, rank=" << options_.rankId << " ret=" << ret);
        return ret;
    }
    const auto rank =
        options_.rankId / localWorldSize_ * localWorldSize_ + (options_.rankId + localWorldSize_ - 1) % localWorldSize_;
    const auto &peer = pools[rank];
    if (peer.size == 0) {
        return OFFLOAD_OK;
    }
    auto path = "/proc/" + std::to_string(peer.pid) + "/fd/" + std::to_string(peer.fd);
    auto fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &status) != 0 || status.st_dev != peer.device || status.st_ino != peer.inode ||
        peer.size > options_.maxDRAMSize || status.st_size < 0 || static_cast<uint64_t>(status.st_size) != peer.size) {
        OFFLOAD_LOG_ERROR("open local peer DRAM failed, rank=" << options_.rankId << " peer=" << rank << " path="
                                                               << path << " size=" << peer.size << " errno=" << errno);
        if (fd >= 0) {
            close(fd);
        }
        return OFFLOAD_ERROR;
    }
    ret = MapPeerPool(fd, rank, peer.size);
    close(fd);
    return ret;
}

int32_t AccOffloadSharedDramAicpuEntry::MapPeerPool(int fd, uint32_t rank, uint64_t size)
{
    auto base = static_cast<uint8_t *>(hybm_get_memory_ptr(entity_, HYBM_MEM_TYPE_HOST));
    if (base == nullptr) {
        OFFLOAD_LOG_ERROR("get shared pool address failed, rank=" << options_.rankId);
        return OFFLOAD_ERROR;
    }
    auto address = base + options_.maxDRAMSize * rank;
    auto mapped = mmap(address, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_POPULATE, fd, 0);
    if (mapped == MAP_FAILED) {
        OFFLOAD_LOG_ERROR("map local peer DRAM failed, rank=" << rank << " size=" << size << " errno=" << errno);
        return OFFLOAD_ERROR;
    }
    auto slice = hybm_register_local_memory(entity_, mapped, size, 0);
    if (slice == nullptr) {
        OFFLOAD_LOG_ERROR("register local peer DRAM failed, rank=" << rank << " size=" << size);
        return OFFLOAD_ERROR;
    }
    slices_.push_back(slice);
    hybm_exchange_info info{};
    auto ret = hybm_export(entity_, slice, 0, &info);
    if (ret != OFFLOAD_OK) {
        hybm_export_info_free(&info);
        return ret;
    }
    sliceInfos_.push_back(info);
    peerPoolSize_ = size;
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramAicpuEntry::PrepareSharedCopy()
{
    const auto first = options_.rankId / localWorldSize_ * localWorldSize_;
    const auto next = first + (options_.rankId + 1) % localWorldSize_;
    const auto previous = first + (options_.rankId + localWorldSize_ - 1) % localWorldSize_;
    auto ret = OFFLOAD_OK;
    if (size_ != 0) {
        ret = hybm_prepare_device_copy(entity_, next, reinterpret_cast<uint64_t>(base_), size_);
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_ERROR("prepare own shared pool failed, peer=" << next << " ret=" << ret);
            return ret;
        }
    }
    if (peerPoolSize_ != 0) {
        auto address = reinterpret_cast<uint64_t>(hostGva_) + options_.maxDRAMSize * previous;
        ret = hybm_prepare_device_copy(entity_, previous, address, peerPoolSize_);
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_ERROR("prepare mapped peer pool failed, peer=" << previous << " ret=" << ret);
        }
    }
    return ret;
}

int32_t AccOffloadSharedDramAicpuEntry::RegisterMemory(const void *address, uint64_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        OFFLOAD_LOG_ERROR("pool not initialized, memory registration failed, address=" << address << " size=" << size);
        return OFFLOAD_ERROR;
    }
    auto slice = hybm_register_local_memory(entity_, address, size, 0);
    if (slice == nullptr) {
        OFFLOAD_LOG_ERROR("offload memory registration failed, address=" << address << " size=" << size);
        return OFFLOAD_ERROR;
    }
    slices_.push_back(slice);
    return OFFLOAD_OK;
}

int32_t AccOffloadSharedDramAicpuEntry::EntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint8_t devIdx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_ || !entryTableRegistered_) {
        OFFLOAD_LOG_ERROR("entry not initialized or no table registered"
                          << ", inited: " << inited_ << ", tableRegistered: " << entryTableRegistered_
                          << ", rankId: " << options_.rankId);
        return OFFLOAD_ERROR;
    }

    AccOffloadEntryGatherLayout layout = entryTable_;
    layout.poolGva = reinterpret_cast<uint64_t>(hostGva_);
    uint32_t capacity = 0;
    for (const auto slice : slices_) {
        const auto base = reinterpret_cast<uint64_t>(hybm_get_slice_va(entity_, slice, 0));
        const auto bytes = hybm_get_slice_size(entity_, slice);
        if (dstPtr >= base && dstPtr - base < bytes) {
            capacity = static_cast<uint32_t>(std::min<uint64_t>((bytes - (dstPtr - base)) / layout.entryBytes, 65536U));
            break;
        }
    }
    if (capacity == 0) {
        OFFLOAD_LOG_ERROR("entry gather destination has no registered space for one entry, destination="
                          << dstPtr << " entryBytes=" << layout.entryBytes << " rank=" << options_.rankId);
        return OFFLOAD_ERROR;
    }
    auto submit = [](void *entity, uint64_t *sources, uint64_t *destinations, uint32_t *lengths, uint32_t *count,
                     void *stream) {
        const hybm_device_copy_params params{sources, destinations, lengths, count, 0U, stream, {}};
        return hybm_data_device_copy(entity, &params);
    };
    return AccOffloadLaunchApi::AccOffloadTransportEntryGather(submit, entity_, dstPtr, idsPtr, countPtr, capacity,
                                                               layout, devIdx);
}

int32_t AccOffloadSharedDramAicpuEntry::GetDva(uint64_t hostPtr, uint64_t *dvaPtr)
{
    return OFFLOAD_FUNCTION_FAILED;
}

int32_t AccOffloadSharedDramAicpuEntry::SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                                   uint32_t *sizePtr, uint8_t devIdx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        OFFLOAD_LOG_ERROR("shared AICPU pool not initialized, device=" << static_cast<uint32_t>(devIdx));
        return OFFLOAD_ERROR;
    }
    AccOffloadAicpuParam param{entity_, srcPtrs, dstPtrs, lenPtrs, sizePtr};
    auto submit = [](const AccOffloadAicpuParam &input, void *stream) {
        const hybm_device_copy_params params{
            input.srcPtrs, input.dstPtrs, input.lenPtrs, input.sizePtr, 0U, stream, {}};
        return hybm_data_device_copy(input.entity, &params);
    };
    return AccOffloadLaunchApi::AccOffloadAicpuLaunch(submit, param, devIdx);
}

void AccOffloadSharedDramAicpuEntry::ReleaseBackendResources()
{
    if (backingFd_ >= 0) {
        if (close(backingFd_) != 0) {
            OFFLOAD_LOG_ERROR("close shared pool backing failed, fd=" << backingFd_ << " errno=" << errno);
        }
        backingFd_ = -1;
    }
    options_.dramShmFd = -1;
    peerPoolSize_ = 0;
    localWorldSize_ = 0;
}

} // namespace ock::offload
