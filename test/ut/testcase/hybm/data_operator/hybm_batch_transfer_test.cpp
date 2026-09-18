/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include <cstring>
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

namespace {
constexpr uint32_t kFirstRank = 1U;
constexpr uint32_t kSecondRank = 2U;
constexpr uint32_t kChunkSize = 1000U;
constexpr uint32_t kIoCount = kChunkSize + 2U;
constexpr ock::mf::ThreadHandle kThread = 10U;
constexpr ock::mf::ChannelHandle kChannel = 20U;
std::vector<const ock::mf::HcommBatchTransferDesc *> submittedDescriptors;
std::vector<uint32_t> submittedCounts;
int32_t batchResult = BM_OK;
size_t batchFailAt = SIZE_MAX;
uint32_t markerReadCount = 0U;
uint32_t singleWriteCount = 0U;
uint32_t batchModeStartCount = 0U;
uint32_t batchModeEndCount = 0U;
std::vector<char> operationOrder;

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
        batchModeStartCount = 0U;
        batchModeEndCount = 0U;
        operationOrder.clear();
        descriptors.resize(kIoCount);
        for (uint32_t i = 0; i < kIoCount; ++i) {
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
        param.total_list_num = kIoCount;
        param.transfer_descs = descriptors.data();
    }

    uint64_t source = 123U;
    uint64_t destination = 0U;
    uint32_t ranks[2] = {kFirstRank, kSecondRank};
    uint32_t starts[2] = {0U, kChunkSize + 1U};
    uint32_t counts[2] = {kChunkSize + 1U, 1U};
    ock::mf::ThreadHandle threads[2] = {kThread, kThread};
    ock::mf::ChannelHandle channels[2] = {kChannel, kChannel};
    std::vector<ock::mf::HcommBatchTransferDesc> descriptors;
    HybmBatchTransferParam param{};
};
} // namespace

extern "C" {
int32_t HcommBatchModeStart(const char *)
{
    batchModeStartCount++;
    return BM_OK;
}

int32_t HcommBatchModeEnd(const char *)
{
    batchModeEndCount++;
    return BM_OK;
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

int32_t HcommBatchTransferOnThread(ock::mf::ThreadHandle thread, ock::mf::ChannelHandle channel,
                                   const ock::mf::HcommBatchTransferDesc *descs, uint32_t count)
{
    EXPECT_EQ(thread, kThread);
    EXPECT_EQ(channel, kChannel);
    operationOrder.push_back('D');
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
    ADD_FAILURE() << "Unexpected HCOMM symbol: " << name;
    return nullptr;
}
} // namespace

TEST_F(HybmBatchTransferTest, UnifiedEntrySubmitsMixedDescriptorsUnchangedByRankAndChunk)
{
    const auto original = descriptors;
    ASSERT_EQ(HybmBatchTransfer(&param), BM_OK);
    EXPECT_EQ(submittedCounts, (std::vector<uint32_t>{kChunkSize, 1U, 1U}));
    ASSERT_EQ(submittedDescriptors.size(), 3U);
    EXPECT_EQ(submittedDescriptors[0], descriptors.data());
    EXPECT_EQ(submittedDescriptors[1], descriptors.data() + kChunkSize);
    EXPECT_EQ(submittedDescriptors[2], descriptors.data() + kChunkSize + 1U);
    EXPECT_EQ(std::memcmp(original.data(), descriptors.data(), descriptors.size() * sizeof(descriptors[0])), 0);
    EXPECT_EQ(markerReadCount, 0U);
    EXPECT_EQ(singleWriteCount, 0U);
    EXPECT_EQ(batchModeStartCount, 1U);
    EXPECT_EQ(batchModeEndCount, 1U);
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
    starts[1] = kIoCount;
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
    constexpr size_t kSecondRankDataCall = 3U;
    uint64_t markerSources[2] = {11U, 22U};
    uint64_t markerDestinations[2] = {0U, 0U};
    ock::mf::HcommBatchTransferDesc markers[2]{};
    for (uint32_t i = 0; i < 2U; ++i) {
        markers[i].transType = ock::mf::HCOMM_TRANSFER_TYPE_READ;
        markers[i].transferInfo.read = {sizeof(uint64_t), &markerDestinations[i], &markerSources[i]};
    }
    param.marker_descs = markers;
    batchFailAt = kSecondRankDataCall;

    EXPECT_EQ(HybmBatchTransfer(&param), BM_ERROR);
    EXPECT_EQ(submittedCounts.size(), kSecondRankDataCall);
    EXPECT_EQ(markerReadCount, 1U);
    EXPECT_EQ(markerDestinations[0], markerSources[0]);
    EXPECT_EQ(markerDestinations[1], 0U);
    EXPECT_EQ(batchModeStartCount, 1U);
    EXPECT_EQ(batchModeEndCount, 1U);
}

TEST_F(HybmBatchTransferTest, OneSideWriteEntryKeepsOriginalTransferPath)
{
    batchResult = BM_NOT_SUPPORTED;
    uint64_t length = sizeof(source);
    void *src = &source;
    void *dst = &destination;
    HybmOneSideOpParam oneSide{};
    oneSide.thread = kThread;
    oneSide.channel = kChannel;
    oneSide.list_num = 1U;
    oneSide.dst_buf_addr_list = &dst;
    oneSide.src_buf_addr_list = &src;
    oneSide.len_list = &length;

    EXPECT_EQ(HybmBatchWrite(&oneSide), BM_OK);
    EXPECT_EQ(destination, source);
    EXPECT_EQ(singleWriteCount, 1U);
}

TEST_F(HybmBatchTransferTest, OneSideReadEntryKeepsOriginalTransferPath)
{
    batchResult = BM_NOT_SUPPORTED;
    uint64_t length = sizeof(source);
    void *src = &source;
    void *dst = &destination;
    HybmOneSideOpParam oneSide{};
    oneSide.thread = kThread;
    oneSide.channel = kChannel;
    oneSide.list_num = 1U;
    oneSide.dst_buf_addr_list = &dst;
    oneSide.src_buf_addr_list = &src;
    oneSide.len_list = &length;

    EXPECT_EQ(HybmBatchRead(&oneSide), BM_OK);
    EXPECT_EQ(destination, source);
    EXPECT_EQ(markerReadCount, 1U);
}

TEST_F(HybmBatchTransferTest, OneSideWriteEntryPreservesCompletionMarker)
{
    batchResult = BM_NOT_SUPPORTED;
    uint64_t markerSource = 456U;
    uint64_t length = sizeof(source);
    void *src = &source;
    void *dst = &destination;
    HybmOneSideOpParam oneSide{};
    oneSide.thread = kThread;
    oneSide.channel = kChannel;
    oneSide.list_num = 1U;
    oneSide.dst_buf_addr_list = &dst;
    oneSide.src_buf_addr_list = &src;
    oneSide.len_list = &length;
    oneSide.remote_flag_addr = reinterpret_cast<uint64_t>(&markerSource);
    oneSide.local_flag_addr = reinterpret_cast<uint64_t>(&destination);
    oneSide.flag_size = sizeof(markerSource);

    EXPECT_EQ(HybmBatchWrite(&oneSide), BM_OK);
    EXPECT_EQ(destination, markerSource);
    EXPECT_EQ(markerReadCount, 1U);
}
