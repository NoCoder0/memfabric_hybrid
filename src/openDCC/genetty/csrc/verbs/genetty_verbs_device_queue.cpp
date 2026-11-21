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
#include "genetty_verbs_device_queue.h"
namespace ock {
namespace openDCC {
namespace verbs {
namespace {
constexpr uint32_t DEVICE_QP_VERSION = 1;
constexpr uint32_t DEVICE_QP_SGE = 1;
constexpr uint32_t DEVICE_QP_INLINE = 32;
constexpr uint32_t DEVICE_QP_SERVICE_LEVEL = 4; /* A3 RoCE service level */
constexpr uint32_t DEVICE_QP_RETRY = 7;
constexpr uint32_t DEVICE_QP_TIMEOUT = 20;
constexpr uint32_t DEVICE_DB_TIMEOUT_MS = 60000;
constexpr uint32_t DEVICE_WRITE_OPCODE = 0;
constexpr uint32_t DEVICE_READ_OPCODE = 4;
constexpr int DEVICE_WC_WRITE = 1;
constexpr int DEVICE_WC_READ = 2;
} // namespace

Result DeviceVerbsQueue::Initialize(void *rdev, uint32_t userId, uint32_t logicId, const GenettyOptions &options,
                                    uint32_t cqDepth, uint64_t id) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(rdev != nullptr && options.sqDepth != 0 && options.rqDepth != 0 &&
                              cqDepth >= options.sqDepth && cqDepth <= INT32_MAX && options.trafficClass <= UINT8_MAX,
                          "Invalid Device QP depths or Rdev", GEN_INVALID_PARAM);
    userId_ = userId;
    logicId_ = logicId;
    id_ = id;
    options_ = options;
    /* The queue must not hold its CQ, which already holds this queue. */
    options_.cq = nullptr;
    HccpQpExtAttrs attrs{};
    attrs.qpMode = HCCP_QP_MODE_OP_EXT;
    attrs.version = DEVICE_QP_VERSION;
    attrs.qpAttr.qp_type = HCCP_QPT_RC;
    attrs.qpAttr.cap.max_send_wr = options.sqDepth;
    attrs.qpAttr.cap.max_recv_wr = options.rqDepth;
    attrs.qpAttr.cap.max_send_sge = DEVICE_QP_SGE;
    attrs.qpAttr.cap.max_recv_sge = DEVICE_QP_SGE;
    attrs.qpAttr.cap.max_inline_data = DEVICE_QP_INLINE;
    attrs.cqAttr.sendCqDepth = static_cast<int>(cqDepth);
    attrs.cqAttr.recvCqDepth = static_cast<int>(cqDepth);
    auto result = HccpApi::RaQpCreateWithAttrs(rdev, &attrs, &qp_);
    GEN_ASSERT_MSG_RETURN(result == 0 && qp_ != nullptr, "Create Device QP failed", GEN_DEVICE_ERROR);
    result = HccpApi::RaGetQpAttr(qp_, &attr_);
    GEN_ASSERT_MSG_RETURN(result == 0, "Query Device QP failed", GEN_DEVICE_ERROR);
    return GEN_OK;
}

Result DeviceVerbsQueue::Export(genetty_link_info_t *info, uint32_t flags) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(qp_ != nullptr && !closing_, "Device QP is not initialized", GEN_NOT_INITIALIZED);
    VerbsLinkInfo link;
    link.Type(DEVICE_RDMA_GENETTY);
    link.Vendor(VT_ASCEND_NPU);
    link.Qpn(attr_.qpn);
    link.Psn(attr_.psn);
    link.Mtu(static_cast<uint32_t>(attr_.pathMtu));
    link.PortNum(1);
    link.Gid(attr_.gid);
    link.SqDepth(options_.sqDepth);
    link.RqDepth(options_.rqDepth);
    return link.Export(info, flags);
}

Result DeviceVerbsQueue::Import(genetty_link_info_t *info, uint32_t flags) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(qp_ != nullptr && !connected_ && !failed_ && !closing_, "Device QP cannot import peer info",
                          GEN_INVALID_PARAM);
    VerbsLinkInfo peer;
    auto result = peer.Import(info, flags);
    if (result != GEN_OK) {
        return result;
    }
    GEN_ASSERT_MSG_RETURN(peer.Qpn() != 0 && (peer.Type() == HOST_RDMA_GENETTY || peer.Type() == DEVICE_RDMA_GENETTY),
                          "Invalid Device QP peer", GEN_INVALID_PARAM);
    HccpTypicalQp local{};
    HccpTypicalQp remote{};
    local.qpn = attr_.qpn;
    local.psn = attr_.psn;
    local.gidIdx = attr_.gidIdx;
    memcpy(local.gid, attr_.gid, sizeof(local.gid));
    local.tc = options_.trafficClass;
    local.sl = DEVICE_QP_SERVICE_LEVEL;
    local.retryCnt = DEVICE_QP_RETRY;
    local.retryTime = DEVICE_QP_TIMEOUT;
    remote.qpn = peer.Qpn();
    remote.psn = peer.Psn();
    memcpy(remote.gid, peer.Gid(), sizeof(remote.gid));
    result = HccpApi::RaTypicalQpModify(qp_, &local, &remote);
    if (result != 0 || HccpApi::RaGetQpAttr(qp_, &attr_) != 0) {
        failed_ = true;
        GEN_LOG_ERROR("Connect Device QP failed, qp(" << attr_.qpn << ")");
        return GEN_DEVICE_ERROR;
    }
    connected_ = true;
    return GEN_OK;
}

Result DeviceVerbsQueue::Post(genetty_batch_op_data_t *data, uint32_t count,
                              genetty_batch_op_options_t *options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(data != nullptr && options != nullptr && count == 1 && options->signal == 1 &&
                              options->kick_db == 1 && (options->op_type == OP_READ || options->op_type == OP_WRITE),
                          "Device post requires one signaled READ/WRITE with doorbell", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(connected_ && !failed_ && !closing_ && !pending_ && qp_ != nullptr,
                          "Device QP is not ready or has an outstanding request", GEN_POST_FAILED);
    GEN_ASSERT_MSG_RETURN(data->laddress != nullptr && data->raddress != nullptr && data->lsize != 0 &&
                              data->lsize <= data->rsize,
                          "Invalid Device RDMA memory range", GEN_INVALID_PARAM);
    if (!tsReady_) {
        auto result = ts_.Initialize(userId_, logicId_);
        if (result != GEN_OK) {
            failed_ = true;
            return result;
        }
        tsReady_ = true;
    }
    HccpSgList sge{reinterpret_cast<uint64_t>(data->laddress), data->lsize, data->lkey};
    HccpSendWrV2 wr{};
    wr.wr_id = options->usr_ctx;
    wr.buf_list = &sge;
    wr.buf_num = 1;
    wr.dst_addr = reinterpret_cast<uint64_t>(data->raddress);
    wr.rkey = data->rkey;
    wr.op = options->op_type == OP_READ ? DEVICE_READ_OPCODE : DEVICE_WRITE_OPCODE;
    wr.send_flag = HCCP_RA_SEND_SIGNALED;
    HccpSendWrRsp response{};
    auto result = HccpApi::RaSendWrV2(qp_, &wr, &response);
    if (result != 0 || ts_.Submit(response.db.db_info, DEVICE_DB_TIMEOUT_MS) != GEN_OK) {
        failed_ = true;
        GEN_LOG_ERROR("Submit Device RDMA request failed, qp(" << attr_.qpn << "), result(" << result << ")");
        return GEN_POST_FAILED;
    }
    requestId_ = options->usr_ctx;
    operation_ = options->op_type;
    pending_ = true;
    return GEN_OK;
}

int32_t DeviceVerbsQueue::Poll(int32_t count, genetty_cq_wc_t *wc) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(count > 0 && wc != nullptr, "Invalid Device CQ poll arguments", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(qp_ != nullptr && !closing_, "Device CQ is closed", GEN_NOT_INITIALIZED);
    HccpLiteWcV2 raw{};
    auto result = HccpApi::RaPollCq(qp_, true, 1, &raw);
    if (result <= 0) {
        return result;
    }
    auto opcode = operation_ == OP_READ ? DEVICE_WC_READ : DEVICE_WC_WRITE;
    bool valid = result == 1 && pending_ && raw.wc.wr_id == requestId_ && raw.wc.opcode == opcode;
    wc[0].usr_ctx = raw.wc.wr_id;
    wc[0].genetty_id = id_;
    wc[0].type = operation_;
    wc[0].status = valid && raw.wc.status == 0 ? OPS_SUCCESS : OPS_ERROR;
    pending_ = false;
    if (wc[0].status != OPS_SUCCESS) {
        failed_ = true;
        GEN_LOG_ERROR("Device CQ completion failed, status(" << raw.wc.status << "), vendor(" << raw.wc.vendor_err
                                                             << "), wr id(" << raw.wc.wr_id << ")");
    }
    return 1;
}

Result DeviceVerbsQueue::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
    GEN_ASSERT_MSG_RETURN(!destroyFailed_, "Device QP destruction failed after consuming its handle", GEN_DEVICE_ERROR);
    auto result = ts_.Destroy();
    if (result != GEN_OK) {
        return result;
    }
    if (qp_ != nullptr) {
        result = HccpApi::RaQpDestroy(qp_);
        if (result != 0) {
            destroyFailed_ = qp_ == nullptr;
            return GEN_DEVICE_ERROR;
        }
    }
    connected_ = false;
    pending_ = false;
    return GEN_OK;
}

bool DeviceVerbsQueue::CleanupPending() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_ && (qp_ != nullptr || destroyFailed_);
}
} // namespace verbs
} // namespace openDCC
} // namespace ock
