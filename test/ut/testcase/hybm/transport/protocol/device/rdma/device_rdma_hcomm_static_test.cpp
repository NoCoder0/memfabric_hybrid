/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 *
 * Unit tests for static helper functions in DeviceRdmaHcommTransportManager.
 * These functions are data-structure-only and do not need ACL/HCOMM hardware.
 */
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cstdio>
#include <fstream>

#define private public
#include "device_rdma_hcomm_transport_manager.h"
#undef private

namespace ock {
namespace mf {
namespace transport {
namespace device {

using PendingTransfer = DeviceRdmaHcommTransportManager::PendingTransfer;
using DeviceTransferBuffers = ::ock::mf::transport::device::DeviceTransferBuffers;

class DeviceRdmaHcommStaticTest : public ::testing::Test {
protected:
    static PendingTransfer MakePending(uint32_t rankId, bool inFlight)
    {
        PendingTransfer pt{};
        pt.rankId = rankId;
        pt.inFlight = inFlight;
        return pt;
    }
};

// ───── ExtractRankPending ─────
TEST_F(DeviceRdmaHcommStaticTest, ExtractRankPending_MovesMatchingEntries)
{
    std::vector<PendingTransfer> src = {
        MakePending(0, true),
        MakePending(1, true),
        MakePending(0, false),
        MakePending(2, true),
    };
    std::vector<PendingTransfer> dst;

    DeviceRdmaHcommTransportManager::ExtractRankPending(src, 0, dst);

    EXPECT_EQ(dst.size(), 2U);
    EXPECT_EQ(src.size(), 2U);
    for (const auto &pt : dst) {
        EXPECT_EQ(pt.rankId, 0U);
    }
    for (const auto &pt : src) {
        EXPECT_NE(pt.rankId, 0U);
    }
}

TEST_F(DeviceRdmaHcommStaticTest, ExtractRankPending_NoMatch_LeavesSrcUnchanged)
{
    std::vector<PendingTransfer> src = {MakePending(1, true), MakePending(2, false)};
    std::vector<PendingTransfer> dst;

    DeviceRdmaHcommTransportManager::ExtractRankPending(src, 0, dst);

    EXPECT_TRUE(dst.empty());
    EXPECT_EQ(src.size(), 2U);
}

TEST_F(DeviceRdmaHcommStaticTest, ExtractRankPending_EmptySrc)
{
    std::vector<PendingTransfer> src;
    std::vector<PendingTransfer> dst;

    DeviceRdmaHcommTransportManager::ExtractRankPending(src, 0, dst);

    EXPECT_TRUE(dst.empty());
    EXPECT_TRUE(src.empty());
}

TEST_F(DeviceRdmaHcommStaticTest, ExtractRankPending_AllMatch)
{
    std::vector<PendingTransfer> src = {
        MakePending(1, true),
        MakePending(1, false),
    };
    std::vector<PendingTransfer> dst;

    DeviceRdmaHcommTransportManager::ExtractRankPending(src, 1, dst);

    EXPECT_EQ(dst.size(), 2U);
    EXPECT_TRUE(src.empty());
}

// ───── RestoreRankPending ─────
TEST_F(DeviceRdmaHcommStaticTest, RestoreRankPending_MovesAll)
{
    std::vector<PendingTransfer> src = {MakePending(0, true), MakePending(1, false)};
    std::vector<PendingTransfer> dst = {MakePending(2, true)};

    DeviceRdmaHcommTransportManager::RestoreRankPending(src, dst);

    EXPECT_TRUE(src.empty());
    EXPECT_EQ(dst.size(), 3U);
}

TEST_F(DeviceRdmaHcommStaticTest, RestoreRankPending_EmptySrc)
{
    std::vector<PendingTransfer> src;
    std::vector<PendingTransfer> dst = {MakePending(0, true)};

    DeviceRdmaHcommTransportManager::RestoreRankPending(src, dst);

    EXPECT_EQ(dst.size(), 1U);
}

// ───── ReleaseDeviceTransferBuffers (shared free function) ─────
TEST_F(DeviceRdmaHcommStaticTest, ReleaseDeviceTransferBuffers_NullDstList)
{
    DeviceTransferBuffers buffers{};
    EXPECT_NO_THROW(ReleaseDeviceTransferBuffers(buffers));
    EXPECT_EQ(buffers.dstList, nullptr);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
