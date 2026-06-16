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
#include <cstdlib>

#include "zbal_test_constants.h"
#include "zbal_defines.h"
#undef ALWAYS_INLINE
#define ALWAYS_INLINE inline
#include "zbal_common_includes.h"
#undef ALWAYS_INLINE
#define ALWAYS_INLINE inline __attribute__((always_inline))

#define private   public
#define protected public
#include "dl_mf_api.h"
#include "dl_cann_api.h"
#include "zbal_mem_mf_bootstrap.h"
#undef private
#undef protected

using namespace zbal;
using namespace zbal::bootstrap;
using namespace zbal::underapi;

static int g_mockSmemInitResult = 0;
static int g_mockSmemShmConfigInitResult = 0;
static int g_mockAclrtGetDeviceResult = 0;
static int g_mockAclrtGetDeviceId = 0;
static int g_mockSmemShmInitResult = 0;
static void *g_mockSmemShmCreateReturn = nullptr;
static uint64_t g_mockSmemShmGetSymmetricSize = 0;
static int g_mockSmemSetLoggerLevelResult = 0;
static int g_mockSmemShmAtomicAllocValueResult = 0;
static uint32_t g_mockSmemShmAtomicAllocValueId = 0;
static int g_mockSmemShmAtomicReleaseValueResult = 0;
static int g_mockSmemShmSubGroupAllGatherResult = 0;
static int g_mockSmemShmSubGroupBarrierResult = 0;

static bool g_smemUnInitCalled = false;
static bool g_smemShmDestroyCalled = false;

static int MockSmemInit(uint32_t)
{
    return g_mockSmemInitResult;
}

static int MockSmemShmConfigInit(smem_shm_config_t *)
{
    return g_mockSmemShmConfigInitResult;
}

static int MockAclrtGetDevice(int32_t *deviceId)
{
    *deviceId = g_mockAclrtGetDeviceId;
    return g_mockAclrtGetDeviceResult;
}

static int MockSmemShmInit(const char *, uint32_t, uint32_t, uint16_t, smem_shm_config_t *)
{
    return g_mockSmemShmInitResult;
}

static int MockSmemSetExternLogger(void (*)(int, const char *))
{
    return 0;
}

static smem_shm_t MockSmemShmCreate(uint32_t, uint32_t, uint32_t, uint64_t, smem_shm_data_op_type, uint32_t, void **gva)
{
    if (g_mockSmemShmCreateReturn != nullptr && gva != nullptr) {
        *gva = reinterpret_cast<void *>(0x10000);
    }
    return g_mockSmemShmCreateReturn;
}

static int MockSmemShmDestroy(smem_shm_t, uint32_t)
{
    g_smemShmDestroyCalled = true;
    return 0;
}

static uint64_t MockSmemShmGetSymmetricSize(smem_shm_t)
{
    return g_mockSmemShmGetSymmetricSize;
}

static void MockSmemUnInit(void)
{
    g_smemUnInitCalled = true;
}

static int MockSmemSetLoggerLevel(int)
{
    return g_mockSmemSetLoggerLevelResult;
}

static int MockSmemShmAtomicAllocValue(smem_shm_t, uint32_t, uint32_t *retVal)
{
    if (g_mockSmemShmAtomicAllocValueResult == 0 && retVal != nullptr) {
        *retVal = g_mockSmemShmAtomicAllocValueId;
    }
    return g_mockSmemShmAtomicAllocValueResult;
}

static int MockSmemShmAtomicReleaseValue(smem_shm_t, int32_t)
{
    return g_mockSmemShmAtomicReleaseValueResult;
}

static int MockSmemShmSubGroupAllGather(smem_shm_t, const char *, uint32_t, uint32_t, const char *, uint32_t, char *,
                                        uint32_t)
{
    return g_mockSmemShmSubGroupAllGatherResult;
}

static int MockSmemShmSubGroupBarrier(smem_shm_t, const char *, uint32_t, uint32_t)
{
    return g_mockSmemShmSubGroupBarrierResult;
}

class TestZBALMemFabricBootstrap : public testing::Test {
public:
    static void SetUpTestSuite()
    {
        char cwd[ZBAL_UT_SIZE_4KB] = {};
        (void)getcwd(cwd, sizeof(cwd));
        tmpDir_ = std::string(cwd) + "/ut_mf_tmp_" + std::to_string(rand());
        Func::MakeDir(tmpDir_, 0755);

        ascendLibDir_ = tmpDir_ + "/ascend_lib";
        Func::MakeDir(ascendLibDir_, 0755);
        Func::MakeDir(ascendLibDir_ + "/lib64", 0755);

        setenv("MEMFABRIC_HYBRID_LIBRARY_PATH", tmpDir_.c_str(), 1);
        setenv("ASCEND_HOME_PATH", ascendLibDir_.c_str(), 1);
    }

    static void TearDownTestSuite()
    {
        unsetenv("MEMFABRIC_HYBRID_LIBRARY_PATH");
        unsetenv("ASCEND_HOME_PATH");
        Func::RemoveDirRecursive(tmpDir_);
    }

    void SetUp() override
    {
        ResetMockState();
        SetupMockFunctionPointers();
        DlMfApi::gLoaded = true;
        DlCannApi::gLoaded = true;
    }

    void TearDown() override
    {
        DlMfApi::gLoaded = false;
        DlCannApi::gLoaded = false;
        ClearMockFunctionPointers();
    }

    void ResetMockState()
    {
        g_mockSmemInitResult = 0;
        g_mockSmemShmConfigInitResult = 0;
        g_mockAclrtGetDeviceResult = 0;
        g_mockAclrtGetDeviceId = 0;
        g_mockSmemShmInitResult = 0;
        g_mockSmemShmCreateReturn = reinterpret_cast<void *>(0x1);
        g_mockSmemShmGetSymmetricSize = ZBAL_UT_SIZE_256MB;
        g_mockSmemSetLoggerLevelResult = 0;
        g_mockSmemShmAtomicAllocValueResult = 0;
        g_mockSmemShmAtomicAllocValueId = ZBAL_UT_NUM_5;
        g_mockSmemShmAtomicReleaseValueResult = 0;
        g_mockSmemShmSubGroupAllGatherResult = 0;
        g_mockSmemShmSubGroupBarrierResult = 0;

        g_smemUnInitCalled = false;
        g_smemShmDestroyCalled = false;

        SetupMockFunctionPointers();
        DlMfApi::gLoaded = true;
        DlCannApi::gLoaded = true;
    }

    void SetupMockFunctionPointers()
    {
        DlMfApi::gMfSmemInit = reinterpret_cast<mfSmemInitFunc>(MockSmemInit);
        DlMfApi::gMfSmemShmConfigInit = reinterpret_cast<mfSmemShmConfigInitFunc>(MockSmemShmConfigInit);
        DlMfApi::gMfSmemShmInit = reinterpret_cast<mfSmemShmInitFunc>(MockSmemShmInit);
        DlMfApi::gMfSmemShmCreate = reinterpret_cast<mfSmemShmCreateFunc>(MockSmemShmCreate);
        DlMfApi::gMfSmemShmDestroy = reinterpret_cast<mfSmemShmDestroyFunc>(MockSmemShmDestroy);
        DlMfApi::gMfSmemShmGetSymmetricSize =
            reinterpret_cast<mfSmemShmGetSymmetricSizeFunc>(MockSmemShmGetSymmetricSize);
        DlMfApi::gMfSmemUnInit = reinterpret_cast<mfSmemUnInitFunc>(MockSmemUnInit);
        DlMfApi::gMfSmemSetLogLevel = reinterpret_cast<mfSmemSetLogLevelFunc>(MockSmemSetLoggerLevel);
        DlMfApi::gMfSmemSetExternLogger = reinterpret_cast<mfSmemSetExternLoggerFunc>(MockSmemSetExternLogger);
        DlMfApi::gMfSmemShmAtomicAllocValue =
            reinterpret_cast<mfSmemShmAtomicAllocValueFunc>(MockSmemShmAtomicAllocValue);
        DlMfApi::gMfSmemShmAtomicReleaseValue =
            reinterpret_cast<mfSmemShmAtomicReleaseValueFunc>(MockSmemShmAtomicReleaseValue);
        DlMfApi::gMfSmemShmSubgroupAllGather =
            reinterpret_cast<mfSmemShmSubgroupAllGatherFunc>(MockSmemShmSubGroupAllGather);
        DlMfApi::gMfSmemShmSubgroupBarrier = reinterpret_cast<mfSmemShmSubgroupBarrierFunc>(MockSmemShmSubGroupBarrier);

        DlCannApi::pAclrtGetDevice = reinterpret_cast<aclrtGetDeviceFunc>(MockAclrtGetDevice);
    }

    void ClearMockFunctionPointers()
    {
        DlMfApi::gMfSmemInit = nullptr;
        DlMfApi::gMfSmemShmConfigInit = nullptr;
        DlMfApi::gMfSmemShmInit = nullptr;
        DlMfApi::gMfSmemShmCreate = nullptr;
        DlMfApi::gMfSmemShmDestroy = nullptr;
        DlMfApi::gMfSmemShmGetSymmetricSize = nullptr;
        DlMfApi::gMfSmemUnInit = nullptr;
        DlMfApi::gMfSmemSetLogLevel = nullptr;
        DlMfApi::gMfSmemSetExternLogger = nullptr;
        DlMfApi::gMfSmemShmAtomicAllocValue = nullptr;
        DlMfApi::gMfSmemShmAtomicReleaseValue = nullptr;
        DlMfApi::gMfSmemShmSubgroupAllGather = nullptr;
        DlMfApi::gMfSmemShmSubgroupBarrier = nullptr;

        DlCannApi::pAclrtGetDevice = nullptr;
    }

    MemBootstrapOptions MakeValidOptions()
    {
        MemBootstrapOptions options;
        options.boostrapType = MBT_MEMFABRIC;
        options.deviceId = ZBAL_UT_DEVICE_ID;
        options.rankCount = ZBAL_UT_NUM_4;
        options.rankId = ZBAL_UT_NUM_0;
        options.totalMemSize = ZBAL_UT_SIZE_256MB;
        options.ipPort = "127.0.0.1:12345";
        return options;
    }

    void SetupInitialized(MemFabricBoostrap &bootstrap)
    {
        bootstrap.initialized_ = true;
        bootstrap.shmHandle_ = reinterpret_cast<smem_shm_t>(0x1);
    }

    static std::string tmpDir_;
    static std::string ascendLibDir_;
};

std::string TestZBALMemFabricBootstrap::tmpDir_;
std::string TestZBALMemFabricBootstrap::ascendLibDir_;

TEST_F(TestZBALMemFabricBootstrap, InitPreCheck)
{
    auto options = MakeValidOptions();
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_EQ(bootstrap.InitPreCheck(), Z_OK);
        bootstrap.initialized_ = true;
        EXPECT_EQ(bootstrap.InitPreCheck(), Z_OK);
    }
    {
        options.rankCount = ZBAL_UT_NUM_0;
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.InitPreCheck(), Z_OK);
    }
    {
        options.rankCount = ZBAL_UT_NUM_4;
        MemFabricBoostrap bootstrap(options);
        unsetenv("MEMFABRIC_HYBRID_LIBRARY_PATH");
        EXPECT_NE(bootstrap.InitPreCheck(), Z_OK);
        setenv("MEMFABRIC_HYBRID_LIBRARY_PATH", tmpDir_.c_str(), 1);
        DlMfApi::gLoaded = false;
        EXPECT_NE(bootstrap.InitPreCheck(), Z_OK);
        DlMfApi::gLoaded = true;
    }
    {
        MemFabricBoostrap bootstrap(options);
        unsetenv("ASCEND_HOME_PATH");
        EXPECT_NE(bootstrap.InitPreCheck(), Z_OK);
        setenv("ASCEND_HOME_PATH", ascendLibDir_.c_str(), 1);
        DlCannApi::gLoaded = false;
        EXPECT_NE(bootstrap.InitPreCheck(), Z_OK);
        DlCannApi::gLoaded = true;
    }
}

TEST_F(TestZBALMemFabricBootstrap, CreateSHMSpace)
{
    auto options = MakeValidOptions();

    g_mockSmemInitResult = -1;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.CreateSHMSpace(), Z_OK);
    }

    ResetMockState();
    g_mockSmemShmConfigInitResult = -1;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.CreateSHMSpace(), Z_OK);
    }

    ResetMockState();
    g_mockAclrtGetDeviceResult = -1;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.CreateSHMSpace(), Z_OK);
    }

    ResetMockState();
    g_mockAclrtGetDeviceId = -1;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.CreateSHMSpace(), Z_OK);
    }

    ResetMockState();
    g_mockSmemShmInitResult = -1;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.CreateSHMSpace(), Z_OK);
    }

    ResetMockState();
    g_mockSmemShmCreateReturn = nullptr;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.CreateSHMSpace(), Z_OK);
        EXPECT_TRUE(g_smemUnInitCalled);
    }

    ResetMockState();
    g_mockSmemShmGetSymmetricSize = ZBAL_UT_NUM_0;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.CreateSHMSpace(), Z_OK);
        EXPECT_TRUE(g_smemShmDestroyCalled);
        EXPECT_TRUE(g_smemUnInitCalled);
    }

    ResetMockState();
    g_mockSmemShmGetSymmetricSize = ZBAL_UT_SIZE_256MB - ZBAL_UT_NUM_1;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.CreateSHMSpace(), Z_OK);
    }

    ResetMockState();
    g_mockSmemSetLoggerLevelResult = -1;
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_EQ(bootstrap.CreateSHMSpace(), Z_OK);
        EXPECT_TRUE(bootstrap.initialized_);
    }

    ResetMockState();
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_EQ(bootstrap.CreateSHMSpace(), Z_OK);
        EXPECT_TRUE(bootstrap.initialized_);
        EXPECT_NE(bootstrap.shmHandle_, nullptr);
        EXPECT_NE(bootstrap.output_.gvaDevice, nullptr);
        EXPECT_NE(bootstrap.output_.myGvaDevice, nullptr);
        EXPECT_EQ(bootstrap.output_.memorySizeDevice, options.totalMemSize);
        EXPECT_EQ(bootstrap.output_.memorySpaceSizeDevice, g_mockSmemShmGetSymmetricSize);
    }
}

TEST_F(TestZBALMemFabricBootstrap, Initialize)
{
    auto options = MakeValidOptions();
    {
        options.rankCount = ZBAL_UT_NUM_0;
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.Initialize(), Z_OK);
    }
    {
        options.rankCount = ZBAL_UT_NUM_4;
        g_mockSmemInitResult = -1;
        MemFabricBoostrap bootstrap(options);
        EXPECT_NE(bootstrap.Initialize(), Z_OK);
    }
    ResetMockState();
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_EQ(bootstrap.Initialize(), Z_OK);
        EXPECT_TRUE(bootstrap.initialized_);
    }
}

TEST_F(TestZBALMemFabricBootstrap, UnInitialize)
{
    auto options = MakeValidOptions();
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_NO_THROW(bootstrap.UnInitialize());
        EXPECT_FALSE(g_smemShmDestroyCalled);
        EXPECT_FALSE(g_smemUnInitCalled);
    }
    {
        MemFabricBoostrap bootstrap(options);
        bootstrap.initialized_ = true;
        g_smemUnInitCalled = false;
        EXPECT_NO_THROW(bootstrap.UnInitialize());
        EXPECT_FALSE(g_smemShmDestroyCalled);
        EXPECT_TRUE(g_smemUnInitCalled);
        EXPECT_FALSE(bootstrap.initialized_);
    }
    ResetMockState();
    {
        MemFabricBoostrap bootstrap(options);
        bootstrap.initialized_ = true;
        bootstrap.shmHandle_ = reinterpret_cast<smem_shm_t>(0x1);
        EXPECT_NO_THROW(bootstrap.UnInitialize());
        EXPECT_TRUE(g_smemShmDestroyCalled);
        EXPECT_TRUE(g_smemUnInitCalled);
        EXPECT_FALSE(bootstrap.initialized_);
        EXPECT_TRUE(bootstrap.shmHandle_ == nullptr);
    }
}

TEST_F(TestZBALMemFabricBootstrap, CommGroupId)
{
    auto options = MakeValidOptions();
    {
        MemFabricBoostrap bootstrap(options);
        uint32_t uniqueId = ZBAL_UT_NUM_0;
        EXPECT_EQ(bootstrap.AcquireCommGroupId(ZBAL_UT_COMM_GROUP_MAX, uniqueId), Z_MEM_NOT_BOOTSTRAP);
        EXPECT_EQ(bootstrap.ReleaseCommGroupId(ZBAL_UT_NUM_5), Z_MEM_NOT_BOOTSTRAP);
    }
    {
        MemFabricBoostrap bootstrap(options);
        SetupInitialized(bootstrap);
        uint32_t uniqueId = ZBAL_UT_NUM_0;
        EXPECT_EQ(bootstrap.AcquireCommGroupId(ZBAL_UT_COMM_GROUP_MAX, uniqueId), Z_OK);
        EXPECT_EQ(uniqueId, ZBAL_UT_NUM_5);
        EXPECT_EQ(bootstrap.ReleaseCommGroupId(ZBAL_UT_NUM_5), Z_OK);
        g_mockSmemShmAtomicAllocValueResult = -1;
        EXPECT_NE(bootstrap.AcquireCommGroupId(ZBAL_UT_COMM_GROUP_MAX, uniqueId), Z_OK);
        g_mockSmemShmAtomicAllocValueResult = 0;
        g_mockSmemShmAtomicReleaseValueResult = -1;
        EXPECT_NE(bootstrap.ReleaseCommGroupId(ZBAL_UT_NUM_5), Z_OK);
    }
}

TEST_F(TestZBALMemFabricBootstrap, SubGroupAllGather)
{
    auto options = MakeValidOptions();
    char sendBuf[ZBAL_UT_NUM_16] = "hello";
    char recvBuf[ZBAL_UT_NUM_64] = {};

    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_EQ(bootstrap.SubGroupAllGather("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, sendBuf, ZBAL_UT_NUM_5, recvBuf,
                                              ZBAL_UT_NUM_64),
                  Z_MEM_NOT_BOOTSTRAP);
    }
    {
        MemFabricBoostrap bootstrap(options);
        SetupInitialized(bootstrap);
        EXPECT_EQ(bootstrap.SubGroupAllGather("", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, sendBuf, ZBAL_UT_NUM_5, recvBuf,
                                              ZBAL_UT_NUM_64),
                  Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupAllGather("key", ZBAL_UT_NUM_0, ZBAL_UT_NUM_0, sendBuf, ZBAL_UT_NUM_5, recvBuf,
                                              ZBAL_UT_NUM_64),
                  Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupAllGather("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_2, sendBuf, ZBAL_UT_NUM_5, recvBuf,
                                              ZBAL_UT_NUM_64),
                  Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupAllGather("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, nullptr, ZBAL_UT_NUM_5, recvBuf,
                                              ZBAL_UT_NUM_64),
                  Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupAllGather("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, sendBuf, ZBAL_UT_NUM_0, recvBuf,
                                              ZBAL_UT_NUM_64),
                  Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupAllGather("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, sendBuf, ZBAL_UT_NUM_5, nullptr,
                                              ZBAL_UT_NUM_64),
                  Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupAllGather("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, sendBuf, ZBAL_UT_NUM_5, recvBuf,
                                              ZBAL_UT_NUM_0),
                  Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupAllGather("test_key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, sendBuf, ZBAL_UT_NUM_5, recvBuf,
                                              ZBAL_UT_NUM_64),
                  Z_OK);
        g_mockSmemShmSubGroupAllGatherResult = -1;
        EXPECT_NE(bootstrap.SubGroupAllGather("test_key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0, sendBuf, ZBAL_UT_NUM_5, recvBuf,
                                              ZBAL_UT_NUM_64),
                  Z_OK);
    }
}

TEST_F(TestZBALMemFabricBootstrap, SubGroupBarrier)
{
    auto options = MakeValidOptions();
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_EQ(bootstrap.SubGroupBarrier("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0), Z_MEM_NOT_BOOTSTRAP);
    }
    {
        MemFabricBoostrap bootstrap(options);
        SetupInitialized(bootstrap);
        EXPECT_EQ(bootstrap.SubGroupBarrier("", ZBAL_UT_NUM_2, ZBAL_UT_NUM_0), Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupBarrier("key", ZBAL_UT_NUM_0, ZBAL_UT_NUM_0), Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupBarrier("key", ZBAL_UT_NUM_2, ZBAL_UT_NUM_2), Z_INVALID_PARAM);
        EXPECT_EQ(bootstrap.SubGroupBarrier("barrier_key", ZBAL_UT_NUM_4, ZBAL_UT_NUM_1), Z_OK);
        g_mockSmemShmSubGroupBarrierResult = -1;
        EXPECT_NE(bootstrap.SubGroupBarrier("barrier_key", ZBAL_UT_NUM_4, ZBAL_UT_NUM_1), Z_OK);
    }
}

TEST_F(TestZBALMemFabricBootstrap, SetLoggerLevel)
{
    auto options = MakeValidOptions();
    {
        MemFabricBoostrap bootstrap(options);
        EXPECT_EQ(bootstrap.SetLoggerLevel(ZBAL_UT_NUM_3), Z_MEM_NOT_BOOTSTRAP);
    }
    {
        MemFabricBoostrap bootstrap(options);
        SetupInitialized(bootstrap);
        EXPECT_EQ(bootstrap.SetLoggerLevel(ZBAL_UT_NUM_3), Z_OK);
        g_mockSmemSetLoggerLevelResult = -1;
        EXPECT_NE(bootstrap.SetLoggerLevel(ZBAL_UT_NUM_3), Z_OK);
    }
}
