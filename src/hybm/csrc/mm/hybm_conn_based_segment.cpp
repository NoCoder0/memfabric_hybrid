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
#include "hybm_conn_based_segment.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "vendor/ascend/dl_hal_api.h"
#include "hybm_def.h"
#include "hybm_ex_info_transfer.h"
#include "hybm_gva.h"
#include "hybm_logger.h"
#include "hybm_numa_util.h"
#include "hybm_va_manager.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "mf_num_util.h"

using namespace ock::mf;

Result HybmConnBasedSegment::ValidateOptions() noexcept
{
    if (options_.segType != HYBM_MST_DRAM || options_.maxSize == 0 || (options_.maxSize % HYBM_LARGE_PAGE_SIZE) != 0) {
        BM_LOG_ERROR("Validate options error type(" << options_.segType << ") size(" << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    if (UINT64_MAX / options_.maxSize < options_.rankCnt) {
        BM_LOG_ERROR("Validate options error rankCnt(" << options_.rankCnt << ") size(" << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    constexpr uint32_t deviceInterMask = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK;
    const uint32_t deviceInter = options_.dataOpType & deviceInterMask;
    const bool hostCombo = (options_.dataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0U && deviceInter != 0U;
    // 显式角色标志优先，未指定时按 size 兜底；两标志互斥、标志与 size 冲突均显式拒绝（见 smem_bm 侧）。
    constexpr uint32_t roleFlagMask = HYBM_FLAG_PROXY | HYBM_FLAG_PROVIDER;
    const uint32_t explicitRoleFlags = options_.flags & roleFlagMask;
    if (explicitRoleFlags == roleFlagMask) {
        BM_LOG_ERROR("provider and proxy role flags are mutually exclusive, flags:" << options_.flags);
        return BM_INVALID_PARAM;
    }
    const bool providerFlag = (explicitRoleFlags & HYBM_FLAG_PROVIDER) != 0U;
    const bool proxyFlag = (explicitRoleFlags & HYBM_FLAG_PROXY) != 0U;
    if (hostCombo && ((providerFlag && options_.size == 0U) || (proxyFlag && options_.size > 0U))) {
        BM_LOG_ERROR("role flags conflict with provider memory size, flags:" << options_.flags
                                                                             << ", size:" << options_.size);
        return BM_INVALID_PARAM;
    }
    const bool provider = providerFlag || (explicitRoleFlags == 0U && options_.size > 0U);
    role_ = !hostCombo ? HostRole::SELF : (provider ? HostRole::PROVIDER : HostRole::PROXY);
    if (role_ != HostRole::SELF) {
        if ((deviceInter & (deviceInter - 1U)) != 0U) {
            BM_LOG_ERROR("HOST_SHM can only pair with one device transport, dataOpType=" << options_.dataOpType);
            return BM_INVALID_PARAM;
        }
        // A zero identity would make unrelated hosts look local and select the wrong shared pool.
        // Reject startup when neither machine-id nor boot-id was readable.
        if (machineId_.major == 0UL && machineId_.minor == 0UL) {
            BM_LOG_ERROR("machine id is all zero, failed to read /etc/machine-id, "
                         "check the file is mounted and readable in the container.");
            return BM_NOT_INITIALIZED;
        }
        if (options_.size > options_.maxSize) {
            BM_LOG_ERROR("HOST_SHM provider size(" << options_.size << ") exceeds maxSize(" << options_.maxSize << ")");
            return BM_INVALID_PARAM;
        }
        if (options_.size != 0U && (options_.size % HYBM_LARGE_PAGE_SIZE) != 0U) {
            BM_LOG_ERROR("HOST_SHM provider size(" << options_.size << ") is not aligned to 2MB");
            return BM_INVALID_PARAM;
        }
    }

    const auto policyInfo = HybmNumaUtil::GetNumaBindPolicyInfo(options_.flags, logicDeviceId_);
    if (!policyInfo.valid) {
        BM_LOG_ERROR("Failed to resolve NUMA affinity from flag:" << std::bitset<UINT32_WIDTH>(options_.flags)
                                                                  << " start index:" << HYBM_BIND_NUMA_FLAG_INDEX
                                                                  << " flag len:" << HYBM_BIND_NUMA_FLAG_LEN);
        return BM_INVALID_PARAM;
    }
    if (socType_ == AscendSocType::ASCEND_950 && policyInfo.policy != NumaBindPolicy::OFF &&
        policyInfo.socketCpus.empty()) {
        BM_LOG_ERROR("Failed to resolve NUMA affinity CPU list, policy:" << static_cast<int32_t>(policyInfo.policy)
                                                                         << " deviceId:" << logicDeviceId_ << " flag:"
                                                                         << std::bitset<UINT32_WIDTH>(options_.flags));
        return BM_INVALID_PARAM;
    }

    return BM_OK;
}

Result HybmConnBasedSegment::ReserveMemorySpace(void **address) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(ValidateOptions() == BM_OK, "Failed to validate options.", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(globalVirtualAddress_ == nullptr, "Already prepare virtual memory.", BM_NOT_INITIALIZED);
    BM_ASSERT_LOG_AND_RETURN(address != nullptr, "Invalid param, address is NULL.", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(PrepareShareMemoryFd() == BM_OK, "PrepareShareMemoryFd failed.", BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(options_.rankId < options_.rankCnt,
                             "rank(" << options_.rankId << ") but total " << options_.rankCnt, BM_INVALID_PARAM);

    uint64_t totalSize = options_.rankCnt * options_.maxSize;
    uint64_t localSize = options_.enable56BitsGva ? options_.maxSize : totalSize;
    auto gvaInfo = HybmVaManager::GetInstance().AllocReserveGva(
        {totalSize, localSize, HYBM_MEM_TYPE_HOST, options_.enable56BitsGva, options_.scene, requestedAddress_});
    BM_ASSERT_LOG_AND_RETURN(gvaInfo.va[HVM_GVA] > 0, "Invalid param, start is 0.", BM_ERROR);
    void *startAddr = reinterpret_cast<void *>(gvaInfo.va[HVM_GVA]);
    if (!options_.enable56BitsGva) {
        void *mapped = mmap(startAddr, totalSize, PROT_NONE,
                            MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE, -1, 0);

        if (mapped == MAP_FAILED || (uint64_t)mapped != (uint64_t)startAddr) {
            BM_LOG_ERROR("Failed to mmap size:" << totalSize << " addr:" << startAddr << " ret:" << mapped
                                                << " error: " << errno);
            return BM_ERROR;
        }
    }
    globalVirtualAddress_ = (uint8_t *)startAddr;
    totalVirtualSize_ = totalSize;
    if (options_.enable56BitsGva) {
        localVirtualBase_ = (uint8_t *)gvaInfo.va[HVM_DVA];
    } else {
        localVirtualBase_ = globalVirtualAddress_ + options_.maxSize * options_.rankId;
    }
    allocatedSize_ = 0UL;
    sliceCount_ = 0;
    BM_LOG_INFO("conn based segment reserved, rankId:" << options_.rankId << " rankCnt:" << options_.rankCnt
                                                       << " base:" << startAddr << " windowSize:" << options_.maxSize
                                                       << " totalSize:" << totalSize);

    if (role_ == HostRole::PROVIDER) {
        auto ret = MapProviderShm();
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider map local shm failed: " << ret);
            if (!options_.enable56BitsGva) {
                (void)munmap(globalVirtualAddress_, totalVirtualSize_);
            }
            HybmVaManager::GetInstance().FreeReserveGva(reinterpret_cast<uintptr_t>(globalVirtualAddress_));
            globalVirtualAddress_ = nullptr;
            totalVirtualSize_ = 0;
            localVirtualBase_ = nullptr;
            return ret;
        }
    }

    *address = globalVirtualAddress_;
    return BM_OK;
}

Result HybmConnBasedSegment::UnReserveMemorySpace() noexcept
{
    BM_LOG_INFO("un-reserve memory space.");
    FreeMemory();
    return BM_OK;
}

void HybmConnBasedSegment::LvaShmReservePhysicalMemory(void *mappedAddress, uint64_t size) noexcept
{
    BM_ASSERT_RET_VOID(mappedAddress != nullptr, "mappedAddress is nullptr");
    auto *pos = static_cast<uint8_t *>(mappedAddress);
    uint64_t setLength = 0;
    while (setLength < size) {
        *pos = 0;
        setLength += HYBM_LARGE_PAGE_SIZE;
        pos += HYBM_LARGE_PAGE_SIZE;
    }

    pos = static_cast<uint8_t *>(mappedAddress) + (size - 1L);
    *pos = 0;
}

Result HybmConnBasedSegment::AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept
{
    if (role_ == HostRole::PROXY) {
        BM_LOG_ERROR("proxy does not allocate local memory, size: " << size);
        return BM_NOT_SUPPORTED;
    }

    if (role_ == HostRole::PROVIDER) {
        // Proxy address records currently allow one pool per provider rank.
        // Multiple slices would overwrite the rank address registration.
        if (!slices_.empty()) {
            BM_LOG_ERROR("provider supports a single pool allocation, providerRank:"
                         << options_.rankId << " allocatedBlocks:" << slices_.size()
                         << " allocatedSize:" << allocatedSize_ << " requestSize:" << size);
            return BM_NOT_SUPPORTED;
        }
        if ((size % HYBM_LARGE_PAGE_SIZE) != 0UL || allocatedSize_ > options_.size ||
            size > options_.size - allocatedSize_) {
            BM_LOG_ERROR("invalid allocate size: " << size << ", used " << allocatedSize_ << " of " << options_.size);
            return BM_INVALID_PARAM;
        }
        void *sliceAddr = localShmBase_ + allocatedSize_;
        auto gva =
            reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * options_.rankId + allocatedSize_;
        // dva 必须填 0（provider 分支不执行 HalHostRegister），ownership 必须保持默认 Local：
        // 若误用 Imported 会让 ClassifyAddressMask 把自有池判为远端内存，preferHostShm 优先级被反转。
        auto ret = HybmVaManager::GetInstance().AddVaInfo(
            {gva, 0, reinterpret_cast<uint64_t>(sliceAddr), size, HYBM_MEM_TYPE_HOST});
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider register pool gva failed, gva:" << gva << " va:" << sliceAddr << " size:" << size
                                                                   << " ret:" << ret);
            return ret;
        }
        allocatedSize_ += size;
        slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, gva,
                                           reinterpret_cast<uint64_t>(sliceAddr), size);
        slices_.emplace(slice->index_, slice);
        BM_LOG_INFO("provider allocate slice(idx:" << slice->index_ << ", size:" << slice->size_ << " gva:" << gva
                                                   << " va:" << sliceAddr << ").");
        return BM_OK;
    }

    if ((size % HYBM_LARGE_PAGE_SIZE) != 0UL || size + allocatedSize_ > options_.maxSize) {
        BM_LOG_ERROR("invalid allocate memory size : " << size << ", now used " << allocatedSize_ << " of "
                                                       << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    void *sliceAddr = localVirtualBase_ + allocatedSize_;
    auto gva = reinterpret_cast<uint64_t>(globalVirtualAddress_ + options_.maxSize * options_.rankId + allocatedSize_);
    void *mapped = nullptr;
    MemAllocMethod allocMethod = MemAllocMethod::MMAP;
    auto ret = MapSlice(mapped, sliceAddr, allocatedSize_, size, gva, allocMethod);
    if (ret != BM_OK) {
        return ret;
    }
    allocatedSize_ += size;
    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, gva,
                                       reinterpret_cast<uint64_t>(mapped), size, allocMethod);
    slices_.emplace(slice->index_, slice);
    BM_LOG_DEBUG("allocate slice(idx:" << slice->index_ << ", size:" << slice->size_ << " va:" << mapped << ").");
    return BM_OK;
}

Result HybmConnBasedSegment::Export(const MemSlicePtr &slice, std::string &exInfo) noexcept
{
    if (slice == nullptr) {
        BM_LOG_ERROR("input slice is nullptr");
        return BM_INVALID_PARAM;
    }
    auto pos = slices_.find(slice->index_);
    if (pos == slices_.end() || pos->second.slice != slice) {
        BM_LOG_ERROR("input slice(idx:" << slice->index_ << ") not exist or not match.");
        return BM_INVALID_PARAM;
    }
    if (role_ == HostRole::PROXY) {
        BM_LOG_ERROR("proxy does not export local slices.");
        return BM_NOT_SUPPORTED;
    }

    auto exp = exportMap_.find(slice->index_);
    if (exp != exportMap_.end()) {
        exInfo = exp->second;
        return BM_OK;
    }

    HostExportInfo info;
    info.gva = slice->gva_;
    info.sliceIndex = static_cast<uint32_t>(slice->index_);
    info.rankId = options_.rankId;
    info.size = slice->size_;
    info.pageTblType = MEM_PT_TYPE_SVM;
    info.memSegType = role_ == HostRole::PROVIDER ? HYBM_DRAM_PROVIDER : HYBM_MST_DRAM;
    info.exchangeType = HYBM_INFO_EXG_IN_NODE;
    info.deviceId = static_cast<uint16_t>(options_.devId);
    const int32_t localPhy = devicePhyId_ >= 0 ? devicePhyId_ : static_cast<int32_t>(options_.devId);
    info.phyDevId = role_ == HostRole::PROVIDER ? 0U : static_cast<uint16_t>(localPhy);
    info.machineId = machineId_;
    auto ret = LiteralExInfoTranslater<HostExportInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export info failed: " << ret);
        return BM_ERROR;
    }
    exportMap_[slice->index_] = exInfo;
    return BM_OK;
}

Result HybmConnBasedSegment::Export(std::string &exInfo) noexcept
{
    (void)exInfo;
    return BM_OK;
}

MemSlicePtr HybmConnBasedSegment::GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept
{
    auto index = MemSlice::GetIndexFrom(slice);
    auto pos = slices_.find(index);
    if (pos == slices_.end()) {
        if (quiet) {
            BM_LOG_DEBUG("Failed to get slice, index(" << index << ") not find");
        } else {
            BM_LOG_ERROR("Failed to get slice, index(" << index << ") not find");
        }
        return nullptr;
    }
    auto target = pos->second.slice;
    if (!target->ValidateId(slice)) {
        if (quiet) {
            BM_LOG_DEBUG("Failed to get slice, slice is invalid index(" << index << ")");
        } else {
            BM_LOG_ERROR("Failed to get slice, slice is invalid index(" << index << ")");
        }
        return nullptr;
    }
    return target;
}

Result HybmConnBasedSegment::DeserializeHostInfos(const std::vector<std::string> &allExInfo,
                                                  std::vector<HostExportInfo> &infos, void *addresses[]) noexcept
{
    LiteralExInfoTranslater<HostExportInfo> translator;
    for (auto i = 0U; i < allExInfo.size(); i++) {
        if (translator.Deserialize(allExInfo[i], infos[i]) != 0) {
            BM_LOG_ERROR("deserialize imported info(" << i << ") failed.");
            return BM_INVALID_PARAM;
        }
        if (addresses != nullptr) {
            addresses[i] = reinterpret_cast<void *>(infos[i].gva);
        }
    }
    return BM_OK;
}

Result HybmConnBasedSegment::Import(const std::vector<std::string> &allExInfo, void *addresses[], uint32_t overrideRank,
                                    std::vector<ImportEntryKind> &entryKinds) noexcept
{
    (void)overrideRank;
    std::vector<HostExportInfo> infos{allExInfo.size()};
    auto ret = DeserializeHostInfos(allExInfo, infos, addresses);
    if (ret != BM_OK) {
        return ret;
    }

    BM_ASSERT_LOG_AND_RETURN(entryKinds.size() == allExInfo.size(), "entryKinds must be sized by caller",
                             BM_INVALID_PARAM);
    if (role_ == HostRole::PROVIDER) {
        // provider 侧看到的 proxy 导出信息没有可连接端点，统一忽略
        std::fill(entryKinds.begin(), entryKinds.end(), ImportEntryKind::IgnoredProxy);
        return BM_OK;
    }
    if (role_ == HostRole::PROXY) {
        // proxy 侧只把 provider 池条目标记为 ProviderPool，其余（其它 proxy 的导出）忽略
        std::fill(entryKinds.begin(), entryKinds.end(), ImportEntryKind::IgnoredProxy);
        // Open outside mutex_ so readiness retries do not block address classification.
        ProviderShmFile preopened;
        ret = PreOpenLocalProviderShm(infos, preopened);
        if (ret != BM_OK) {
            return ret;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto i = 0U; i < infos.size(); i++) {
            if (infos[i].memSegType != HYBM_DRAM_PROVIDER) {
                continue;
            }
            ret = ImportHostProvider(i, infos[i], entryKinds, preopened);
            if (ret != BM_OK) {
                return ret;
            }
        }
        return BM_OK;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    try {
        std::copy(infos.begin(), infos.end(), std::back_inserter(imports_));
    } catch (...) {
        BM_LOG_ERROR("copy failed.");
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

Result HybmConnBasedSegment::Mmap() noexcept
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (role_ == HostRole::PROVIDER) {
        imports_.clear();
        return BM_OK;
    }
    if (role_ == HostRole::PROXY) {
        auto &vaMgr = HybmVaManager::GetInstance();
        for (const auto &item : importedProviders_) {
            if (item.first == localProviderRank_) {
                continue;
            }
            const auto &info = item.second;
            AllocatedGvaInfo existing;
            if (vaMgr.FindAllocatedVaInfo(info.gva, info.size, HVM_GVA, existing)) {
                const bool sameRange = existing.base.va[HVM_GVA] == info.gva && existing.base.size == info.size;
                if (sameRange && existing.IsImported()) {
                    vaMgr.RemoveOneVaInfo(existing.base.va[HVM_GVA]);
                }
            }
            auto ret =
                vaMgr.AddVaInfo({info.gva, 0, 0, info.size, HYBM_MEM_TYPE_HOST}, false, AddressOwnership::Imported);
            BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "register remote provider gva failed, ret = " << ret, ret);
            mappedGvaMem_.insert(info.gva);
        }
        imports_.clear();
        return BM_OK;
    }

    for (const auto &import : imports_) {
        if (import.rankId == options_.rankId || import.size == 0) {
            continue;
        }
        auto &vaMgr = HybmVaManager::GetInstance();
        AllocatedGvaInfo existing;
        if (vaMgr.FindAllocatedVaInfo(import.gva, import.size, HVM_GVA, existing)) {
            const bool sameRange = existing.base.va[HVM_GVA] == import.gva && existing.base.size == import.size;
            if (sameRange && existing.IsImported()) {
                vaMgr.RemoveOneVaInfo(existing.base.va[HVM_GVA]);
            }
        }
        const auto [it, inserted] = mappedGvaMem_.insert(import.gva);
        auto ret =
            vaMgr.AddVaInfo({import.gva, 0, 0, import.size, HYBM_MEM_TYPE_HOST}, false, AddressOwnership::Imported);
        if (ret != BM_OK) {
            if (inserted) {
                mappedGvaMem_.erase(it);
            }
            BM_LOG_ERROR("AddVaInfo failed, rankId=" << import.rankId << " gva=" << import.gva << " ret=" << ret);
            return ret;
        }
    }
    imports_.clear();
    return 0;
}

Result HybmConnBasedSegment::Unmap() noexcept
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (LocalProviderMapped()) {
        const auto ret = UnmapLocalProviderRegionAsReserved();
        if (ret != BM_OK) {
            return ret;
        }
    }
    CloseImportedShmFd();
    for (auto gva : mappedGvaMem_) {
        HybmVaManager::GetInstance().RemoveOneVaInfo(gva);
    }
    mappedGvaMem_.clear();
    return 0;
}

bool HybmConnBasedSegment::MemoryInRange(const void *begin, uint64_t size) const noexcept
{
    if (begin < globalVirtualAddress_) {
        return false;
    }

    if (reinterpret_cast<const uint8_t *>(begin) + size > globalVirtualAddress_ + totalVirtualSize_) {
        return false;
    }

    return true;
}

bool HybmConnBasedSegment::IsLocalRange(const void *begin, uint64_t size) const noexcept
{
    if (begin < localVirtualBase_) {
        return false;
    }
    if (reinterpret_cast<const uint8_t *>(begin) + size > localVirtualBase_ + options_.maxSize) {
        return false;
    }
    return true;
}

void HybmConnBasedSegment::FreeMemory() noexcept
{
    while (!slices_.empty()) {
        auto slice = slices_.begin()->second.slice;
        const bool ownsBackingMemory = role_ == HostRole::SELF && (slice->gva_ != 0U);
        ReleaseSliceMemory(slice);
        if (ownsBackingMemory) {
            FreeAllocatedMemory(reinterpret_cast<void *>(slice->vAddress_), slice->size_, slice->allocMethod_);
        }
    }
    Unmap();

    // provider 自身映射的解除：
    if (role_ == HostRole::PROVIDER && localShmBase_ != nullptr) {
        const uint64_t base = reinterpret_cast<uint64_t>(localShmBase_);
        const uint64_t reservedBase = reinterpret_cast<uint64_t>(globalVirtualAddress_);
        const bool insideReserved =
            globalVirtualAddress_ != nullptr && base >= reservedBase && base < reservedBase + totalVirtualSize_;
        if (!insideReserved && munmap(localShmBase_, localShmSize_) != 0) {
            BM_LOG_ERROR("Failed to munmap provider shm, va:" << reinterpret_cast<void *>(localShmBase_)
                                                              << " size:" << localShmSize_ << " error:" << errno);
        }
        localShmBase_ = nullptr;
        localShmSize_ = 0UL;
    }
    if (localShmFd_ >= 0) {
        if (role_ == HostRole::PROVIDER) {
            auto shmPath = GetProviderShmFilePath(useHugetlbfs_);
            if (unlink(shmPath.c_str()) != 0 && errno != ENOENT) {
                BM_LOG_WARN("Failed to unlink provider shm " << shmPath << " error:" << errno);
            }
        }
        close(localShmFd_);
        localShmFd_ = -1;
    }

    if (localVirtualBase_ != nullptr && allocatedSize_ > 0) {
        localVirtualBase_ = nullptr;
    }
    if (globalVirtualAddress_ != nullptr) {
        if (!options_.enable56BitsGva && munmap(globalVirtualAddress_, totalVirtualSize_) != 0) {
            BM_LOG_ERROR("Failed to unmap global memory");
        }
        HybmVaManager::GetInstance().FreeReserveGva(reinterpret_cast<uintptr_t>(globalVirtualAddress_));
        globalVirtualAddress_ = nullptr;
    }
    localShmBase_ = nullptr;
    localShmSize_ = 0UL;
    totalVirtualSize_ = 0UL;
}

Result HybmConnBasedSegment::PrepareShareMemoryFd() const noexcept
{
    if (options_.shmFd < 0) {
        return BM_OK;
    }

    struct stat buf {};
    auto ret = fstat(options_.shmFd, &buf);
    if (ret != 0) {
        BM_LOG_ERROR("share mem fd: " << options_.shmFd << " stat failed: " << errno << ":" << strerror(errno));
        return BM_INVALID_PARAM;
    }

    if (static_cast<uint64_t>(buf.st_size) >= options_.size) {
        return BM_OK;
    }

    ret = ftruncate(options_.shmFd, static_cast<off_t>(options_.size));
    if (ret != 0) {
        BM_LOG_ERROR("share mem fd: " << options_.shmFd << " truncate from " << buf.st_size << " to " << options_.size
                                      << " failed: " << errno << ":" << strerror(errno));
        return BM_ERROR;
    }

    return BM_OK;
}

Result HybmConnBasedSegment::MapSlice(void *&mapped, void *sliceAddr, uint64_t lvOffset, uint64_t size, uint64_t gva,
                                      MemAllocMethod &allocMethod) noexcept
{
    if (size == 0) {
        return BM_OK;
    }

    void *dva = nullptr;
    {
        std::optional<CpuAffinityGuard> cpuGuard;
        if (socType_ == AscendSocType::ASCEND_950) {
            uint32_t flag = options_.flags;
            if (NumUtil::ExtractBits(flag, HYBM_PERFORMANCE_MODE_FLAG_INDEX, HYBM_PERFORMANCE_MODE_FLAG_LEN) != 1) {
                flag |= (1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX) | HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG;
                BM_LOG_INFO("Enable performance mode and NUMA auto affinity by default on ASCEND_950"
                            << ", deviceId:" << logicDeviceId_);
            }

            const auto policyInfo = HybmNumaUtil::GetNumaBindPolicyInfo(flag, logicDeviceId_);
            if (policyInfo.valid && policyInfo.policy != NumaBindPolicy::OFF && !policyInfo.socketCpus.empty()) {
                BM_LOG_INFO("ConnBasedSegment CPU affinity policy:"
                            << static_cast<int32_t>(policyInfo.policy)
                            << ", socketCpuCount:" << policyInfo.socketCpus.size() << " deviceId:" << logicDeviceId_);
                cpuGuard.emplace(policyInfo);
            }
        }

        mapped = AllocMemory(sliceAddr, lvOffset, size, allocMethod);
        if (mapped == MAP_FAILED) {
            BM_LOG_ERROR("Failed to alloc size:" << size << " addr:" << sliceAddr << " mapped:" << mapped
                                                 << " error:" << errno << ", " << SafeStrError(errno));
            return BM_ERROR;
        }
        if (options_.shmFd < 0) {
            LvaShmReservePhysicalMemory(mapped, size);
        }
    }

    uint32_t needReg = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK | HYBM_DOP_TYPE_MTE;
    if (options_.dataOpType & needReg) {
        auto ret = DlHalApi::HalHostRegister(mapped, size, HOST_MEM_MAP_DEV, logicDeviceId_, &dva);
        if (ret != BM_OK) {
            BM_LOG_ERROR("register host va failed, ret:" << ret);
            FreeAllocatedMemory(mapped, size, allocMethod);
            return BM_ERROR;
        }
    }
    int ret = HybmVaManager::GetInstance().AddVaInfo({gva, (uint64_t)dva, (uint64_t)mapped, size, HYBM_MEM_TYPE_HOST});
    if (ret != 0) {
        BM_LOG_ERROR("AddVaInfo failed, size: " << size << " ret: " << ret);
        if (options_.dataOpType & needReg) {
            DlHalApi::HalHostUnregisterEx(mapped, logicDeviceId_, HOST_MEM_MAP_DEV);
        }
        FreeAllocatedMemory(mapped, size, allocMethod);
        return ret;
    }
    return BM_OK;
}

void *HybmConnBasedSegment::AllocMemory(void *sliceAddr, uint64_t lvOffset, uint64_t size, MemAllocMethod &allocMethod)
{
    void *mapped;
    auto prot = PROT_READ | PROT_WRITE;
    int mmapFd = options_.shmFd < 0 ? -1 : options_.shmFd;
    int mmapFlags =
        options_.shmFd < 0 ? (MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE) : (MAP_FIXED | MAP_SHARED | MAP_POPULATE);
    uint64_t mmapOffset = options_.shmFd < 0 ? 0 : lvOffset;

    constexpr uint32_t MAP_HUGE_1GB = (30U << 26);
    mapped = mmap(sliceAddr, size, prot, mmapFlags | MAP_HUGETLB | MAP_HUGE_1GB, mmapFd, mmapOffset);
    if (mapped == sliceAddr) {
        allocMethod = MemAllocMethod::MMAP;
        return mapped;
    }

    mapped = mmap(sliceAddr, size, prot, mmapFlags | MAP_HUGETLB, mmapFd, mmapOffset);
    if (mapped == sliceAddr) {
        allocMethod = MemAllocMethod::MMAP;
        return mapped;
    }
    BM_LOG_WARN("Unable to alloc size:" << size << " with hugepage via mmap, error: " << errno << ", "
                                        << SafeStrError(errno));

    if (options_.enable56BitsGva && options_.shmFd < 0) {
        uint64_t allocFlag = MEM_HOST | MEM_TYPE_DDR | MEM_PAGE_HUGE;
        if (socType_ == AscendSocType::ASCEND_950) {
            allocFlag = MEM_HOST | MEM_TYPE_DDR | MEM_PAGE_NORMAL;
        }
        void *halAllocPtr = nullptr;
        int ret = DlHalApi::HalMemAlloc(&halAllocPtr, size, allocFlag);
        if (ret == 0 && halAllocPtr != nullptr) {
            allocMethod = MemAllocMethod::HAL_MEM_ALLOC;
            return halAllocPtr;
        }
    }

    static const bool enable4KPage = MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_ENABLE_4K_PAGE, 0u) != 0u;
    if (enable4KPage) {
        mapped = mmap(sliceAddr, size, prot, mmapFlags, mmapFd, mmapOffset);
        if (mapped == sliceAddr) {
            allocMethod = MemAllocMethod::MMAP;
            return mapped;
        }
    }
    return MAP_FAILED;
}

Result HybmConnBasedSegment::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (role_ == HostRole::PROVIDER) {
        return BM_OK;
    }
    if (role_ == HostRole::PROXY) {
        if (LocalProviderMapped() && std::find(ranks.begin(), ranks.end(), localProviderRank_) != ranks.end()) {
            const auto ret = TeardownLocalProviderShm();
            if (ret != BM_OK) {
                return ret;
            }
        }
        for (const auto rank : ranks) {
            auto it = importedProviders_.find(rank);
            if (it != importedProviders_.end()) {
                HybmVaManager::GetInstance().RemoveOneVaInfo(it->second.gva);
                mappedGvaMem_.erase(it->second.gva);
                importedProviders_.erase(it);
            }
        }
        return BM_OK;
    }

    for (auto &rank : ranks) {
        if (rank >= options_.rankCnt) {
            BM_LOG_ERROR("input rank is invalid! rank:" << rank << " rankSize:" << options_.rankCnt);
            return BM_INVALID_PARAM;
        }
    }
    for (const auto rank : ranks) {
        uint64_t gvaLocal = reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * rank;
        auto it = mappedGvaMem_.lower_bound(gvaLocal);
        auto st = it;
        while (it != mappedGvaMem_.end() && (*it) < gvaLocal + options_.maxSize) {
            HybmVaManager::GetInstance().RemoveOneVaInfo(*it);
            ++it;
        }
        if (st != it) {
            mappedGvaMem_.erase(st, it);
        }
    }
    imports_.erase(std::remove_if(imports_.begin(), imports_.end(),
                                  [&ranks](const HostExportInfo &info) {
                                      return std::find(ranks.begin(), ranks.end(), info.rankId) != ranks.end();
                                  }),
                   imports_.end());
    return BM_OK;
}

Result HybmConnBasedSegment::RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    auto ret = RegisterMemCommon(addr, size, slice);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, ret);
    slices_.emplace(slice->index_, slice);
    return BM_OK;
}

Result HybmConnBasedSegment::ReleaseSliceMemory(const MemSlicePtr &slice) noexcept
{
    if (slice == nullptr) {
        BM_LOG_ERROR("input slice is nullptr");
        return BM_INVALID_PARAM;
    }
    auto pos = slices_.find(slice->index_);
    if (pos == slices_.end()) {
        BM_LOG_ERROR("input slice(idx:" << slice->index_ << ") not exist.");
        return BM_INVALID_PARAM;
    }
    if (pos->second.slice != slice) {
        BM_LOG_ERROR("input slice(magic:" << std::hex << slice->magic_ << ") not match.");
        return BM_INVALID_PARAM;
    }
    HybmVaManager::GetInstance().RemoveOneVaInfo(slice->vAddress_, HVM_HVA);
    exportMap_.erase(slice->index_);
    slices_.erase(pos);

#if defined(ASCEND_NPU)
    uint32_t needUnregister = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK | HYBM_DOP_TYPE_MTE;
    const bool needsDeviceUnregister =
        role_ == HostRole::SELF || (slice->gva_ == 0 && slice->memType_ == HYBM_MEM_TYPE_HOST);
    if (needsDeviceUnregister && slice->memType_ == HYBM_MEM_TYPE_HOST && slice->size_ > 0 &&
        (options_.dataOpType & needUnregister) != 0) {
        auto unregRet =
            DlHalApi::HalHostUnregisterEx(reinterpret_cast<void *>(slice->vAddress_), logicDeviceId_, HOST_MEM_MAP_DEV);
        if (unregRet != 0) {
            BM_LOG_WARN("HalHostUnregisterEx not successful, idx:" << slice->index_ << " ret:" << unregRet
                                                                   << "; teardown continues");
        }
    }
#endif
    return BM_OK;
}

void HybmConnBasedSegment::FreeAllocatedMemory(void *ptr, uint64_t size, MemAllocMethod allocMethod) noexcept
{
    if (ptr == nullptr || ptr == MAP_FAILED) {
        return;
    }

    if (allocMethod == MemAllocMethod::HAL_MEM_ALLOC) {
        int ret = DlHalApi::HalMemFree(ptr);
        if (ret != 0) {
            BM_LOG_ERROR("Failed to free memory allocated by HalMemAlloc, ptr:" << ptr << " ret:" << ret);
        } else {
            BM_LOG_INFO("Successfully freed memory via HalMemFree, ptr:" << ptr << " size:" << size);
        }
    } else {
        if (munmap(ptr, size) != 0) {
            BM_LOG_ERROR("Failed to munmap memory, ptr:" << ptr << " size:" << size << " error:" << errno);
        } else {
            BM_LOG_INFO("Successfully freed memory via munmap, ptr:" << ptr << " size:" << size);
        }
    }
}

Result HybmConnBasedSegment::GetExportSliceSize(size_t &size) noexcept
{
    size = sizeof(HostExportInfo);
    return BM_OK;
}
