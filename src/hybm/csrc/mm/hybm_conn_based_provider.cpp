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

#include <fcntl.h>
#include <linux/mempolicy.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "dl_hal_api.h"
#include "hybm_def.h"
#include "hybm_ex_info_transfer.h"
#include "hybm_gva.h"
#include "hybm_logger.h"
#include "hybm_numa_util.h"
#include "hybm_shm_util.h"
#include "hybm_va_manager.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "mf_num_util.h"
#include "mf_scope_guard.h"

using namespace ock::mf;

namespace {
constexpr const char *HOST_PROVIDER_HUGEPAGE_DIR = "/dev/hugepages";
constexpr const char *HOST_PROVIDER_FALLBACK_DIR = "/dev/shm";
constexpr const char *HOST_PROVIDER_SHM_NAME = "/memfabric_hybrid_provider";

Result ReleaseProviderMapping(void *base, uint64_t mapLen, bool enable56BitsGva) noexcept
{
    if (!enable56BitsGva) {
        void *reserved = mmap(base, mapLen, PROT_NONE, MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE, -1, 0);
        if (reserved == MAP_FAILED || reserved != base) {
            BM_LOG_ERROR("Failed to restore local provider region as reserved, addr:" << base << " size:" << mapLen
                                                                                      << " error:" << errno);
            return BM_ERROR;
        }
    } else {
        if (munmap(base, mapLen) != 0) {
            BM_LOG_WARN("Failed to munmap local provider region, addr:" << base << " size:" << mapLen
                                                                        << " error:" << errno);
        }
    }
    return BM_OK;
}
} // namespace

std::string HybmConnBasedSegment::GetProviderShmFilePath(bool useHugetlbfs) const noexcept
{
    const char *baseDir = useHugetlbfs ? HOST_PROVIDER_HUGEPAGE_DIR : HOST_PROVIDER_FALLBACK_DIR;
    return std::string(baseDir) + HOST_PROVIDER_SHM_NAME;
}

Result HybmConnBasedSegment::OpenProviderShm(const std::string &shmPath, ProviderShmFile &file) noexcept
{
    const int fd = open(shmPath.c_str(), O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        // ENOENT/ENXIO means the provider has not created its shm file yet, which is retryable.
        if (errno == ENOENT || errno == ENXIO) {
            BM_LOG_WARN("provider shm " << shmPath << " not created yet, error:" << errno << " "
                                        << SafeStrError(errno));
            return BM_NOT_INITIALIZED;
        }
        BM_LOG_ERROR("Failed to open provider shm " << shmPath << " error:" << errno << " " << SafeStrError(errno));
        return BM_ERROR;
    }
    struct stat fileStat {};
    if (fstat(fd, &fileStat) != 0) {
        BM_LOG_ERROR("Failed to stat provider shm " << shmPath << " error:" << errno);
        close(fd);
        return BM_ERROR;
    }
    if (fileStat.st_size <= 0) {
        // The provider created the file but has not truncated it to its final size yet, which is retryable.
        BM_LOG_WARN("provider shm " << shmPath << " is empty, not ready yet.");
        close(fd);
        return BM_NOT_INITIALIZED;
    }
    file.fd = fd;
    file.mapSize = std::min(static_cast<uint64_t>(fileStat.st_size), options_.maxSize);
    file.dev = fileStat.st_dev;
    file.ino = fileStat.st_ino;
    return BM_OK;
}

Result HybmConnBasedSegment::OpenProviderShmWithRetry(const std::string &shmPath, ProviderShmFile &file) noexcept
{
    constexpr uint32_t maxRetries = 50;
    constexpr uint32_t retryIntervalSec = 2;
    for (uint32_t attempt = 0; attempt < maxRetries; attempt++) {
        auto ret = OpenProviderShm(shmPath, file);
        if (ret == BM_OK) {
            if (attempt > 0) {
                BM_LOG_INFO("provider shm " << shmPath << " ready after " << (attempt + 1) << " attempt(s).");
            }
            return BM_OK;
        }
        if (ret != BM_NOT_INITIALIZED) {
            // Non-transient failure (e.g. permission denied): do not retry.
            BM_LOG_ERROR("Failed to open provider shm " << shmPath << ", ret:" << ret);
            return ret;
        }
        if (attempt + 1 == maxRetries) {
            break;
        }
        BM_LOG_WARN("provider shm " << shmPath << " not ready, attempt " << (attempt + 1) << "/" << maxRetries
                                    << ", wait " << retryIntervalSec << "s");
        (void)sleep(retryIntervalSec);
    }
    BM_LOG_ERROR("provider shm " << shmPath << " still not ready after " << maxRetries << " attempts.");
    return BM_ERROR;
}

// 池文件位置由 provider 创建时决定（CreateLocalProviderShm 按当时的可用大页选择目录），
// 不能按本进程此刻的大页余量重新选择：provider 把大页用光后，后启动的 proxy 会据此选 /dev/shm，
// 然后等一个永远不存在的文件直到超时。这里两个候选目录都试，以实际打开成功者为准并回填
// useHugetlbfs_，供后续身份校验复用同一路径。
Result HybmConnBasedSegment::OpenLocalProviderShm(ProviderShmFile &file, uint32_t providerRank) noexcept
{
    constexpr uint32_t maxRetries = 50;
    constexpr uint32_t retryIntervalSec = 2;
    std::array<std::string, 2> candidates = {GetProviderShmFilePath(true), GetProviderShmFilePath(false)};
    if (!TryHugetlbfsAvailable()) {
        std::swap(candidates[0], candidates[1]);
    }
    for (uint32_t attempt = 0; attempt < maxRetries; attempt++) {
        for (const auto &shmPath : candidates) {
            auto ret = OpenProviderShm(shmPath, file);
            if (ret == BM_OK) {
                useHugetlbfs_ = (shmPath == GetProviderShmFilePath(true));
                if (attempt > 0) {
                    BM_LOG_INFO("provider shm " << shmPath << " ready after " << (attempt + 1) << " attempt(s).");
                }
                return BM_OK;
            }
            if (ret != BM_NOT_INITIALIZED) {
                BM_LOG_ERROR("Failed to open provider shm " << shmPath << ", providerRank:" << providerRank
                                                            << " ret:" << ret);
                return ret;
            }
        }
        if (attempt + 1 == maxRetries) {
            break;
        }
        BM_LOG_WARN("provider shm not ready, providerRank:" << providerRank << ", attempt " << (attempt + 1) << "/"
                                                            << maxRetries << ", wait " << retryIntervalSec << "s");
        (void)sleep(retryIntervalSec);
    }
    BM_LOG_ERROR("provider shm still not ready after " << maxRetries << " attempts, providerRank:" << providerRank
                                                       << ", candidates:" << candidates[0] << " " << candidates[1]);
    return BM_ERROR;
}

Result HybmConnBasedSegment::MapProviderShmAt(const std::string &shmPath, int fd, void *base, uint64_t mapSize) noexcept
{
    void *mapped = mmap(base, mapSize, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED || mapped != base) {
        BM_LOG_ERROR("Failed to mmap provider shm " << shmPath << " addr:" << base << " size:" << mapSize << " ret:"
                                                    << mapped << " error:" << errno << " " << SafeStrError(errno));
        return BM_ERROR;
    }
    return BM_OK;
}

// Restore this rank at localVirtualBase_, rather than the global rank-zero base.
// Reserve only the formerly mapped range; leave the rest of the rank window intact.
Result HybmConnBasedSegment::UnmapLocalProviderRegionAsReserved() noexcept
{
    if (localShmBase_ == nullptr) {
        return BM_OK;
    }
    void *base = localShmBase_;
    const uint64_t mapLen = localShmSize_;
    auto ret = ReleaseProviderMapping(base, mapLen, options_.enable56BitsGva);
    if (ret != BM_OK) {
        return ret;
    }
    localShmBase_ = nullptr;
    localShmSize_ = 0UL;
    return BM_OK;
}

void HybmConnBasedSegment::CloseImportedShmFd() noexcept
{
    if (importedShmFd_ >= 0) {
        close(importedShmFd_);
        importedShmFd_ = -1;
    }
}

// Release the local mapping and descriptors; the caller removes address records.
Result HybmConnBasedSegment::TeardownLocalProviderShm() noexcept
{
    const auto ret = UnmapLocalProviderRegionAsReserved();
    if (ret != BM_OK) {
        return ret;
    }
    CloseImportedShmFd();
    localProviderRank_ = std::numeric_limits<uint32_t>::max();
    return BM_OK;
}

// 锁外预打开本批描述中同机 provider 的共享内存文件：文件就绪等待最长 100s，持锁进行会拖住数据面的
// GetAddressScope。本批没有同机描述时不打开任何文件。
Result HybmConnBasedSegment::PreOpenLocalProviderShm(const std::vector<HostExportInfo> &infos,
                                                     ProviderShmFile &file) noexcept
{
    for (const auto &info : infos) {
        if (info.memSegType != HYBM_DRAM_PROVIDER || info.machineId != machineId_) {
            continue;
        }
        auto ret = OpenLocalProviderShm(file, info.rankId);
        if (ret != BM_OK) {
            BM_LOG_ERROR("proxy open local provider shm failed, providerRank:" << info.rankId << " ret:" << ret);
        }
        return ret;
    }
    return BM_OK;
}

// 未被消费的预打开句柄在此统一关闭：Import 的任何提前返回都不再依赖调用点手动 close。
HybmConnBasedSegment::ProviderShmFile::~ProviderShmFile()
{
    if (fd >= 0) {
        (void)close(fd);
    }
}

// Identical descriptions are idempotent; conflicting descriptions fail.
// Verify local file identity before reuse and remap a replaced provider pool.
// Remove the imported entry if either the first mapping or remapping fails.
Result HybmConnBasedSegment::ImportHostProvider(uint32_t index, const HostExportInfo &info,
                                                std::vector<ImportEntryKind> &entryKinds,
                                                ProviderShmFile &file) noexcept
{
    auto it = importedProviders_.find(info.rankId);
    if (it != importedProviders_.end()) {
        const auto &first = it->second;
        const bool sameDesc = first.gva == info.gva && first.size == info.size && first.sliceIndex == info.sliceIndex &&
                              first.machineId == info.machineId;
        if (!sameDesc) {
            BM_LOG_ERROR("provider description conflict, providerRank:"
                         << info.rankId << ", keep first gva:" << reinterpret_cast<void *>(first.gva)
                         << " size:" << first.size << ", reject gva:" << reinterpret_cast<void *>(info.gva)
                         << " size:" << info.size);
            return BM_INVALID_PARAM;
        }
        if (info.machineId == machineId_) {
            bool needRemap = !LocalProviderMapped();
            if (!needRemap) {
                auto ret = VerifyMappedProviderShm(info, file);
                if (ret != BM_OK) {
                    BM_LOG_WARN("provider shm identity changed or file gone, teardown and remap, providerRank:"
                                << info.rankId << " ret:" << ret);
                    needRemap = true;
                }
            }
            if (needRemap) {
                auto ret = TeardownLocalProviderShm();
                if (ret != BM_OK) {
                    return ret;
                }
                HybmVaManager::GetInstance().RemoveOneVaInfo(info.gva);
                mappedGvaMem_.erase(info.gva);
                ret = MapLocalProviderShmInternal(info, file);
                if (ret != BM_OK) {
                    importedProviders_.erase(it);
                    BM_LOG_ERROR("provider remap failed after restart, providerRank:" << info.rankId << " ret:" << ret
                                                                                      << ", entry removed");
                    return ret;
                }
            }
        }
    } else {
        importedProviders_.emplace(info.rankId, info);
        if (info.machineId == machineId_) {
            auto ret = MapLocalProviderShmInternal(info, file);
            if (ret != BM_OK) {
                // 典型场景：proxy 早于 provider 启动，等待文件超时。摘掉登记后重试可重新走映射。
                importedProviders_.erase(info.rankId);
                BM_LOG_ERROR("proxy map local provider shm failed, providerRank=" << info.rankId << " ret:" << ret);
                return ret;
            }
        }
    }
    entryKinds[index] = ImportEntryKind::ProviderPool;
    return BM_OK;
}

// Recreating the same path leaves existing mappings attached to the old inode.
// Compare device and inode before reusing a local mapping.
Result HybmConnBasedSegment::VerifyMappedProviderShm(const HostExportInfo &info, ProviderShmFile &file) noexcept
{
    const std::string shmPath = GetProviderShmFilePath(useHugetlbfs_);
    // Keep the fd used for the identity check so a remap uses that same inode.
    if (file.fd >= 0) {
        (void)close(std::exchange(file.fd, -1));
    }
    const auto ret = OpenProviderShm(shmPath, file);
    if (ret != BM_OK) {
        return ret;
    }
    if (file.dev != providerShmDev_ || file.ino != providerShmIno_) {
        BM_LOG_ERROR("provider shm identity changed, mapped dev:"
                     << providerShmDev_ << " ino:" << providerShmIno_ << ", current dev:" << file.dev
                     << " ino:" << file.ino << ", path:" << shmPath << ", providerRank:" << info.rankId
                     << ", refuse to keep reading the stale mapping.");
        return BM_ERROR;
    }
    return BM_OK;
}

Result HybmConnBasedSegment::MapLocalProviderShmInternal(const HostExportInfo &info, ProviderShmFile &file) noexcept
{
    void *base = localVirtualBase_;
    if (base == nullptr) {
        BM_LOG_ERROR("Invalid local provider map base, providerRank:" << info.rankId);
        return BM_ERROR;
    }

    if (file.fd < 0) {
        // Import holds mutex_ here. Only the pre-open path waits for the file to become ready.
        auto ret = OpenLocalProviderShm(file, info.rankId);
        if (ret != BM_OK) {
            BM_LOG_ERROR("proxy open provider shm failed, providerRank:"
                         << info.rankId << " gva:" << reinterpret_cast<void *>(info.gva) << " size:" << info.size
                         << " ret:" << ret);
            return ret;
        }
    }
    const std::string shmPath = GetProviderShmFilePath(useHugetlbfs_);
    const auto mapSize = file.mapSize;
    const uint64_t globalBase = reinterpret_cast<uint64_t>(globalVirtualAddress_);
    const uint64_t providerGvaBase = globalBase + options_.maxSize * static_cast<uint64_t>(info.rankId);
    if (info.gva < providerGvaBase) {
        BM_LOG_ERROR("provider gva(" << reinterpret_cast<void *>(info.gva) << ") is before provider window("
                                     << reinterpret_cast<void *>(providerGvaBase) << ")");
        return BM_ERROR;
    }
    const uint64_t offset = info.gva - providerGvaBase;
    if (offset > mapSize || info.size > mapSize - offset) {
        BM_LOG_ERROR("provider slice out of shm range, offset=" << offset << " size=" << info.size
                                                                << " mapSize=" << mapSize);
        return BM_ERROR;
    }

    auto ret = MapProviderShmAt(shmPath, file.fd, base, mapSize);
    if (ret != BM_OK) {
        return ret;
    }
    // 只登记地址转换关系，设备地址（dva）填 0 由传输层维护；不做整块池注册。
    const uint64_t hostVa = reinterpret_cast<uint64_t>(base) + offset;
    ret = HybmVaManager::GetInstance().AddVaInfo({info.gva, 0, hostVa, info.size, HYBM_MEM_TYPE_HOST}, false,
                                                 AddressOwnership::Imported);
    if (ret != BM_OK) {
        BM_LOG_ERROR("register local provider va failed, ret=" << ret << " gva:" << reinterpret_cast<void *>(info.gva)
                                                               << " size:" << info.size);
        (void)ReleaseProviderMapping(base, mapSize, options_.enable56BitsGva);
        return ret;
    }

    localShmBase_ = reinterpret_cast<uint8_t *>(base);
    localShmSize_ = mapSize;
    providerShmDev_ = file.dev;
    providerShmIno_ = file.ino;
    localProviderRank_ = info.rankId;
    mappedGvaMem_.insert(info.gva);
    importedShmFd_ = std::exchange(file.fd, -1);
    BM_LOG_INFO("proxy mapped local provider shm, providerRank:"
                << info.rankId << " gva:" << reinterpret_cast<void *>(info.gva)
                << " hva:" << reinterpret_cast<void *>(hostVa) << " mapSize:" << mapSize << " sliceSize:" << info.size
                << " path:" << shmPath << " dev:" << file.dev << " ino:" << file.ino
                << "; buffers in this pool used for cross-node "
                   "access must be registered via RegisterMemory.");
    return BM_OK;
}

// Local pools use mapped size; remote providers cover their full rank window.
// Reserved window holes are UNKNOWN; addresses outside all pools remain LOCAL_HOST.
// Slice length does not define either range.
HybmConnBasedSegment::AddressScope HybmConnBasedSegment::GetAddressScope(uint64_t gva) const noexcept
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (role_ == HostRole::SELF) {
        return AddressScope::LOCAL_HOST;
    }
    const uint64_t globalBase = reinterpret_cast<uint64_t>(globalVirtualAddress_);
    if (globalBase == 0U || gva < globalBase || gva >= globalBase + totalVirtualSize_) {
        return AddressScope::LOCAL_HOST;
    }
    const uint32_t rank = static_cast<uint32_t>((gva - globalBase) / options_.maxSize);
    const uint64_t windowBase = globalBase + options_.maxSize * rank;
    if (rank == localProviderRank_ && LocalProviderMapped()) {
        if (gva < windowBase + localShmSize_) {
            return AddressScope::LOCAL_PROVIDER_SHM;
        }
    }
    if (role_ == HostRole::PROVIDER && rank == options_.rankId) {
        // provider 自己的窗口：池映射在窗口起点，按映射长度判本机。
        if (gva < windowBase + localShmSize_) {
            return AddressScope::LOCAL_PROVIDER_SHM;
        }
    }
    if (importedProviders_.find(rank) != importedProviders_.end() && rank != localProviderRank_) {
        // 远端区间按窗口大小判：窗口内切片外的部分也属于远端 provider，绝不能当本地内存。
        return AddressScope::REMOTE_PROVIDER;
    }
    // 落在某个 rank 窗口内、但不属于任何已知 provider 区间：本 rank 窗口内池外的合法空洞
    // （未映射区 / PROT_NONE）。CAS 返回值判定：只有首个把 MAX 换成自己 rank 的调用打印一次。
    uint32_t expected = std::numeric_limits<uint32_t>::max();
    if (windowHoleWarnedRank_.compare_exchange_strong(expected, rank, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
        BM_LOG_WARN("address falls in rank window but outside any known provider region, gva:"
                    << reinterpret_cast<const void *>(gva) << " rank:" << rank
                    << " windowBase:" << reinterpret_cast<void *>(windowBase) << " maxSize:" << options_.maxSize);
    }
    return AddressScope::UNKNOWN;
}

Result HybmConnBasedSegment::OpenAndLockLocalShm(const std::string &shmPath, int &fd) noexcept
{
    fd = open(shmPath.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        if (errno == EEXIST) {
            return BM_NOT_INITIALIZED;
        }
        BM_LOG_ERROR("Failed to create provider shm " << shmPath << " error:" << errno << " " << SafeStrError(errno));
        return BM_ERROR;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        BM_LOG_ERROR("Failed to lock provider shm " << shmPath << " error:" << errno << " " << SafeStrError(errno));
        close(fd);
        fd = -1;
        return BM_ERROR;
    }
    return BM_OK;
}

bool HybmConnBasedSegment::CleanupStaleLocalShm(const std::string &shmPath) noexcept
{
    int staleFd = open(shmPath.c_str(), O_RDWR);
    if (staleFd < 0) {
        if (errno == ENOENT) {
            return true;
        }
        BM_LOG_ERROR("Failed to open existing provider shm " << shmPath << " error:" << errno);
        return false;
    }
    if (flock(staleFd, LOCK_EX | LOCK_NB) != 0) {
        close(staleFd);
        BM_LOG_ERROR("Provider shm " << shmPath << " is in use, error:" << errno);
        return false;
    }
    if (unlink(shmPath.c_str()) != 0 && errno != ENOENT) {
        close(staleFd);
        BM_LOG_ERROR("Failed to cleanup stale provider shm " << shmPath << " error:" << errno);
        return false;
    }
    close(staleFd);
    return true;
}

namespace {
void *MapInterleavedProviderShm(void *base, uint64_t size, int fd) noexcept
{
    const auto mapMemory = [base, size, fd]() {
        return mmap(base, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED | MAP_POPULATE, fd, 0);
    };
    int policy = MPOL_DEFAULT;
    if (syscall(SYS_get_mempolicy, &policy, nullptr, 0, nullptr, 0) != 0) {
        BM_LOG_WARN("Provider NUMA policy query failed, errno:" << errno << ", size:" << size);
        return mapMemory();
    }
    if (policy != MPOL_DEFAULT) {
        return mapMemory();
    }
    constexpr size_t maxNodes = 1024;
    std::array<unsigned long, maxNodes / (8 * sizeof(unsigned long))> nodes{};
    if (syscall(SYS_get_mempolicy, nullptr, nodes.data(), maxNodes, nullptr, MPOL_F_MEMS_ALLOWED) != 0) {
        BM_LOG_WARN("Provider allowed NUMA nodes query failed, errno:" << errno << ", size:" << size);
        return mapMemory();
    }
    if (syscall(SYS_set_mempolicy, MPOL_INTERLEAVE, nodes.data(), maxNodes) != 0) {
        BM_LOG_WARN("Provider NUMA interleave request failed, errno:" << errno << ", size:" << size);
        return mapMemory();
    }
    void *mapped = mapMemory();
    const int mapError = errno;
    if (syscall(SYS_set_mempolicy, MPOL_DEFAULT, nullptr, 0) != 0) {
        const int restoreError = errno;
        BM_LOG_ERROR("Restore provider thread NUMA policy failed, addr:" << mapped << ", size:" << size
                                                                         << ", errno:" << restoreError);
        if (mapped != MAP_FAILED && munmap(mapped, size) != 0) {
            BM_LOG_ERROR("Unmap provider memory after NUMA policy failure failed, addr:" << mapped << ", size:" << size
                                                                                         << ", errno:" << errno);
        }
        errno = restoreError;
        return MAP_FAILED;
    }
    BM_LOG_INFO("Requested provider pool NUMA interleave, size:" << size << ", restored thread policy to default");
    errno = mapError;
    return mapped;
}
} // namespace

Result HybmConnBasedSegment::AcquireProviderShm(const std::string &shmPath) noexcept
{
    // Serialize creation and stale-file takeover before the new inode has its own lock.
    const char *dir = useHugetlbfs_ ? HOST_PROVIDER_HUGEPAGE_DIR : HOST_PROVIDER_FALLBACK_DIR;
    const int dirFd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0) {
        BM_LOG_ERROR("Failed to open provider shm directory " << dir << ", errno:" << errno);
        return BM_ERROR;
    }
    auto guard = make_scope_guard(dirFd, [](int fd) { close(fd); });
    if (flock(dirFd, LOCK_EX) != 0) {
        BM_LOG_ERROR("Failed to lock provider shm directory " << dir << ", errno:" << errno);
        return BM_ERROR;
    }
    auto ret = OpenAndLockLocalShm(shmPath, localShmFd_);
    if (ret != BM_NOT_INITIALIZED) {
        return ret;
    }
    if (!CleanupStaleLocalShm(shmPath)) {
        return BM_ERROR;
    }
    return OpenAndLockLocalShm(shmPath, localShmFd_);
}

Result HybmConnBasedSegment::MapProviderShm() noexcept
{
    useHugetlbfs_ = TryHugetlbfsAvailable();
    auto shmPath = GetProviderShmFilePath(useHugetlbfs_);
    auto ret = AcquireProviderShm(shmPath);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to open provider shm " << shmPath << " error:" << errno);
        return ret;
    }
    if (ftruncate(localShmFd_, static_cast<off_t>(options_.size)) != 0) {
        BM_LOG_ERROR("Failed to truncate provider shm " << shmPath << " size:" << options_.size << " error:" << errno);
        (void)unlink(shmPath.c_str());
        close(localShmFd_);
        localShmFd_ = -1;
        return BM_ERROR;
    }
    const bool interleave = (options_.dataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) != 0U &&
                            (options_.flags & (1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX)) == 0U;
    void *mapped = interleave ? MapInterleavedProviderShm(localVirtualBase_, options_.size, localShmFd_)
                              : mmap(localVirtualBase_, options_.size, PROT_READ | PROT_WRITE,
                                     MAP_FIXED | MAP_SHARED | MAP_POPULATE, localShmFd_, 0);
    if (mapped == MAP_FAILED || mapped != localVirtualBase_) {
        BM_LOG_ERROR("Failed to mmap provider shm "
                     << shmPath << " addr:" << reinterpret_cast<void *>(localVirtualBase_) << " size:" << options_.size
                     << " error:" << errno);
        (void)unlink(shmPath.c_str());
        close(localShmFd_);
        localShmFd_ = -1;
        return BM_ERROR;
    }
    localShmBase_ = reinterpret_cast<uint8_t *>(mapped);
    localShmSize_ = options_.size;
    BM_LOG_INFO("provider shm mapped: path=" << shmPath << " va=" << mapped << " size=" << options_.size
                                             << " useHugetlbfs=" << useHugetlbfs_);
    return BM_OK;
}
