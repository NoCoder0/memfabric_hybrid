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
#ifndef MEM_FABRIC_HYBRID_HYBM_SHM_UTIL_H
#define MEM_FABRIC_HYBRID_HYBM_SHM_UTIL_H

namespace ock {
namespace mf {
// Check whether /dev/hugepages is mounted as hugetlbfs.
bool IsHugetlbfsMounted() noexcept;

// Check for free huge pages in /proc/meminfo.
bool HasAvailableHugePages() noexcept;

// Require both the hugetlbfs mount and free huge pages.
bool TryHugetlbfsAvailable() noexcept;
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_SHM_UTIL_H
