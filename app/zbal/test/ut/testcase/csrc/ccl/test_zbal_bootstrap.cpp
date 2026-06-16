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

#include "zbal_test_constants.h"
#include "test_zbal_def.h"
#include "zbal_bootstrap_types.h"
#include "zbal_init_state.h"
#include "zbal_bootstrap.h"
#include "zbal_sma.h"

#define private public
#include "zbal_bootstrap_default.h"
#undef private

using namespace zbal;
using namespace zbal::bootstrap;

class MockMemBootstrap : public MemBootstrap {
public:
    explicit MockMemBootstrap(const MemBootstrapOptions &options) : MemBootstrap(options) {}

    ZResult Initialize() noexcept override
    {
        initCalled_ = true;
        return initResult_;
    }

    void UnInitialize() noexcept override
    {
        uninitCalled_ = true;
    }

    ZResult AcquireCommGroupId(uint32_t max, uint32_t &uniqueId) noexcept override
    {
        acqCalled_ = true;
        acqMax_ = max;
        if (acqResult_ == Z_OK) {
            uniqueId = acqReturnId_;
        }
        return acqResult_;
    }

    ZResult ReleaseCommGroupId(uint32_t uniqueId) noexcept override
    {
        relCalled_ = true;
        relId_ = uniqueId;
        return relResult_;
    }

    ZResult SubGroupAllGather(const std::string &key, uint32_t rankSize, uint32_t rankId, const char *sendBuf,
                              uint32_t sendSize, char *recvBuf, uint32_t recvSize) noexcept override
    {
        agCalled_ = true;
        agKey_ = key;
        agRankSize_ = rankSize;
        agRankId_ = rankId;
        return agResult_;
    }

    ZResult SubGroupBarrier(const std::string &key, uint32_t rankSize, uint32_t rankId) noexcept override
    {
        barrierCalled_ = true;
        barrierKey_ = key;
        barrierRankSize_ = rankSize;
        barrierRankId_ = rankId;
        return barrierResult_;
    }

    ZResult SetLoggerLevel(int level) noexcept override
    {
        logCalled_ = true;
        logLevel_ = level;
        return logResult_;
    }

    void SetOutput(const MemBootstrapOutput &out)
    {
        output_ = out;
    }

    ZResult initResult_ = Z_OK;
    ZResult acqResult_ = Z_OK;
    uint32_t acqReturnId_ = ZBAL_UT_NUM_0;
    ZResult relResult_ = Z_OK;
    ZResult agResult_ = Z_OK;
    ZResult barrierResult_ = Z_OK;
    ZResult logResult_ = Z_OK;

    bool initCalled_ = false;
    bool uninitCalled_ = false;
    bool acqCalled_ = false;
    uint32_t acqMax_ = ZBAL_UT_NUM_0;
    bool relCalled_ = false;
    uint32_t relId_ = ZBAL_UT_NUM_0;
    bool agCalled_ = false;
    std::string agKey_;
    uint32_t agRankSize_ = ZBAL_UT_NUM_0;
    uint32_t agRankId_ = ZBAL_UT_NUM_0;
    bool barrierCalled_ = false;
    std::string barrierKey_;
    uint32_t barrierRankSize_ = ZBAL_UT_NUM_0;
    uint32_t barrierRankId_ = ZBAL_UT_NUM_0;
    bool logCalled_ = false;
    int logLevel_ = ZBAL_UT_NUM_0;
};

class TestableMemBootstrap : public MemBootstrap {
public:
    explicit TestableMemBootstrap(const MemBootstrapOptions &options) : MemBootstrap(options) {}

    using MemBootstrap::VerifyOptions;

    ZResult Initialize() noexcept override
    {
        return Z_OK;
    }
    void UnInitialize() noexcept override {}
    ZResult AcquireCommGroupId(uint32_t, uint32_t &) noexcept override
    {
        return Z_OK;
    }
    ZResult ReleaseCommGroupId(uint32_t) noexcept override
    {
        return Z_OK;
    }
    ZResult SubGroupAllGather(const std::string &, uint32_t, uint32_t, const char *, uint32_t, char *,
                              uint32_t) noexcept override
    {
        return Z_OK;
    }
    ZResult SubGroupBarrier(const std::string &, uint32_t, uint32_t) noexcept override
    {
        return Z_OK;
    }
    ZResult SetLoggerLevel(int) noexcept override
    {
        return Z_OK;
    }
};

class TestZBALBootstrap : public testing::Test {
public:
    void SetUp() override
    {
        Bootstrap::Destroy();
    }

    void TearDown() override
    {
        Bootstrap::Destroy();
    }

    void InitValidOptions(zbal_bootstrap_options_t &options)
    {
        bzero(&options, sizeof(zbal_bootstrap_options_t));
        options.btType = BOOT_BY_MEMFABRIC;
        options.worldSize = ZBAL_UT_NUM_4;
        options.rankId = ZBAL_UT_NUM_0;
        options.deviceId = ZBAL_UT_DEVICE_ID;
        options.deviceMemorySize = ZBAL_UT_SIZE_256MB;
        options.commGroupCap = ZBAL_UT_NUM_16;
        options.commMetaSpaceSize = ZBAL_UT_META_SIZE;
    }

    void InitMemBootstrapOptions(MemBootstrapOptions &options)
    {
        options.boostrapType = MBT_MEMFABRIC;
        options.deviceId = ZBAL_UT_DEVICE_ID;
        options.rankCount = ZBAL_UT_NUM_4;
        options.rankId = ZBAL_UT_NUM_0;
        options.totalMemSize = ZBAL_UT_SIZE_256MB;
        options.ipPort = "127.0.0.1:12345";
    }

    std::shared_ptr<MockMemBootstrap> CreateReadyMock()
    {
        auto mock = std::make_shared<MockMemBootstrap>(MemBootstrapOptions{});
        mock->initResult_ = Z_OK;

        MemBootstrapOutput mockOutput;
        mockOutput.gvaDevice = reinterpret_cast<void *>(0x10000);
        mockOutput.myGvaDevice = reinterpret_cast<void *>(0x20000);
        mockOutput.memorySizeDevice = ZBAL_UT_SIZE_256MB;
        mockOutput.memorySpaceSizeDevice = ZBAL_UT_SIZE_256MB * ZBAL_TEST_NUMBER_TWO;
        mock->SetOutput(mockOutput);

        return mock;
    }

    void InjectMockIntoBootstrap(Bootstrap *bootstrap, MockMemBootstrap *mock)
    {
        bootstrap->memBootstrap_ = mock;
        bootstrap->inited_ = true;

        auto &memOutput = mock->GetOutput();
        bootstrap->output_.deviceGva = memOutput.gvaDevice;
        bootstrap->output_.myDeviceGva = memOutput.myGvaDevice;
        bootstrap->output_.createdDeviceMemorySpaceSize = memOutput.memorySpaceSizeDevice;
        bootstrap->output_.allocatedDeviceMemorySize = memOutput.memorySizeDevice;
        bootstrap->output_.myCommMetaDeviceGva = memOutput.myGvaDevice;
        bootstrap->output_.metaSizeOfDevice = bootstrap->options_.commMetaSpaceSize;
        bootstrap->output_.metaSizeOfDevice =
            bootstrap->output_.metaSizeOfDevice * ZBAL_TEST_SIZE_1KB * bootstrap->options_.commGroupCap;
        bootstrap->output_.mySMAGva = reinterpret_cast<void *>(
            reinterpret_cast<uintptr_t>(bootstrap->output_.myDeviceGva) + bootstrap->output_.metaSizeOfDevice);
        bootstrap->output_.smaSizeOfDevice =
            bootstrap->output_.allocatedDeviceMemorySize - bootstrap->output_.metaSizeOfDevice;
    }
};

TEST_F(TestZBALBootstrap, LifecycleCreateGetDestroy)
{
    EXPECT_TRUE(Bootstrap::Get() == nullptr);

    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    Bootstrap::Destroy();
    Bootstrap::Destroy();
}

TEST_F(TestZBALBootstrap, VerifyOptionsAllBranches)
{
    zbal_bootstrap_options_t options;

    InitValidOptions(options);
    options.btType = BOOT_BY_BUTT;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    InitValidOptions(options);
    options.ipPort[0] = '\0';
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    InitValidOptions(options);
    options.worldSize = ZBAL_RANK_COUNT_MAX_LIMIT + ZBAL_UT_NUM_1;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    InitValidOptions(options);
    options.rankId = options.worldSize;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    InitValidOptions(options);
    options.deviceId = ZBAL_DEVICE_COUNT_MAX_LIMIT;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    InitValidOptions(options);
    options.deviceMemorySize = ZBAL_MEMORY_SIZE_CAP;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    InitValidOptions(options);
    options.commGroupCap = COMM_GROUP_COUNT_CAP_MAX;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    InitValidOptions(options);
    options.commGroupCap = ZBAL_UT_NUM_16;
    options.commMetaSpaceSize = static_cast<uint16_t>(ZBAL_UT_SIZE_256MB / ZBAL_TEST_SIZE_1KB);
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);

    InitValidOptions(options);
    options.commMetaSpaceSize = static_cast<uint16_t>(ZBAL_OPERATE_PARAM_SIZE / ZBAL_TEST_SIZE_1KB);
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, CreateFailsWhenNoMemFabric)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);

    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
    EXPECT_TRUE(Bootstrap::Get() == nullptr);
}

TEST_F(TestZBALBootstrap, DelegationWhenNotBootstrapped)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    uint32_t uniqueId = ZBAL_UT_NUM_0;
    EXPECT_TRUE(bootstrap->AcquireCommGroupId(ZBAL_TEST_NUMBER_TEN, uniqueId) == Z_NOT_BOOTSTRAPPED);
    EXPECT_TRUE(bootstrap->ReleaseCommGroupId(ZBAL_UT_NUM_0) == Z_NOT_BOOTSTRAPPED);

    char sendBuf[ZBAL_UT_BUF_SIZE_16] = "hello";
    char recvBuf[ZBAL_UT_NUM_64] = {};
    EXPECT_TRUE(bootstrap->SubGroupAllGather("test_key", ZBAL_TEST_NUMBER_TWO, ZBAL_UT_NUM_0, sendBuf,
                                             ZBAL_TEST_NUMBER_FIVE, recvBuf,
                                             ZBAL_TEST_NUMBER_SIXTYFOUR) == Z_NOT_BOOTSTRAPPED);
    EXPECT_TRUE(bootstrap->SubGroupBarrier("test_key", ZBAL_TEST_NUMBER_TWO, ZBAL_UT_NUM_0) == Z_NOT_BOOTSTRAPPED);
    EXPECT_TRUE(bootstrap->SetLoggerLevel(ZBAL_UT_NUM_0) == Z_NOT_BOOTSTRAPPED);

    EXPECT_TRUE(bootstrap->GetOutput().deviceGva == nullptr);

    EXPECT_NO_THROW(bootstrap->UnInitialize());
}

TEST_F(TestZBALBootstrap, MemBootstrapCreateTypeValidation)
{
    MemBootstrapOptions options;
    InitMemBootstrapOptions(options);

    options.boostrapType = MBT_BUTT;
    EXPECT_TRUE(MemBootstrap::Create(options) == nullptr);

    options.boostrapType = MBT_MEMFABRIC;
    EXPECT_TRUE(MemBootstrap::Create(options) != nullptr);
}

TEST_F(TestZBALBootstrap, MemBootstrapVerifyOptionsAllBranches)
{
    MemBootstrapOptions options;
    InitMemBootstrapOptions(options);

    {
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_OK);
    }
    {
        options.rankCount = ZBAL_UT_NUM_0;
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_INVALID_PARAM);
    }
    {
        options.rankCount = ZBAL_UT_NUM_4;
        options.rankId = ZBAL_UT_NUM_4;
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_INVALID_PARAM);
    }
    {
        options.rankId = ZBAL_UT_NUM_0;
        options.deviceId = ZBAL_DEVICE_COUNT_MAX_LIMIT;
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_INVALID_PARAM);
    }
    {
        options.deviceId = ZBAL_UT_DEVICE_ID;
        options.totalMemSize = ZBAL_MEMORY_SIZE_CAP;
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_INVALID_PARAM);
    }
    {
        options.totalMemSize = ZBAL_UT_SIZE_256MB;
        options.ipPort = "";
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_INVALID_PARAM);
    }
}

TEST_F(TestZBALBootstrap, AcquireCommGroupIdWithMock)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    mock->acqReturnId_ = ZBAL_UT_NUM_5;
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    uint32_t uniqueId = ZBAL_UT_NUM_0;
    EXPECT_TRUE(bootstrap->AcquireCommGroupId(ZBAL_UT_COMM_GROUP_MAX, uniqueId) == Z_OK);
    EXPECT_TRUE(uniqueId == ZBAL_UT_NUM_5);
    EXPECT_TRUE(mock->acqMax_ == ZBAL_UT_COMM_GROUP_MAX);

    mock->acqResult_ = Z_ERROR;
    EXPECT_TRUE(bootstrap->AcquireCommGroupId(ZBAL_TEST_NUMBER_TEN, uniqueId) == Z_ERROR);
}

TEST_F(TestZBALBootstrap, AcquireCommGroupIdMaxZero)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    mock->acqReturnId_ = ZBAL_UT_NUM_0;
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    uint32_t uniqueId = ZBAL_UT_NUM_0;
    EXPECT_TRUE(bootstrap->AcquireCommGroupId(ZBAL_UT_NUM_0, uniqueId) == Z_OK);
    EXPECT_TRUE(mock->acqMax_ == ZBAL_UT_NUM_0);
}

TEST_F(TestZBALBootstrap, ReleaseCommGroupIdWithMock)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    EXPECT_TRUE(bootstrap->ReleaseCommGroupId(ZBAL_UT_NUM_5) == Z_OK);
    EXPECT_TRUE(mock->relId_ == ZBAL_UT_NUM_5);
}

TEST_F(TestZBALBootstrap, SubGroupAllGatherWithMock)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    char sendBuf[ZBAL_UT_BUF_SIZE_16] = "hello";
    char recvBuf[ZBAL_UT_NUM_64] = {};
    EXPECT_TRUE(bootstrap->SubGroupAllGather("test_key", ZBAL_TEST_NUMBER_TWO, ZBAL_UT_NUM_0, sendBuf,
                                             ZBAL_TEST_NUMBER_FIVE, recvBuf, ZBAL_TEST_NUMBER_SIXTYFOUR) == Z_OK);
    EXPECT_TRUE(mock->agKey_ == "test_key");
    EXPECT_TRUE(mock->agRankSize_ == ZBAL_TEST_NUMBER_TWO);
}

TEST_F(TestZBALBootstrap, SubGroupBarrierWithMock)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    EXPECT_TRUE(bootstrap->SubGroupBarrier("barrier_key", ZBAL_TEST_NUMBER_FOUR, ZBAL_UT_NUM_1) == Z_OK);
    EXPECT_TRUE(mock->barrierKey_ == "barrier_key");
}

TEST_F(TestZBALBootstrap, SetLoggerLevelWithMock)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    EXPECT_TRUE(bootstrap->SetLoggerLevel(ZBAL_TEST_NUMBER_THREE) == Z_OK);
    EXPECT_TRUE(mock->logLevel_ == ZBAL_TEST_NUMBER_THREE);
}

TEST_F(TestZBALBootstrap, GetOutputAndFieldTranslation)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    const auto &output = bootstrap->GetOutput();
    EXPECT_TRUE(output.deviceGva == reinterpret_cast<void *>(0x10000));
    EXPECT_TRUE(output.myDeviceGva == reinterpret_cast<void *>(0x20000));
    EXPECT_TRUE(output.createdDeviceMemorySpaceSize == ZBAL_UT_SIZE_256MB * ZBAL_TEST_NUMBER_TWO);
    EXPECT_TRUE(output.allocatedDeviceMemorySize == ZBAL_UT_SIZE_256MB);
    EXPECT_TRUE(output.myCommMetaDeviceGva == reinterpret_cast<void *>(0x20000));

    uint64_t expectedMetaSize =
        static_cast<uint64_t>(options.commMetaSpaceSize) * ZBAL_UT_SIZE_1KB * options.commGroupCap;
    EXPECT_TRUE(output.metaSizeOfDevice == expectedMetaSize);

    void *expectedSMAGva =
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(output.myDeviceGva) + output.metaSizeOfDevice);
    EXPECT_TRUE(output.mySMAGva == expectedSMAGva);
    EXPECT_TRUE(output.smaSizeOfDevice == output.allocatedDeviceMemorySize - output.metaSizeOfDevice);
}

TEST_F(TestZBALBootstrap, UnInitializeWithMock)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    EXPECT_NO_THROW(bootstrap->UnInitialize());
    EXPECT_TRUE(mock->uninitCalled_);
}

TEST_F(TestZBALBootstrap, UnInitializeTwice)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    bootstrap->UnInitialize();
    EXPECT_TRUE(mock->uninitCalled_);

    mock->uninitCalled_ = false;
    EXPECT_NO_THROW(bootstrap->UnInitialize());
    EXPECT_FALSE(mock->uninitCalled_);
}

TEST_F(TestZBALBootstrap, DestroyMemoryBootstrapSwapPattern)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    EXPECT_TRUE(bootstrap->memBootstrap_ != nullptr);
    bootstrap->UnInitialize();
    EXPECT_TRUE(bootstrap->memBootstrap_ == nullptr);
    EXPECT_FALSE(bootstrap->inited_);
}

TEST_F(TestZBALBootstrap, InitializeWhenAlreadyInited)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    EXPECT_TRUE(bootstrap->Initialize() == Z_OK);
    EXPECT_FALSE(mock->initCalled_);
}

TEST_F(TestZBALBootstrap, DestructorTriggersUnInitialize)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);

    auto mock = CreateReadyMock();

    {
        Bootstrap bootstrap(options);
        InjectMockIntoBootstrap(&bootstrap, mock.get());
        EXPECT_FALSE(mock->uninitCalled_);
    }

    EXPECT_TRUE(mock->uninitCalled_);
}

TEST_F(TestZBALBootstrap, CreateWhenSingletonExists)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    Bootstrap::gBootstrap = bootstrap;

    auto result = Bootstrap::Create(options);
    EXPECT_TRUE(result == bootstrap);

    Bootstrap::gBootstrap = nullptr;
}

TEST_F(TestZBALBootstrap, DestroyWhenSingletonNull)
{
    Bootstrap::gBootstrap = nullptr;
    EXPECT_NO_THROW(Bootstrap::Destroy());
}

TEST_F(TestZBALBootstrap, GetWhenSingletonExists)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    Bootstrap::gBootstrap = bootstrap;
    EXPECT_TRUE(Bootstrap::Get() == bootstrap);

    Bootstrap::gBootstrap = nullptr;
}

TEST_F(TestZBALBootstrap, CreateMemBootstrapOptionsTranslation)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.flags = 0xABCD;
    options.dataOperationType = 0x1234;

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    EXPECT_TRUE(bootstrap->options_.flags == 0xABCD);
    EXPECT_TRUE(bootstrap->options_.dataOperationType == 0x1234);
    EXPECT_TRUE(bootstrap->options_.btType == BOOT_BY_MEMFABRIC);
    EXPECT_TRUE(bootstrap->options_.deviceId == ZBAL_UT_DEVICE_ID);
    EXPECT_TRUE(bootstrap->options_.worldSize == ZBAL_UT_NUM_4);
    EXPECT_TRUE(bootstrap->options_.rankId == ZBAL_UT_NUM_0);
    EXPECT_TRUE(bootstrap->options_.deviceMemorySize == ZBAL_UT_SIZE_256MB);
}

TEST_F(TestZBALBootstrap, FullDelegationFlow)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    mock->acqReturnId_ = ZBAL_TEST_NUMBER_SIXTYFOUR;
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    uint32_t uniqueId = ZBAL_UT_NUM_0;
    EXPECT_TRUE(bootstrap->AcquireCommGroupId(ZBAL_TEST_NUMBER_ONE_HUNDRED, uniqueId) == Z_OK);
    EXPECT_TRUE(uniqueId == ZBAL_TEST_NUMBER_SIXTYFOUR);

    EXPECT_TRUE(bootstrap->ReleaseCommGroupId(ZBAL_TEST_NUMBER_SIXTYFOUR) == Z_OK);
    EXPECT_TRUE(bootstrap->SubGroupBarrier("sync", ZBAL_TEST_NUMBER_TWO, ZBAL_UT_NUM_0) == Z_OK);
    EXPECT_TRUE(bootstrap->SetLoggerLevel(ZBAL_TEST_NUMBER_TWO) == Z_OK);

    EXPECT_NO_THROW(bootstrap->UnInitialize());
    EXPECT_TRUE(mock->uninitCalled_);
}

TEST_F(TestZBALBootstrap, GetOutputWhenNotInitialized)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    const auto &output = bootstrap->GetOutput();
    EXPECT_TRUE(output.deviceGva == nullptr);
    EXPECT_TRUE(output.myDeviceGva == nullptr);
    EXPECT_TRUE(output.myCommMetaDeviceGva == nullptr);
    EXPECT_TRUE(output.mySMAGva == nullptr);
    EXPECT_TRUE(output.allocatedDeviceMemorySize == ZBAL_UT_NUM_0);
    EXPECT_TRUE(output.createdDeviceMemorySpaceSize == ZBAL_UT_NUM_0);
    EXPECT_TRUE(output.metaSizeOfDevice == ZBAL_UT_NUM_0);
    EXPECT_TRUE(output.smaSizeOfDevice == ZBAL_UT_NUM_0);
}

TEST_F(TestZBALBootstrap, GetOutputFieldConsistency)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    const auto &output = bootstrap->GetOutput();

    uint64_t expectedMetaSize =
        static_cast<uint64_t>(options.commMetaSpaceSize) * ZBAL_TEST_SIZE_1KB * options.commGroupCap;
    EXPECT_TRUE(output.metaSizeOfDevice == expectedMetaSize);

    void *expectedSMAGva =
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(output.myDeviceGva) + output.metaSizeOfDevice);
    EXPECT_TRUE(output.mySMAGva == expectedSMAGva);

    EXPECT_TRUE(output.smaSizeOfDevice == output.allocatedDeviceMemorySize - output.metaSizeOfDevice);
}

TEST_F(TestZBALBootstrap, InitializeWithInvalidOptions)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.deviceId = ZBAL_DEVICE_COUNT_MAX_LIMIT;

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    ZResult result = bootstrap->Initialize();
    EXPECT_TRUE(result == Z_INVALID_PARAM);
    EXPECT_FALSE(bootstrap->inited_);
}

TEST_F(TestZBALBootstrap, InitializeOptionsValidButMemBootstrapFails)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    ZResult result = bootstrap->Initialize();
    EXPECT_NE(result, Z_OK);
    EXPECT_FALSE(bootstrap->inited_);
}

TEST_F(TestZBALBootstrap, CreateMemBootstrapDirectCallFailsWithoutMemfabric)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    ZResult result = bootstrap->CreateMemBootstrap();
    EXPECT_NE(result, Z_OK);
    EXPECT_TRUE(bootstrap->memBootstrap_ == nullptr);
}

TEST_F(TestZBALBootstrap, MemBootstrapCreateWithEmptyIpPort)
{
    MemBootstrapOptions options;
    InitMemBootstrapOptions(options);
    options.ipPort = "";

    auto bootstrap = MemBootstrap::Create(options);
    ASSERT_TRUE(bootstrap != nullptr);

    ZResult result = bootstrap->Initialize();
    EXPECT_NE(result, Z_OK);
}

TEST_F(TestZBALBootstrap, MemBootstrapCreateWithValidOptions)
{
    MemBootstrapOptions options;
    InitMemBootstrapOptions(options);

    auto bootstrap = MemBootstrap::Create(options);
    ASSERT_TRUE(bootstrap != nullptr);

    ZResult result = bootstrap->Initialize();
    EXPECT_NE(result, Z_OK);
}

TEST_F(TestZBALBootstrap, DestroyWithNullMemBootstrap)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    bootstrap->inited_ = true;

    EXPECT_NO_THROW(bootstrap->UnInitialize());
    EXPECT_FALSE(bootstrap->inited_);
}

TEST_F(TestZBALBootstrap, DelegationWithNullMemBootstrap)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    bootstrap->inited_ = true;
    bootstrap->memBootstrap_ = nullptr;

    uint32_t uniqueId = ZBAL_UT_NUM_0;
    EXPECT_TRUE(bootstrap->AcquireCommGroupId(ZBAL_UT_NUM_10, uniqueId) == Z_NOT_BOOTSTRAPPED);
    EXPECT_TRUE(bootstrap->ReleaseCommGroupId(ZBAL_UT_NUM_0) == Z_NOT_BOOTSTRAPPED);
    EXPECT_TRUE(bootstrap->SubGroupBarrier("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0) == Z_NOT_BOOTSTRAPPED);
    EXPECT_TRUE(bootstrap->SetLoggerLevel(ZBAL_UT_NUM_1) == Z_NOT_BOOTSTRAPPED);
}

TEST_F(TestZBALBootstrap, MemBootstrapVerifyOptionsBoundary)
{
    MemBootstrapOptions options;
    InitMemBootstrapOptions(options);

    {
        options.rankCount = ZBAL_UT_NUM_1;
        options.rankId = ZBAL_UT_NUM_0;
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_OK);
    }
    {
        options.rankCount = ZBAL_MAX_RANK_SIZE;
        options.rankId = ZBAL_MAX_RANK_SIZE - ZBAL_UT_NUM_1;
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_OK);
    }
    {
        options.rankCount = ZBAL_UT_NUM_4;
        options.rankId = ZBAL_UT_NUM_0;
        options.totalMemSize = ZBAL_UT_SIZE_256MB;
        options.deviceId = ZBAL_DEVICE_COUNT_MAX_LIMIT - ZBAL_UT_NUM_1;
        TestableMemBootstrap mb(options);
        EXPECT_TRUE(mb.VerifyOptions() == Z_OK);
    }
}

TEST_F(TestZBALBootstrap, VerifyOptionsTotalMetaEqualsDeviceMemory)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.deviceMemorySize =
        static_cast<uint64_t>(options.commGroupCap) * options.commMetaSpaceSize * ZBAL_TEST_SIZE_1KB;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsMetaSpaceSizeEqualsOperateParamSize)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.commMetaSpaceSize = ZBAL_UT_NUM_64;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsMetaSpaceSizeAboveOperateParamSize)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.commMetaSpaceSize = ZBAL_UT_NUM_65;
    options.deviceMemorySize =
        static_cast<uint64_t>(options.commGroupCap) * options.commMetaSpaceSize * ZBAL_TEST_SIZE_1KB * ZBAL_UT_NUM_2;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsCommGroupCapAtMaxFails)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.commGroupCap = COMM_GROUP_COUNT_CAP_MAX;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsCommGroupCapBelowMaxPasses)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.commGroupCap = COMM_GROUP_COUNT_CAP_MAX - ZBAL_UT_NUM_1;
    options.deviceMemorySize =
        static_cast<uint64_t>(options.commGroupCap) * options.commMetaSpaceSize * ZBAL_TEST_SIZE_1KB + ZBAL_UT_NUM_1;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsDeviceMemAtCapFails)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.deviceMemorySize = ZBAL_MEMORY_SIZE_CAP;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsDeviceMemBelowCapPasses)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.deviceMemorySize = ZBAL_MEMORY_SIZE_CAP - ZBAL_UT_NUM_1;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsRankIdEqualsWorldSize)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.worldSize = ZBAL_UT_NUM_4;
    options.rankId = ZBAL_UT_NUM_4;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsWorldSizeExceedsMaxFails)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.worldSize = ZBAL_RANK_COUNT_MAX_LIMIT + ZBAL_UT_NUM_1;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, VerifyOptionsBtTypeButtFails)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    options.btType = BOOT_BY_BUTT;
    EXPECT_TRUE(Bootstrap::Create(options) == nullptr);
}

TEST_F(TestZBALBootstrap, InitializeFullFlowOptionsValid)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);
    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    ZResult result = bootstrap->Initialize();
    EXPECT_NE(result, Z_OK);
    EXPECT_FALSE(bootstrap->inited_);
}

TEST_F(TestZBALBootstrap, ZBALBootstrapApiSuccessPathViaSingleton)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    Bootstrap::gBootstrap = bootstrap;
    ZBALInitState::Instance().Reset();

    zbal_bootstrap_output_t output;
    std::memset(&output, 0, sizeof(output));
    int32_t result = zbal_bootstrap(&options, &output);

    EXPECT_EQ(result, Z_OK);

    EXPECT_TRUE(output.deviceGva == reinterpret_cast<void *>(0x10000));
    EXPECT_TRUE(output.allocatedDeviceMemorySize == ZBAL_UT_SIZE_256MB);

    auto &state = ZBALInitState::Instance();
    EXPECT_TRUE(state.Bootstrapped());
    EXPECT_EQ(state.ext_.btType, options.btType);
    EXPECT_EQ(state.ext_.worldSize, options.worldSize);
    EXPECT_EQ(state.ext_.worldRankId, options.rankId);
    EXPECT_EQ(state.ext_.deviceId, options.deviceId);
    EXPECT_EQ(state.ext_.commMetaSpaceSize, options.commMetaSpaceSize);
    EXPECT_EQ(state.ext_.commGroupCap, options.commGroupCap);
    EXPECT_EQ(state.ext_.gvaDevice, output.deviceGva);
    EXPECT_EQ(state.ext_.mySMAGva, output.mySMAGva);
    EXPECT_EQ(state.ext_.smaSizeOfDevice, output.smaSizeOfDevice);
    EXPECT_EQ(state.ext_.localDeviceMemSize, output.allocatedDeviceMemorySize);
    EXPECT_EQ(state.ext_.symmetricMemSpace, output.createdDeviceMemorySpaceSize);

    Bootstrap::gBootstrap = nullptr;
    ZBALInitState::Instance().Reset();
}

TEST_F(TestZBALBootstrap, ZBALUnbootstrapSuccessPathWithSingleton)
{
    zbal_bootstrap_options_t options;
    InitValidOptions(options);

    auto bootstrap = ZMakeRef<Bootstrap>(options);
    ASSERT_TRUE(bootstrap != nullptr);

    auto mock = CreateReadyMock();
    InjectMockIntoBootstrap(bootstrap.Get(), mock.get());

    Bootstrap::gBootstrap = bootstrap;
    auto &state = ZBALInitState::Instance();
    state.Bootstrapped(true);
    state.SmaInitialized(false);

    int32_t result = zbal_unbootstrap(ZBAL_UT_NUM_0);
    EXPECT_EQ(result, Z_OK);

    EXPECT_TRUE(Bootstrap::gBootstrap == nullptr);
    EXPECT_TRUE(mock->uninitCalled_);

    EXPECT_FALSE(state.Bootstrapped());
    EXPECT_FALSE(state.SmaInitialized());

    ZBALInitState::Instance().Reset();
}
