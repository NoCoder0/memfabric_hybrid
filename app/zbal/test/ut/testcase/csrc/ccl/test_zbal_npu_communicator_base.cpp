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
#include <cstring>

#include "zbal_test_constants.h"
#include "test_zbal_def.h"

#define private   public
#define protected public

#include "zbal_comm_group_meta.h"
#include "zbal_comm_types.h"
#include "zbal_npu_operators.h"
#include "zbal_bootstrap_types.h"
#include "dl_cann_api.h"
#include "zbal_communicator.h"
#include "zbal_npu_communicator_base.h"
#include "zbal_npu_communicator_aiv.h"

#undef private
#undef protected

using namespace zbal;
using namespace zbal::operators;
using namespace zbal::bootstrap;
using namespace zbal::underapi;

/* ================================================================
 * Mock DlCannApi function pointers — global control variables
 * ================================================================ */

static int g_mockAclrtMemcpyRet = 0;
static int g_mockAclrtMallocHostRet = 0;
static int g_mockAclrtMemsetProfRet = 0;
static int g_mockAclrtHostRegisterRet = 0;
static void *g_lastMallocHostPtr = nullptr;
static void *g_lastHostRegisterDevPtr = nullptr;

static void ResetMockState()
{
    g_mockAclrtMemcpyRet = 0;
    g_mockAclrtMallocHostRet = 0;
    g_mockAclrtMemsetProfRet = 0;
    g_mockAclrtHostRegisterRet = 0;
    g_lastMallocHostPtr = nullptr;
    g_lastHostRegisterDevPtr = nullptr;
}

static int32_t MockAclrtMemset(void *, size_t, int32_t, size_t)
{
    if (g_mockAclrtMemsetProfRet != 0) {
        return g_mockAclrtMemsetProfRet;
    }
    return 0;
}
static int32_t MockRtGetC2cCtrlAddr(uint64_t *addr, uint32_t *len)
{
    if (addr != nullptr) {
        *addr = 0x1000;
    }
    if (len != nullptr) {
        *len = ZBAL_UT_NUM_8;
    }
    return 0;
}

static int32_t MockAclrtMemcpy(void *, size_t, const void *, size_t, uint32_t)
{
    return g_mockAclrtMemcpyRet;
}

static int32_t MockAclrtMallocHost(void **ptr, size_t size)
{
    if (g_mockAclrtMallocHostRet != 0) {
        return g_mockAclrtMallocHostRet;
    }
    *ptr = calloc(1, size);
    g_lastMallocHostPtr = *ptr;
    return 0;
}

static int MockAclrtFreeHostProf(void *ptr)
{
    free(ptr);
    if (g_lastMallocHostPtr == ptr) {
        g_lastMallocHostPtr = nullptr;
    }
    return 0;
}

static int32_t MockAclrtHostRegister(void *hostPtr, uint64_t size, aclrtHostRegisterType, void **outDevPtr)
{
    if (g_mockAclrtHostRegisterRet != 0) {
        return g_mockAclrtHostRegisterRet;
    }
    *outDevPtr = hostPtr;
    g_lastHostRegisterDevPtr = hostPtr;
    return 0;
}

static int32_t MockAclrtHostUnregisterProf(void *)
{
    return 0;
}

/* ================================================================
 * Helper
 * ================================================================ */

static void SetupDefaultOptions(CommGroupOptions &opt)
{
    opt.name = "npu_test";
    opt.worldSize = ZBAL_UT_NUM_1;
    opt.groupSize = ZBAL_UT_NUM_1;
    opt.myWorldRank = ZBAL_UT_NUM_0;
    opt.myGroupRank = ZBAL_UT_NUM_0;
    opt.deviceId = ZBAL_UT_NUM_0;
    opt.dataOpType = ZBAL_UT_NUM_0;
    opt.gva = nullptr;
    opt.metaSize = ZBAL_UT_META_SIZE;
    opt.myMetaGva = 0x1000;
    opt.myParamDataGva = 0x2000;
    opt.myAddressExchangeGva = 0x3000;
    opt.sizeForCommGroupInfo = sizeof(CommGroupInfo);
    opt.sizeForParam = ZBAL_UT_SIZE_FOR_PARAM;
    opt.sizeForExchangeAddress = ZBAL_UT_SIZE_FOR_EXCHANGE_ADDRESS;
    opt.groupIndex = ZBAL_UT_NUM_0;
    opt.fftsConfig = ZBAL_UT_NUM_0;
    opt.localDeviceMemSize = ZBAL_UT_NUM_0;
}

static void ClearEnvRebalance()
{
    ::unsetenv("DEEPEP_ENABLE_REBALANCE");
    ::unsetenv("DEEPEP_BALANCE_FACTOR_HIGH");
    ::unsetenv("DEEPEP_BALANCE_FACTOR_LOW");
}

/* ================================================================
 * Test fixture
 * ================================================================ */

class TestZBALNpuCommunicatorBase : public testing::Test {
public:
    static void SetUpTestCase()
    {
        DlCannApi::pAclrtMemset = MockAclrtMemset;
        DlCannApi::pRtGetC2cCtrlAddr = MockRtGetC2cCtrlAddr;
        DlCannApi::pAclrtMemcpy = MockAclrtMemcpy;
        DlCannApi::pAclrtMallocHost = MockAclrtMallocHost;
        DlCannApi::pAclrtFreeHost = MockAclrtFreeHostProf;
        DlCannApi::pAclrtHostRegister = MockAclrtHostRegister;
        DlCannApi::pAclrtHostUnregister = MockAclrtHostUnregisterProf;
    }

    static void TearDownTestCase()
    {
        DlCannApi::pAclrtMemset = nullptr;
        DlCannApi::pRtGetC2cCtrlAddr = nullptr;
        DlCannApi::pAclrtMemcpy = nullptr;
        DlCannApi::pAclrtMallocHost = nullptr;
        DlCannApi::pAclrtFreeHost = nullptr;
        DlCannApi::pAclrtHostRegister = nullptr;
        DlCannApi::pAclrtHostUnregister = nullptr;
    }

    void SetUp() override
    {
        Communicator::DestroyAll();
        SetupDefaultOptions(opt_);
        ClearEnvRebalance();
        EnvHelper::PROF_ENABLED = false;
        EnvHelper::PROF_TRACING_MAX_COUNT = 0;
        ResetMockState();
    }

    void TearDown() override
    {
        Communicator::DestroyAll();
        EnvHelper::PROF_ENABLED = false;
        EnvHelper::PROF_TRACING_MAX_COUNT = 0;
        ClearEnvRebalance();
        ResetMockState();
    }

    CommGroupOptions opt_;
};

/* ================================================================
 * 1. Constructor and basic accessors (world / non-world / with ref)
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, ConstructorAndBasicAccessors)
{
    NpuCommunicatorAIV world(opt_, true, nullptr);
    EXPECT_TRUE(world.IsWorldGroup());
    EXPECT_EQ(world.Name(), "npu_test");
    EXPECT_EQ(world.GroupId(), UINT16_MAX);

    CommunicatorPtr worldRef(&world);
    NpuCommunicatorAIV sub(opt_, false, worldRef);
    EXPECT_FALSE(sub.IsWorldGroup());

    NpuCommunicatorAIV nonWorld(opt_, false, nullptr);
    EXPECT_FALSE(nonWorld.IsWorldGroup());
}

/* ================================================================
 * ZBAL_UT_NUM_2. ConstructCommGroupInfo (all fields + waitSymbol)
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, ConstructCommGroupInfo)
{
    opt_.groupSize = ZBAL_UT_NUM_4;
    opt_.myGroupRank = ZBAL_UT_NUM_2;
    opt_.myMetaGva = 0xAAAA;
    opt_.myParamDataGva = 0xBBBB;
    opt_.myAddressExchangeGva = 0xCCCC;
    opt_.sizeForCommGroupInfo = ZBAL_UT_SIZE_FOR_COMM_GROUP_INFO;
    opt_.sizeForParam = ZBAL_UT_NUM_512;
    opt_.sizeForExchangeAddress = ZBAL_UT_SIZE_1KB;
    opt_.fftsConfig = 0xDEAD;
    opt_.localDeviceMemSize = 0xF000;
    opt_.dataOpType = ZBAL_UT_NUM_3;

    for (auto groupIdx : {uint16_t{0}, ZBAL_UT_NUM_5, ZBAL_UT_NUM_7}) {
        opt_.groupIndex = groupIdx;
        NpuCommunicatorAIV comm(opt_, false, nullptr);
        comm.ConstructCommGroupInfo(opt_);

        const auto &m = comm.GetMetaInfo();
        EXPECT_EQ(m.groupIndex, groupIdx);
        EXPECT_EQ(m.groupSize, ZBAL_UT_NUM_4);
        EXPECT_EQ(m.myGroupRank, ZBAL_UT_NUM_2);
        EXPECT_EQ(m.myMetaGva, 0xAAAAu);
        EXPECT_EQ(m.myParamDataGva, 0xBBBBu);
        EXPECT_EQ(m.myAddressExchangeGva, 0xCCCCu);
        EXPECT_EQ(m.sizeForCommGroupInfo, ZBAL_UT_SIZE_FOR_COMM_GROUP_INFO);
        EXPECT_EQ(m.sizeForParam, ZBAL_UT_NUM_512);
        EXPECT_EQ(m.sizeForExchangeAddress, ZBAL_UT_SIZE_1KB);
        EXPECT_EQ(m.fftsConfig, 0xDEADu);
        EXPECT_EQ(m.localDeviceMemSize, 0xF000u);
        EXPECT_EQ(m.dataOpType, 3u);
        EXPECT_NE(m.waitSymbol, 0u);
    }
}

/* ================================================================
 * ZBAL_UT_NUM_3. Initialize
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, InitializeNoBootstrap)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EXPECT_EQ(comm.Initialize(), Z_NOT_BOOTSTRAPPED);
}

TEST_F(TestZBALNpuCommunicatorBase, InitializeDoubleInitReturnsOk)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    comm.initialized_ = true;
    EXPECT_EQ(comm.Initialize(), Z_OK);
}

/* ================================================================
 * ZBAL_UT_NUM_4. AssignGatherGroupId
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, AssignGatherGroupIdNotInitialized)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    AutoReleaseGroupId groupId;
    EXPECT_EQ(comm.AssignGatherGroupId(groupId), Z_NOT_INITIALIZED);
}

TEST_F(TestZBALNpuCommunicatorBase, AssignGatherGroupIdSuccess)
{
    opt_.groupSize = 1;
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    comm.initialized_ = true;

    AutoReleaseGroupId groupId;
    groupId.gatheredGroupInfo_.resize(1);
    groupId.gatheredGroupInfo_[0].myWorldRankId = 0;
    groupId.uniqueGroupId_ = ZBAL_UT_NUM_100;

    g_mockAclrtMemcpyRet = 0;
    EXPECT_EQ(comm.AssignGatherGroupId(groupId), Z_OK);
}

TEST_F(TestZBALNpuCommunicatorBase, AssignGatherGroupIdMemcpyFails)
{
    opt_.groupSize = 1;
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    comm.initialized_ = true;

    AutoReleaseGroupId groupId;
    groupId.gatheredGroupInfo_.resize(1);
    groupId.gatheredGroupInfo_[0].myWorldRankId = 0;

    g_mockAclrtMemcpyRet = 1;
    EXPECT_EQ(comm.AssignGatherGroupId(groupId), Z_COMM_GROUP_H2D_FAILED);
}

/* ================================================================
 * ZBAL_UT_NUM_5. SetupProfMemory
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, SetupProfMemoryDisabled)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = false;
    EXPECT_EQ(comm.SetupProfMemory(), Z_OK);
    EXPECT_EQ(comm.groupInfo_.devMemoryForProfiling, 0u);
}

TEST_F(TestZBALNpuCommunicatorBase, SetupProfMemoryTracingCountTooLowClamped)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_10000;
    EXPECT_EQ(comm.SetupProfMemory(), Z_OK);
    EXPECT_NE(comm.perfHostMemory_, nullptr);
    EXPECT_NE(comm.groupInfo_.devMemoryForProfiling, 0u);
    EXPECT_NE(comm.groupInfo_.hostMemoryForProfiling, 0u);
}

TEST_F(TestZBALNpuCommunicatorBase, SetupProfMemoryTracingCountTooHighClamped)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_999999;
    EXPECT_EQ(comm.SetupProfMemory(), Z_OK);
    EXPECT_NE(comm.perfHostMemory_, nullptr);
    EXPECT_NE(comm.groupInfo_.devMemoryForProfiling, 0u);
}

TEST_F(TestZBALNpuCommunicatorBase, SetupProfMemoryValidTracingCount)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_30000;
    EXPECT_EQ(comm.SetupProfMemory(), Z_OK);
    EXPECT_NE(comm.perfHostMemory_, nullptr);
    EXPECT_NE(comm.groupInfo_.devMemoryForProfiling, 0u);
    EXPECT_EQ(comm.groupInfo_.tracePointPerCore, 30000u);
}

TEST_F(TestZBALNpuCommunicatorBase, SetupProfMemoryMallocHostFails)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_20480;
    g_mockAclrtMallocHostRet = 1;
    EXPECT_EQ(comm.SetupProfMemory(), 1);
    EXPECT_EQ(comm.perfHostMemory_, nullptr);
}

TEST_F(TestZBALNpuCommunicatorBase, SetupProfMemoryMemsetFails)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_20480;
    g_mockAclrtMemsetProfRet = 1;
    EXPECT_EQ(comm.SetupProfMemory(), 1);
    EXPECT_EQ(comm.perfHostMemory_, nullptr);
}

TEST_F(TestZBALNpuCommunicatorBase, SetupProfMemoryHostRegisterFails)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_20480;
    g_mockAclrtHostRegisterRet = 1;
    EXPECT_EQ(comm.SetupProfMemory(), 1);
    EXPECT_EQ(comm.perfHostMemory_, nullptr);
}

/* ================================================================
 * ZBAL_UT_NUM_6. DestroyProfMemory
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, DestroyProfMemoryWhenNull)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    comm.perfHostMemory_ = nullptr;
    comm.DestroyProfMemory();
    EXPECT_EQ(comm.perfHostMemory_, nullptr);
}

TEST_F(TestZBALNpuCommunicatorBase, DestroyProfMemoryWhenNonNull)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_20480;
    comm.SetupProfMemory();
    ASSERT_NE(comm.perfHostMemory_, nullptr);
    comm.DestroyProfMemory();
    EXPECT_EQ(comm.perfHostMemory_, nullptr);
}

/* ================================================================
 * ZBAL_UT_NUM_7. DumpProfilingTrace / SignalDumpTrace
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, DumpProfilingTraceNoProfMemory)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    comm.groupInfo_.hostMemoryForProfiling = 0;
    comm.DumpProfilingTrace();
    SUCCEED();
}

TEST_F(TestZBALNpuCommunicatorBase, DumpProfilingTraceOpenFailsEmptyDir)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_20480;
    comm.SetupProfMemory();
    ASSERT_NE(comm.perfHostMemory_, nullptr);
    ASSERT_NE(comm.groupInfo_.hostMemoryForProfiling, 0u);

    comm.groupInfo_.peerGroupRank2WorldRank[0] = ZBAL_UT_NUM_5;
    EnvHelper::PROF_DIR = "";
    comm.DumpProfilingTrace();
    SUCCEED();
}

TEST_F(TestZBALNpuCommunicatorBase, DumpProfilingTraceEmptyMemoryLoops)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_20480;
    comm.SetupProfMemory();
    ASSERT_NE(comm.perfHostMemory_, nullptr);
    ASSERT_NE(comm.groupInfo_.hostMemoryForProfiling, 0u);
    ASSERT_GT(comm.groupInfo_.tracePointPerCore, 16u);

    comm.groupInfo_.peerGroupRank2WorldRank[0] = ZBAL_UT_NUM_3;
    comm.groupInfo_.groupIndex = 1;
    EnvHelper::PROF_DIR = "/tmp";
    comm.DumpProfilingTrace();
    SUCCEED();
}

TEST_F(TestZBALNpuCommunicatorBase, SignalDumpTrace)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    comm.groupInfo_.hostMemoryForProfiling = 0;
    comm.SignalDumpTrace();
    SUCCEED();
}

/* ================================================================
 * ZBAL_UT_NUM_8. UnInitialize
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, UnInitializeWhenNotInitialized)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    comm.initialized_ = false;
    comm.UnInitialize();
    EXPECT_FALSE(comm.initialized_);
}

TEST_F(TestZBALNpuCommunicatorBase, UnInitializeWhenInitializedNoProf)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    comm.initialized_ = true;
    comm.groupInfo_.hostMemoryForProfiling = 0;
    comm.UnInitialize();
    EXPECT_FALSE(comm.initialized_);
}

TEST_F(TestZBALNpuCommunicatorBase, UnInitializeWhenInitializedWithProf)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EnvHelper::PROF_ENABLED = true;
    EnvHelper::PROF_TRACING_MAX_COUNT = ZBAL_UT_NUM_20480;
    comm.SetupProfMemory();
    comm.initialized_ = true;
    EXPECT_NE(comm.perfHostMemory_, nullptr);
    comm.groupInfo_.hostMemoryForProfiling = 0;
    comm.UnInitialize();
    EXPECT_FALSE(comm.initialized_);
    EXPECT_EQ(comm.perfHostMemory_, nullptr);
}

/* ================================================================
 * ZBAL_UT_NUM_9. Collective ops delegate to stubs
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, CollectiveOpsDelegateToStubs)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    char buf[ZBAL_UT_NUM_64] = {};

    EXPECT_EQ(comm.AllReduce(buf, buf, buf, 1, ZBAL_DATA_TYPE_FP32, ZBAL_REDUCE_SUM, nullptr), Z_OK);
    EXPECT_EQ(comm.ReduceScatter(buf, buf, 1, ZBAL_DATA_TYPE_FP32, ZBAL_REDUCE_SUM, nullptr), Z_OK);
    EXPECT_EQ(comm.AllGather(buf, buf, 1, ZBAL_DATA_TYPE_FP32, nullptr), Z_OK);
    EXPECT_EQ(comm.AlltoAllV(buf, buf, buf, buf, buf, ZBAL_DATA_TYPE_FP32, nullptr), Z_OK);
    EXPECT_EQ(comm.Broadcast(buf, 1, ZBAL_DATA_TYPE_FP32, 0, nullptr), Z_OK);
    EXPECT_EQ(comm.Scatter(buf, buf, 1, ZBAL_DATA_TYPE_FP32, 0, nullptr), Z_OK);
    EXPECT_EQ(comm.Scatter(buf, buf, ZBAL_UT_NUM_4, ZBAL_DATA_TYPE_INT32, 0, nullptr), Z_OK);
    EXPECT_EQ(comm.Scatter(buf, buf, ZBAL_UT_NUM_8, ZBAL_DATA_TYPE_FP16, 0, nullptr), Z_OK);
    EXPECT_EQ(comm.Barrier(nullptr), Z_OK);
    EXPECT_EQ(comm.Send(buf, ZBAL_DATA_TYPE_FP32, 0, nullptr), Z_OK);
    EXPECT_EQ(comm.Recv(buf, 1, ZBAL_DATA_TYPE_FP32, 0, nullptr), Z_OK);
}

/* ================================================================
 * ZBAL_UT_NUM_10. Dispatch/Combine ops delegate to stubs
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, DispatchCombineOpsDelegateToStubs)
{
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    zbal_tensor_info_t info;
    memset(&info, 0, sizeof(info));

    EXPECT_EQ(comm.DispatchNormalLayout(&info, 1, 1, 1, &info, &info, &info, &info, nullptr, 0), Z_OK);
    EXPECT_EQ(comm.DispatchLowLatency(&info, &info, ZBAL_UT_NUM_16, ZBAL_UT_NUM_2, 1, 0, 1, ZBAL_UT_NUM_42, 0, &info,
                                      &info, &info, &info, &info, &info, &info, nullptr, 0),
              Z_OK);
    EXPECT_EQ(comm.CombineLowLatency(&info, &info, &info, &info, &info, &info, ZBAL_UT_NUM_16, nullptr, 0), Z_OK);
}

/* ================================================================
 * ZBAL_UT_NUM_11. DispatchNormalNotify - factor validation
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, DispatchNormalNotifyFactorValidation)
{
    struct FactorCase {
        const char *desc;
        const char *factorHigh;
        const char *factorLow;
        bool expectInvalid;
    };

    FactorCase cases[] = {
        {"default factors", nullptr, nullptr, false}, {"custom valid", "2.0", "1.0", false},
        {"borderline valid", "1.2", "1.0", false},    {"factorHigh too low", "1.0", nullptr, true},
        {"factorLow too low", nullptr, "0.5", true},  {"both invalid", "1.0", "0.3", true},
        {"extreme valid", "10.0", "0.95", false},
    };

    for (auto &tc : cases) {
        if (tc.factorHigh) {
            ::setenv("DEEPEP_BALANCE_FACTOR_HIGH", tc.factorHigh, 1);
        }
        if (tc.factorLow) {
            ::setenv("DEEPEP_BALANCE_FACTOR_LOW", tc.factorLow, 1);
        }
        NpuCommunicatorAIV comm(opt_, false, nullptr);
        comm.ConstructCommGroupInfo(opt_);
        zbal_tensor_info_t info;
        memset(&info, 0, sizeof(info));

        auto result = comm.DispatchNormalNotify(&info, 1, 1, &info, &info, &info, &info, &info, nullptr, 0);

        if (tc.expectInvalid) {
            EXPECT_EQ(result, Z_INVALID_PARAM) << "case: " << tc.desc;
        } else {
            EXPECT_EQ(result, Z_OK) << "case: " << tc.desc;
        }

        ClearEnvRebalance();
    }

    /* boundary: nullptr tensor info */
    NpuCommunicatorAIV comm(opt_, false, nullptr);
    comm.ConstructCommGroupInfo(opt_);
    EXPECT_EQ(comm.DispatchNormalNotify(nullptr, 0, 1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0), Z_OK);
}

/* ================================================================
 * ZBAL_UT_NUM_12. DispatchNormal / CombineNormal - rebalance env
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, DispatchNormalRebalanceEnv)
{
    const char *envVals[] = {nullptr, "0", "1", "42"};
    zbal_tensor_info_t info;
    memset(&info, 0, sizeof(info));

    for (auto *val : envVals) {
        if (val) {
            ::setenv("DEEPEP_ENABLE_REBALANCE", val, 1);
        } else {
            ::unsetenv("DEEPEP_ENABLE_REBALANCE");
        }

        NpuCommunicatorAIV comm(opt_, false, nullptr);
        comm.ConstructCommGroupInfo(opt_);
        EXPECT_EQ(comm.DispatchNormal(&info, &info, &info, &info, &info, 1, NO_QUANT, &info, &info, nullptr, 0), Z_OK)
            << "DispatchNormal env=" << (val ? val : "<unset>");
    }

    const char *combineVals[] = {nullptr, "1"};
    for (auto *val : combineVals) {
        if (val) {
            ::setenv("DEEPEP_ENABLE_REBALANCE", val, 1);
        } else {
            ::unsetenv("DEEPEP_ENABLE_REBALANCE");
        }
        NpuCommunicatorAIV comm(opt_, false, nullptr);
        comm.ConstructCommGroupInfo(opt_);
        EXPECT_EQ(comm.CombineNormal(&info, &info, &info, &info, &info, &info, 1, &info, nullptr, 0), Z_OK)
            << "CombineNormal env=" << (val ? val : "<unset>");
    }

    ClearEnvRebalance();
}

/* ================================================================
 * ZBAL_UT_NUM_13. opRunTimes_ static counter
 * ================================================================ */

TEST_F(TestZBALNpuCommunicatorBase, OpRunTimesStaticCounter)
{
    NpuCommunicatorBase::opRunTimes_ = 0;
    EXPECT_EQ(NpuCommunicatorBase::opRunTimes_, 0u);
    NpuCommunicatorBase::opRunTimes_ = ZBAL_UT_NUM_5;
    EXPECT_EQ(NpuCommunicatorBase::opRunTimes_, 5u);
    NpuCommunicatorBase::opRunTimes_ = 0;
}
