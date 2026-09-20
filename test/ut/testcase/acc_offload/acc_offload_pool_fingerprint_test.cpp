/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the Mulan PSL v2 for
 * more details.
 */

#include <gtest/gtest.h>

#include <vector>

#include "acc_offload_pool_fingerprint.h"

namespace ock {
namespace offload {
namespace {

/* hybm's SoC enumerators live in the sibling ock::mf namespace; hoist them so
 * the test bodies can use them unqualified. */
using ock::mf::ASCEND_910B;
using ock::mf::ASCEND_910C;
using ock::mf::ASCEND_950;
using ock::mf::ASCEND_UNKNOWN;

std::vector<PoolFingerprint> MakeUniformGroup(uint32_t rankCount, uint32_t localWorldSize, uint32_t socType)
{
    std::vector<PoolFingerprint> fps(rankCount);
    for (uint32_t r = 0; r < rankCount; r++) {
        fps[r] = MakePoolFingerprint(0x100000000ULL, 32ULL << 30, 32ULL << 30, rankCount, localWorldSize, r, socType);
    }
    return fps;
}

TEST(PoolFingerprintTest, UniformGroupPasses)
{
    // single machine: any SoC passes
    auto a5Single = MakeUniformGroup(8, 8, ASCEND_950);
    ASSERT_EQ(ValidatePoolFingerprints(a5Single.data(), a5Single.size()), OFFLOAD_OK);
    auto single = MakeUniformGroup(1, 1, ASCEND_UNKNOWN);
    ASSERT_EQ(ValidatePoolFingerprints(single.data(), single.size()), OFFLOAD_OK);
}

TEST(PoolFingerprintTest, MultiNodeA3PoolPasses)
{
    // 2 machines x 2 ranks on A3 nodes
    auto fps = MakeUniformGroup(4, 2, ASCEND_910C);
    ASSERT_EQ(ValidatePoolFingerprints(fps.data(), fps.size()), OFFLOAD_OK);
}

TEST(PoolFingerprintTest, MultiNodeNonA3PoolFails)
{
    struct SocCase {
        uint32_t socType;
        const char *name;
    };
    std::vector<SocCase> cases = {
        {ASCEND_950, "A5"},
        {ASCEND_910B, "A2"},
        {ASCEND_UNKNOWN, "unknown"},
    };
    for (const auto &tc : cases) {
        auto fps = MakeUniformGroup(4, 2, tc.socType);
        EXPECT_EQ(ValidatePoolFingerprints(fps.data(), fps.size()), OFFLOAD_ERROR) << "soc: " << tc.name;
    }
}

TEST(PoolFingerprintTest, InvalidInputFails)
{
    auto fps = MakeUniformGroup(4, 4, ASCEND_910C);
    ASSERT_EQ(ValidatePoolFingerprints(nullptr, fps.size()), OFFLOAD_ERROR);
    ASSERT_EQ(ValidatePoolFingerprints(fps.data(), 0), OFFLOAD_ERROR);
}

TEST(PoolFingerprintTest, WorldSizeMismatchFails)
{
    auto fps = MakeUniformGroup(4, 4, ASCEND_910C);
    fps[0].worldSize = 8; // disagrees with the gathered rank count
    for (uint32_t r = 1; r < fps.size(); r++) {
        fps[r].worldSize = 8;
    }
    ASSERT_EQ(ValidatePoolFingerprints(fps.data(), fps.size()), OFFLOAD_ERROR);
}

TEST(PoolFingerprintTest, FieldMismatchFails)
{
    struct FieldCase {
        const char *name;
        void (*mutate)(PoolFingerprint &fp);
    };
    std::vector<FieldCase> cases = {
        {"poolGva", [](PoolFingerprint &fp) { fp.poolGva += 0x1000; }},
        {"reserveSize", [](PoolFingerprint &fp) { fp.reserveSize += 1; }},
        {"worldSize", [](PoolFingerprint &fp) { fp.worldSize += 1; }},
        {"localWorldSize", [](PoolFingerprint &fp) { fp.localWorldSize += 1; }},
        {"socType", [](PoolFingerprint &fp) { fp.socType = ASCEND_950; }},
    };
    for (const auto &tc : cases) {
        auto fps = MakeUniformGroup(4, 2, ASCEND_910C);
        tc.mutate(fps[2]); // any non-zero rank disagrees with the rank0 reference
        EXPECT_EQ(ValidatePoolFingerprints(fps.data(), fps.size()), OFFLOAD_ERROR) << "field: " << tc.name;
    }
}

TEST(PoolFingerprintTest, AllocSizeMayDifferPasses)
{
    // allocSize is each rank's own physical DRAM: ranks may allocate unequal
    // sizes (equality is scenario-decided and caller-guaranteed, not checked)
    auto fps = MakeUniformGroup(4, 2, ASCEND_910C);
    fps[1].allocSize = 8ULL << 30;
    fps[3].allocSize = 16ULL << 30;
    ASSERT_EQ(ValidatePoolFingerprints(fps.data(), fps.size()), OFFLOAD_OK);
}

TEST(PoolFingerprintTest, GlobalRankOrchestrationFails)
{
    // duplicated rank id: rank 2 also reports globalRank 1 -> slot 1 would be
    // written by two ranks silently; the init-time check must intercept it
    auto fps = MakeUniformGroup(4, 2, ASCEND_910C);
    fps[2].globalRank = 1;
    ASSERT_EQ(ValidatePoolFingerprints(fps.data(), fps.size()), OFFLOAD_ERROR);

    // rank0 not reporting 0 (missing rank0 in the orchestration)
    auto shift = MakeUniformGroup(4, 2, ASCEND_910C);
    for (uint32_t r = 0; r < shift.size(); r++) {
        shift[r].globalRank = r + 1;
    }
    ASSERT_EQ(ValidatePoolFingerprints(shift.data(), shift.size()), OFFLOAD_ERROR);
}

TEST(PoolFingerprintTest, NormalizeLocalWorldSize)
{
    ASSERT_EQ(NormalizeLocalWorldSize(8, 0), 8U); // legacy default == single machine
    ASSERT_EQ(NormalizeLocalWorldSize(8, 8), 8U); // explicit single machine is equal after normalize
    ASSERT_EQ(NormalizeLocalWorldSize(8, 2), 2U); // 4 machines x 2 ranks
}

TEST(PoolFingerprintTest, MakePoolFingerprintFields)
{
    auto fp = MakePoolFingerprint(0x2000, 111, 222, 4, 2, 3, ASCEND_910C);
    ASSERT_EQ(fp.poolGva, 0x2000U);
    ASSERT_EQ(fp.reserveSize, 111U);
    ASSERT_EQ(fp.allocSize, 222U);
    ASSERT_EQ(fp.worldSize, 4U);
    ASSERT_EQ(fp.localWorldSize, 2U);
    ASSERT_EQ(fp.globalRank, 3U);
    ASSERT_EQ(fp.socType, ASCEND_910C);
}

} // namespace
} // namespace offload
} // namespace ock
