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
#include "hybm_shm_util.h"

#include <sys/stat.h>
#include <sys/vfs.h>

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <sstream>

#include "hybm_functions.h"
#include "hybm_logger.h"

namespace ock {
namespace mf {
namespace {
constexpr const char *HUGETLBFS_DIR = "/dev/hugepages";
constexpr const char *SHM_FALLBACK_DIR = "/dev/shm";
constexpr unsigned long HUGETLBFS_MAGIC = 0x958458f6UL;
} // namespace

bool IsHugetlbfsMounted() noexcept
{
    struct statfs fsInfo {};
    if (statfs(HUGETLBFS_DIR, &fsInfo) != 0) {
        BM_LOG_WARN("Failed to access hugepage dir " << HUGETLBFS_DIR << " error:" << errno << ", "
                                                     << SafeStrError(errno) << ", will fallback to "
                                                     << SHM_FALLBACK_DIR);
        return false;
    }
    if (static_cast<unsigned long>(fsInfo.f_type) != HUGETLBFS_MAGIC) {
        BM_LOG_WARN("Hugepage dir " << HUGETLBFS_DIR << " is not hugetlbfs, fs type:" << std::hex
                                    << static_cast<unsigned long>(fsInfo.f_type) << ", will fallback to "
                                    << SHM_FALLBACK_DIR);
        return false;
    }
    return true;
}

bool HasAvailableHugePages() noexcept
{
    std::ifstream memInfoFile("/proc/meminfo");
    if (!memInfoFile.is_open()) {
        BM_LOG_WARN("Failed to open /proc/meminfo, will fallback to " << SHM_FALLBACK_DIR);
        return false;
    }

    std::string line;
    while (std::getline(memInfoFile, line)) {
        std::istringstream lineStream(line);
        std::string key;
        uint64_t value = 0;

        if (!(lineStream >> key >> value)) {
            continue;
        }

        if (key != "HugePages_Free:") {
            continue;
        }

        if (value == 0) {
            BM_LOG_WARN("HugePages_Free is 0, will fallback to " << SHM_FALLBACK_DIR);
            return false;
        }
        return true;
    }

    BM_LOG_WARN("Failed to parse HugePages_Free from /proc/meminfo, will fallback to " << SHM_FALLBACK_DIR);
    return false;
}

bool TryHugetlbfsAvailable() noexcept
{
    return IsHugetlbfsMounted() && HasAvailableHugePages();
}

} // namespace mf
} // namespace ock
