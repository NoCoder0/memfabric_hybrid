/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>

namespace ock {
namespace mf {
namespace {
enum class DcmiMockMode {
    SUCCESS,
    OPEN_FAILED,
    INIT_SYMBOL_FAILED,
    AFFINITY_SYMBOL_FAILED,
    INIT_FAILED,
    AFFINITY_FAILED,
};

constexpr int32_t DCMI_TEST_ERROR = -1;
constexpr int32_t TEST_DEVICE_ID = 3;
constexpr const char *TEST_CPU_LIST = "0-3";

DcmiMockMode gDcmiMockMode = DcmiMockMode::SUCCESS;
std::string gDcmiLibraryName;
int32_t gDcmiDeviceId = -1;
uint32_t gDcmiCloseCount = 0U;
int32_t gDcmiFakeHandle = 0;

int32_t DcmiInitForTest()
{
    return gDcmiMockMode == DcmiMockMode::INIT_FAILED ? DCMI_TEST_ERROR : 0;
}

int32_t DcmiGetAffinityCpuInfoForTest(int32_t deviceId, char *cpuList, int32_t *length)
{
    gDcmiDeviceId = deviceId;
    if (gDcmiMockMode == DcmiMockMode::AFFINITY_FAILED) {
        return DCMI_TEST_ERROR;
    }
    std::strncpy(cpuList, TEST_CPU_LIST, static_cast<size_t>(*length));
    return 0;
}

void *DlopenForDcmiTest(const char *libraryName, int)
{
    gDcmiLibraryName = libraryName;
    return gDcmiMockMode == DcmiMockMode::OPEN_FAILED ? nullptr : &gDcmiFakeHandle;
}

void *DlsymForDcmiTest(void *, const char *symbolName)
{
    if (std::strcmp(symbolName, "dcmiv2_init") == 0) {
        if (gDcmiMockMode == DcmiMockMode::INIT_SYMBOL_FAILED) {
            return nullptr;
        }
        return reinterpret_cast<void *>(DcmiInitForTest);
    }
    if (std::strcmp(symbolName, "dcmiv2_get_affinity_cpu_info_by_dev_id") == 0) {
        if (gDcmiMockMode == DcmiMockMode::AFFINITY_SYMBOL_FAILED) {
            return nullptr;
        }
        return reinterpret_cast<void *>(DcmiGetAffinityCpuInfoForTest);
    }
    return nullptr;
}

const char *DlerrorForDcmiTest()
{
    return "DCMI mock error";
}

int32_t DlcloseForDcmiTest(void *)
{
    ++gDcmiCloseCount;
    return 0;
}
} // namespace
} // namespace mf
} // namespace ock

#define DlHalApi DlHalApiForDcmiTest
#define private  public
#include "dl_hal_api.h"
#undef private

#define dlopen  DlopenForDcmiTest
#define dlsym   DlsymForDcmiTest
#define dlerror DlerrorForDcmiTest
#define dlclose DlcloseForDcmiTest
#include "dl_hal_api.cpp"
#undef dlclose
#undef dlerror
#undef dlsym
#undef dlopen
#undef DlHalApi

namespace ock {
namespace mf {
class DlHalApiDcmiTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        gDcmiMockMode = DcmiMockMode::SUCCESS;
        gDcmiLibraryName.clear();
        gDcmiDeviceId = -1;
        gDcmiCloseCount = 0U;
        DlHalApiForDcmiTest::dcmiHandle = nullptr;
        DlHalApiForDcmiTest::pDcmiInit = nullptr;
        DlHalApiForDcmiTest::pDcmiGetAffinityCpuInfo = nullptr;
    }
};

TEST_F(DlHalApiDcmiTest, LoadDcmiLibraryFailsWhenLibraryMissing)
{
    gDcmiMockMode = DcmiMockMode::OPEN_FAILED;

    EXPECT_EQ(DlHalApiForDcmiTest::LoadDcmiLibrary(), BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(gDcmiLibraryName, "libdcmi.so");
}

TEST_F(DlHalApiDcmiTest, LoadDcmiLibraryFailsWhenInitSymbolMissing)
{
    gDcmiMockMode = DcmiMockMode::INIT_SYMBOL_FAILED;

    EXPECT_EQ(DlHalApiForDcmiTest::LoadDcmiLibrary(), BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(gDcmiCloseCount, 1U);
}

TEST_F(DlHalApiDcmiTest, LoadDcmiLibraryFailsWhenAffinitySymbolMissing)
{
    gDcmiMockMode = DcmiMockMode::AFFINITY_SYMBOL_FAILED;

    EXPECT_EQ(DlHalApiForDcmiTest::LoadDcmiLibrary(), BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(gDcmiCloseCount, 1U);
}

TEST_F(DlHalApiDcmiTest, LoadDcmiLibraryFailsWhenInitFails)
{
    gDcmiMockMode = DcmiMockMode::INIT_FAILED;

    EXPECT_EQ(DlHalApiForDcmiTest::LoadDcmiLibrary(), DCMI_TEST_ERROR);
    EXPECT_EQ(gDcmiCloseCount, 1U);
}

TEST_F(DlHalApiDcmiTest, DcmiGetAffinityCpuInfoHandlesSuccessAndFailure)
{
    std::string cpuList;
    EXPECT_EQ(DlHalApiForDcmiTest::DcmiGetAffinityCpuInfo(TEST_DEVICE_ID, cpuList), BM_OK);
    EXPECT_EQ(cpuList, TEST_CPU_LIST);
    EXPECT_EQ(gDcmiDeviceId, TEST_DEVICE_ID);
    EXPECT_EQ(gDcmiCloseCount, 1U);

    gDcmiMockMode = DcmiMockMode::AFFINITY_FAILED;
    EXPECT_EQ(DlHalApiForDcmiTest::DcmiGetAffinityCpuInfo(TEST_DEVICE_ID, cpuList), DCMI_TEST_ERROR);
    EXPECT_EQ(gDcmiCloseCount, 2U);
}
} // namespace mf
} // namespace ock
