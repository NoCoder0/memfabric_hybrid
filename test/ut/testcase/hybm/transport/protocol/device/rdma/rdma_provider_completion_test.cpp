/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#include <vector>

#include "vendor/vendor_api.h"
#include "vendor/ascend/dl_hal_api.h"
#include "vendor/ascend/dl_hccp_api.h"
#include "hybm_define.h"
#include "rdma_provider_completion.h"
#include "hybm_stream.h"
#include "device_chip_info.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::mf;
using namespace ock::mf::transport::device;

namespace {
void *const SIGNAL_ADDRESS = reinterpret_cast<void *>(0x200000UL);
void *const RDEV = reinterpret_cast<void *>(0x900UL);
void *const MR_HANDLE = reinterpret_cast<void *>(0x901UL);
void *const QP = reinterpret_cast<void *>(0x902UL);
void *const DATA_STREAM = reinterpret_cast<void *>(0x903UL);
constexpr uint64_t NOTIFY_BASE = 0x400000UL;

struct CompletionApiState {
    bool allocationFails{false};
    bool registrationFails{false};
    bool deregistrationFails{false};
    bool notifyCreationFails{false};
    bool offsetQueryFails{false};
    bool useLegacyOffset{false};
    bool streamDestroyFails{false};
    bool notifyDestroyFails{false};
    bool waitFails{false};
    bool mrRegistered{false};
    bool nativeInitializationFails{false};
    bool nativeWaitFails{false};
    bool nativeStreamDestroyFails{false};
    unsigned int nativeStreamDestroys{0};
    std::vector<StreamTask> nativeTasks;
    unsigned int allocations{0};
    unsigned int registrations{0};
    unsigned int deregistrations{0};
    unsigned int frees{0};
    unsigned int streamCreates{0};
    unsigned int streamDestroys{0};
    unsigned int notifyCreates{0};
    unsigned int notifyDestroys{0};
    unsigned int submissions{0};
    unsigned int waits{0};
    unsigned int synchronizations{0};
    std::vector<void *> doorbellStreams;
    std::vector<void *> waitStreams;
};
CompletionApiState gApi;
} // namespace

class ProviderRdmaCompletionTest : public testing::Test {
protected:
    void SetUp() override
    {
        gApi = {};
        MOCKER_CPP(&ock::mf::AscendApiImpl::SetDevice, ock::mf::Result(*)(ock::mf::AscendApiImpl *, int32_t, bool))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, int32_t device, bool) {
                EXPECT_EQ(device, 2);
                return BM_OK;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::Malloc,
                   ock::mf::Result(*)(ock::mf::AscendApiImpl *, void **, size_t, uint32_t))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void **address, size_t size, uint32_t) {
                ++gApi.allocations;
                EXPECT_EQ(size, HYBM_LARGE_PAGE_SIZE);
                *address = gApi.allocationFails ? nullptr : SIGNAL_ADDRESS;
                return gApi.allocationFails ? -1 : 0;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::Memcpy,
                   ock::mf::Result(*)(ock::mf::AscendApiImpl *, void *, size_t, const void *, size_t, uint32_t))
            .stubs()
            .will(invoke(
                +[](ock::mf::AscendApiImpl *, void *dst, size_t capacity, const void *src, size_t size, uint32_t kind) {
                    EXPECT_EQ(dst, SIGNAL_ADDRESS);
                    EXPECT_EQ(capacity, sizeof(uint32_t));
                    EXPECT_EQ(size, sizeof(uint32_t));
                    EXPECT_EQ(*static_cast<const uint32_t *>(src), 1U);
                    EXPECT_EQ(kind, ACL_MEMCPY_HOST_TO_DEVICE);
                    return BM_OK;
                }));
        MOCKER(DlHccpApi::RaRegisterMR).stubs().will(invoke(+[](const void *rdev, HccpMrInfo *info, void *&mr) {
            ++gApi.registrations;
            EXPECT_EQ(rdev, RDEV);
            EXPECT_EQ(info->addr, SIGNAL_ADDRESS);
            EXPECT_EQ(info->size, HYBM_LARGE_PAGE_SIZE);
            mr = gApi.registrationFails ? nullptr : MR_HANDLE;
            info->rkey = 42;
            gApi.mrRegistered = !gApi.registrationFails;
            return gApi.registrationFails ? -1 : 0;
        }));
        MOCKER(DlHccpApi::RaDeregisterMR).stubs().will(invoke(+[](const void *rdev, void *mr) {
            ++gApi.deregistrations;
            EXPECT_EQ(rdev, RDEV);
            EXPECT_EQ(mr, MR_HANDLE);
            if (!gApi.deregistrationFails) {
                gApi.mrRegistered = false;
            }
            return gApi.deregistrationFails ? -1 : 0;
        }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::Free, ock::mf::Result(*)(ock::mf::AscendApiImpl *, void *))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void *address) {
                ++gApi.frees;
                EXPECT_EQ(address, SIGNAL_ADDRESS);
                EXPECT_FALSE(gApi.mrRegistered);
                return BM_OK;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::CreateStream, ock::mf::Result(*)(ock::mf::AscendApiImpl *, void **))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void **stream) {
                *stream = reinterpret_cast<void *>(0x1000UL + ++gApi.streamCreates);
                return BM_OK;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::DestroyStream, ock::mf::Result(*)(ock::mf::AscendApiImpl *, void *))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void *) {
                ++gApi.streamDestroys;
                return gApi.streamDestroyFails ? -1 : 0;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::CreateNotify,
                   ock::mf::Result(*)(ock::mf::AscendApiImpl *, void **, uint64_t))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void **notify, uint64_t flag) {
                ++gApi.notifyCreates;
                EXPECT_EQ(flag, 0U);
                *notify = gApi.notifyCreationFails ? nullptr : reinterpret_cast<void *>(0x2000UL + gApi.notifyCreates);
                return gApi.notifyCreationFails ? -1 : 0;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::GetNotifyId,
                   ock::mf::Result(*)(ock::mf::AscendApiImpl *, void *, uint32_t *))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void *notify, uint32_t *id) {
                *id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(notify) - 0x2000UL);
                return BM_OK;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::DestroyNotify, ock::mf::Result(*)(ock::mf::AscendApiImpl *, void *))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void *) {
                ++gApi.notifyDestroys;
                EXPECT_GT(gApi.streamDestroys + gApi.nativeStreamDestroys, 0U);
                EXPECT_FALSE(gApi.nativeStreamDestroyFails);
                EXPECT_FALSE(gApi.streamDestroyFails);
                return gApi.notifyDestroyFails ? -1 : 0;
            }));
        MOCKER(DlHalApi::HalResourceDetailQuery)
            .stubs()
            .will(invoke(+[](uint32_t logic, halResourceIdInputInfo *input, halResourceDetailInfo *detail) {
                EXPECT_EQ(logic, 5U);
                EXPECT_EQ(input->type, DRV_NOTIFY_ID);
                EXPECT_EQ(input->tsId, 0U);
                detail->value0 = input->resourceId * sizeof(uint32_t);
                return gApi.offsetQueryFails || gApi.useLegacyOffset ? -1 : 0;
            }));
        MOCKER(DlHalApi::DrvNotifyIdAddrOffset).stubs().will(invoke(+[](uint32_t logic, drvNotifyInfo *info) {
            EXPECT_EQ(logic, 5U);
            info->devAddrOffset = info->notifyId * sizeof(uint32_t);
            return gApi.offsetQueryFails ? -1 : 0;
        }));
        MOCKER(DlHccpApi::RaGetNotifyBaseAddr).stubs().will(invoke(+[](void *rdev, uint64_t *base, uint64_t *size) {
            EXPECT_EQ(rdev, RDEV);
            *base = NOTIFY_BASE;
            *size = 4096;
            return BM_OK;
        }));
        MOCKER(DlHccpApi::RaGetNotifyMrInfo).stubs().will(invoke(+[](void *, HccpMrInfo *info) {
            info->lkey = 99;
            return BM_OK;
        }));
        MOCKER(DlHccpApi::RaSendWrV2).stubs().will(invoke(+[](void *qp, send_wr_v2 *wr, send_wr_rsp *rsp) {
            ++gApi.submissions;
            EXPECT_EQ(qp, QP);
            EXPECT_EQ(wr->op, 4U);
            EXPECT_EQ(wr->send_flag, RA_SEND_SIGNALED | RA_SEND_FENCE);
            EXPECT_EQ(wr->dst_addr, 0x500000UL);
            EXPECT_EQ(wr->rkey, 42U);
            EXPECT_EQ(wr->buf_num, 1U);
            EXPECT_EQ(wr->buf_list->len, sizeof(uint32_t));
            EXPECT_EQ(wr->buf_list->lkey, 99U);
            EXPECT_GE(wr->buf_list->addr, NOTIFY_BASE + sizeof(uint32_t));
            rsp->db.db_index = 8;
            rsp->db.db_info = 9;
            return BM_OK;
        }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::RDMADBSend,
                   ock::mf::Result(*)(ock::mf::AscendApiImpl *, uint32_t, uint64_t, void *))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, uint32_t index, uint64_t info, void *stream) {
                EXPECT_EQ(index, 8U);
                EXPECT_EQ(info, 9U);
                EXPECT_NE(stream, DATA_STREAM);
                gApi.doorbellStreams.push_back(stream);
                return BM_OK;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::WaitAndResetNotify,
                   ock::mf::Result(*)(ock::mf::AscendApiImpl *, void *, void *, uint32_t))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void *, void *stream, uint32_t timeout) {
                ++gApi.waits;
                EXPECT_EQ(timeout, 5U);
                EXPECT_EQ(stream, gApi.doorbellStreams.back());
                gApi.waitStreams.push_back(stream);
                return gApi.waitFails ? -1 : 0;
            }));
        MOCKER_CPP(&ock::mf::AscendApiImpl::SynchronizeStream, ock::mf::Result(*)(ock::mf::AscendApiImpl *, void *))
            .stubs()
            .will(invoke(+[](ock::mf::AscendApiImpl *, void *stream) {
                ++gApi.synchronizations;
                EXPECT_EQ(stream, gApi.waitStreams.back());
                return BM_OK;
            }));
    }

    void PrepareNativeStream()
    {
        MOCKER_CPP(&DeviceChipInfo::Init, int (*)(DeviceChipInfo *)).stubs().will(returnValue(BM_OK));
        MOCKER_CPP(&HybmStream::Initialize, int (*)(HybmStream *)).stubs().will(invoke(+[](HybmStream *stream) {
            EXPECT_EQ(stream->GetDevId(), 5U);
            return gApi.nativeInitializationFails ? -1 : BM_OK;
        }));
        MOCKER_CPP(&HybmStream::SubmitTasks, int (*)(HybmStream *, const StreamTask &))
            .stubs()
            .will(invoke(+[](HybmStream *, const StreamTask &task) {
                gApi.nativeTasks.push_back(task);
                return BM_OK;
            }));
        MOCKER_CPP(&HybmStream::Synchronize, int (*)(HybmStream *, uint32_t))
            .stubs()
            .will(invoke(+[](HybmStream *, uint32_t) { return gApi.nativeWaitFails ? -1 : BM_OK; }));
        MOCKER_CPP(&HybmStream::Destroy, void (*)(HybmStream *)).stubs().will(invoke(+[](HybmStream *) {
            ++gApi.nativeStreamDestroys;
        }));
        MOCKER_CPP(&HybmStream::GetId, uint32_t(*)(const HybmStream *)).stubs().will(invoke(+[](const HybmStream *) {
            return gApi.nativeStreamDestroyFails ? 17U : UINT32_MAX;
        }));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

TEST_F(ProviderRdmaCompletionTest, SignalPublishesOneAndUnregistersBeforeFree)
{
    RdmaProviderSignal signal(2, RDEV);
    ASSERT_TRUE(signal.Init());
    EXPECT_EQ(signal.GetAddress(), reinterpret_cast<uint64_t>(SIGNAL_ADDRESS));
    EXPECT_EQ(signal.GetRkey(), 42U);
    ASSERT_TRUE(signal.Close());
    EXPECT_EQ(gApi.deregistrations, 1U);
    EXPECT_EQ(gApi.frees, 1U);
}

TEST_F(ProviderRdmaCompletionTest, SignalAllocationFailureDoesNotRegisterOrFree)
{
    gApi.allocationFails = true;
    RdmaProviderSignal signal(2, RDEV);
    EXPECT_FALSE(signal.Init());
    EXPECT_EQ(gApi.registrations, 0U);
    EXPECT_EQ(gApi.frees, 0U);
}

TEST_F(ProviderRdmaCompletionTest, SignalRegistrationFailureReleasesAllocation)
{
    gApi.registrationFails = true;
    RdmaProviderSignal signal(2, RDEV);
    EXPECT_FALSE(signal.Init());
    EXPECT_EQ(gApi.deregistrations, 0U);
    EXPECT_EQ(gApi.frees, 1U);
}

TEST_F(ProviderRdmaCompletionTest, FailedDeregistrationRetainsSignalUntilRetry)
{
    RdmaProviderSignal signal(2, RDEV);
    ASSERT_TRUE(signal.Init());
    gApi.deregistrationFails = true;
    EXPECT_FALSE(signal.Close());
    EXPECT_EQ(gApi.frees, 0U);
    gApi.deregistrationFails = false;
    EXPECT_TRUE(signal.Close());
    EXPECT_EQ(gApi.deregistrations, 2U);
    EXPECT_EQ(gApi.frees, 1U);
}

TEST_F(ProviderRdmaCompletionTest, EachQpUsesItsOwnNotifyAndStreamWithoutPollingCq)
{
    RdmaProviderCompletion first(2, 5, RDEV);
    RdmaProviderCompletion second(2, 5, RDEV);
    ASSERT_TRUE(first.Init());
    ASSERT_TRUE(second.Init());
    ASSERT_TRUE(first.SubmitFenceRead(QP, 0x500000UL, 42) && first.WaitFenceNotify());
    ASSERT_TRUE(second.SubmitFenceRead(QP, 0x500000UL, 42) && second.WaitFenceNotify());
    EXPECT_NE(gApi.waitStreams[0], gApi.waitStreams[1]);
    EXPECT_EQ(gApi.synchronizations, 2U);
    EXPECT_TRUE(first.Close());
    EXPECT_TRUE(second.Close());
    EXPECT_EQ(gApi.notifyDestroys, 2U);
}

TEST_F(ProviderRdmaCompletionTest, OffsetFallbackUsesExplicitLogicDevice)
{
    gApi.useLegacyOffset = true;
    RdmaProviderCompletion completion(2, 5, RDEV);
    ASSERT_TRUE(completion.Init());
    EXPECT_TRUE(completion.SubmitFenceRead(QP, 0x500000UL, 42) && completion.WaitFenceNotify());
    EXPECT_TRUE(completion.Close());
}

TEST_F(ProviderRdmaCompletionTest, InitializationFailureCleansPrivateResources)
{
    gApi.offsetQueryFails = true;
    RdmaProviderCompletion completion(2, 5, RDEV);
    EXPECT_FALSE(completion.Init());
    EXPECT_EQ(gApi.streamDestroys, 1U);
    EXPECT_EQ(gApi.notifyDestroys, 1U);
}

TEST_F(ProviderRdmaCompletionTest, NotifyCreationFailureClosesStream)
{
    gApi.notifyCreationFails = true;
    RdmaProviderCompletion completion(2, 5, RDEV);
    EXPECT_FALSE(completion.Init());
    EXPECT_EQ(gApi.streamDestroys, 1U);
    EXPECT_EQ(gApi.notifyDestroys, 0U);
}

TEST_F(ProviderRdmaCompletionTest, FailedWaitCannotReuseNotifyAndFailedStreamDestroyPreservesIt)
{
    RdmaProviderCompletion completion(2, 5, RDEV);
    ASSERT_TRUE(completion.Init());
    gApi.waitFails = true;
    EXPECT_FALSE(completion.SubmitFenceRead(QP, 0x500000UL, 42) && completion.WaitFenceNotify());
    EXPECT_FALSE(completion.SubmitFenceRead(QP, 0x500000UL, 42) && completion.WaitFenceNotify());
    EXPECT_EQ(gApi.submissions, 1U);
    EXPECT_EQ(gApi.waits, 1U);
    EXPECT_EQ(gApi.synchronizations, 0U);
    gApi.streamDestroyFails = true;
    EXPECT_FALSE(completion.Close());
    EXPECT_EQ(gApi.notifyDestroys, 0U);
    gApi.streamDestroyFails = false;
    EXPECT_TRUE(completion.Close());
    EXPECT_EQ(gApi.notifyDestroys, 1U);
}

TEST_F(ProviderRdmaCompletionTest, FailedNotifyDestructionRetainsItWithoutDestroyingStreamTwice)
{
    RdmaProviderCompletion completion(2, 5, RDEV);
    ASSERT_TRUE(completion.Init());
    gApi.notifyDestroyFails = true;
    EXPECT_FALSE(completion.Close());
    EXPECT_EQ(gApi.streamDestroys, 1U);
    EXPECT_EQ(gApi.notifyDestroys, 1U);
    gApi.notifyDestroyFails = false;
    EXPECT_TRUE(completion.Close());
    EXPECT_EQ(gApi.streamDestroys, 1U);
    EXPECT_EQ(gApi.notifyDestroys, 2U);
}

TEST_F(ProviderRdmaCompletionTest, OwnedNativeStreamOrdersDoorbellsBeforeNotifyWithoutRuntimeWait)
{
    PrepareNativeStream();
    RdmaProviderCompletion completion(2, 5, RDEV, true);
    ASSERT_TRUE(completion.Init());
    send_wr_rsp data{};
    data.db.db_info = 11;
    ASSERT_TRUE(completion.SubmitNativeDoorbell(data));
    ASSERT_TRUE(completion.SubmitFenceRead(QP, 0x500000UL, 42) && completion.WaitFenceNotify());
    ASSERT_EQ(gApi.nativeTasks.size(), 3U);
    EXPECT_EQ(gApi.nativeTasks[0].type, STREAM_TASK_TYPE_WRITE_VAL);
    EXPECT_EQ(gApi.nativeTasks[0].sqe.writeValueSqe.write_value_part0, 11U);
    EXPECT_EQ(gApi.nativeTasks[1].type, STREAM_TASK_TYPE_WRITE_VAL);
    EXPECT_EQ(gApi.nativeTasks[1].sqe.writeValueSqe.write_value_part0, 9U);
    EXPECT_EQ(gApi.nativeTasks[2].type, STREAM_TASK_TYPE_NOTIFY);
    EXPECT_EQ(static_cast<uint32_t>(gApi.nativeTasks[2].sqe.notifySqe.notify_id), 1U);
    EXPECT_EQ(gApi.nativeTasks[2].sqe.notifySqe.timeout, 5U);
    EXPECT_EQ(gApi.streamCreates, 0U);
    EXPECT_TRUE(gApi.doorbellStreams.empty());
    EXPECT_EQ(gApi.waits, 0U);
    EXPECT_EQ(gApi.synchronizations, 0U);
    EXPECT_TRUE(completion.Close());
}

TEST_F(ProviderRdmaCompletionTest, FailedNativeWaitCannotSucceedOrReuseCompletion)
{
    PrepareNativeStream();
    RdmaProviderCompletion completion(2, 5, RDEV, true);
    ASSERT_TRUE(completion.Init());
    gApi.nativeWaitFails = true;
    EXPECT_FALSE(completion.SubmitFenceRead(QP, 0x500000UL, 42) && completion.WaitFenceNotify());
    EXPECT_FALSE(completion.SubmitFenceRead(QP, 0x500000UL, 42) && completion.WaitFenceNotify());
    EXPECT_EQ(gApi.submissions, 1U);
    EXPECT_EQ(gApi.waits, 0U);
    EXPECT_TRUE(completion.Close());
}

TEST_F(ProviderRdmaCompletionTest, FailedNativeCleanupRetainsNotifyUntilStreamReleased)
{
    PrepareNativeStream();
    RdmaProviderCompletion completion(2, 5, RDEV, true);
    ASSERT_TRUE(completion.Init());
    gApi.nativeStreamDestroyFails = true;
    EXPECT_FALSE(completion.Close());
    EXPECT_EQ(gApi.notifyDestroys, 0U);
    gApi.nativeStreamDestroyFails = false;
    EXPECT_TRUE(completion.Close());
    EXPECT_EQ(gApi.notifyDestroys, 1U);
}

TEST_F(ProviderRdmaCompletionTest, NativeInitializationFailureDoesNotAllocateNotify)
{
    PrepareNativeStream();
    gApi.nativeInitializationFails = true;
    RdmaProviderCompletion completion(2, 5, RDEV, true);
    EXPECT_FALSE(completion.Init());
    EXPECT_EQ(gApi.streamCreates, 0U);
    EXPECT_EQ(gApi.notifyCreates, 0U);
    EXPECT_TRUE(completion.Close());
}
