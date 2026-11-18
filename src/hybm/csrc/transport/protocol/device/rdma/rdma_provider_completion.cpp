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
#include "rdma_provider_completion.h"
#include "hybm_stream.h"
#include "device_chip_info.h"

#include "vendor/vendor_api.h"
#include "vendor/ascend/dl_hal_api.h"
#include "vendor/ascend/dl_hccp_api.h"
#include "hybm_define.h"
#include "hybm_logger.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {
namespace {
constexpr uint32_t PROVIDER_NOTIFY_VALUE = 1U;
constexpr uint32_t PROVIDER_DOORBELL_SIZE_64BIT = 3U;
constexpr uint32_t PROVIDER_DOORBELL_RDMA_SEND = 2U;
constexpr uint32_t PROVIDER_NOTIFY_WAIT_TIMEOUT_SECONDS = 5U;
} // namespace

RdmaProviderSignal::~RdmaProviderSignal()
{
    (void)Close();
}

bool RdmaProviderSignal::Init()
{
    if (ready_) {
        return true;
    }
    if (buffer_ != nullptr || mr_ != nullptr || rdev_ == nullptr) {
        BM_LOG_ERROR("provider signal initialization requires a valid RDEV and no pending resources");
        return false;
    }
    auto ret = VendorApi::Api()->SetDevice(static_cast<int32_t>(userId_));
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider signal set device failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    ret = VendorApi::Api()->Malloc(&buffer_, HYBM_LARGE_PAGE_SIZE, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider signal allocation failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    ret = VendorApi::Api()->Memcpy(buffer_, sizeof(PROVIDER_NOTIFY_VALUE), &PROVIDER_NOTIFY_VALUE,
                                   sizeof(PROVIDER_NOTIFY_VALUE), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider signal initialization copy failed, deviceId=" << userId_ << " ret=" << ret);
        (void)Close();
        return false;
    }
    info_.addr = buffer_;
    info_.size = HYBM_LARGE_PAGE_SIZE;
    info_.access = RA_ACCESS_NORMAL;
    ret = DlHccpApi::RaRegisterMR(rdev_, &info_, mr_);
    if (ret != BM_OK || mr_ == nullptr) {
        BM_LOG_ERROR("provider signal MR registration failed, deviceId=" << userId_ << " ret=" << ret);
        (void)Close();
        return false;
    }
    ready_ = true;
    return true;
}

bool RdmaProviderSignal::Close()
{
    ready_ = false;
    if (buffer_ == nullptr && mr_ == nullptr) {
        return true;
    }
    auto ret = VendorApi::Api()->SetDevice(static_cast<int32_t>(userId_));
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider signal cleanup set device failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    if (mr_ != nullptr) {
        ret = DlHccpApi::RaDeregisterMR(rdev_, mr_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider signal MR deregistration failed, deviceId=" << userId_ << " ret=" << ret);
            return false;
        }
        mr_ = nullptr;
    }
    if (buffer_ != nullptr) {
        ret = VendorApi::Api()->Free(buffer_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider signal free failed, deviceId=" << userId_ << " ret=" << ret);
            return false;
        }
        buffer_ = nullptr;
    }
    info_ = {};
    return true;
}

RdmaProviderCompletion::~RdmaProviderCompletion()
{
    (void)Close();
}

bool RdmaProviderCompletion::Init()
{
    if (ready_) {
        return !failed_;
    }
    if (stream_ != nullptr || notify_ != nullptr || nativeStream_ != nullptr || rdev_ == nullptr) {
        BM_LOG_ERROR("provider completion initialization requires a valid RDEV and no pending resources");
        return false;
    }
    auto ret = VendorApi::Api()->SetDevice(static_cast<int32_t>(userId_));
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion set device failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    if (useNativeStream_) {
        DeviceChipInfo chip(userId_);
        ret = chip.Init();
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider native doorbell lookup failed, deviceId=" << userId_ << " ret=" << ret);
            return false;
        }
        nativeDoorbellAddress_ = chip.GetRoceDoorbellAddress();
        nativeStream_ = std::make_shared<HybmStream>(logicId_, 0U, 0U);
        ret = nativeStream_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider native stream initialization failed, deviceId=" << userId_ << " ret=" << ret);
            (void)Close();
            return false;
        }
    } else {
        ret = VendorApi::Api()->CreateStream(&stream_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider completion stream creation failed, deviceId=" << userId_ << " ret=" << ret);
            return false;
        }
    }
    ret = VendorApi::Api()->CreateNotify(&notify_, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion notify creation failed, deviceId=" << userId_ << " ret=" << ret);
        (void)Close();
        return false;
    }
    uint32_t notifyId = UINT32_MAX;
    ret = VendorApi::Api()->GetNotifyId(notify_, &notifyId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion notify ID query failed, deviceId=" << userId_ << " ret=" << ret);
        (void)Close();
        return false;
    }
    halResourceIdInputInfo input{};
    input.type = DRV_NOTIFY_ID;
    input.tsId = 0;
    input.resourceId = notifyId;
    halResourceDetailInfo detail{DRV_RES_QUERY_OFFSET, 0U, 0U, {0U}};
    uint64_t offset = 0;
    ret = DlHalApi::HalResourceDetailQuery(logicId_, &input, &detail);
    if (ret == BM_OK) {
        offset = detail.value0;
    } else {
        drvNotifyInfo info{};
        info.tsId = 0;
        info.notifyId = notifyId;
        ret = DlHalApi::DrvNotifyIdAddrOffset(logicId_, &info);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider completion notify offset query failed, logicId=" << logicId_ << " ret=" << ret);
            (void)Close();
            return false;
        }
        offset = info.devAddrOffset;
    }
    uint64_t base = 0;
    uint64_t size = 0;
    HccpMrInfo mr{};
    ret = DlHccpApi::RaGetNotifyBaseAddr(rdev_, &base, &size);
    if (ret != BM_OK || base == 0 || offset > size || size - offset < sizeof(PROVIDER_NOTIFY_VALUE)) {
        BM_LOG_ERROR("provider completion notify base query failed, deviceId=" << userId_ << " ret=" << ret << " size="
                                                                               << size << " offset=" << offset);
        (void)Close();
        return false;
    }
    ret = DlHccpApi::RaGetNotifyMrInfo(rdev_, &mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion notify MR query failed, deviceId=" << userId_ << " ret=" << ret);
        (void)Close();
        return false;
    }
    notifyId_ = notifyId;
    notifyAddress_ = base + offset;
    notifyLkey_ = mr.lkey;
    failed_ = false;
    ready_ = true;
    BM_LOG_INFO("provider completion ready, userId=" << userId_ << " logicId=" << logicId_
                                                     << " nativeStream=" << useNativeStream_);
    return true;
}

bool RdmaProviderCompletion::SubmitFenceRead(void *qp, uint64_t remoteSignalAddr, uint32_t remoteSignalRkey)
{
    if (!ready_ || failed_ || qp == nullptr || remoteSignalAddr == 0) {
        BM_LOG_ERROR(
            "provider completion fence read requires an initialized, usable QP completion, deviceId=" << userId_);
        return false;
    }
    failed_ = true;
    auto ret = VendorApi::Api()->SetDevice(static_cast<int32_t>(userId_));
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion wait set device failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    sg_list sg{notifyAddress_, sizeof(PROVIDER_NOTIFY_VALUE), notifyLkey_};
    send_wr_v2 wr{};
    wr.wr_id = nextWrId_++;
    wr.buf_list = &sg;
    wr.buf_num = 1;
    wr.dst_addr = remoteSignalAddr;
    wr.rkey = remoteSignalRkey;
    wr.op = 4;
    wr.send_flag = RA_SEND_SIGNALED | RA_SEND_FENCE;
    send_wr_rsp rsp{};
    ret = DlHccpApi::RaSendWrV2(qp, &wr, &rsp);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion fenced read failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    if (!SubmitDoorbell(rsp)) {
        return false;
    }
    return true;
}

bool RdmaProviderCompletion::SubmitDoorbell(const send_wr_rsp &response)
{
    if (useNativeStream_) {
        return SubmitNativeDoorbell(response);
    }
    auto ret = VendorApi::Api()->RDMADBSend(response.db.db_index, response.db.db_info, stream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion doorbell failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    return true;
}

bool RdmaProviderCompletion::WaitFenceNotify()
{
    if (!useNativeStream_) {
        if (!WaitRuntimeNotify()) {
            return false;
        }
        failed_ = false;
        return true;
    }
    if (!WaitNativeNotify()) {
        return false;
    }
    failed_ = false;
    return true;
}

bool RdmaProviderCompletion::WaitRuntimeNotify()
{
    // Runtime v9.1.0 forwards this value unchanged to the seconds-based notify-wait SQE.
    auto ret = VendorApi::Api()->WaitAndResetNotify(notify_, stream_, PROVIDER_NOTIFY_WAIT_TIMEOUT_SECONDS);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion notify wait failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    ret = VendorApi::Api()->SynchronizeStream(stream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion stream wait failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    return true;
}

bool RdmaProviderCompletion::SubmitNativeDoorbell(const send_wr_rsp &response)
{
    StreamTask task{};
    task.type = STREAM_TASK_TYPE_WRITE_VAL;
    auto &sqe = task.sqe.writeValueSqe;
    sqe.header.type = RT_STARS_SQE_TYPE_WRITE_VALUE;
    sqe.header.wr_cqe = nativeStream_->GetWqeFlag();
    sqe.header.rt_stream_id = static_cast<uint16_t>(nativeStream_->GetId());
    sqe.kernel_credit = RT_STARS_DEFAULT_KERNEL_CREDIT;
    sqe.awsize = PROVIDER_DOORBELL_SIZE_64BIT;
    sqe.sub_type = PROVIDER_DOORBELL_RDMA_SEND;
    sqe.write_value_part0 = static_cast<uint32_t>(response.db.db_info & MASK_32_BIT);
    sqe.write_value_part1 = static_cast<uint32_t>(response.db.db_info >> UINT32_BIT_NUM);
    sqe.write_addr_low = static_cast<uint32_t>(nativeDoorbellAddress_ & MASK_32_BIT);
    sqe.write_addr_high = static_cast<uint32_t>((nativeDoorbellAddress_ >> UINT32_BIT_NUM) & MASK_17_BIT);
    auto ret = nativeStream_->SubmitTasks(task);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider native doorbell failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    return true;
}

bool RdmaProviderCompletion::WaitNativeNotify()
{
    StreamTask task{};
    task.type = STREAM_TASK_TYPE_NOTIFY;
    auto &sqe = task.sqe.notifySqe;
    sqe.header.type = RT_STARS_SQE_TYPE_NOTIFY_WAIT;
    sqe.header.ie = RT_STARS_SQE_INT_DIR_NO;
    sqe.header.pre_p = RT_STARS_SQE_INT_DIR_NO;
    sqe.header.post_p = RT_STARS_SQE_INT_DIR_NO;
    sqe.header.wr_cqe = nativeStream_->GetWqeFlag();
    sqe.kernel_credit = RT_STARS_NEVER_TIMEOUT_KERNEL_CREDIT;
    sqe.header.rt_stream_id = static_cast<uint16_t>(nativeStream_->GetId());
    sqe.notify_id = notifyId_;
    sqe.timeout = PROVIDER_NOTIFY_WAIT_TIMEOUT_SECONDS;
    auto ret = nativeStream_->SubmitTasks(task);
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider native notify submission failed, ret=" << ret);
        return false;
    }
    ret = nativeStream_->Synchronize();
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider native notify wait failed, ret=" << ret);
        return false;
    }
    return true;
}

bool RdmaProviderCompletion::Close()
{
    ready_ = false;
    if (stream_ == nullptr && notify_ == nullptr && nativeStream_ == nullptr) {
        return true;
    }
    auto ret = VendorApi::Api()->SetDevice(static_cast<int32_t>(userId_));
    if (ret != BM_OK) {
        BM_LOG_ERROR("provider completion cleanup set device failed, deviceId=" << userId_ << " ret=" << ret);
        return false;
    }
    if (stream_ != nullptr) {
        ret = VendorApi::Api()->DestroyStream(stream_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider completion stream destruction failed, deviceId=" << userId_ << " ret=" << ret);
            return false;
        }
        stream_ = nullptr;
    }
    if (nativeStream_ != nullptr) {
        nativeStream_->Destroy();
        if (nativeStream_->GetId() != UINT32_MAX) {
            BM_LOG_ERROR("provider native stream cleanup remains pending, streamId=" << nativeStream_->GetId());
            return false;
        }
        nativeStream_.reset();
    }
    if (notify_ != nullptr) {
        ret = VendorApi::Api()->DestroyNotify(notify_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider completion notify destruction failed, deviceId=" << userId_ << " ret=" << ret);
            return false;
        }
        notify_ = nullptr;
    }
    notifyAddress_ = 0;
    notifyLkey_ = 0;
    failed_ = false;
    return true;
}
} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
