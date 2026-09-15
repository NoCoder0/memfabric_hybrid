/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#define private public
#include "hybm_data_op_device_urma.h"
#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "dl_hcomm_api.h"
#include "hybm_define.h"
#include "hybm_rbtree_range_pool.h"
#include "hybm_transport_manager.h"
#undef private

using namespace ock::mf;

namespace {
constexpr uint32_t LOCAL_RANK = 0;
constexpr uint32_t REMOTE_RANK = 1;
constexpr uint64_t TEST_SWAP_ALLOC_SIZE = 4096ULL;

int32_t MockAclrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
{
    (void)kind;
    EXPECT_NE(dst, nullptr);
    EXPECT_NE(src, nullptr);
    EXPECT_GE(destMax, count);
    std::memcpy(dst, src, count);
    return BM_OK;
}

int32_t MockAclrtMemcpyFailed(void *, size_t, const void *, size_t, uint32_t)
{
    return BM_ERROR;
}

int32_t MockAclrtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind, void *)
{
    return MockAclrtMemcpy(dst, destMax, src, count, kind);
}

int32_t MockAclrtMemcpyAsyncFailed(void *, size_t, const void *, size_t, uint32_t, void *)
{
    return BM_ERROR;
}

class TransportManagerMock : public transport::TransportManager {
public:
    Result OpenDevice(const transport::TransportOptions &) override
    {
        return BM_OK;
    }
    Result CloseDevice() override
    {
        return BM_OK;
    }
    Result RegisterMemoryRegion(const transport::TransportMemoryRegion &) override
    {
        registerMemoryRegionCount++;
        return registerMemoryRegionResult;
    }
    Result UnregisterMemoryRegion(uint64_t) override
    {
        unregisterMemoryRegionCount++;
        return unregisterMemoryRegionResult;
    }
    bool QueryHasRegistered(uint64_t addr, uint64_t size) override
    {
        queryHasRegisteredCount++;
        if (queryHasRegistered) {
            return queryHasRegistered(addr, size);
        }
        return queryHasRegisteredResult;
    }
    Result QueryMemoryKey(uint64_t, transport::TransportMemoryKey &) override
    {
        return BM_OK;
    }
    void UpdateMemoryKey(transport::TransportMemoryKey &, void *) override {}
    Result Prepare(const transport::HybmTransPrepareOptions &) override
    {
        return BM_OK;
    }
    Result RemoveRanks(const std::vector<uint32_t> &) override
    {
        return BM_OK;
    }
    Result UpdateRankOptions(const transport::HybmTransPrepareOptions &) override
    {
        return BM_OK;
    }
    const std::string &GetNic() const override
    {
        return nic;
    }
    const transport::TransportPrivateData GetPrivateData() const override
    {
        return {};
    }
    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override
    {
        readRemoteCount++;
        EXPECT_EQ(rankId, REMOTE_RANK);
        std::memcpy(reinterpret_cast<void *>(lAddr), reinterpret_cast<const void *>(rAddr), size);
        return readRemoteResult;
    }
    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override
    {
        writeRemoteCount++;
        EXPECT_EQ(rankId, REMOTE_RANK);
        std::memcpy(reinterpret_cast<void *>(rAddr), reinterpret_cast<const void *>(lAddr), size);
        return writeRemoteResult;
    }
    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override
    {
        readRemoteAsyncCount++;
        return ReadRemote(rankId, lAddr, rAddr, size);
    }
    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override
    {
        writeRemoteAsyncCount++;
        return WriteRemote(rankId, lAddr, rAddr, size);
    }
    Result Synchronize(uint32_t rankId) override
    {
        synchronizeCount++;
        EXPECT_EQ(rankId, REMOTE_RANK);
        if (beforeSynchronize) {
            beforeSynchronize();
        }
        return synchronizeResult;
    }
    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &desc) override
    {
        writeRemoteBatchAsyncCount++;
        EXPECT_EQ(rankId, REMOTE_RANK);
        (void)desc;
        return BM_NOT_SUPPORTED;
    }
    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &desc) override
    {
        readRemoteBatchAsyncCount++;
        EXPECT_EQ(rankId, REMOTE_RANK);
        (void)desc;
        return BM_NOT_SUPPORTED;
    }

    Result TransferRemoteBatchAsync(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                    const transport::RankGroupMap &groupMap, std::vector<uint32_t> &localIndices,
                                    transport::RankGroupMap &unregisteredGroups,
                                    std::set<uint32_t> &batchRanks) override
    {
        (void)direction;
        multiRankBatchSizes.push_back(params.batchSize);
        multiRankGroupMaps.push_back(groupMap);
        for (const auto &[p2pInfo, indices] : groupMap) {
            for (uint32_t index : indices) {
                if (index >= params.batchSize) {
                    return BM_INVALID_PARAM;
                }
            }
        }
        bool hasRemote = false;
        std::set<uint32_t> currentBatchRanks;
        for (const auto &[p2pInfo, indices] : groupMap) {
            if (p2pInfo.first == LOCAL_RANK && p2pInfo.second == LOCAL_RANK) {
                localIndices.insert(localIndices.end(), indices.begin(), indices.end());
                continue;
            }
            const bool isRead = p2pInfo.second == LOCAL_RANK;
            const uint32_t remoteRank = isRead ? p2pInfo.first : p2pInfo.second;
            for (uint32_t index : indices) {
                void *local = isRead ? params.destinations[index] : params.sources[index];
                if (!QueryHasRegistered(reinterpret_cast<uint64_t>(local), params.dataSizes[index])) {
                    unregisteredGroups[p2pInfo].push_back(index);
                    continue;
                }
                hasRemote = true;
                std::memcpy(params.destinations[index], params.sources[index], params.dataSizes[index]);
                currentBatchRanks.insert(remoteRank);
            }
        }
        if (!hasRemote) {
            return BM_OK;
        }
        multiRankSubmitCount++;
        if (beforeBatchSubmit) {
            beforeBatchSubmit();
        }
        if (failAfterBatchSubmit || multiRankSubmitCount == failBatchSubmitAt) {
            return BM_ERROR;
        }
        batchRanks.insert(currentBatchRanks.begin(), currentBatchRanks.end());
        return BM_OK;
    }

    std::function<void()> beforeBatchSubmit;
    std::function<void()> beforeSynchronize;
    std::function<bool(uint64_t, uint64_t)> queryHasRegistered;
    bool failAfterBatchSubmit{false};
    uint64_t failBatchSubmitAt{UINT64_MAX};
    uint64_t multiRankSubmitCount{0};
    std::vector<uint32_t> multiRankBatchSizes;
    std::vector<transport::RankGroupMap> multiRankGroupMaps;
    std::string nic{"eth0"};
    bool queryHasRegisteredResult{true};
    uint64_t registerMemoryRegionCount{0};
    uint64_t unregisterMemoryRegionCount{0};
    uint64_t queryHasRegisteredCount{0};
    uint64_t readRemoteCount{0};
    uint64_t writeRemoteCount{0};
    uint64_t readRemoteAsyncCount{0};
    uint64_t writeRemoteAsyncCount{0};
    uint64_t readRemoteBatchAsyncCount{0};
    uint64_t writeRemoteBatchAsyncCount{0};
    uint64_t synchronizeCount{0};
    Result readRemoteResult{BM_OK};
    Result writeRemoteResult{BM_OK};
    Result registerMemoryRegionResult{BM_OK};
    Result unregisterMemoryRegionResult{BM_OK};
    Result synchronizeResult{BM_OK};
};

class TestDataOpDeviceURMA : public DataOpDeviceURMA {
public:
    TestDataOpDeviceURMA(uint32_t rankId, std::shared_ptr<transport::TransportManager> tm)
        : DataOpDeviceURMA(rankId, std::move(tm))
    {}

    ~TestDataOpDeviceURMA() override
    {
        if (allocatedSwapBase_ != nullptr) {
            free(allocatedSwapBase_);
            allocatedSwapBase_ = nullptr;
        }
    }

    Result AllocSwapMemory() override
    {
        if (allocatedSwapBase_ == nullptr) {
            allocatedSwapBase_ = malloc(TEST_SWAP_ALLOC_SIZE);
        }
        urmaSwapBaseAddr_ = allocatedSwapBase_;
        return urmaSwapBaseAddr_ == nullptr ? BM_MALLOC_FAILED : BM_OK;
    }

    Result InitializeWithSwap(uint64_t swapSizeBytes)
    {
        if (inited_) {
            return BM_OK;
        }
        urmaSwapSpaceSize_ = swapSizeBytes;
        if (urmaSwapSpaceSize_ == 0) {
            inited_ = true;
            return BM_OK;
        }
        auto ret = AllocSwapMemory();
        if (ret != BM_OK) {
            return ret;
        }
        transport::TransportMemoryRegion input;
        input.addr = reinterpret_cast<uint64_t>(urmaSwapBaseAddr_);
        input.size = urmaSwapSpaceSize_;
        input.flags = transport::REG_MR_FLAG_HBM;
        if (transportManager_ != nullptr) {
            ret = transportManager_->RegisterMemoryRegion(input);
            if (ret != BM_OK) {
                FreeSwapMemory();
                return BM_MALLOC_FAILED;
            }
        }
        urmaSwapMemoryAllocator_ =
            std::make_shared<RbtreeRangePool>(static_cast<uint8_t *>(urmaSwapBaseAddr_), urmaSwapSpaceSize_);
        inited_ = true;
        return BM_OK;
    }

private:
    void *allocatedSwapBase_{nullptr};
};

struct DlAclApiCopyFnGuard {
    aclrtMemcpyFunc oldMemcpy{DlAclApi::pAclrtMemcpy};
    aclrtMemcpyAsyncFunc oldMemcpyAsync{DlAclApi::pAclrtMemcpyAsync};
    aclrtSynchronizeStreamFunc oldSynchronizeStream{DlAclApi::pAclrtSynchronizeStream};
    aclrtMemcpyBatchFunc oldMemcpyBatch{DlAclApi::pAclrtMemcpyBatch};
    aclrtFreeHostFunc oldFreeHost{DlAclApi::pAclrtFreeHost};

    ~DlAclApiCopyFnGuard()
    {
        DlAclApi::pAclrtMemcpy = oldMemcpy;
        DlAclApi::pAclrtMemcpyAsync = oldMemcpyAsync;
        DlAclApi::pAclrtSynchronizeStream = oldSynchronizeStream;
        DlAclApi::pAclrtMemcpyBatch = oldMemcpyBatch;
        DlAclApi::pAclrtFreeHost = oldFreeHost;
    }
};

int32_t MockAclrtSynchronizeStream(void *)
{
    return BM_OK;
}

int32_t MockAclrtMemcpyBatch(void **dsts, size_t *destMax, void **srcs, size_t *sizes, size_t numBatches,
                             aclrtMemcpyBatchAttr *, size_t *, size_t, size_t *)
{
    for (size_t i = 0; i < numBatches; ++i) {
        EXPECT_GE(destMax[i], sizes[i]);
        std::memcpy(dsts[i], srcs[i], sizes[i]);
    }
    return BM_OK;
}

int32_t MockAclrtFreeHost(void *)
{
    return BM_OK;
}

void ExpectLocalDataCopy(DataOpDeviceURMA &dataOp, hybm_data_copy_direction direction)
{
    char src[16] = "local_copy";
    char dst[16] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = LOCAL_RANK;

    EXPECT_EQ(dataOp.DataCopy(params, direction, options), BM_OK);
    EXPECT_EQ(std::memcmp(dst, src, sizeof(src)), 0);
}

void ExpectLocalBatchCopy(DataOpDeviceURMA &dataOp, hybm_data_copy_direction direction)
{
    char src0[8] = "aa";
    char src1[8] = "bb";
    char dst0[8] = {};
    char dst1[8] = {};
    void *sources[2] = {src0, src1};
    void *destinations[2] = {dst0, dst1};
    uint64_t sizes[2] = {sizeof(src0), sizeof(src1)};
    hybm_batch_copy_params params{sources, destinations, sizes, 2U};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = LOCAL_RANK;
    options.groupMap[{LOCAL_RANK, LOCAL_RANK}] = {0U, 1U};

    EXPECT_EQ(dataOp.BatchDataCopy(params, direction, options), BM_OK);
    EXPECT_EQ(std::memcmp(dst0, src0, sizeof(src0)), 0);
    EXPECT_EQ(std::memcmp(dst1, src1, sizeof(src1)), 0);
}

struct MixedRankBatch {
    static constexpr uint32_t LOCAL_INDEX = 0;
    static constexpr uint32_t REMOTE_INDEX = 1;
    static constexpr uint32_t BATCH_SIZE = 2;
    static constexpr size_t COPY_SIZE = 8;
    char localSource[COPY_SIZE] = "local";
    char remoteSource[COPY_SIZE] = "remote";
    char localDestination[COPY_SIZE] = {};
    char remoteDestination[COPY_SIZE] = {};
    void *sources[BATCH_SIZE] = {localSource, remoteSource};
    void *destinations[BATCH_SIZE] = {localDestination, remoteDestination};
    uint64_t sizes[BATCH_SIZE] = {COPY_SIZE, COPY_SIZE};
    hybm_batch_copy_params params{sources, destinations, sizes, BATCH_SIZE};
    ExtOptions options{};

    explicit MixedRankBatch(bool isRead = false)
    {
        options.groupMap[{LOCAL_RANK, LOCAL_RANK}] = {LOCAL_INDEX};
        const auto remotePair =
            isRead ? std::make_pair(REMOTE_RANK, LOCAL_RANK) : std::make_pair(LOCAL_RANK, REMOTE_RANK);
        options.groupMap[remotePair] = {REMOTE_INDEX};
    }
};
} // namespace

class HybmDataOpDeviceUrmaTest : public testing::Test {
public:
    void SetUp() override
    {
        tm = std::make_shared<TransportManagerMock>();
        dataOp = std::make_shared<TestDataOpDeviceURMA>(LOCAL_RANK, tm);
        DlAclApi::pAclrtMemcpy = MockAclrtMemcpy;
        DlAclApi::pAclrtMemcpyAsync = MockAclrtMemcpyAsync;
        DlAclApi::pAclrtSynchronizeStream = MockAclrtSynchronizeStream;
        DlAclApi::pAclrtMemcpyBatch = MockAclrtMemcpyBatch;
        DlAclApi::pAclrtFreeHost = MockAclrtFreeHost;
    }

protected:
    DlAclApiCopyFnGuard guard;
    std::shared_ptr<TransportManagerMock> tm;
    std::shared_ptr<TestDataOpDeviceURMA> dataOp;
};

TEST_F(HybmDataOpDeviceUrmaTest, InitializeIsIdempotentAndUnInitializeClearsState)
{
    EXPECT_EQ(dataOp->InitializeWithSwap(TEST_SWAP_ALLOC_SIZE), BM_OK);
    EXPECT_TRUE(dataOp->inited_);
    void *swapBase = dataOp->urmaSwapBaseAddr_;
    EXPECT_EQ(tm->registerMemoryRegionCount, 1U);

    EXPECT_EQ(dataOp->InitializeWithSwap(TEST_SWAP_ALLOC_SIZE), BM_OK);
    EXPECT_EQ(dataOp->urmaSwapBaseAddr_, swapBase);
    EXPECT_EQ(tm->registerMemoryRegionCount, 1U);

    dataOp->UnInitialize();
    EXPECT_FALSE(dataOp->inited_);
    EXPECT_EQ(tm->unregisterMemoryRegionCount, 1U);
}

TEST_F(HybmDataOpDeviceUrmaTest, InitializeReturnsMallocFailedWhenSwapRegistrationFails)
{
    tm->registerMemoryRegionResult = BM_ERROR;

    EXPECT_EQ(dataOp->InitializeWithSwap(TEST_SWAP_ALLOC_SIZE), BM_MALLOC_FAILED);
    EXPECT_FALSE(dataOp->inited_);
    EXPECT_EQ(dataOp->urmaSwapBaseAddr_, nullptr);
    EXPECT_EQ(tm->registerMemoryRegionCount, 1U);
}

TEST_F(HybmDataOpDeviceUrmaTest, DataCopyLocalDirectionsUseAclMemcpy)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    char src[16] = "hybm_urma_ut";
    char dst[16] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = LOCAL_RANK;

    EXPECT_EQ(dataOp->DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_OK);
    EXPECT_STREQ(dst, src);
    std::memset(dst, 0, sizeof(dst));
    EXPECT_EQ(dataOp->DataCopy(params, HYBM_GLOBAL_HOST_TO_LOCAL_HOST, options), BM_OK);
    EXPECT_STREQ(dst, src);
    EXPECT_EQ(dataOp->DataCopy(params, HYBM_DATA_COPY_DIRECTION_AUTO, options), BM_INVALID_PARAM);
}

TEST_F(HybmDataOpDeviceUrmaTest, DataCopyAllLocalDirectionsSucceed)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    const hybm_data_copy_direction directions[] = {
        HYBM_LOCAL_HOST_TO_GLOBAL_HOST,      HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE, HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST,
        HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE,  HYBM_GLOBAL_HOST_TO_GLOBAL_HOST,  HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE,
        HYBM_GLOBAL_HOST_TO_LOCAL_HOST,      HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST,
        HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST, HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE,
    };

    for (auto direction : directions) {
        ExpectLocalDataCopy(*dataOp, direction);
    }
}

TEST_F(HybmDataOpDeviceUrmaTest, DataCopyRemoteWriteAndReadUseTransportManager)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    char src[16] = "remote_write";
    char dst[16] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = REMOTE_RANK;

    EXPECT_EQ(dataOp->DataCopy(params, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST, options), BM_OK);
    EXPECT_EQ(tm->writeRemoteCount, 1U);
    EXPECT_STREQ(dst, src);

    std::memset(src, 0, sizeof(src));
    std::strcpy(dst, "remote_read");
    options.srcRankId = REMOTE_RANK;
    options.destRankId = LOCAL_RANK;
    EXPECT_EQ(dataOp->DataCopy(params, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST, options), BM_OK);
    EXPECT_EQ(tm->readRemoteCount, 1U);
    EXPECT_STREQ(src, dst);
}

TEST_F(HybmDataOpDeviceUrmaTest, DataCopySafePutUsesSwapWhenLocalSourceIsNotRegistered)
{
    EXPECT_EQ(dataOp->InitializeWithSwap(TEST_SWAP_ALLOC_SIZE), BM_OK);
    tm->queryHasRegisteredResult = false;
    char src[16] = "safe_put";
    char dst[16] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = REMOTE_RANK;

    EXPECT_EQ(dataOp->DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_OK);
    EXPECT_GE(tm->queryHasRegisteredCount, 1U);
    EXPECT_EQ(tm->writeRemoteCount, 1U);
    EXPECT_STREQ(dst, src);
}

TEST_F(HybmDataOpDeviceUrmaTest, DataCopySafeGetUsesSwapWhenLocalDestinationIsNotRegistered)
{
    EXPECT_EQ(dataOp->InitializeWithSwap(TEST_SWAP_ALLOC_SIZE), BM_OK);
    tm->queryHasRegisteredResult = false;
    char src[16] = "safe_get";
    char dst[16] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = REMOTE_RANK;
    options.destRankId = LOCAL_RANK;

    EXPECT_EQ(dataOp->DataCopy(params, HYBM_GLOBAL_HOST_TO_LOCAL_HOST, options), BM_OK);
    EXPECT_GE(tm->queryHasRegisteredCount, 1U);
    EXPECT_EQ(tm->readRemoteCount, 1U);
    EXPECT_STREQ(dst, src);
}

TEST_F(HybmDataOpDeviceUrmaTest, DataCopyRejectsRemoteToRemoteWhenLocalRankIsAbsent)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    char src[4] = {};
    char dst[4] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = 2UL;
    options.destRankId = 3UL;

    EXPECT_EQ(dataOp->DataCopy(params, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST, options), BM_INVALID_PARAM);
}

TEST_F(HybmDataOpDeviceUrmaTest, DataCopyAsyncUnsupportedAndWaitSucceeds)
{
    hybm_copy_params params{};
    ExtOptions options{};
    EXPECT_EQ(dataOp->DataCopyAsync(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_ERROR);
    EXPECT_EQ(dataOp->Wait(0), BM_OK);
}

TEST_F(HybmDataOpDeviceUrmaTest, DataCopyPropagatesAclMemcpyFailure)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    DlAclApi::pAclrtMemcpy = MockAclrtMemcpyFailed;
    char src[4] = {1, 2, 3, 4};
    char dst[4] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = LOCAL_RANK;

    EXPECT_EQ(dataOp->DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_DL_FUNCTION_FAILED);
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankCopiesLocalBetweenRemoteSubmissionAndSynchronization)
{
    const bool readDirections[] = {false, true};
    for (bool isRead : readDirections) {
        MixedRankBatch batch(isRead);
        tm->beforeBatchSubmit = [&batch]() { EXPECT_EQ(batch.localDestination[0], '\0'); };
        tm->beforeSynchronize = [&batch]() {
            EXPECT_EQ(std::memcmp(batch.localDestination, batch.localSource, MixedRankBatch::COPY_SIZE), 0);
        };
        const auto direction = isRead ? HYBM_GLOBAL_HOST_TO_LOCAL_HOST : HYBM_LOCAL_HOST_TO_GLOBAL_HOST;
        EXPECT_EQ(dataOp->BatchDataCopy(batch.params, direction, batch.options), BM_OK);
        EXPECT_EQ(std::memcmp(batch.remoteDestination, batch.remoteSource, MixedRankBatch::COPY_SIZE), 0);
    }
    EXPECT_EQ(tm->multiRankSubmitCount, 2U);
    EXPECT_EQ(tm->readRemoteBatchAsyncCount, 0U);
    EXPECT_EQ(tm->synchronizeCount, 2U);
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankLocalFailureDrainsRemoteAndPreservesCopyError)
{
    MixedRankBatch batch;
    DlAclApi::pAclrtMemcpy = MockAclrtMemcpyFailed;
    tm->synchronizeResult = BM_ERROR;

    EXPECT_EQ(dataOp->BatchDataCopy(batch.params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, batch.options),
              BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(tm->multiRankSubmitCount, 1U);
    EXPECT_EQ(tm->synchronizeCount, 1U);
    EXPECT_EQ(batch.localDestination[0], '\0');
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankSubmissionFailureSkipsSynchronizationAndLocalCopy)
{
    MixedRankBatch batch;
    tm->failAfterBatchSubmit = true;
    tm->beforeSynchronize = [&batch]() { EXPECT_EQ(batch.localDestination[0], '\0'); };

    EXPECT_EQ(dataOp->BatchDataCopy(batch.params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, batch.options), BM_ERROR);
    EXPECT_EQ(tm->multiRankSubmitCount, 1U);
    EXPECT_EQ(tm->synchronizeCount, 0U);
    EXPECT_EQ(batch.localDestination[0], '\0');
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankLargeBatchSynchronizesAndStopsSlicesIndependently)
{
    const uint32_t batchSize = HCOMM_BATCH_TRANSFER_MAX_DESC_NUM * 2U + 1U;
    std::vector<char> sources(batchSize, 'x');
    std::vector<char> destinations(batchSize, '\0');
    std::vector<void *> sourceAddrs(batchSize);
    std::vector<void *> destinationAddrs(batchSize);
    std::vector<uint64_t> dataSizes(batchSize, sizeof(char));
    std::vector<uint32_t> indices(batchSize);
    for (uint32_t index = 0; index < batchSize; ++index) {
        sourceAddrs[index] = &sources[index];
        destinationAddrs[index] = &destinations[index];
        indices[index] = index;
    }
    hybm_batch_copy_params params{sourceAddrs.data(), destinationAddrs.data(), dataSizes.data(), batchSize};
    ExtOptions options{};
    options.groupMap[{LOCAL_RANK, REMOTE_RANK}] = indices;

    ASSERT_EQ(dataOp->BatchDataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_OK);
    EXPECT_EQ(tm->multiRankBatchSizes,
              (std::vector<uint32_t>{HCOMM_BATCH_TRANSFER_MAX_DESC_NUM, HCOMM_BATCH_TRANSFER_MAX_DESC_NUM, 1U}));
    ASSERT_EQ(tm->multiRankGroupMaps.size(), 3U);
    const auto &firstSliceIndices = tm->multiRankGroupMaps[0].at({LOCAL_RANK, REMOTE_RANK});
    const auto &secondSliceIndices = tm->multiRankGroupMaps[1].at({LOCAL_RANK, REMOTE_RANK});
    const auto &lastSliceIndices = tm->multiRankGroupMaps[2].at({LOCAL_RANK, REMOTE_RANK});
    ASSERT_EQ(firstSliceIndices.size(), HCOMM_BATCH_TRANSFER_MAX_DESC_NUM);
    ASSERT_EQ(secondSliceIndices.size(), HCOMM_BATCH_TRANSFER_MAX_DESC_NUM);
    EXPECT_EQ(firstSliceIndices.front(), 0U);
    EXPECT_EQ(firstSliceIndices.back(), HCOMM_BATCH_TRANSFER_MAX_DESC_NUM - 1U);
    EXPECT_EQ(secondSliceIndices.front(), 0U);
    EXPECT_EQ(secondSliceIndices.back(), HCOMM_BATCH_TRANSFER_MAX_DESC_NUM - 1U);
    EXPECT_EQ(lastSliceIndices, (std::vector<uint32_t>{0U}));
    EXPECT_EQ(tm->multiRankSubmitCount, 3U);
    EXPECT_EQ(tm->synchronizeCount, 3U);
    EXPECT_EQ(destinations, sources);

    tm->multiRankSubmitCount = 0U;
    tm->synchronizeCount = 0U;
    tm->multiRankBatchSizes.clear();
    tm->multiRankGroupMaps.clear();
    tm->failBatchSubmitAt = 2U;
    std::memset(destinations.data(), 0, destinations.size());
    EXPECT_EQ(dataOp->BatchDataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_ERROR);
    EXPECT_EQ(tm->multiRankSubmitCount, 2U);
    EXPECT_EQ(tm->synchronizeCount, 1U);
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankSynchronizationFailureIsReturnedAfterLocalCopy)
{
    MixedRankBatch batch;
    tm->synchronizeResult = BM_ERROR;

    EXPECT_EQ(dataOp->BatchDataCopy(batch.params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, batch.options), BM_ERROR);
    EXPECT_EQ(tm->synchronizeCount, 1U);
    EXPECT_EQ(std::memcmp(batch.localDestination, batch.localSource, MixedRankBatch::COPY_SIZE), 0);
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankLocalOnlyDoesNotSubmitRemote)
{
    MixedRankBatch batch;
    batch.options.groupMap.erase({LOCAL_RANK, REMOTE_RANK});

    EXPECT_EQ(dataOp->BatchDataCopy(batch.params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, batch.options), BM_OK);
    EXPECT_EQ(tm->multiRankSubmitCount, 0U);
    EXPECT_EQ(tm->synchronizeCount, 0U);
    EXPECT_EQ(std::memcmp(batch.localDestination, batch.localSource, MixedRankBatch::COPY_SIZE), 0);
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankRemoteOnlySkipsLocalCopy)
{
    MixedRankBatch batch;
    batch.options.groupMap.erase({LOCAL_RANK, LOCAL_RANK});
    DlAclApi::pAclrtMemcpy = MockAclrtMemcpyFailed;

    EXPECT_EQ(dataOp->BatchDataCopy(batch.params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, batch.options), BM_OK);
    EXPECT_EQ(tm->multiRankSubmitCount, 1U);
    EXPECT_EQ(tm->synchronizeCount, 1U);
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankInvalidLocalIndexPreventsRemoteSubmission)
{
    MixedRankBatch batch;
    batch.options.groupMap[{LOCAL_RANK, LOCAL_RANK}] = {MixedRankBatch::BATCH_SIZE};

    EXPECT_EQ(dataOp->BatchDataCopy(batch.params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, batch.options), BM_INVALID_PARAM);
    EXPECT_EQ(tm->multiRankSubmitCount, 0U);
    EXPECT_EQ(tm->synchronizeCount, 0U);
    EXPECT_EQ(batch.localDestination[0], '\0');
}

TEST_F(HybmDataOpDeviceUrmaTest, BatchDataCopyAllLocalDirectionsSucceed)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    const hybm_data_copy_direction directions[] = {
        HYBM_LOCAL_HOST_TO_GLOBAL_HOST,      HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE, HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST,
        HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE,  HYBM_GLOBAL_HOST_TO_GLOBAL_HOST,  HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE,
        HYBM_GLOBAL_HOST_TO_LOCAL_HOST,      HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST,
        HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST, HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE,
    };

    for (auto direction : directions) {
        ExpectLocalBatchCopy(*dataOp, direction);
    }
}

TEST_F(HybmDataOpDeviceUrmaTest, BatchDataCopyDefaultUsesDataCopyWhenLocalMemoryIsNotRegistered)
{
    EXPECT_EQ(dataOp->InitializeWithSwap(TEST_SWAP_ALLOC_SIZE), BM_OK);
    tm->queryHasRegisteredResult = false;
    char src0[8] = "s0";
    char src1[8] = "s1";
    char dst0[8] = {};
    char dst1[8] = {};
    void *sources[2] = {src0, src1};
    void *destinations[2] = {dst0, dst1};
    uint64_t sizes[2] = {sizeof(src0), sizeof(src1)};
    hybm_batch_copy_params params{sources, destinations, sizes, 2U};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = REMOTE_RANK;

    EXPECT_EQ(dataOp->BatchDataCopyDefault(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_OK);
    EXPECT_EQ(tm->writeRemoteBatchAsyncCount, 0U);
    EXPECT_EQ(tm->writeRemoteCount, 2U);
    EXPECT_STREQ(dst0, src0);
    EXPECT_STREQ(dst1, src1);
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankMixesRegisteredBatchAndUnregisteredFallback)
{
    constexpr uint32_t kBatchSize = 2U;
    ASSERT_EQ(dataOp->InitializeWithSwap(TEST_SWAP_ALLOC_SIZE), BM_OK);
    char registeredSource[8] = "batch";
    char fallbackSource[8] = "single";
    char registeredDestination[8] = {};
    char fallbackDestination[8] = {};
    void *sources[kBatchSize] = {registeredSource, fallbackSource};
    void *destinations[kBatchSize] = {registeredDestination, fallbackDestination};
    uint64_t sizes[kBatchSize] = {sizeof(registeredSource), sizeof(fallbackSource)};
    hybm_batch_copy_params params{sources, destinations, sizes, kBatchSize};
    ExtOptions options{};
    options.groupMap[{LOCAL_RANK, REMOTE_RANK}] = {0U, 1U};
    const uint64_t registeredAddr = reinterpret_cast<uint64_t>(registeredSource);
    tm->queryHasRegistered = [registeredAddr](uint64_t addr, uint64_t) { return addr == registeredAddr; };

    EXPECT_EQ(dataOp->BatchDataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_OK);
    EXPECT_EQ(tm->multiRankSubmitCount, 1U);
    EXPECT_EQ(tm->writeRemoteCount, 1U);
    EXPECT_EQ(tm->writeRemoteBatchAsyncCount, 0U);
    EXPECT_EQ(tm->synchronizeCount, 1U);
    EXPECT_STREQ(registeredDestination, registeredSource);
    EXPECT_STREQ(fallbackDestination, fallbackSource);
}

TEST_F(HybmDataOpDeviceUrmaTest, BatchDataCopyLocalAsyncFailureReturnsDlFailure)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    DlAclApi::pAclrtMemcpyAsync = MockAclrtMemcpyAsyncFailed;
    char src[8] = "async";
    char dst[8] = {};
    void *sources[1] = {src};
    void *destinations[1] = {dst};
    uint64_t sizes[1] = {sizeof(src)};
    hybm_batch_copy_params params{sources, destinations, sizes, 1U};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = LOCAL_RANK;
    options.groupMap[{LOCAL_RANK, LOCAL_RANK}] = {0U};

    EXPECT_EQ(dataOp->BatchDataCopy(params, HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE, options), BM_DL_FUNCTION_FAILED);
}

TEST_F(HybmDataOpDeviceUrmaTest, BatchDataCopyRejectsUnsupportedDirection)
{
    char src[4] = {};
    char dst[4] = {};
    void *sources[1] = {src};
    void *destinations[1] = {dst};
    uint64_t sizes[1] = {sizeof(src)};
    hybm_batch_copy_params params{sources, destinations, sizes, 1U};
    ExtOptions options{};

    EXPECT_EQ(dataOp->BatchDataCopy(params, HYBM_DATA_COPY_DIRECTION_AUTO, options), BM_INVALID_PARAM);
}

TEST_F(HybmDataOpDeviceUrmaTest, BatchDataCopyRequiresRankGroups)
{
    char src[4] = {};
    char dst[4] = {};
    void *sources[1] = {src};
    void *destinations[1] = {dst};
    uint64_t sizes[1] = {sizeof(src)};
    hybm_batch_copy_params params{sources, destinations, sizes, 1U};
    ExtOptions options{};

    EXPECT_EQ(dataOp->BatchDataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_INVALID_PARAM);
    EXPECT_EQ(tm->multiRankSubmitCount, 0U);
}

TEST_F(HybmDataOpDeviceUrmaTest, InitializeSkipsSwapAllocationWhenSwapSpaceSizeIsZero)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    EXPECT_TRUE(dataOp->inited_);
    EXPECT_EQ(dataOp->urmaSwapSpaceSize_, 0ULL);
    EXPECT_EQ(dataOp->urmaSwapBaseAddr_, nullptr);
    EXPECT_EQ(dataOp->urmaSwapMemoryAllocator_, nullptr);
    EXPECT_EQ(tm->registerMemoryRegionCount, 0U);
}

TEST_F(HybmDataOpDeviceUrmaTest, InitializeIsIdempotentWhenSwapSpaceSizeIsZero)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    EXPECT_TRUE(dataOp->inited_);
    EXPECT_EQ(tm->registerMemoryRegionCount, 0U);

    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    EXPECT_TRUE(dataOp->inited_);
    EXPECT_EQ(tm->registerMemoryRegionCount, 0U);

    dataOp->UnInitialize();
    EXPECT_FALSE(dataOp->inited_);
    EXPECT_EQ(tm->unregisterMemoryRegionCount, 0U);
}

TEST_F(HybmDataOpDeviceUrmaTest, SafePutReturnsErrorWhenSwapAllocatorIsNotInitialized)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    tm->queryHasRegisteredResult = false;
    char src[16] = "no_swap_put";
    char dst[16] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = REMOTE_RANK;

    EXPECT_EQ(dataOp->DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_ERROR);
    EXPECT_GE(tm->queryHasRegisteredCount, 1U);
    EXPECT_EQ(tm->writeRemoteCount, 0U);
}

TEST_F(HybmDataOpDeviceUrmaTest, SafeGetReturnsErrorWhenSwapAllocatorIsNotInitialized)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    tm->queryHasRegisteredResult = false;
    char src[16] = "no_swap_get";
    char dst[16] = {};
    hybm_copy_params params{src, dst, sizeof(src)};
    ExtOptions options{};
    options.srcRankId = REMOTE_RANK;
    options.destRankId = LOCAL_RANK;

    EXPECT_EQ(dataOp->DataCopy(params, HYBM_GLOBAL_HOST_TO_LOCAL_HOST, options), BM_ERROR);
    EXPECT_GE(tm->queryHasRegisteredCount, 1U);
    EXPECT_EQ(tm->readRemoteCount, 0U);
}

TEST_F(HybmDataOpDeviceUrmaTest, BatchDataCopyDefaultFailsWhenSwapAllocatorIsNotInitialized)
{
    EXPECT_EQ(dataOp->Initialize(), BM_OK);
    tm->queryHasRegisteredResult = false;
    char src[8] = "s0";
    char dst[8] = {};
    void *sources[1] = {src};
    void *destinations[1] = {dst};
    uint64_t sizes[1] = {sizeof(src)};
    hybm_batch_copy_params params{sources, destinations, sizes, 1U};
    ExtOptions options{};
    options.srcRankId = LOCAL_RANK;
    options.destRankId = REMOTE_RANK;

    EXPECT_EQ(dataOp->BatchDataCopyDefault(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_ERROR);
    EXPECT_EQ(tm->writeRemoteCount, 0U);
}

TEST_F(HybmDataOpDeviceUrmaTest, MultiRankMixedReadWriteUsesOneSubmission)
{
    constexpr size_t kCopySize = 8U;
    constexpr uint32_t kBatchSize = 2U;
    char localSource[kCopySize] = "write";
    char remoteSource[kCopySize] = "read";
    char localDestination[kCopySize] = {};
    char remoteDestination[kCopySize] = {};
    void *sources[kBatchSize] = {localSource, remoteSource};
    void *destinations[kBatchSize] = {remoteDestination, localDestination};
    uint64_t sizes[kBatchSize] = {kCopySize, kCopySize};
    hybm_batch_copy_params params{sources, destinations, sizes, kBatchSize};
    ExtOptions options{};
    options.groupMap[{LOCAL_RANK, REMOTE_RANK}] = {0U};
    options.groupMap[{REMOTE_RANK, LOCAL_RANK}] = {1U};

    ASSERT_EQ(dataOp->BatchDataCopy(params, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST, options), BM_OK);
    EXPECT_EQ(tm->multiRankSubmitCount, 1U);
    EXPECT_EQ(tm->writeRemoteBatchAsyncCount, 0U);
    EXPECT_EQ(tm->readRemoteBatchAsyncCount, 0U);
    EXPECT_EQ(tm->synchronizeCount, 1U);
    EXPECT_EQ(std::memcmp(remoteDestination, localSource, kCopySize), 0);
    EXPECT_EQ(std::memcmp(localDestination, remoteSource, kCopySize), 0);
}
