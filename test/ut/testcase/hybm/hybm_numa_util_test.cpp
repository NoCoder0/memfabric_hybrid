/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
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
#include <mockcpp/mockcpp.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "dl_acl_api.h"
#include "dl_hal_api.h"

namespace ock {
namespace mf {
constexpr int32_t DCMI_DEVICE_ID_NOT_SET = -1;
AscendSocType gSocTypeForNumaUtilTest = AscendSocType::ASCEND_UNKNOWN;
uint32_t gDcmiCallCountForNumaUtilTest = 0;
int32_t gDcmiDeviceIdForNumaUtilTest = DCMI_DEVICE_ID_NOT_SET;
Result gDcmiResultForNumaUtilTest = BM_DL_FUNCTION_FAILED;
std::string gDcmiCpuListForNumaUtilTest;
std::unordered_map<std::string, std::string> gSysfsValuesForNumaUtilTest;

class DlAclApiForNumaUtilTest {
public:
    static AscendSocType GetAscendSocType()
    {
        return gSocTypeForNumaUtilTest;
    }
};

class DlHalApiForNumaUtilTest {
public:
    static Result DcmiGetAffinityCpuInfo(int32_t deviceId, std::string &cpuList)
    {
        ++gDcmiCallCountForNumaUtilTest;
        gDcmiDeviceIdForNumaUtilTest = deviceId;
        cpuList = gDcmiCpuListForNumaUtilTest;
        return gDcmiResultForNumaUtilTest;
    }
};
} // namespace mf
} // namespace ock

#define HybmNumaUtil     HybmNumaUtilForTest
#define CpuAffinityGuard CpuAffinityGuardForTest
#define DlAclApi         DlAclApiForNumaUtilTest
#define DlHalApi         DlHalApiForNumaUtilTest
#include "hybm_numa_util.cpp"
#undef DlHalApi
#undef DlAclApi
#undef HybmNumaUtil
#undef CpuAffinityGuard

namespace ock {
namespace mf {
bool ReadFirstLineForNumaUtilTest(const std::string &path, std::string &value)
{
    const auto iter = gSysfsValuesForNumaUtilTest.find(path);
    if (iter == gSysfsValuesForNumaUtilTest.end()) {
        return false;
    }
    value = iter->second;
    return !value.empty();
}

class HybmNumaUtilTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
        gSocTypeForNumaUtilTest = AscendSocType::ASCEND_UNKNOWN;
        gDcmiCallCountForNumaUtilTest = 0;
        gDcmiDeviceIdForNumaUtilTest = DCMI_DEVICE_ID_NOT_SET;
        gDcmiResultForNumaUtilTest = BM_DL_FUNCTION_FAILED;
        gDcmiCpuListForNumaUtilTest.clear();
        gSysfsValuesForNumaUtilTest = {
            {"/sys/devices/system/cpu/online", "0-5"},
            {"/sys/devices/system/node/node7/cpulist", "2-3"},
            {"/sys/devices/system/cpu/cpu0/topology/physical_package_id", "0"},
            {"/sys/devices/system/cpu/cpu1/topology/physical_package_id", "0"},
            {"/sys/devices/system/cpu/cpu2/topology/physical_package_id", "1"},
            {"/sys/devices/system/cpu/cpu3/topology/physical_package_id", "1"},
            {"/sys/devices/system/cpu/cpu4/topology/physical_package_id", "2"},
            {"/sys/devices/system/cpu/cpu5/topology/physical_package_id", "2"},
        };
        MOCKER(ReadFirstLineImpl).stubs().will(invoke(ReadFirstLineForNumaUtilTest));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }
};

// ========================
// Test Case 1: GetNumaCpuListImpl - Invalid Node
// ========================
TEST_F(HybmNumaUtilTest, GetNumaCpuListImpl_MissingNode)
{
    constexpr uint32_t missingNumaIndex = 8U;
    std::vector<int32_t> cpus{1, 2, 3};

    EXPECT_FALSE(GetNumaCpuListImpl(missingNumaIndex, cpus));
}

// ========================
// Test Case 2: ParseCpuListImpl - Success
// ========================
TEST_F(HybmNumaUtilTest, ParseCpuListImpl_Success)
{
    std::vector<int32_t> cpus;

    EXPECT_TRUE(ParseCpuListImpl("0-2,4,2", cpus));
    EXPECT_EQ(cpus, (std::vector<int32_t>{0, 1, 2, 4}));
}

// ========================
// Test Case 3: GetNumaBindPolicyInfo - Performance Disabled
// ========================
TEST_F(HybmNumaUtilTest, GetNumaBindPolicyInfo_PerformanceDisabled)
{
    const NumaBindPolicyInfo info = HybmNumaUtilForTest::GetNumaBindPolicyInfo(0U);

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.policy, NumaBindPolicy::OFF);
    EXPECT_EQ(info.numaIndex, 0U);
    EXPECT_TRUE(info.numaCpus.empty());
    EXPECT_TRUE(info.socketCpus.empty());
}

// ========================
// Test Case 4: GetNumaBindPolicyInfo - Non-A5 Auto Policy
// ========================
TEST_F(HybmNumaUtilTest, GetNumaBindPolicyInfo_NonA5AutoPolicy)
{
    gSocTypeForNumaUtilTest = AscendSocType::ASCEND_UNKNOWN;
    const uint32_t flags = (1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX) | HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG;

    const NumaBindPolicyInfo info = HybmNumaUtilForTest::GetNumaBindPolicyInfo(flags);

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.policy, NumaBindPolicy::AUTO);
    EXPECT_EQ(info.numaIndex, HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG);
    EXPECT_EQ(gDcmiCallCountForNumaUtilTest, 0U);
}

// ========================
// Test Case 5: GetNumaBindPolicyInfo - Non-A5 Manual Policy
// ========================
TEST_F(HybmNumaUtilTest, GetNumaBindPolicyInfo_NonA5ManualPolicy)
{
    gSocTypeForNumaUtilTest = AscendSocType::ASCEND_UNKNOWN;
    const uint32_t numaIndex = 3U;
    const uint32_t flags = (1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX) | numaIndex;

    const NumaBindPolicyInfo info = HybmNumaUtilForTest::GetNumaBindPolicyInfo(flags);

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.policy, NumaBindPolicy::MANUAL);
    EXPECT_EQ(info.numaIndex, numaIndex);
    EXPECT_EQ(gDcmiCallCountForNumaUtilTest, 0U);
}

// ========================
// Test Case 6: GetNumaBindPolicyInfo - A5 Auto Policy DCMI Failed
// ========================
TEST_F(HybmNumaUtilTest, GetNumaBindPolicyInfo_A5AutoPolicyDcmiFailed)
{
    gSocTypeForNumaUtilTest = AscendSocType::ASCEND_950;
    const uint32_t flags = (1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX) | HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG;

    const NumaBindPolicyInfo info = HybmNumaUtilForTest::GetNumaBindPolicyInfo(flags);

    EXPECT_FALSE(info.valid);
    EXPECT_EQ(info.policy, NumaBindPolicy::OFF);
    EXPECT_TRUE(info.numaCpus.empty());
    EXPECT_TRUE(info.socketCpus.empty());
    EXPECT_EQ(gDcmiCallCountForNumaUtilTest, 1U);
}

// ========================
// Test Case 7: GetNumaBindPolicyInfo - A5 Auto Policy Success
// ========================
TEST_F(HybmNumaUtilTest, GetNumaBindPolicyInfo_A5AutoPolicySuccess)
{
    constexpr int32_t deviceId = 7;
    gSocTypeForNumaUtilTest = AscendSocType::ASCEND_950;
    gDcmiResultForNumaUtilTest = BM_OK;
    gDcmiCpuListForNumaUtilTest = "1,4";
    const uint32_t flags = (1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX) | HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG;

    const NumaBindPolicyInfo info = HybmNumaUtilForTest::GetNumaBindPolicyInfo(flags, deviceId);

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.policy, NumaBindPolicy::AUTO);
    EXPECT_EQ(info.numaIndex, HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG);
    EXPECT_EQ(info.numaCpus, (std::vector<int32_t>{1, 4}));
    EXPECT_EQ(info.socketCpus, (std::vector<int32_t>{0, 1, 4, 5}));
    EXPECT_EQ(gDcmiCallCountForNumaUtilTest, 1U);
    EXPECT_EQ(gDcmiDeviceIdForNumaUtilTest, deviceId);
}

// ========================
// Test Case 8: GetNumaBindPolicyInfo - A5 Manual Policy Success
// ========================
TEST_F(HybmNumaUtilTest, GetNumaBindPolicyInfo_A5ManualPolicySuccess)
{
    constexpr uint32_t numaIndex = 7U;
    gSocTypeForNumaUtilTest = AscendSocType::ASCEND_950;
    const uint32_t flags = (1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX) | numaIndex;

    const NumaBindPolicyInfo info = HybmNumaUtilForTest::GetNumaBindPolicyInfo(flags);

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.policy, NumaBindPolicy::MANUAL);
    EXPECT_EQ(info.numaIndex, numaIndex);
    EXPECT_EQ(info.numaCpus, (std::vector<int32_t>{2, 3}));
    EXPECT_EQ(info.socketCpus, (std::vector<int32_t>{2, 3}));
    EXPECT_EQ(gDcmiCallCountForNumaUtilTest, 0U);
    EXPECT_EQ(gDcmiDeviceIdForNumaUtilTest, DCMI_DEVICE_ID_NOT_SET);
}

// ========================
// Test Case 9: GetNumaBindPolicyInfo - A5 Manual Policy NUMA Failed
// ========================
TEST_F(HybmNumaUtilTest, GetNumaBindPolicyInfo_A5ManualPolicyNumaFailed)
{
    gSocTypeForNumaUtilTest = AscendSocType::ASCEND_950;
    constexpr uint32_t invalidNumaIndex = 126U;
    const uint32_t flags = (1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX) | invalidNumaIndex;

    const NumaBindPolicyInfo info = HybmNumaUtilForTest::GetNumaBindPolicyInfo(flags);

    EXPECT_FALSE(info.valid);
    EXPECT_EQ(info.policy, NumaBindPolicy::OFF);
    EXPECT_TRUE(info.numaCpus.empty());
    EXPECT_TRUE(info.socketCpus.empty());
    EXPECT_EQ(gDcmiCallCountForNumaUtilTest, 0U);
}
} // namespace mf
} // namespace ock
