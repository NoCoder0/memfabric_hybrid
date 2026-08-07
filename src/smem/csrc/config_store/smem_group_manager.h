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

#ifndef SMEM_SMEM_GROUP_MANAGER_H
#define SMEM_SMEM_GROUP_MANAGER_H

#include "smem_group_manager_def.h"
#include "smem_ref.h"
#include "smem_group_manager_client.h"

namespace ock {

static constexpr uint32_t SMEM_GROUP_MAX = 1024U;

namespace smem {

class SmemGroupManager : virtual public SmReferable {
public:
    ~SmemGroupManager() noexcept override = default;

    SmemGroupManager(const SmemGroupManager &) = delete;
    SmemGroupManager &operator=(const SmemGroupManager &) = delete;

    virtual int Join(const RankFullInfo &info) noexcept = 0;
    virtual int Leave() noexcept = 0;
    virtual int ExtendMemory(const MultiBytes &additionalSlices) noexcept = 0;

    [[nodiscard]] uint32_t GetLocalRankId() const noexcept
    {
        return localRankId_;
    }

    void SetExecutor(SmemGroupManagerClientPtr executor) noexcept
    {
        executor_ = std::move(executor);
    }

protected:
    SmemGroupManager() noexcept = default;

    void SetLocalRankId(uint32_t rankId) noexcept
    {
        localRankId_ = rankId;
    }

    uint32_t localRankId_{0};
    RankFullInfo localRankInfo_;         //< Cached identity info supplied to Join()
    SmemGroupManagerClientPtr executor_; //< Client-side handler for server-driven state transitions
};

using SmemGroupManagerPtr = SmRef<SmemGroupManager>;

} // namespace smem
} // namespace ock

#endif // SMEM_SMEM_GROUP_MANAGER_H
