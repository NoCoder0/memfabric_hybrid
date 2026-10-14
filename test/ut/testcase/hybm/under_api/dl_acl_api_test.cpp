/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
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

#define private public
#include "dl_acl_api.h"
#undef private

using namespace ock::mf;

namespace {

struct DlAclApiFnGuard {
    aclrtGetVersionFunc oldGetVersion{DlAclApi::pAclrtGetVersion};
    aclrtGetSocNameFunc oldGetSocName{DlAclApi::pAclrtGetSocName};
    aclrtMemcpyFunc oldMemcpy{DlAclApi::pAclrtMemcpy};
    aclrtFreeFunc oldFree{DlAclApi::pAclrtFree};
    aclrtStreamAbortFunc oldStreamAbort{DlAclApi::pAclrtStreamAbort};

    ~DlAclApiFnGuard()
    {
        DlAclApi::pAclrtGetVersion = oldGetVersion;
        DlAclApi::pAclrtGetSocName = oldGetSocName;
        DlAclApi::pAclrtMemcpy = oldMemcpy;
        DlAclApi::pAclrtFree = oldFree;
        DlAclApi::pAclrtStreamAbort = oldStreamAbort;
    }
};

void *g_lastAbortedStream = nullptr;

int FakeStreamAbort(void *stream)
{
    g_lastAbortedStream = stream;
    return 0;
}

TEST(DlAclApiTest, AclrtGetVersion_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtGetVersion = nullptr;

    int32_t major = 0, minor = 0, patch = 0;
    auto ret = DlAclApi::AclrtGetVersion(&major, &minor, &patch);
    ASSERT_EQ(ret, -1);
}

TEST(DlAclApiTest, AclrtGetVersion_Success)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtGetVersion = [](int32_t *major, int32_t *minor, int32_t *patch) -> int32_t {
        *major = 1;
        *minor = 17;
        *patch = 0;
        return 0;
    };

    int32_t major = 0, minor = 0, patch = 0;
    auto ret = DlAclApi::AclrtGetVersion(&major, &minor, &patch);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(major, 1);
    ASSERT_EQ(minor, 17);
    ASSERT_EQ(patch, 0);
}

TEST(DlAclApiTest, AclrtMemcpy_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtMemcpy = nullptr;

    auto ret = DlAclApi::AclrtMemcpy(nullptr, 0, nullptr, 0, 0);
    ASSERT_NE(ret, 0);
}

TEST(DlAclApiTest, AclrtFree_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtFree = nullptr;

    auto ret = DlAclApi::AclrtFree(nullptr);
    ASSERT_NE(ret, 0);
}

// ── Bulk null-func-ptr tests for all DlAclApi inline wrappers ──

TEST(DlAclApiTest, AclrtSetDevice_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtSetDevice = nullptr;
    // force=true bypasses AclrtGetDevice check → directly returns BM_UNDER_API_UNLOAD
    auto ret = DlAclApi::AclrtSetDevice(0, true);
    ASSERT_NE(ret, 0);
}

TEST(DlAclApiTest, AclrtSetDevice_SameDevice)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtGetDevice = [](int32_t *id) -> int32_t {
        *id = 0;
        return 0;
    };
    DlAclApi::pAclrtSetDevice = [](int32_t) -> int32_t { return 0; };
    ASSERT_EQ(DlAclApi::AclrtSetDevice(0), 0);
}

TEST(DlAclApiTest, AclrtCreateStream_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtCreateStream = nullptr;
    void *stream = nullptr;
    ASSERT_NE(DlAclApi::AclrtCreateStream(&stream), 0);
}

TEST(DlAclApiTest, AclrtDestroyStream_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtDestroyStream = nullptr;
    ASSERT_NE(DlAclApi::AclrtDestroyStream(nullptr), 0);
}

TEST(DlAclApiTest, AclrtMallocHost_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtMallocHost = nullptr;
    void *ptr = nullptr;
    ASSERT_NE(DlAclApi::AclrtMallocHost(&ptr, 1024), 0);
}

TEST(DlAclApiTest, AclrtFreeHost_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtFreeHost = nullptr;
    ASSERT_NE(DlAclApi::AclrtFreeHost(nullptr), 0);
}

TEST(DlAclApiTest, AclrtMemcpyAsync_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtMemcpyAsync = nullptr;
    ASSERT_NE(DlAclApi::AclrtMemcpyAsync(nullptr, 0, nullptr, 0, 0, nullptr), 0);
}

TEST(DlAclApiTest, AclrtMemset_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtMemset = nullptr;
    ASSERT_NE(DlAclApi::AclrtMemset(nullptr, 0, 0, 0), 0);
}

TEST(DlAclApiTest, RtGetDeviceInfo_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pRtGetDeviceInfo = nullptr;
    int64_t val = 0;
    ASSERT_NE(DlAclApi::RtGetDeviceInfo(0, 0, 0, &val), 0);
}

TEST(DlAclApiTest, AclrtGetPhyDevIdByLogicDevId_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = nullptr;
    int32_t phyId = 0;
    ASSERT_NE(DlAclApi::AclrtGetPhyDevIdByLogicDevId(0, &phyId), 0);
}

TEST(DlAclApiTest, AclrtDeviceEnablePeerAccess_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtDeviceEnablePeerAccess = nullptr;
    ASSERT_NE(DlAclApi::AclrtDeviceEnablePeerAccess(0, 0), 0);
}

TEST(DlAclApiTest, AclrtSynchronizeStream_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtSynchronizeStream = nullptr;
    ASSERT_NE(DlAclApi::AclrtSynchronizeStream(nullptr), 0);
}

// aclrtStreamAbort 为 OPTIONAL 符号：老 CANN 或未支持的平台不导出，此时必须安全降级
TEST(DlAclApiTest, AclrtStreamAbort_NullFuncPtr)
{
    DlAclApiFnGuard guard;
    DlAclApi::pAclrtStreamAbort = nullptr;
    void *stream = reinterpret_cast<void *>(0xA520UL);
    ASSERT_NE(DlAclApi::AclrtStreamAbort(stream), 0);
}

TEST(DlAclApiTest, AclrtStreamAbort_ForwardsStreamToLoadedSymbol)
{
    DlAclApiFnGuard guard;
    g_lastAbortedStream = nullptr;
    DlAclApi::pAclrtStreamAbort = FakeStreamAbort;

    void *stream = reinterpret_cast<void *>(0xA520UL);
    ASSERT_EQ(DlAclApi::AclrtStreamAbort(stream), 0);
    ASSERT_EQ(g_lastAbortedStream, stream);
}

} // namespace
