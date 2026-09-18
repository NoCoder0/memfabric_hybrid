/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <cstdlib>

namespace {
int g_openCount;
int g_closeCount;
bool g_missingSymbol;
int g_libraryToken;

void *TestDlopen(const char *, int)
{
    ++g_openCount;
    return &g_libraryToken;
}

int TestDlclose(void *)
{
    ++g_closeCount;
    return 0;
}

void *TestDlsym(void *, const char *)
{
    return g_missingSymbol ? nullptr : &g_libraryToken;
}
} // namespace

// Compile the actual device loader with a fake dynamic loader and no device logging.
#define MF_HYBM_OPS_HYBM_KERNEL_HYBM_KERNEL_LOG_H
#define HYBM_LOGE(...) ((void)0)
#define HYBM_LOGW(...) ((void)0)
#define HYBM_LOGI(...) ((void)0)
#define HYBM_LOGD(...) ((void)0)
#define dlopen         TestDlopen
#define dlclose        TestDlclose
#define dlsym          TestDlsym
// Other UTs include the same source. Keep this TU's C entry points private to
// the loader tests by giving them distinct names, including the batch entry.
#define HybmBatchWrite    LoaderTestHybmBatchWrite
#define HybmBatchRead     LoaderTestHybmBatchRead
#define HybmBatchTransfer LoaderTestHybmBatchTransfer
#include "../../../../src/hybm/ops/hybm_kernel/hybm_batch_transfer.cc"
#undef HybmBatchWrite
#undef HybmBatchRead
#undef HybmBatchTransfer
#undef dlopen
#undef dlclose
#undef dlsym

class HybmDeviceLoaderTest : public testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_EQ(ResetHcommSymbols(), BM_OK);
        g_openCount = 0;
        g_closeCount = 0;
        g_missingSymbol = false;
    }

    void TearDown() override
    {
        EXPECT_EQ(ResetHcommSymbols(), BM_OK);
    }
};

TEST_F(HybmDeviceLoaderTest, OwnerClosesHandleAtEndOfLifetime)
{
    {
        HcommLibrary library;
        EXPECT_EQ(library.Result(), BM_OK);
        EXPECT_EQ(library.Result(), BM_OK);
        EXPECT_EQ(g_openCount, 1);
        EXPECT_EQ(g_closeCount, 0);
    }
    EXPECT_EQ(g_closeCount, 1);
    EXPECT_EQ(g_hcommWriteOnThread, nullptr);
    EXPECT_EQ(g_hcommHandle, nullptr);
    EXPECT_EQ(ResetHcommSymbols(), BM_OK);
    EXPECT_EQ(g_closeCount, 1);
}

TEST_F(HybmDeviceLoaderTest, MissingRequiredSymbolDoesNotDoubleCloseAtDestruction)
{
    g_missingSymbol = true;
    {
        HcommLibrary library;
        EXPECT_EQ(library.Result(), BM_DL_FUNCTION_FAILED);
        EXPECT_EQ(g_openCount, 1);
        EXPECT_EQ(g_closeCount, 1);
    }
    EXPECT_EQ(g_closeCount, 1);
}

TEST_F(HybmDeviceLoaderTest, StaticOwnerReusesHandleAndClosesOnNormalExit)
{
    EXPECT_EXIT(
        {
            // Register before the static owner so this check runs after its destructor.
            std::atexit(
                []() { std::_Exit(g_openCount == 1 && g_closeCount == 1 && g_hcommHandle == nullptr ? 0 : 1); });
            if (EnsureHcommLoaded() != BM_OK || EnsureHcommLoaded() != BM_OK || g_closeCount != 0) {
                std::_Exit(2);
            }
            std::exit(0);
        },
        testing::ExitedWithCode(0), "");
}
