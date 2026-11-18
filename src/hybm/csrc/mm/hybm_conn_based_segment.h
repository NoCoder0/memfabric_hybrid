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
#ifndef MF_HYBRID_HYBM_CONN_BASED_SEGMENT_H
#define MF_HYBRID_HYBM_CONN_BASED_SEGMENT_H

#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include "hybm_mem_segment.h"
#include "hybm_mem_common.h"

namespace ock {
namespace mf {
struct HostExportInfo {
    uint64_t magic{DRAM_SLICE_EXPORT_INFO_MAGIC};
    uint64_t version{EXPORT_INFO_VERSION};
    uint64_t gva{0};
    uint32_t sliceIndex{0};
    uint32_t rankId{0};
    uint64_t size{0};
    MemPageTblType pageTblType{};
    MemSegType memSegType{};
    MemSegInfoExchangeType exchangeType{};
    uint16_t deviceId{0};
    // Physical device ID used for peer matching; falls back to devId when unavailable.
    // Provider pool exports use zero.
    uint16_t phyDevId{0};
    MachineId machineId{};
    char padding_[UNIFIED_EXCHANGE_SEG_INFO_SIZE - 72]{};
};

static_assert(sizeof(HostExportInfo) == UNIFIED_EXCHANGE_SEG_INFO_SIZE,
              "HostExportInfo must be UNIFIED_EXCHANGE_SEG_INFO_SIZE(192) bytes");

class HybmConnBasedSegment : public MemSegment {
public:
    explicit HybmConnBasedSegment(const MemSegmentOptions &options, int eid) : MemSegment{options, eid} {}
    ~HybmConnBasedSegment() override
    {
        FreeMemory();
    }

    Result ValidateOptions() noexcept override;
    Result ReserveMemorySpace(void **address) noexcept override;
    Result UnReserveMemorySpace() noexcept override;
    Result AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept override;
    Result Export(std::string &exInfo) noexcept override;
    Result Export(const MemSlicePtr &slice, std::string &exInfo) noexcept override;
    Result Import(const std::vector<std::string> &allExInfo, void *addresses[], uint32_t overrideRank,
                  std::vector<ImportEntryKind> &entryKinds) noexcept override;
    Result Mmap() noexcept override;
    Result Unmap() noexcept override;
    MemSlicePtr GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept override;
    bool MemoryInRange(const void *begin, uint64_t size) const noexcept override;
    bool IsLocalRange(const void *begin, uint64_t size) const noexcept override;
    Result RemoveImported(const std::vector<uint32_t> &ranks) noexcept override;
    Result RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept override;
    Result ReleaseSliceMemory(const MemSlicePtr &slice) noexcept override;
    Result GetExportSliceSize(size_t &size) noexcept override;
    hybm_mem_type GetMemoryType() const noexcept override
    {
        return HYBM_MEM_TYPE_HOST;
    }

private:
    void FreeMemory() noexcept;
    Result PrepareShareMemoryFd() const noexcept;
    Result MapSlice(void *&mapped, void *sliceAddr, uint64_t lvOffset, uint64_t size, uint64_t gva,
                    MemAllocMethod &allocMethod) noexcept;
    static void LvaShmReservePhysicalMemory(void *mappedAddress, uint64_t size) noexcept;
    void *AllocMemory(void *sliceAddr, uint64_t lvOffset, uint64_t size, MemAllocMethod &allocMethod);
    static void FreeAllocatedMemory(void *ptr, uint64_t size, MemAllocMethod allocMethod) noexcept;

    // Provider owns the shared pool; Proxy maps the local pool and records remote ranges.
    // This mode requires HOST_SHM combined with a device transport.
    std::string GetProviderShmFilePath(bool useHugetlbfs) const noexcept;
    Result MapProviderShm() noexcept;
    Result AcquireProviderShm(const std::string &shmPath) noexcept;
    Result OpenAndLockLocalShm(const std::string &shmPath, int &fd) noexcept;
    bool CleanupStaleLocalShm(const std::string &shmPath) noexcept;
    // Import opens this file before taking mutex_; mapping may take ownership of fd.
    // Close any descriptor that was not consumed by a mapping.
    struct ProviderShmFile {
        int fd{-1};
        uint64_t mapSize{0};
        dev_t dev{0};
        ino_t ino{0};
        ProviderShmFile() = default;
        ~ProviderShmFile();
        ProviderShmFile(const ProviderShmFile &) = delete;
        ProviderShmFile &operator=(const ProviderShmFile &) = delete;
    };
    Result PreOpenLocalProviderShm(const std::vector<HostExportInfo> &infos, ProviderShmFile &file) noexcept;
    Result ImportHostProvider(uint32_t index, const HostExportInfo &info, std::vector<ImportEntryKind> &entryKinds,
                              ProviderShmFile &file) noexcept;
    Result MapLocalProviderShmInternal(const HostExportInfo &info, ProviderShmFile &file) noexcept;
    Result VerifyMappedProviderShm(const HostExportInfo &info, ProviderShmFile &file) noexcept;
    Result OpenProviderShm(const std::string &shmPath, ProviderShmFile &file) noexcept;
    Result OpenProviderShmWithRetry(const std::string &shmPath, ProviderShmFile &file) noexcept;
    Result OpenLocalProviderShm(ProviderShmFile &file, uint32_t providerRank) noexcept;
    Result MapProviderShmAt(const std::string &shmPath, int fd, void *base, uint64_t mapSize) noexcept;
    Result UnmapLocalProviderRegionAsReserved() noexcept;
    void CloseImportedShmFd() noexcept;
    Result TeardownLocalProviderShm() noexcept;

    Result DeserializeHostInfos(const std::vector<std::string> &allExInfo, std::vector<HostExportInfo> &infos,
                                void *addresses[]) noexcept;

public:
    enum class AddressScope : uint32_t { LOCAL_HOST = 0, LOCAL_PROVIDER_SHM, REMOTE_PROVIDER, UNKNOWN };
    bool IsHostCombo() const noexcept
    {
        return clusterRole_ == HYBM_CLUSTER_ROLE_PROVIDER || clusterRole_ == HYBM_CLUSTER_ROLE_PROXY;
    }
    bool IsHostProvider() const noexcept
    {
        return clusterRole_ == HYBM_CLUSTER_ROLE_PROVIDER;
    }
    bool IsHostProxy() const noexcept
    {
        return clusterRole_ == HYBM_CLUSTER_ROLE_PROXY;
    }
    void *GetHostShmBase() const noexcept
    {
        return localShmBase_;
    }
    uint64_t GetHostShmSize() const noexcept
    {
        return localShmSize_;
    }
    uint32_t GetLocalProviderRank() const noexcept
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return localProviderRank_;
    }
    AddressScope GetAddressScope(uint64_t gva) const noexcept;
    std::unordered_map<uint32_t, HostExportInfo> GetImportedProviders() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return importedProviders_;
    }
    bool HasImportedProvider(uint32_t rankId) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return importedProviders_.find(rankId) != importedProviders_.end();
    }
    uint64_t GetProviderGvaBase(uint32_t providerRank) const noexcept
    {
        return reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * providerRank;
    }

private:
    // 仅 proxy 使用此判据：provider 建池同样写 localShmBase_，不带角色区分会让 Unmap() 误走 proxy
    // 语义的解除路径，把 provider 自己的池换成 PROT_NONE（或直接 munmap）。
    bool LocalProviderMapped() const noexcept
    {
        return clusterRole_ == HYBM_CLUSTER_ROLE_PROXY && localShmBase_ != nullptr;
    }

    mutable std::shared_mutex mutex_;
    uint64_t totalVirtualSize_{0UL};
    uint8_t *localVirtualBase_{nullptr};
    uint64_t allocatedSize_{0UL};
    std::map<uint32_t, MemSliceStatus> slices_;
    std::map<uint32_t, std::string> exportMap_;
    std::vector<HostExportInfo> imports_;
    std::set<uint64_t> mappedGvaMem_; // gva sets

    hybm_cluster_role_type clusterRole_{HYBM_CLUSTER_ROLE_DEFAULT};
    int localShmFd_{-1};
    uint8_t *localShmBase_{nullptr}; // 映射基址：provider 是自己的池，proxy 是映射进来的同机池
    uint64_t localShmSize_{0UL}; // 映射长度：实际 mmap 的长度（共享内存文件长度），与切片长度无关
    bool useHugetlbfs_{false};
    dev_t providerShmDev_{0}; // 已映射文件的设备号，用于检测 provider 重启（同名新文件 inode 不同）
    ino_t providerShmIno_{0}; // 已映射文件的 inode 号，用途同上
    // 窗口空洞告警标志：记录首个踩到空洞并告警的 rank，MAX 表示尚未告警，进程生命周期内只告警一次
    mutable std::atomic<uint32_t> windowHoleWarnedRank_{std::numeric_limits<uint32_t>::max()};
    uint32_t localProviderRank_{std::numeric_limits<uint32_t>::max()};
    std::unordered_map<uint32_t, HostExportInfo> importedProviders_;
    int importedShmFd_{-1};
};
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_CONN_BASED_SEGMENT_H
