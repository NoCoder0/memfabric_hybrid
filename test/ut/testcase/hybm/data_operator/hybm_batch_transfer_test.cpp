/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include <array>
#include <cstring>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <dlfcn.h>

namespace {
void *BatchTestDlopen(const char *, int);
void *BatchTestDlsym(void *, const char *);
int BatchTestDlclose(void *);
} // namespace

// Exercise the kernel on the CPU without requiring the device logging runtime.
#define MF_HYBM_OPS_HYBM_KERNEL_HYBM_KERNEL_LOG_H
#define HYBM_LOGE(...) ((void)0)
#define HYBM_LOGW(...) ((void)0)
#define HYBM_LOGI(...) ((void)0)
#define HYBM_LOGD(...) ((void)0)
#define dlopen         BatchTestDlopen
#define dlsym          BatchTestDlsym
#define dlclose        BatchTestDlclose
#include "../../../../../src/hybm/ops/hybm_kernel/hybm_batch_transfer.cc"
#undef dlopen
#undef dlsym
#undef dlclose
#include "../../../../../src/hybm/ops/hybm_kernel/hybm_urma_device_copy.cc"

namespace {
constexpr uint32_t FIRST_RANK = 1U;
constexpr uint32_t SECOND_RANK = 2U;
constexpr uint32_t CHUNK_SIZE = 128U;
constexpr uint32_t IO_COUNT = CHUNK_SIZE + 2U;
constexpr ock::mf::ThreadHandle THREAD = 10U;
constexpr ock::mf::ChannelHandle CHANNEL = 20U;
std::vector<const ock::mf::HcommBatchTransferDesc *> submittedDescriptors;
std::vector<uint32_t> submittedCounts;
int32_t batchResult = BM_OK;
size_t batchFailAt = SIZE_MAX;
uint32_t markerReadCount = 0U;
uint32_t singleWriteCount = 0U;
uint32_t drainCount = 0U;
std::vector<uint64_t> notifyRecordIds;
uint32_t batchModeStartCount = 0U;
uint32_t batchModeEndCount = 0U;
uint32_t batchModeStartFailAt = UINT32_MAX;
uint32_t batchModeEndFailAt = UINT32_MAX;
int32_t batchModeStartResult = BM_OK;
int32_t batchModeEndResult = BM_OK;
std::vector<char> batchModeOrder;
std::vector<char> operationOrder;
std::vector<std::string> batchStartTags;
std::vector<std::string> batchEndTags;

class HybmBatchTransferTest : public testing::Test {
protected:
    void SetUp() override
    {
        submittedDescriptors.clear();
        submittedCounts.clear();
        batchResult = BM_OK;
        batchFailAt = SIZE_MAX;
        markerReadCount = 0U;
        singleWriteCount = 0U;
        drainCount = 0U;
        notifyRecordIds.clear();
        batchModeStartCount = 0U;
        batchModeEndCount = 0U;
        batchModeStartFailAt = UINT32_MAX;
        batchModeEndFailAt = UINT32_MAX;
        batchModeStartResult = BM_OK;
        batchModeEndResult = BM_OK;
        batchModeOrder.clear();
        operationOrder.clear();
        batchStartTags.clear();
        batchEndTags.clear();
        descriptors.resize(IO_COUNT);
        for (uint32_t i = 0; i < IO_COUNT; ++i) {
            auto &desc = descriptors[i];
            if (i % 2U == 0U) {
                desc.transType = ock::mf::HCOMM_TRANSFER_TYPE_WRITE;
                desc.transferInfo.write = {sizeof(source), &destination, &source};
            } else {
                desc.transType = ock::mf::HCOMM_TRANSFER_TYPE_READ;
                desc.transferInfo.read = {sizeof(source), &destination, &source};
            }
        }
        param.rank_num = 2U;
        param.rank_id_list = ranks;
        param.rank_start_idx_list = starts;
        param.rank_list_num_list = counts;
        param.thread_list = threads;
        param.channel_list = channels;
        param.total_list_num = IO_COUNT;
        param.transfer_descs = descriptors.data();
    }

    uint64_t source = 123U;
    uint64_t destination = 0U;
    uint32_t ranks[2] = {FIRST_RANK, SECOND_RANK};
    uint32_t starts[2] = {0U, CHUNK_SIZE + 1U};
    uint32_t counts[2] = {CHUNK_SIZE + 1U, 1U};
    ock::mf::ThreadHandle threads[2] = {THREAD, THREAD};
    ock::mf::ChannelHandle channels[2] = {CHANNEL, CHANNEL};
    std::vector<ock::mf::HcommBatchTransferDesc> descriptors;
    HybmBatchTransferParam param{};
};
} // namespace

extern "C" {
int32_t HcommBatchModeStart(const char *tag)
{
    batchStartTags.emplace_back(tag);
    batchModeStartCount++;
    batchModeOrder.push_back('S');
    return batchModeStartCount == batchModeStartFailAt ? BM_ERROR : batchModeStartResult;
}

int32_t HcommBatchModeEnd(const char *tag)
{
    batchEndTags.emplace_back(tag);
    batchModeEndCount++;
    batchModeOrder.push_back('E');
    return batchModeEndCount == batchModeEndFailAt ? BM_ERROR : batchModeEndResult;
}

int32_t HcommChannelFenceOnThread(ock::mf::ThreadHandle, ock::mf::ChannelHandle)
{
    operationOrder.push_back('F');
    return BM_OK;
}

int32_t HcommReadOnThread(ock::mf::ThreadHandle, ock::mf::ChannelHandle, void *dst, const void *src, uint64_t len)
{
    markerReadCount++;
    operationOrder.push_back('M');
    std::memcpy(dst, src, len);
    return BM_OK;
}

int32_t HcommWriteOnThread(ock::mf::ThreadHandle, ock::mf::ChannelHandle, void *dst, const void *src, uint64_t len)
{
    singleWriteCount++;
    std::memcpy(dst, src, len);
    return BM_OK;
}

int32_t HcommChannelDrainOnThread(ock::mf::ThreadHandle, ock::mf::ChannelHandle)
{
    drainCount++;
    operationOrder.push_back('A');
    return BM_OK;
}

int32_t HcommAclrtNotifyRecordOnThread(ock::mf::ThreadHandle, uint64_t notifyId)
{
    notifyRecordIds.push_back(notifyId);
    operationOrder.push_back('N');
    return BM_OK;
}

int32_t HcommBatchTransferOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel,
                                   const ock::mf::HcommBatchTransferDesc *descs, uint32_t count)
{
    EXPECT_EQ(thread, THREAD);
    EXPECT_EQ(channel, CHANNEL);
    operationOrder.push_back('D');
    batchModeOrder.push_back('D');
    submittedDescriptors.push_back(descs);
    submittedCounts.push_back(count);
    if (submittedCounts.size() == batchFailAt) {
        return BM_ERROR;
    }
    return batchResult;
}
}

namespace {
// Resolve the existing HCOMM test doubles through the same loader path as the device.
void *BatchTestDlopen(const char *name, int)
{
    EXPECT_STREQ(name, "libccl_kernel.so");
    static int handle;
    return &handle;
}

int BatchTestDlclose(void *)
{
    return 0;
}

void *BatchTestDlsym(void *, const char *name)
{
    if (std::strcmp(name, "HcommBatchModeStart") == 0) {
        return reinterpret_cast<void *>(&HcommBatchModeStart);
    }
    if (std::strcmp(name, "HcommBatchModeEnd") == 0) {
        return reinterpret_cast<void *>(&HcommBatchModeEnd);
    }
    if (std::strcmp(name, "HcommChannelFenceOnThread") == 0) {
        return reinterpret_cast<void *>(&HcommChannelFenceOnThread);
    }
    if (std::strcmp(name, "HcommReadOnThread") == 0) {
        return reinterpret_cast<void *>(&HcommReadOnThread);
    }
    if (std::strcmp(name, "HcommWriteOnThread") == 0) {
        return reinterpret_cast<void *>(&HcommWriteOnThread);
    }
    if (std::strcmp(name, "HcommBatchTransferOnThread") == 0) {
        return reinterpret_cast<void *>(&HcommBatchTransferOnThread);
    }
    if (std::strcmp(name, "HcommChannelDrainOnThread") == 0) {
        return reinterpret_cast<void *>(&HcommChannelDrainOnThread);
    }
    if (std::strcmp(name, "HcommAclrtNotifyRecordOnThread") == 0) {
        return reinterpret_cast<void *>(&HcommAclrtNotifyRecordOnThread);
    }
    ADD_FAILURE() << "Unexpected HCOMM symbol: " << name;
    return nullptr;
}
} // namespace

TEST_F(HybmBatchTransferTest, UnifiedEntrySubmitsMixedDescriptorsUnchangedByRankAndChunk)
{
    const auto original = descriptors;
    ASSERT_EQ(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{CHUNK_SIZE, 1U, 1U}));
    ASSERT_EQ(submittedDescriptors.size(), 3U);
    EXPECT_EQ(submittedDescriptors[0], descriptors.data());
    EXPECT_EQ(submittedDescriptors[1], descriptors.data() + CHUNK_SIZE);
    EXPECT_EQ(submittedDescriptors[2], descriptors.data() + CHUNK_SIZE + 1U);
    EXPECT_EQ(std::memcmp(original.data(), descriptors.data(), descriptors.size() * sizeof(descriptors[0])), 0);
    EXPECT_EQ(markerReadCount, 0U);
    EXPECT_EQ(singleWriteCount, 0U);
    EXPECT_EQ(batchModeStartCount, 2U);
    EXPECT_EQ(batchModeEndCount, 2U);
}

TEST_F(HybmBatchTransferTest, UnsupportedBatchReturnsNotSupported)
{
    batchResult = BM_NOT_SUPPORTED;
    EXPECT_EQ(HybmBatchTransfer(&param), BM_NOT_SUPPORTED);
    EXPECT_EQ(submittedCounts.size(), 1U);
    EXPECT_EQ(markerReadCount, 0U);
    EXPECT_EQ(singleWriteCount, 0U);
    EXPECT_EQ(batchModeEndCount, 1U);
}

TEST_F(HybmBatchTransferTest, RejectsMissingDescriptorsAndOutOfRangeRank)
{
    param.transfer_descs = nullptr;
    EXPECT_NE(HybmBatchTransfer(&param), BM_OK);
    param.transfer_descs = descriptors.data();
    starts[1] = IO_COUNT;
    EXPECT_NE(HybmBatchTransfer(&param), BM_OK);
    EXPECT_TRUE(submittedCounts.empty());
}

TEST_F(HybmBatchTransferTest, SubmitsOneCompletionMarkerAfterEachRankFence)
{
    uint64_t markerSources[2] = {11U, 22U};
    uint64_t markerDestinations[2] = {0U, 0U};
    ock::mf::HcommBatchTransferDesc markers[2]{};
    for (uint32_t i = 0; i < 2U; ++i) {
        markers[i].transType = ock::mf::HCOMM_TRANSFER_TYPE_READ;
        markers[i].transferInfo.read = {sizeof(uint64_t), &markerDestinations[i], &markerSources[i]};
    }
    param.marker_descs = markers;

    ASSERT_EQ(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(markerReadCount, 2U);
    EXPECT_EQ(markerDestinations[0], markerSources[0]);
    EXPECT_EQ(markerDestinations[1], markerSources[1]);
    EXPECT_EQ(operationOrder, (std::vector<char>{'D', 'D', 'F', 'M', 'D', 'F', 'M'}));
}

TEST_F(HybmBatchTransferTest, BatchErrorDoesNotUseSingleFallback)
{
    batchResult = BM_ERROR;
    EXPECT_NE(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(markerReadCount, 0U);
    EXPECT_EQ(singleWriteCount, 0U);
}

TEST_F(HybmBatchTransferTest, SecondRankErrorReturnsOverallFailure)
{
    constexpr size_t SECOND_RANK_DATA_CALL = 3U;
    uint64_t markerSources[2] = {11U, 22U};
    uint64_t markerDestinations[2] = {0U, 0U};
    ock::mf::HcommBatchTransferDesc markers[2]{};
    for (uint32_t i = 0; i < 2U; ++i) {
        markers[i].transType = ock::mf::HCOMM_TRANSFER_TYPE_READ;
        markers[i].transferInfo.read = {sizeof(uint64_t), &markerDestinations[i], &markerSources[i]};
    }
    param.marker_descs = markers;
    batchFailAt = SECOND_RANK_DATA_CALL;

    EXPECT_EQ(HybmBatchTransfer(&param), BM_ERROR);
    EXPECT_EQ(submittedCounts.size(), SECOND_RANK_DATA_CALL);
    EXPECT_EQ(markerReadCount, 1U);
    EXPECT_EQ(markerDestinations[0], markerSources[0]);
    EXPECT_EQ(markerDestinations[1], 0U);
    EXPECT_EQ(batchModeStartCount, 2U);
    EXPECT_EQ(batchModeEndCount, 2U);
}

TEST_F(HybmBatchTransferTest, FlushesBeforeNextChunkAndPreservesCustomTag)
{
    constexpr auto CUSTOM_TAG = "BatchFlushTest";
    ASSERT_EQ(HybmSubmitBatchTransfer(&param, CUSTOM_TAG), BM_OK);
    EXPECT_EQ(batchModeOrder, (std::vector<char>{'S', 'D', 'E', 'S', 'D', 'D', 'E'}));
    EXPECT_EQ(batchStartTags, (std::vector<std::string>{CUSTOM_TAG, CUSTOM_TAG}));
    EXPECT_EQ(batchEndTags, batchStartTags);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{CHUNK_SIZE, 1U, 1U}));
}

TEST_F(HybmBatchTransferTest, FlushFailureStopsBeforeNextChunkAndAttemptsCleanup)
{
    batchModeEndFailAt = 1U;
    EXPECT_EQ(HybmBatchTransfer(&param), BM_ERROR);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{CHUNK_SIZE}));
    EXPECT_EQ(batchModeStartCount, 1U);
    EXPECT_EQ(batchModeEndCount, 2U);
    EXPECT_EQ(operationOrder, (std::vector<char>{'D'}));
}

TEST_F(HybmBatchTransferTest, RestartFailureStopsBeforeNextChunkAndAttemptsCleanup)
{
    batchModeStartFailAt = 2U;
    EXPECT_EQ(HybmBatchTransfer(&param), BM_ERROR);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{CHUNK_SIZE}));
    EXPECT_EQ(batchModeStartCount, 2U);
    EXPECT_EQ(batchModeEndCount, 2U);
    EXPECT_EQ(operationOrder, (std::vector<char>{'D'}));
}

TEST_F(HybmBatchTransferTest, SingleRankAtChunkLimitDoesNotFlushAnExtraBatch)
{
    param.rank_num = 1U;
    counts[0] = CHUNK_SIZE;
    param.total_list_num = CHUNK_SIZE;

    ASSERT_EQ(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{CHUNK_SIZE}));
    EXPECT_EQ(batchModeOrder, (std::vector<char>{'S', 'D', 'E'}));
}

TEST_F(HybmBatchTransferTest, TwoFullChunksFlushOnlyBetweenChunks)
{
    const auto descriptor = descriptors.front();
    descriptors.resize(2U * CHUNK_SIZE, descriptor);
    param.rank_num = 1U;
    counts[0] = descriptors.size();
    param.total_list_num = counts[0];
    param.transfer_descs = descriptors.data();

    ASSERT_EQ(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{CHUNK_SIZE, CHUNK_SIZE}));
    EXPECT_EQ(batchModeOrder, (std::vector<char>{'S', 'D', 'E', 'S', 'D', 'E'}));
    ASSERT_EQ(submittedDescriptors.size(), 2U);
    EXPECT_EQ(submittedDescriptors[1], descriptors.data() + CHUNK_SIZE);
}

TEST_F(HybmBatchTransferTest, UnsupportedBatchModeStillSubmitsEveryChunk)
{
    batchModeStartResult = BM_NOT_SUPPORTED;
    batchModeEndResult = BM_NOT_SUPPORTED;

    ASSERT_EQ(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{CHUNK_SIZE, 1U, 1U}));
    EXPECT_EQ(batchModeOrder, (std::vector<char>{'S', 'D', 'E', 'S', 'D', 'D', 'E'}));
}

TEST_F(HybmBatchTransferTest, Splits129DescriptorsAt128Boundary)
{
    constexpr uint32_t BATCH_LIMIT = 128U;
    constexpr uint32_t DESCRIPTOR_COUNT = BATCH_LIMIT + 1U;
    param.rank_num = 1U;
    counts[0] = DESCRIPTOR_COUNT;
    param.total_list_num = DESCRIPTOR_COUNT;

    ASSERT_EQ(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{BATCH_LIMIT, 1U}));
    EXPECT_EQ(batchModeOrder, (std::vector<char>{'S', 'D', 'E', 'S', 'D', 'E'}));
    ASSERT_EQ(submittedDescriptors.size(), 2U);
    EXPECT_EQ(submittedDescriptors[1], descriptors.data() + BATCH_LIMIT);
}

TEST_F(HybmBatchTransferTest, SingleRankDrainsBeforeRecordingNotify)
{
    param.rank_num = 1U;
    param.notify_id = 7U;
    param.require_drain = 1U;

    ASSERT_EQ(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(drainCount, 1U);
    ASSERT_EQ(notifyRecordIds.size(), 1U);
    EXPECT_EQ(notifyRecordIds[0], 7U);
    EXPECT_EQ(operationOrder, (std::vector<char>{'D', 'D', 'F', 'A', 'N'}));
}

TEST_F(HybmBatchTransferTest, RejectsNotifyForMultipleRanks)
{
    param.notify_id = 7U;
    EXPECT_EQ(HybmBatchTransfer(&param), BM_INVALID_PARAM);
    EXPECT_TRUE(submittedCounts.empty());
    EXPECT_TRUE(notifyRecordIds.empty());
}

class HybmUrmaDeviceCopyTest : public HybmBatchTransferTest {
protected:
    void SetUp() override
    {
        HybmBatchTransferTest::SetUp();
        context.capacity = tile.size();
        context.rankId = FIRST_RANK;
        context.thread = THREAD;
        context.channel = CHANNEL;
        context.poolBase = 0x100000U;
        context.poolSize = 0x10000U;
        context.importedBase = 0x400000U;
        context.descriptors = tile.data();
        context.marker.transType = ock::mf::HCOMM_TRANSFER_TYPE_READ;
        context.marker.transferInfo.read = {sizeof(source), &destination, &source};
        context.maxRecords = HYBM_DEVICE_COPY_MAX_RECORDS;
        context.recordCount = &recordCount;
        args = {&context, sources.data(), destinations.data(), lengths.data(), &count, 0U};
        for (size_t i = 0; i < sources.size(); ++i) {
            sources[i] = context.poolBase + 0x100U * (i + 1U);
            destinations[i] = 0x800000U + 0x200U * (i + 1U);
            lengths[i] = 16U * (i + 1U);
        }
    }

    HybmUrmaDeviceCopyContext context{};
    HybmUrmaDeviceCopyArgs args{};
    std::array<ock::mf::HcommBatchTransferDesc, 2> tile{};
    std::array<uint64_t, 5> sources{};
    std::array<uint64_t, 5> destinations{};
    std::array<uint32_t, 5> lengths{};
    uint32_t count{2U};
    uint32_t recordCount{0U};
};

TEST_F(HybmUrmaDeviceCopyTest, MixedOffloadAndRestoreTranslateOnlyHostAddresses)
{
    std::swap(sources[1], destinations[1]);

    ASSERT_EQ(HybmUrmaDeviceCopy(&args), BM_OK);

    EXPECT_EQ(recordCount, count);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{2U}));
    EXPECT_EQ(tile[0].transType, ock::mf::HCOMM_TRANSFER_TYPE_READ);
    EXPECT_EQ(tile[0].transferInfo.read.src, reinterpret_cast<void *>(context.importedBase + 0x100U));
    EXPECT_EQ(tile[0].transferInfo.read.dst, reinterpret_cast<void *>(destinations[0]));
    EXPECT_EQ(tile[0].transferInfo.read.len, lengths[0]);
    EXPECT_EQ(tile[1].transType, ock::mf::HCOMM_TRANSFER_TYPE_WRITE);
    EXPECT_EQ(tile[1].transferInfo.write.src, reinterpret_cast<void *>(sources[1]));
    EXPECT_EQ(tile[1].transferInfo.write.dst, reinterpret_cast<void *>(context.importedBase + 0x200U));
    EXPECT_EQ(tile[1].transferInfo.write.len, lengths[1]);
    EXPECT_EQ(operationOrder, (std::vector<char>{'D', 'F', 'M'}));
    EXPECT_EQ(destination, source);
    EXPECT_EQ(batchStartTags, (std::vector<std::string>{""}));
    EXPECT_EQ(batchEndTags, batchStartTags);
}

TEST_F(HybmUrmaDeviceCopyTest, MultipleTilesSubmitEveryRecordOnceAndSignalEachTile)
{
    count = sources.size();

    for (uint32_t start = 0; start < count; start += context.capacity) {
        args.startIndex = start;
        destination = 0U;
        ASSERT_EQ(HybmUrmaDeviceCopy(&args), BM_OK);
        ASSERT_EQ(recordCount, count);
        const auto used = std::min(context.capacity, count - start);
        ASSERT_EQ(submittedCounts.back(), used);
        for (uint32_t i = 0; i < used; ++i) {
            const auto input = start + i;
            EXPECT_EQ(tile[i].transType, ock::mf::HCOMM_TRANSFER_TYPE_READ);
            EXPECT_EQ(tile[i].transferInfo.read.src,
                      reinterpret_cast<void *>(context.importedBase + sources[input] - context.poolBase));
            EXPECT_EQ(tile[i].transferInfo.read.dst, reinterpret_cast<void *>(destinations[input]));
            EXPECT_EQ(tile[i].transferInfo.read.len, lengths[input]);
        }
        EXPECT_EQ(destination, source);
    }

    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{2U, 2U, 1U}));
    EXPECT_EQ(markerReadCount, 3U);
    EXPECT_EQ(operationOrder, (std::vector<char>{'D', 'F', 'M', 'D', 'F', 'M', 'D', 'F', 'M'}));
    EXPECT_EQ(batchStartTags, (std::vector<std::string>{"", "", ""}));
    EXPECT_EQ(batchEndTags, batchStartTags);
}

TEST_F(HybmUrmaDeviceCopyTest, ReplayReadsUpdatedMetadataAtTheSameAddresses)
{
    ASSERT_EQ(HybmUrmaDeviceCopy(&args), BM_OK);
    ASSERT_EQ(recordCount, 2U);

    count = 1U;
    sources[0] = 0x900000U;
    destinations[0] = context.poolBase + 0x500U;
    lengths[0] = 128U;
    destination = 0U;

    ASSERT_EQ(HybmUrmaDeviceCopy(&args), BM_OK);

    EXPECT_EQ(recordCount, 1U);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{2U, 1U}));
    EXPECT_EQ(tile[0].transType, ock::mf::HCOMM_TRANSFER_TYPE_WRITE);
    EXPECT_EQ(tile[0].transferInfo.write.src, reinterpret_cast<void *>(sources[0]));
    EXPECT_EQ(tile[0].transferInfo.write.dst, reinterpret_cast<void *>(context.importedBase + 0x500U));
    EXPECT_EQ(tile[0].transferInfo.write.len, 128U);
    EXPECT_EQ(markerReadCount, 2U);
    EXPECT_EQ(destination, source);
    EXPECT_EQ(batchStartTags, (std::vector<std::string>{"", ""}));
    EXPECT_EQ(batchEndTags, batchStartTags);
}
