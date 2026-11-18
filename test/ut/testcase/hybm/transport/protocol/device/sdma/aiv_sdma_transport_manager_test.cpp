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
#include <mockcpp/mockcpp.hpp>

#define private   public
#define protected public
#include "aiv_sdma_transport_manager.h"
#include "vendor/vendor_api.h"
#include "dl_op_api.h"
#include "vendor/vendor_api.h"
#undef private
#undef protected

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::device;

constexpr int32_t TEST_STREAM_COUNT = 2;
constexpr int32_t TEST_MALLOC_FAIL_AT_CALL = 2;
constexpr uint64_t TEST_WORKSPACE_SIZE = 1024; // 1KB workspace size for testing

namespace {
static AscendApiImpl &AscendRtApi()
{
    return *static_cast<AscendApiImpl *>(VendorApi::Api());
}

struct AclFnGuard {
    aclrtMallocFunc oldAclrtMalloc{AscendRtApi().pAclrtMalloc_};
    aclrtFreeFunc oldAclrtFree{AscendRtApi().pAclrtFree_};
    aclrtMemcpyFunc oldAclrtMemcpy{AscendRtApi().pAclrtMemcpy_};
    aclrtMemsetFunc oldAclrtMemset{AscendRtApi().pAclrtMemset_};
    aclrtCreateStreamWithConfigFunc oldAclrtCreateStreamWithConfig{AscendRtApi().pAclrtCreateStreamWithConfig_};
    aclrtDestroyStreamFunc oldAclrtDestroyStream{AscendRtApi().pAclrtDestroyStream_};
    aclrtStreamGetIdFunc oldAclrtStreamGetId{AscendRtApi().pAclrtStreamGetId_};
    aclrtCreateNotifyFunc oldAclrtCreateNotify{AscendRtApi().pAclrtCreateNotify_};
    aclrtGetNotifyIdFunc oldAclrtGetNotifyId{AscendRtApi().pAclrtGetNotifyId_};
    aclrtDestroyNotifyFunc oldAclrtDestroyNotify{AscendRtApi().pAclrtDestroyNotify_};
    aclrtGetCurrentContextFunc oldAclrtGetCurrentContext{AscendRtApi().pAclrtGetCurrentContext_};
    aclrtSynchronizeStreamFunc oldAclrtSynchronizeStream{AscendRtApi().pAclrtSynchronizeStream_};
    aclrtGetDeviceFunc oldAclrtGetDevice{AscendRtApi().pAclrtGetDevice_};
    rtGetDeviceInfoFunc oldRtGetDeviceInfo{AscendRtApi().pRtGetDeviceInfo_};
    rtStreamGetSqidFunc oldRtStreamGetSqid{AscendRtApi().pRtStreamGetSqid_};
    rtStreamGetCqidFunc oldRtStreamGetCqid{AscendRtApi().pRtStreamGetCqid_};
    aclrtSetStreamAttributeFunc oldAclrtSetStreamAttribute{AscendRtApi().pAclrtSetStreamAttribute_};

    ~AclFnGuard()
    {
        AscendRtApi().pAclrtMalloc_ = oldAclrtMalloc;
        AscendRtApi().pAclrtFree_ = oldAclrtFree;
        AscendRtApi().pAclrtMemcpy_ = oldAclrtMemcpy;
        AscendRtApi().pAclrtMemset_ = oldAclrtMemset;
        AscendRtApi().pAclrtCreateStreamWithConfig_ = oldAclrtCreateStreamWithConfig;
        AscendRtApi().pAclrtDestroyStream_ = oldAclrtDestroyStream;
        AscendRtApi().pAclrtStreamGetId_ = oldAclrtStreamGetId;
        AscendRtApi().pAclrtCreateNotify_ = oldAclrtCreateNotify;
        AscendRtApi().pAclrtGetNotifyId_ = oldAclrtGetNotifyId;
        AscendRtApi().pAclrtDestroyNotify_ = oldAclrtDestroyNotify;
        AscendRtApi().pAclrtGetCurrentContext_ = oldAclrtGetCurrentContext;
        AscendRtApi().pAclrtSynchronizeStream_ = oldAclrtSynchronizeStream;
        AscendRtApi().pAclrtGetDevice_ = oldAclrtGetDevice;
        AscendRtApi().pRtGetDeviceInfo_ = oldRtGetDeviceInfo;
        AscendRtApi().pRtStreamGetSqid_ = oldRtStreamGetSqid;
        AscendRtApi().pRtStreamGetCqid_ = oldRtStreamGetCqid;
        AscendRtApi().pAclrtSetStreamAttribute_ = oldAclrtSetStreamAttribute;
    }
};

struct DlOpApiFnGuard {
    aclCreateTensorFunc oldAclCreateTensor{DlOpApi::pAclCreateTensor};
    aclDestroyTensorFunc oldAclDestroyTensor{DlOpApi::pAclDestroyTensor};
    aclnnShmemSdmaStarsQueryGetWorkspaceSizeFunc oldAclnnShmemSdmaStarsQueryGetWorkspaceSize{
        DlOpApi::pAclnnShmemSdmaStarsQueryGetWorkspaceSize};
    aclnnShmemSdmaStarsQueryFunc oldAclnnShmemSdmaStarsQuery{DlOpApi::pAclnnShmemSdmaStarsQuery};

    ~DlOpApiFnGuard()
    {
        DlOpApi::pAclCreateTensor = oldAclCreateTensor;
        DlOpApi::pAclDestroyTensor = oldAclDestroyTensor;
        DlOpApi::pAclnnShmemSdmaStarsQueryGetWorkspaceSize = oldAclnnShmemSdmaStarsQueryGetWorkspaceSize;
        DlOpApi::pAclnnShmemSdmaStarsQuery = oldAclnnShmemSdmaStarsQuery;
    }
};

static uint64_t g_mockDeviceAddr = 0x100000000000ULL;

int32_t FakeAclrtMallocOk(void **ptr, size_t size, uint32_t flags)
{
    (void)size;
    (void)flags;
    *ptr = reinterpret_cast<void *>(g_mockDeviceAddr);
    g_mockDeviceAddr += 0x1000;
    return 0;
}

int32_t FakeAclrtMallocFail(void **ptr, size_t size, uint32_t flags)
{
    (void)ptr;
    (void)size;
    (void)flags;
    return -1;
}

int32_t FakeAclrtFreeOk(void *ptr)
{
    (void)ptr;
    return 0;
}

int32_t FakeAclrtMemcpyOk(void *dst, size_t dstSize, const void *src, size_t srcSize, uint32_t kind)
{
    (void)dst;
    (void)dstSize;
    (void)src;
    (void)srcSize;
    (void)kind;
    return 0;
}

int32_t FakeAclrtMemsetOk(void *dst, size_t destMax, int32_t value, size_t count)
{
    (void)dst;
    (void)destMax;
    (void)value;
    (void)count;
    return 0;
}

int32_t FakeAclrtCreateStreamWithConfigOk(void **stream, uint32_t prot, uint32_t config)
{
    (void)prot;
    (void)config;
    *stream = reinterpret_cast<void *>(0xABCD0000ULL + reinterpret_cast<uintptr_t>(stream));
    return 0;
}

int32_t FakeAclrtCreateStreamWithConfigFail(void **stream, uint32_t prot, uint32_t config)
{
    (void)stream;
    (void)prot;
    (void)config;
    return -1;
}

int32_t FakeAclrtDestroyStreamOk(void *stream)
{
    (void)stream;
    return 0;
}

int32_t FakeAclrtStreamGetIdOk(void *stream, int32_t *streamId)
{
    (void)stream;
    *streamId = 1;
    return 0;
}

int32_t FakeAclrtCreateNotifyOk(void **notify, uint64_t flag)
{
    (void)flag;
    *notify = reinterpret_cast<void *>(0xEEEE0000ULL);
    return 0;
}

int32_t FakeAclrtCreateNotifyFail(void **notify, uint64_t flag)
{
    (void)notify;
    (void)flag;
    return -1;
}

int32_t FakeAclrtGetNotifyIdOk(void *notify, uint32_t *notifyId)
{
    (void)notify;
    *notifyId = 1;
    return 0;
}

int32_t FakeAclrtDestroyNotifyOk(void *notify)
{
    (void)notify;
    return 0;
}

int32_t FakeAclrtGetCurrentContextOk(void **context)
{
    *context = reinterpret_cast<void *>(0xCCCC0000ULL);
    return 0;
}

int32_t FakeAclrtGetCurrentContextFail(void **context)
{
    (void)context;
    return -1;
}

int32_t FakeAclrtSynchronizeStreamOk(void *stream)
{
    (void)stream;
    return 0;
}

int32_t FakeAclrtGetDeviceOk(int32_t *deviceId)
{
    *deviceId = 0;
    return 0;
}

int32_t FakeRtGetDeviceInfoOk(int32_t devId, int32_t deviceType, int32_t infoType, int64_t *value)
{
    (void)devId;
    (void)deviceType;
    (void)infoType;
    *value = 0;
    return 0;
}

int32_t FakeRtStreamGetSqidOk(void *stream, uint32_t *sqId)
{
    (void)stream;
    *sqId = 1;
    return 0;
}

int32_t FakeRtStreamGetCqidOk(void *stream, uint32_t *cqId, uint32_t *logicCqId)
{
    (void)stream;
    *cqId = 1;
    *logicCqId = 1;
    return 0;
}

int32_t FakeAclrtSetStreamAttributeOk(void *stream, int32_t stmAttrType, void *value)
{
    (void)stream;
    (void)stmAttrType;
    (void)value;
    return 0;
}

aclTensor *FakeAclCreateTensorOk(const int64_t *shape, int64_t shapeSize, aclDataType dataType, const int64_t *strides,
                                 int64_t strideSize, aclFormat format, const int64_t *offset, int64_t offsetSize,
                                 void *addr)
{
    (void)shape;
    (void)shapeSize;
    (void)dataType;
    (void)strides;
    (void)strideSize;
    (void)format;
    (void)offset;
    (void)offsetSize;
    (void)addr;
    return reinterpret_cast<aclTensor *>(0xFFFF0000ULL);
}

aclTensor *FakeAclCreateTensorFail(const int64_t *shape, int64_t shapeSize, aclDataType dataType,
                                   const int64_t *strides, int64_t strideSize, aclFormat format, const int64_t *offset,
                                   int64_t offsetSize, void *addr)
{
    (void)shape;
    (void)shapeSize;
    (void)dataType;
    (void)strides;
    (void)strideSize;
    (void)format;
    (void)offset;
    (void)offsetSize;
    (void)addr;
    return nullptr;
}

int32_t FakeAclDestroyTensorOk(aclTensor *tensor)
{
    (void)tensor;
    return 0;
}

int32_t FakeAclnnShmemSdmaStarsQueryGetWorkspaceSizeOk(aclTensor *input, aclTensor *output, uint64_t *workspaceSize,
                                                       aclOpExecutor **executor)
{
    (void)input;
    (void)output;
    *workspaceSize = TEST_WORKSPACE_SIZE;
    *executor = reinterpret_cast<aclOpExecutor *>(0xAAAA0000ULL);
    return 0;
}

int32_t FakeAclnnShmemSdmaStarsQueryOk(void *workspace, uint64_t workspaceSize, aclOpExecutor *executor, void *stream)
{
    (void)workspace;
    (void)workspaceSize;
    (void)executor;
    (void)stream;
    return 0;
}

class SdmaTransportManagerTest : public testing::Test {
public:
    void SetUp() override
    {
        g_mockDeviceAddr = 0x100000000000ULL;
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    void SetupMockOk()
    {
        AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
        AscendRtApi().pAclrtFree_ = &FakeAclrtFreeOk;
        AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;
        AscendRtApi().pAclrtMemset_ = &FakeAclrtMemsetOk;
        AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
        AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;
        AscendRtApi().pAclrtStreamGetId_ = &FakeAclrtStreamGetIdOk;
        AscendRtApi().pAclrtCreateNotify_ = &FakeAclrtCreateNotifyOk;
        AscendRtApi().pAclrtGetNotifyId_ = &FakeAclrtGetNotifyIdOk;
        AscendRtApi().pAclrtDestroyNotify_ = &FakeAclrtDestroyNotifyOk;
        AscendRtApi().pAclrtGetCurrentContext_ = &FakeAclrtGetCurrentContextOk;
        AscendRtApi().pAclrtSynchronizeStream_ = &FakeAclrtSynchronizeStreamOk;
        AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
        AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
        AscendRtApi().pRtStreamGetSqid_ = &FakeRtStreamGetSqidOk;
        AscendRtApi().pRtStreamGetCqid_ = &FakeRtStreamGetCqidOk;
        AscendRtApi().pAclrtSetStreamAttribute_ = &FakeAclrtSetStreamAttributeOk;

        DlOpApi::pAclCreateTensor = &FakeAclCreateTensorOk;
        DlOpApi::pAclDestroyTensor = &FakeAclDestroyTensorOk;
        DlOpApi::pAclnnShmemSdmaStarsQueryGetWorkspaceSize = &FakeAclnnShmemSdmaStarsQueryGetWorkspaceSizeOk;
        DlOpApi::pAclnnShmemSdmaStarsQuery = &FakeAclnnShmemSdmaStarsQueryOk;
    }
};
} // namespace

TEST_F(SdmaTransportManagerTest, OpenDeviceSuccess)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;
    SetupMockOk();

    SdmaTransportManager mgr;
    TransportOptions options{};
    options.rankId = 0;
    options.rankCount = 1;

    auto ret = mgr.OpenDevice(options);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_TRUE(mgr.inited_);
    EXPECT_EQ(mgr.rankId_, 0U);
    EXPECT_EQ(mgr.rankCount_, 1U);
    EXPECT_FALSE(mgr.streams_.empty());
}

TEST_F(SdmaTransportManagerTest, OpenDeviceCreateStreamFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
    AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigFail;

    SdmaTransportManager mgr;
    TransportOptions options{};
    options.rankId = 0;
    options.rankCount = 1;

    auto ret = mgr.OpenDevice(options);
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_FALSE(mgr.inited_);
}

TEST_F(SdmaTransportManagerTest, OpenDeviceMallocWorkspaceFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
    DlOpApiFnGuard apiGuard;
    AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtStreamGetId_ = &FakeAclrtStreamGetIdOk;
    AscendRtApi().pRtStreamGetSqid_ = &FakeRtStreamGetSqidOk;
    AscendRtApi().pRtStreamGetCqid_ = &FakeRtStreamGetCqidOk;
    AscendRtApi().pAclrtGetCurrentContext_ = &FakeAclrtGetCurrentContextOk;
    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocFail;

    SdmaTransportManager mgr;
    TransportOptions options{};
    options.rankId = 0;
    options.rankCount = 1;

    auto ret = mgr.OpenDevice(options);
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_FALSE(mgr.inited_);
}

TEST_F(SdmaTransportManagerTest, OpenDeviceCreateNotifyFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
    AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtStreamGetId_ = &FakeAclrtStreamGetIdOk;
    AscendRtApi().pRtStreamGetSqid_ = &FakeRtStreamGetSqidOk;
    AscendRtApi().pRtStreamGetCqid_ = &FakeRtStreamGetCqidOk;
    AscendRtApi().pAclrtGetCurrentContext_ = &FakeAclrtGetCurrentContextOk;
    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemset_ = &FakeAclrtMemsetOk;
    AscendRtApi().pAclrtCreateNotify_ = &FakeAclrtCreateNotifyFail;

    SdmaTransportManager mgr;
    TransportOptions options{};
    options.rankId = 0;
    options.rankCount = 1;

    auto ret = mgr.OpenDevice(options);
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_FALSE(mgr.inited_);
}

TEST_F(SdmaTransportManagerTest, CloseDeviceNotInited)
{
    AclFnGuard aclGuard;
    SdmaTransportManager mgr;

    auto ret = mgr.CloseDevice();
    EXPECT_EQ(ret, BM_OK);
}

TEST_F(SdmaTransportManagerTest, CloseDeviceInited)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;
    SetupMockOk();

    SdmaTransportManager mgr;
    TransportOptions options{};
    options.rankId = 0;
    options.rankCount = 1;

    auto ret = mgr.OpenDevice(options);
    EXPECT_EQ(ret, BM_OK);

    ret = mgr.CloseDevice();
    EXPECT_EQ(ret, BM_OK);
    EXPECT_FALSE(mgr.inited_);
    EXPECT_TRUE(mgr.streams_.empty());
    EXPECT_EQ(mgr.opResInfoDevicePtr_, nullptr);
}

TEST_F(SdmaTransportManagerTest, CreateStarsStreamsSuccess)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
    AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtStreamGetId_ = &FakeAclrtStreamGetIdOk;
    AscendRtApi().pRtStreamGetSqid_ = &FakeRtStreamGetSqidOk;
    AscendRtApi().pRtStreamGetCqid_ = &FakeRtStreamGetCqidOk;
    AscendRtApi().pAclrtGetCurrentContext_ = &FakeAclrtGetCurrentContextOk;

    SdmaTransportManager mgr;
    auto ret = mgr.CreateStarsStreams(2);

    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(mgr.streams_.size(), 2U);
}

TEST_F(SdmaTransportManagerTest, CreateStarsStreamsGetContextFail)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
    AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtStreamGetId_ = &FakeAclrtStreamGetIdOk;
    AscendRtApi().pRtStreamGetSqid_ = &FakeRtStreamGetSqidOk;
    AscendRtApi().pRtStreamGetCqid_ = &FakeRtStreamGetCqidOk;
    AscendRtApi().pAclrtGetCurrentContext_ = &FakeAclrtGetCurrentContextFail;

    SdmaTransportManager mgr;
    auto ret = mgr.CreateStarsStreams(TEST_STREAM_COUNT);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, MallocSdmaWorkspaceSuccess)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemset_ = &FakeAclrtMemsetOk;

    SdmaTransportManager mgr;
    auto ret = mgr.MallocSdmaWorkspace(16 * 1024);

    EXPECT_EQ(ret, BM_OK);
    EXPECT_NE(mgr.sdmaWorkspaceAddr_, 0ULL);
}

TEST_F(SdmaTransportManagerTest, MallocSdmaWorkspaceFail)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocFail;

    SdmaTransportManager mgr;
    auto ret = mgr.MallocSdmaWorkspace(16 * 1024);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, GetSdmaWorkSpaceAddr)
{
    SdmaTransportManager mgr;
    mgr.sdmaWorkspaceAddr_ = 0x12345678ULL;

    EXPECT_EQ(mgr.GetSdmaWorkSpaceAddr(), 0x12345678ULL);
}

TEST_F(SdmaTransportManagerTest, CopyHostOpResToDeviceSuccess)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;
    AscendRtApi().pAclrtMemset_ = &FakeAclrtMemsetOk;

    SdmaTransportManager mgr;
    mgr.sdmaWorkspaceAddr_ = 0x1000ULL;
    mgr.streams_.resize(TEST_STREAM_COUNT);

    auto ret = mgr.CopyHostOpResToDevice();

    EXPECT_EQ(ret, BM_OK);
    EXPECT_NE(mgr.opResInfo_.streamsAddr, 0ULL);
    EXPECT_EQ(mgr.opResInfo_.workspaceAddr, 0x1000ULL);
    EXPECT_NE(mgr.opResInfoDevicePtr_, nullptr);
}

TEST_F(SdmaTransportManagerTest, CopyHostOpResToDeviceMallocFail)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocFail;

    SdmaTransportManager mgr;
    mgr.sdmaWorkspaceAddr_ = 0x1000ULL;
    mgr.streams_.resize(TEST_STREAM_COUNT);

    auto ret = mgr.CopyHostOpResToDevice();

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, CreateAclTensorSuccess)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;
    DlOpApi::pAclCreateTensor = &FakeAclCreateTensorOk;

    SdmaTransportManager mgr;
    std::vector<uint64_t> hostData = {1, 2, 3};
    std::vector<int64_t> shape = {3};
    void *deviceAddr = nullptr;
    aclTensor *tensor = nullptr;

    auto ret = mgr.CreateAclTensor(hostData, shape, &deviceAddr, aclDataType::ACL_UINT64, &tensor);

    EXPECT_EQ(ret, BM_OK);
    EXPECT_NE(deviceAddr, nullptr);
    EXPECT_NE(tensor, nullptr);

    // Cleanup
    VendorApi::Api()->Free(deviceAddr);
    DlOpApi::AclDestroyTensor(tensor);
}

TEST_F(SdmaTransportManagerTest, CreateAclTensorFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;
    DlOpApi::pAclCreateTensor = &FakeAclCreateTensorFail;

    SdmaTransportManager mgr;
    std::vector<uint64_t> hostData = {1, 2, 3};
    std::vector<int64_t> shape = {3};
    void *deviceAddr = nullptr;
    aclTensor *tensor = nullptr;

    auto ret = mgr.CreateAclTensor(hostData, shape, &deviceAddr, aclDataType::ACL_UINT64, &tensor);

    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_EQ(deviceAddr, nullptr);
    EXPECT_EQ(tensor, nullptr);
}

TEST_F(SdmaTransportManagerTest, LaunchSdmaAicpuKernelSuccess)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;
    AscendRtApi().pAclrtSetStreamAttribute_ = &FakeAclrtSetStreamAttributeOk;
    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;
    AscendRtApi().pAclrtSynchronizeStream_ = &FakeAclrtSynchronizeStreamOk;
    DlOpApi::pAclCreateTensor = &FakeAclCreateTensorOk;
    DlOpApi::pAclDestroyTensor = &FakeAclDestroyTensorOk;
    DlOpApi::pAclnnShmemSdmaStarsQueryGetWorkspaceSize = &FakeAclnnShmemSdmaStarsQueryGetWorkspaceSizeOk;
    DlOpApi::pAclnnShmemSdmaStarsQuery = &FakeAclnnShmemSdmaStarsQueryOk;

    SdmaTransportManager mgr;
    auto ret = mgr.LaunchSdmaAicpuKernel(0x1000ULL, 0x2000ULL);

    EXPECT_EQ(ret, BM_OK);
}

TEST_F(SdmaTransportManagerTest, QueryHasRegisteredAlwaysTrue)
{
    SdmaTransportManager mgr;
    EXPECT_TRUE(mgr.QueryHasRegistered(0x1000, 0x100));
}

TEST_F(SdmaTransportManagerTest, GetNicReturnsEmpty)
{
    SdmaTransportManager mgr;
    EXPECT_TRUE(mgr.GetNic().empty());
}

TEST_F(SdmaTransportManagerTest, CreateNotifyIdsSuccess)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtCreateNotify_ = &FakeAclrtCreateNotifyOk;
    AscendRtApi().pAclrtGetNotifyId_ = &FakeAclrtGetNotifyIdOk;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;

    SdmaTransportManager mgr;
    mgr.sdmaWorkspaceAddr_ = 0x100000000000ULL;

    auto ret = mgr.CreateNotifyIds();

    EXPECT_EQ(ret, BM_OK);
}

TEST_F(SdmaTransportManagerTest, CreateNotifyIdsCreateNotifyFail)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtCreateNotify_ = &FakeAclrtCreateNotifyFail;

    SdmaTransportManager mgr;
    mgr.sdmaWorkspaceAddr_ = 0x100000000000ULL;

    auto ret = mgr.CreateNotifyIds();

    EXPECT_EQ(ret, BM_ERROR);
}

namespace {
// 全局静态变量用于模拟失败场景
static int32_t g_streamIdFailCount = 0;
static int32_t g_sqidFailCount = 0;
static int32_t g_cqidFailCount = 0;
static int32_t g_memcpyCallCount = 0;

int FakeAclrtStreamGetIdFailAtSecondCall(void *stream, int32_t *streamId)
{
    (void)stream;
    if (g_streamIdFailCount++ == 1) {
        return -1;
    }
    *streamId = 1;
    return 0;
}

int FakeRtStreamGetSqidFailAtSecondCall(const void *stream, uint32_t *sqId)
{
    (void)stream;
    if (g_sqidFailCount++ == 1) {
        return -1;
    }
    *sqId = 1;
    return 0;
}

int FakeRtStreamGetCqidFailAtSecondCall(const void *stream, uint32_t *cqId, uint32_t *logicCqId)
{
    (void)stream;
    if (g_cqidFailCount++ == 1) {
        return -1;
    }
    *cqId = 1;
    *logicCqId = 1;
    return 0;
}

int32_t FakeAclrtMemcpyFail(void *dst, size_t dstSize, const void *src, size_t srcSize, uint32_t kind)
{
    (void)dst;
    (void)dstSize;
    (void)src;
    (void)srcSize;
    (void)kind;
    return -1;
}

int32_t FakeAclrtSetStreamAttributeFail(void *stream, int32_t stmAttrType, void *value)
{
    (void)stream;
    (void)stmAttrType;
    (void)value;
    return -1;
}

int32_t FakeAclrtMemcpyFailAfterNCall(void *dst, size_t dstSize, const void *src, size_t srcSize, uint32_t kind)
{
    (void)dst;
    (void)dstSize;
    (void)src;
    (void)srcSize;
    (void)kind;
    if (g_memcpyCallCount++ >= MAX_AIV_PER_NPU + 1) {
        return -1;
    }
    return 0;
}

static int32_t g_mallocCallCount = 0;

int32_t FakeAclrtMallocFailAfterSecondCall(void **ptr, size_t size, uint32_t flags)
{
    (void)size;
    (void)flags;
    if (g_mallocCallCount++ >= TEST_MALLOC_FAIL_AT_CALL) {
        return -1;
    }
    *ptr = reinterpret_cast<void *>(0x100000000000ULL + g_mallocCallCount * 0x1000);
    return 0;
}

} // namespace

TEST_F(SdmaTransportManagerTest, CreateStarsStreamsGetStreamIdFail)
{
    AclFnGuard aclGuard;

    g_streamIdFailCount = 0;

    AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
    AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtStreamGetId_ = &FakeAclrtStreamGetIdFailAtSecondCall;
    AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;

    SdmaTransportManager mgr;
    auto ret = mgr.CreateStarsStreams(3);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, CreateStarsStreamsGetSqidFail)
{
    AclFnGuard aclGuard;

    g_sqidFailCount = 0;

    AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
    AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtStreamGetId_ = &FakeAclrtStreamGetIdOk;
    AscendRtApi().pRtStreamGetSqid_ = &FakeRtStreamGetSqidFailAtSecondCall;
    AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;

    SdmaTransportManager mgr;
    auto ret = mgr.CreateStarsStreams(3);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, CreateStarsStreamsGetCqidFail)
{
    AclFnGuard aclGuard;

    g_cqidFailCount = 0;

    AscendRtApi().pAclrtGetDevice_ = &FakeAclrtGetDeviceOk;
    AscendRtApi().pRtGetDeviceInfo_ = &FakeRtGetDeviceInfoOk;
    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtStreamGetId_ = &FakeAclrtStreamGetIdOk;
    AscendRtApi().pRtStreamGetSqid_ = &FakeRtStreamGetSqidOk;
    AscendRtApi().pRtStreamGetCqid_ = &FakeRtStreamGetCqidFailAtSecondCall;
    AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;

    SdmaTransportManager mgr;
    auto ret = mgr.CreateStarsStreams(3);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, MallocSdmaWorkspaceMemsetFail)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemset_ = [](void *dst, size_t destMax, int32_t value, size_t count) -> int32_t {
        (void)dst;
        (void)destMax;
        (void)value;
        (void)count;
        return -1;
    };

    SdmaTransportManager mgr;
    auto ret = mgr.MallocSdmaWorkspace(16 * 1024);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, CopyHostOpResToDeviceMemcpyFail)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemcpy_ = [](void *dst, size_t dstSize, const void *src, size_t srcSize,
                                     uint32_t kind) -> int32_t {
        (void)dst;
        (void)dstSize;
        (void)src;
        (void)srcSize;
        (void)kind;
        return -1;
    };
    AscendRtApi().pAclrtMemset_ = &FakeAclrtMemsetOk;

    SdmaTransportManager mgr;
    mgr.sdmaWorkspaceAddr_ = 0x1000ULL;
    mgr.streams_.resize(TEST_STREAM_COUNT);

    auto ret = mgr.CopyHostOpResToDevice();

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, CreateAclTensorMemcpyFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyFail;

    SdmaTransportManager mgr;
    std::vector<uint64_t> hostData = {1, 2, 3};
    std::vector<int64_t> shape = {3};
    void *deviceAddr = nullptr;
    aclTensor *tensor = nullptr;

    auto ret = mgr.CreateAclTensor(hostData, shape, &deviceAddr, aclDataType::ACL_UINT64, &tensor);

    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_EQ(deviceAddr, nullptr);
    EXPECT_EQ(tensor, nullptr);
}

TEST_F(SdmaTransportManagerTest, LaunchSdmaAicpuKernelCreateStreamFail)
{
    AclFnGuard aclGuard;

    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigFail;

    SdmaTransportManager mgr;
    auto ret = mgr.LaunchSdmaAicpuKernel(0x1000ULL, 0x2000ULL);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, LaunchSdmaAicpuKernelSetStreamAttributeFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;
    AscendRtApi().pAclrtSetStreamAttribute_ = &FakeAclrtSetStreamAttributeFail;

    SdmaTransportManager mgr;
    auto ret = mgr.LaunchSdmaAicpuKernel(0x1000ULL, 0x2000ULL);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, LaunchSdmaAicpuKernelCreateInputTensorFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;
    AscendRtApi().pAclrtSetStreamAttribute_ = &FakeAclrtSetStreamAttributeOk;
    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;
    DlOpApi::pAclCreateTensor = &FakeAclCreateTensorFail;

    SdmaTransportManager mgr;
    auto ret = mgr.LaunchSdmaAicpuKernel(0x1000ULL, 0x2000ULL);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, LaunchSdmaAicpuKernelQueryWorkspaceFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;
    AscendRtApi().pAclrtSetStreamAttribute_ = &FakeAclrtSetStreamAttributeOk;
    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocOk;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;
    DlOpApi::pAclCreateTensor = &FakeAclCreateTensorOk;
    DlOpApi::pAclDestroyTensor = &FakeAclDestroyTensorOk;
    DlOpApi::pAclnnShmemSdmaStarsQueryGetWorkspaceSize =
        [](aclTensor *input, aclTensor *output, uint64_t *workspaceSize, aclOpExecutor **executor) -> int32_t {
        (void)input;
        (void)output;
        (void)workspaceSize;
        (void)executor;
        return -1;
    };

    SdmaTransportManager mgr;
    auto ret = mgr.LaunchSdmaAicpuKernel(0x1000ULL, 0x2000ULL);

    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(SdmaTransportManagerTest, LaunchSdmaAicpuKernelMallocWorkspaceFail)
{
    AclFnGuard aclGuard;
    DlOpApiFnGuard opGuard;

    g_mallocCallCount = 0;

    AscendRtApi().pAclrtCreateStreamWithConfig_ = &FakeAclrtCreateStreamWithConfigOk;
    AscendRtApi().pAclrtDestroyStream_ = &FakeAclrtDestroyStreamOk;
    AscendRtApi().pAclrtSetStreamAttribute_ = &FakeAclrtSetStreamAttributeOk;
    AscendRtApi().pAclrtMalloc_ = &FakeAclrtMallocFailAfterSecondCall;
    AscendRtApi().pAclrtMemcpy_ = &FakeAclrtMemcpyOk;
    DlOpApi::pAclCreateTensor = &FakeAclCreateTensorOk;
    DlOpApi::pAclDestroyTensor = &FakeAclDestroyTensorOk;
    DlOpApi::pAclnnShmemSdmaStarsQueryGetWorkspaceSize = &FakeAclnnShmemSdmaStarsQueryGetWorkspaceSizeOk;

    SdmaTransportManager mgr;
    auto ret = mgr.LaunchSdmaAicpuKernel(0x1000ULL, 0x2000ULL);

    EXPECT_EQ(ret, BM_ERROR);
}
