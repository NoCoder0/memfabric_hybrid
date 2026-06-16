/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <gtest/gtest.h>

#include <fstream>
#include <cstdio>

#define private   public
#define protected public
#include "zbal_sma_device_info.h"
#undef private
#undef protected

#include "zbal_test_constants.h"
#include "zbal_sma_device_pool.h"

using namespace zbal;
using namespace zbal::sma;
using namespace zbal::sma::device;

class TestDeviceInfoObserver : public testing::Test {
protected:
    void SetUp() override
    {
        observer_ = &DeviceInfoObserver::getInstance();
        observer_->record_history_ = false;
        observer_->snapshots_.clear();
        observer_->trace_next_ = 0;
        observer_->max_trace_len_ = kMaxTraceLen;
    }

    void TearDown() override
    {
        std::remove("/tmp/test_zbal_snapshot_0.json");
    }

    DeviceInfoObserver *observer_;
};

/* ==================== recordTrace ==================== */

TEST_F(TestDeviceInfoObserver, RecordTraceHistoryDisabledEarlyReturn)
{
    observer_->record_history_ = false;
    observer_->snapshots_.clear();
    observer_->recordTrace(TraceAction::ALLOC, 0x1000, ZBAL_UT_NUM_512, nullptr, 0);
    observer_->recordTrace(TraceAction::FREE_REQUESTED, 0x2000, ZBAL_UT_SIZE_1KB, nullptr, 0);
    EXPECT_TRUE(observer_->snapshots_.empty());
}

TEST_F(TestDeviceInfoObserver, RecordTraceHistoryEnabled)
{
    observer_->record_history_ = true;
    observer_->max_trace_len_ = ZBAL_UT_NUM_3;
    observer_->snapshots_.clear();
    observer_->recordTrace(TraceAction::ALLOC, 0x1000, ZBAL_UT_NUM_512, nullptr, 0);
    observer_->recordTrace(TraceAction::FREE_REQUESTED, 0x2000, ZBAL_UT_SIZE_1KB, nullptr, 0);
    observer_->recordTrace(TraceAction::SEGMENT_ALLOC, 0x3000, ZBAL_UT_SIZE_2KB, nullptr, 0);

    ASSERT_EQ(observer_->snapshots_.size(), 1u);
    EXPECT_EQ(observer_->snapshots_[0].trace_infos_.size(), 3u);
    EXPECT_EQ(observer_->snapshots_[0].trace_infos_[0].action_, TraceAction::ALLOC);
    EXPECT_EQ(observer_->snapshots_[0].trace_infos_[1].action_, TraceAction::FREE_REQUESTED);
    EXPECT_EQ(observer_->snapshots_[0].trace_infos_[ZBAL_UT_NUM_2].action_, TraceAction::SEGMENT_ALLOC);
}

TEST_F(TestDeviceInfoObserver, RecordTraceRollingUpdate)
{
    observer_->record_history_ = true;
    observer_->max_trace_len_ = ZBAL_UT_NUM_2;
    observer_->snapshots_.clear();
    observer_->recordTrace(TraceAction::ALLOC, 0x1000, ZBAL_UT_NUM_128, nullptr, 0);
    observer_->recordTrace(TraceAction::FREE_REQUESTED, 0x2000, ZBAL_UT_NUM_256, nullptr, 0);
    observer_->recordTrace(TraceAction::SEGMENT_FREE, 0x3000, ZBAL_UT_NUM_512, nullptr, 0);

    ASSERT_EQ(observer_->snapshots_.size(), 1u);
    EXPECT_EQ(observer_->snapshots_[0].trace_infos_.size(), 2u);
    EXPECT_EQ(observer_->snapshots_[0].trace_infos_[0].action_, TraceAction::SEGMENT_FREE);
}

TEST_F(TestDeviceInfoObserver, RecordTraceMultiDevice)
{
    observer_->record_history_ = true;
    observer_->max_trace_len_ = ZBAL_UT_NUM_2;
    observer_->snapshots_.clear();
    observer_->recordTrace(TraceAction::ALLOC, 0x1000, ZBAL_UT_NUM_128, nullptr, 0);
    observer_->recordTrace(TraceAction::SEGMENT_ALLOC, 0x5000, ZBAL_UT_SIZE_4KB, nullptr, ZBAL_UT_NUM_2);

    ASSERT_GE(observer_->snapshots_.size(), 3u);
    EXPECT_EQ(observer_->snapshots_[0].trace_infos_.size(), 1u);
    EXPECT_EQ(observer_->snapshots_[ZBAL_UT_NUM_2].trace_infos_.size(), 1u);
}

TEST_F(TestDeviceInfoObserver, RecordTraceAllActionTypes)
{
    observer_->record_history_ = true;
    observer_->max_trace_len_ = ZBAL_UT_NUM_10;
    observer_->snapshots_.clear();
    observer_->recordTrace(TraceAction::ALLOC, 0x1000, ZBAL_UT_NUM_100, nullptr, 0);
    observer_->recordTrace(TraceAction::FREE_REQUESTED, 0x2000, ZBAL_UT_NUM_200, nullptr, 0);
    observer_->recordTrace(TraceAction::FREE_COMPLETED, 0x3000, ZBAL_UT_NUM_300, nullptr, 0);
    observer_->recordTrace(TraceAction::SEGMENT_ALLOC, 0x4000, ZBAL_UT_NUM_400, nullptr, 0);
    observer_->recordTrace(TraceAction::SEGMENT_FREE, 0x5000, ZBAL_UT_NUM_500, nullptr, 0);
    observer_->recordTrace(TraceAction::SNAPSHOT, 0x6000, ZBAL_UT_NUM_600, nullptr, 0);
    observer_->recordTrace(TraceAction::OOM, 0x7000, ZBAL_UT_NUM_700, nullptr, 0);

    ASSERT_EQ(observer_->snapshots_.size(), 1u);
    EXPECT_EQ(observer_->snapshots_[0].trace_infos_.size(), 7u);
}

/* ==================== takeSnapshot ==================== */

TEST_F(TestDeviceInfoObserver, TakeSnapshotSingleBlockChain)
{
    DeviceBlockPool pool;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, &pool, reinterpret_cast<void *>(0x2000), BT_SMALL);
    block.allocated_ = true;
    block.requested_size_ = ZBAL_UT_SIZE_1KB;
    block.is_safe_ = true;

    std::vector<const DeviceBlock *> all_blocks = {&block};
    observer_->takeSnapshot(all_blocks, 0);

    ASSERT_GE(observer_->snapshots_.size(), 1u);
    const auto &seg = observer_->snapshots_[0].seg_infos_;
    ASSERT_EQ(seg.size(), 1u);
    EXPECT_EQ(seg[0].address_, 0x2000);
    EXPECT_EQ(seg[0].device_, 0);
    EXPECT_FALSE(seg[0].is_large_);
    EXPECT_FALSE(seg[0].is_private_);
    EXPECT_EQ(seg[0].total_size_, ZBAL_UT_SIZE_1KB);
    EXPECT_EQ(seg[0].allocated_size_, ZBAL_UT_SIZE_1KB);
    EXPECT_EQ(seg[0].active_size_, ZBAL_UT_SIZE_1KB);
}

TEST_F(TestDeviceInfoObserver, TakeSnapshotBlockChainWithPrevIsSkipped)
{
    DeviceBlockPool pool;
    DeviceBlock head(0, nullptr, ZBAL_UT_NUM_512, &pool, reinterpret_cast<void *>(0x1000), BT_SMALL);
    DeviceBlock mid(0, nullptr, ZBAL_UT_SIZE_1KB, &pool, reinterpret_cast<void *>(0x1200), BT_SMALL);
    DeviceBlock tail(0, nullptr, ZBAL_UT_NUM_256, &pool, reinterpret_cast<void *>(0x1600), BT_SMALL);
    head.next_ = &mid;
    mid.prev_ = &head;
    mid.next_ = &tail;
    tail.prev_ = &mid;
    head.allocated_ = true;
    mid.allocated_ = true;
    tail.allocated_ = false;
    head.requested_size_ = ZBAL_UT_NUM_512;
    mid.requested_size_ = ZBAL_UT_SIZE_1KB;
    tail.requested_size_ = ZBAL_UT_NUM_256;

    std::vector<const DeviceBlock *> all_blocks = {&head, &mid, &tail};
    observer_->takeSnapshot(all_blocks, 0);

    ASSERT_GE(observer_->snapshots_.size(), 1u);
    const auto &seg = observer_->snapshots_[0].seg_infos_;
    ASSERT_EQ(seg.size(), 1u);
    EXPECT_EQ(seg[0].total_size_, 512u + 1024u + 256u);
}

TEST_F(TestDeviceInfoObserver, TakeSnapshotLargePrivateBlock)
{
    DeviceBlockPool privatePool(true);
    DeviceBlock block(0, nullptr, kLargeBuffer, &privatePool, reinterpret_cast<void *>(0x5000), BT_BIG);
    block.allocated_ = true;
    block.requested_size_ = kLargeBuffer;

    std::vector<const DeviceBlock *> all_blocks = {&block};
    observer_->takeSnapshot(all_blocks, 0);

    ASSERT_GE(observer_->snapshots_.size(), 1u);
    const auto &seg = observer_->snapshots_[0].seg_infos_;
    ASSERT_EQ(seg.size(), 1u);
    EXPECT_TRUE(seg[0].is_large_);
    EXPECT_TRUE(seg[0].is_private_);
}

TEST_F(TestDeviceInfoObserver, TakeSnapshotBlockActiveByEventCount)
{
    DeviceBlockPool pool;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, &pool, reinterpret_cast<void *>(0x3000), BT_SMALL);
    block.allocated_ = false;
    block.event_count_ = ZBAL_UT_NUM_2;
    block.requested_size_ = ZBAL_UT_SIZE_1KB;

    std::vector<const DeviceBlock *> all_blocks = {&block};
    observer_->takeSnapshot(all_blocks, 0);

    ASSERT_GE(observer_->snapshots_.size(), 1u);
    const auto &seg = observer_->snapshots_[0].seg_infos_;
    ASSERT_EQ(seg.size(), 1u);
    EXPECT_EQ(seg[0].allocated_size_, 0);
    EXPECT_EQ(seg[0].active_size_, ZBAL_UT_SIZE_1KB);
}

TEST_F(TestDeviceInfoObserver, TakeSnapshotAddsTraceRecord)
{
    observer_->record_history_ = true;
    observer_->max_trace_len_ = ZBAL_UT_NUM_5;
    observer_->snapshots_.clear();

    DeviceBlockPool pool;
    DeviceBlock block(0, nullptr, ZBAL_UT_SIZE_1KB, &pool, reinterpret_cast<void *>(0x1000), BT_SMALL);
    block.allocated_ = true;

    std::vector<const DeviceBlock *> all_blocks = {&block};
    observer_->takeSnapshot(all_blocks, 0);

    bool hasSnapshotTrace = false;
    for (const auto &trace : observer_->snapshots_[0].trace_infos_) {
        if (trace.action_ == TraceAction::SNAPSHOT) {
            hasSnapshotTrace = true;
            break;
        }
    }
    EXPECT_TRUE(hasSnapshotTrace);
}

/* ==================== dumpSnapshot / dumpSnapshotJson ==================== */

TEST_F(TestDeviceInfoObserver, DumpSnapshotReturnsEmptyForNoData)
{
    observer_->snapshots_.clear();
    observer_->snapshots_.resize(1);
    const auto &snap = observer_->dumpSnapshot(0);
    EXPECT_TRUE(snap.seg_infos_.empty());
    EXPECT_TRUE(snap.trace_infos_.empty());
}

TEST_F(TestDeviceInfoObserver, DumpSnapshotReturnsRecordedData)
{
    observer_->record_history_ = true;
    observer_->max_trace_len_ = ZBAL_UT_NUM_5;
    observer_->snapshots_.clear();
    observer_->recordTrace(TraceAction::ALLOC, 0x1000, ZBAL_UT_NUM_256, nullptr, 0);

    const auto &snap = observer_->dumpSnapshot(0);
    ASSERT_EQ(snap.trace_infos_.size(), 1u);
    EXPECT_EQ(snap.trace_infos_[0].action_, TraceAction::ALLOC);
}

TEST_F(TestDeviceInfoObserver, DumpSnapshotJsonWithSegments)
{
    observer_->snapshots_.clear();
    observer_->snapshots_.resize(1);

    SegmentInfo seg;
    seg.device_ = 0;
    seg.address_ = 0x1000;
    seg.total_size_ = ZBAL_UT_SIZE_4KB;
    seg.allocated_size_ = ZBAL_UT_SIZE_2KB;
    seg.active_size_ = ZBAL_UT_SIZE_2KB;
    seg.requested_size_ = ZBAL_UT_SIZE_2KB;
    seg.is_large_ = false;
    seg.is_private_ = false;

    BlockInfo blk;
    blk.size_ = ZBAL_UT_SIZE_2KB;
    blk.requested_size_ = ZBAL_UT_SIZE_2KB;
    blk.allocated_ = true;
    blk.active_ = true;
    seg.blocks_.push_back(blk);

    BlockInfo blk2;
    blk2.size_ = ZBAL_UT_SIZE_2KB;
    blk2.requested_size_ = 0;
    blk2.allocated_ = false;
    blk2.active_ = false;
    seg.blocks_.push_back(blk2);

    observer_->snapshots_[0].seg_infos_.push_back(seg);
    observer_->snapshots_[0].trace_infos_.push_back(
        TraceInfo(TraceAction::ALLOC, 0, 0x1000, ZBAL_UT_SIZE_2KB, nullptr));
    observer_->dumpSnapshotJson(0, "/tmp/test_zbal_snapshot_");

    std::ifstream in("/tmp/test_zbal_snapshot_0.json");
    ASSERT_TRUE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\"segments\""), std::string::npos);
    EXPECT_NE(content.find("\"device_traces\""), std::string::npos);
    EXPECT_NE(content.find("active_allocated"), std::string::npos);
    EXPECT_NE(content.find("inactive"), std::string::npos);
}

TEST_F(TestDeviceInfoObserver, DumpSnapshotJsonEmptySnapshots)
{
    observer_->snapshots_.clear();
    observer_->snapshots_.resize(1);
    observer_->dumpSnapshotJson(0, "/tmp/test_zbal_snapshot_");
    SUCCEED();
}

/* ==================== recordHistory ==================== */

TEST_F(TestDeviceInfoObserver, RecordHistoryEnableDisable)
{
    EXPECT_FALSE(observer_->record_history_);
    observer_->recordHistory(true, ZBAL_UT_NUM_100);
    EXPECT_TRUE(observer_->record_history_);
    EXPECT_EQ(observer_->max_trace_len_, ZBAL_UT_NUM_100);
    observer_->recordHistory(false, 0);
    EXPECT_FALSE(observer_->record_history_);
    EXPECT_EQ(observer_->max_trace_len_, 0);
}

TEST_F(TestDeviceInfoObserver, RecordHistoryClearsSnapshotsOnDisable)
{
    observer_->record_history_ = true;
    observer_->snapshots_.clear();
    observer_->recordTrace(TraceAction::ALLOC, 0x1000, ZBAL_UT_NUM_128, nullptr, 0);
    EXPECT_EQ(observer_->snapshots_.size(), 1u);
    observer_->recordHistory(false, 0);
    EXPECT_TRUE(observer_->snapshots_.empty());
    EXPECT_FALSE(observer_->record_history_);
}
