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
#include <gtest/gtest.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "smem_common_includes.h"

#define private public
#include "smem_reconnect_manager.h"
#include "hybm_entity_default.h"
#undef private

using namespace ock::smem;

TEST(SmemReconnectManagerTest, ReconnectClearsHybmImportsOnceAndPreservesImportRetry)
{
    ock::mf::MemEntityDefault entity(3200);
    entity.initialized_ = true;
    entity.options_.rankId = 0U;
    entity.importedRanks_[0U] = {};
    int cleanups = 0;
    SmemReconnectManager manager([&] {
        ++cleanups;
        return entity.RemoveAllImported();
    });
    std::vector<RankFullInfo> peers(1);
    peers.front().rankId = 1U;
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers,
                                                  [&] {
                                                      entity.importedRanks_[1U] = {};
                                                      entity.importedRanks_[2U] = {};
                                                      entity.importedMemories_[1U] = {};
                                                      entity.importedMemories_[3U] = {};
                                                      return SM_OK;
                                                  }),
              SM_OK);
    EXPECT_EQ(cleanups, 0);

    manager.disconnectEpoch_.fetch_add(1UL);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers,
                                                  [&] {
                                                      EXPECT_EQ(entity.importedRanks_.size(), 1U);
                                                      EXPECT_EQ(entity.importedRanks_.count(0U), 1U);
                                                      EXPECT_TRUE(entity.importedMemories_.empty());
                                                      entity.importedRanks_[1U].serverId = 11U;
                                                      return SM_ERROR;
                                                  }),
              SM_ERROR);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers,
                                                  [&] {
                                                      EXPECT_EQ(entity.importedRanks_.at(1U).serverId, 11U);
                                                      entity.importedRanks_[1U].serverId = 22U;
                                                      return SM_OK;
                                                  }),
              SM_OK);
    peers.front().rankId = 2U;
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers,
                                                  [&] {
                                                      entity.importedRanks_[2U] = {};
                                                      return SM_OK;
                                                  }),
              SM_OK);
    EXPECT_EQ(cleanups, 1);
    EXPECT_EQ(entity.importedRanks_.at(1U).serverId, 22U);
    manager.disconnectEpoch_.fetch_add(1UL);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers, [] { return SM_OK; }), SM_OK);
    EXPECT_EQ(cleanups, 2);
    EXPECT_EQ(entity.importedRanks_.size(), 1U);
    EXPECT_EQ(entity.importedRanks_.count(0U), 1U);
}

TEST(SmemReconnectManagerTest, CleanupFailureContinuesWithoutRetryInSameEpoch)
{
    int cleanups = 0;
    SmemReconnectManager manager([&] {
        ++cleanups;
        return SM_ERROR;
    });
    std::vector<RankFullInfo> peers(1);
    manager.disconnectEpoch_.fetch_add(1UL);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers, [] { return SM_OK; }), SM_OK);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers, [] { return SM_OK; }), SM_OK);
    EXPECT_EQ(cleanups, 1);
    manager.disconnectEpoch_.fetch_add(1UL);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers, [] { return SM_OK; }), SM_OK);
    EXPECT_EQ(cleanups, 2);
}

TEST(SmemReconnectManagerTest, DisconnectDuringCleanupOrImportDefersNextEpoch)
{
    int cleanups = 0;
    int imports = 0;
    SmemReconnectManager *current = nullptr;
    SmemReconnectManager manager([&] {
        if (++cleanups == 1) {
            current->disconnectEpoch_.fetch_add(1UL);
        }
        return SM_OK;
    });
    current = &manager;
    std::vector<RankFullInfo> peers(1);
    auto import = [&] {
        ++imports;
        return SM_OK;
    };
    manager.disconnectEpoch_.fetch_add(1UL);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers, import), SM_NOT_CONNECTED);
    EXPECT_EQ(imports, 0);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers, import), SM_OK);
    EXPECT_EQ(cleanups, 2);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers,
                                                  [&] {
                                                      manager.disconnectEpoch_.fetch_add(1UL);
                                                      return SM_OK;
                                                  }),
              SM_NOT_CONNECTED);
    EXPECT_EQ(manager.ImportAfterReconnectCleanup(peers, import), SM_OK);
    EXPECT_EQ(cleanups, 3);
    EXPECT_EQ(imports, 2);
}
