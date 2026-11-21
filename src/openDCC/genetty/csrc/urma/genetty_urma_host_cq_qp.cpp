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
#include "genetty_urma_host_cq_qp.h"

#include <algorithm>
#include <vector>

#include "genetty_urma_info.h"

namespace ock {
namespace openDCC {
namespace urma {
Result UrmaHostCQ::VerifyOptions(const GenettyCQOptions &options) noexcept
{
    GEN_ASSERT_MSG_RETURN(options.queueDepth != 0, "Invalid cq options, as queue depth is 0", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(options.polling_type < GCQ_TYPE_COUNT,
                          "Invalid cq options, as polling type(" << static_cast<uint32_t>(options.polling_type)
                                                                 << ") is invalid", GEN_INVALID_PARAM);
    return GEN_OK;
}

Result UrmaHostCQ::Initialize(const GenettyCQOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Already initialized, cq(" << id_ << ")");
        return GEN_OK;
    }

    /* step1: verify the options and the context */
    auto result = VerifyOptions(options);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize cq failed, as verify options failed, result(" << result << ")");
        return result;
    }

    if (UNLIKELY(hostCtx_ == nullptr || hostCtx_->Context() == nullptr)) {
        GEN_LOG_ERROR("Initialize cq failed, as context is null, queue depth(" << options.queueDepth << ")");
        return GEN_INVALID_PARAM;
    }

    /* step2: create the jfc with the given depth and polling type */
    auto pollingType = options.polling_type == GCQ_TYPE_EVENT ? EVENT_POLLING : BUSY_POLLING;
    auto cfg = hostCtx_->Context()->CreateJfcCfg(options.queueDepth);
    result = hostCtx_->Context()->CreateJfc(cfg, pollingType, jfc_);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize cq failed, as create jfc failed, result(" << result << "), queue depth("
                                                                            << options.queueDepth << ")");
        return result;
    }

    /* step3: assign id */
    options_ = options;
    id_ = GENETTY_CQ_ID_GENERATOR.fetch_add(1, std::memory_order_relaxed);
    inited_ = true;

    GEN_LOG_DEBUG("Initialize cq(" << id_ << ") finished, queue depth(" << options.queueDepth << "), polling type("
                                   << options.polling_type << ")");
    return GEN_OK;
}

void UrmaHostCQ::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Not initialized, cq(" << id_ << ")");
        return;
    }

    if (jfc_ != nullptr) {
        jfc_->Destroy();
        jfc_ = nullptr;
    }

    inited_ = false;
    GEN_LOG_DEBUG("Destroy cq(" << id_ << ") finished, queue depth(" << options_.queueDepth << ")");
}

int32_t UrmaHostCQ::GetEventFd(int32_t *fd, uint32_t flags) noexcept
{
    if (UNLIKELY(fd == nullptr)) {
        GEN_LOG_ERROR("Get cq event fd failed, as fd is null, cq(" << id_ << ")");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(!inited_ || jfc_ == nullptr)) {
        GEN_LOG_ERROR("Get cq event fd failed, as cq is not initialized, cq(" << id_ << "), flags(" << flags << ")");
        return GEN_NOT_INITIALIZED;
    }

    auto eventFd = jfc_->GetInterruptFd();
    if (UNLIKELY(eventFd < 0)) {
        GEN_LOG_ERROR("Get cq event fd failed, as the cq has no event channel, cq(" << id_ << ")");
        return GEN_ERROR;
    }

    *fd = eventFd;
    GEN_LOG_DEBUG("Get cq event fd(" << eventFd << ") successfully, cq(" << id_ << ")");
    return GEN_OK;
}

int32_t UrmaHostCQ::Poll(int32_t num, genetty_cq_wc_t *wc, uint32_t flags) noexcept
{
    if (UNLIKELY(num <= 0 || wc == nullptr)) {
        GEN_LOG_ERROR("Poll cq failed, as the parameters are invalid, num(" << num << "), cq(" << id_ << ")");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(!inited_ || jfc_ == nullptr)) {
        GEN_LOG_ERROR("Poll cq failed, as cq is not initialized, cq(" << id_ << "), num(" << num << ")");
        return GEN_NOT_INITIALIZED;
    }

    std::vector<urma_cr_t> crs(static_cast<size_t>(num));
    auto polled = jfc_->Poll(num, crs.data());
    if (UNLIKELY(polled < 0)) {
        GEN_LOG_ERROR("Poll cq failed, cq(" << id_ << "), num(" << num << "), result(" << polled << "), flags("
                                            << flags << ")");
        return GEN_ERROR;
    }

    for (int32_t i = 0; i < polled; i++) {
        wc[i].usr_ctx = crs[i].user_ctx;
        wc[i].genetty_id = id_;
        wc[i].status = crs[i].status == URMA_CR_SUCCESS ? OPS_SUCCESS : OPS_ERROR;
        wc[i].type = crs[i].opcode == URMA_CR_OPC_WRITE_WITH_IMM ? OP_WRITE_IMM : OP_WRITE;
    }

    return polled;
}

int32_t UrmaHostCQ::Rearm(int32_t solicited_only, uint32_t flags) noexcept
{
    if (UNLIKELY(!inited_ || jfc_ == nullptr)) {
        GEN_LOG_ERROR("Rearm cq failed, as cq is not initialized, cq(" << id_ << "), flags(" << flags << ")");
        return GEN_NOT_INITIALIZED;
    }

    auto result = jfc_->Rearm(solicited_only != 0);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Rearm cq failed, cq(" << id_ << "), result(" << result << "), solicited only("
                                             << solicited_only << "), flags(" << flags << ")");
        return result;
    }

    return GEN_OK;
}

int32_t UrmaHostCQ::Ack(int32_t eventCount, uint32_t flags) noexcept
{
    (void)flags; /* the flags is reserved for now */
    if (UNLIKELY(!inited_ || jfc_ == nullptr || eventCount < 0)) {
        GEN_LOG_ERROR("Ack cq failed, as the parameters are invalid, cq(" << id_ << "), event count(" << eventCount
                                                                         << ")");
        return GEN_INVALID_PARAM;
    }

    jfc_->Ack(static_cast<uint32_t>(eventCount));
    return GEN_OK;
}

Result UrmaHostCQ::GetDeviceAccessInfo(uint32_t flags, genetty_cq_device_access_info_t *info) noexcept
{
    (void)flags, (void)info;
    GEN_LOG_ERROR("Get cq device access info failed, as it is not supported by the host urma cq, cq(" << id_ << ")");
    return GEN_ERROR;
}

Result UrmaHostQp::VerifyOptions(const GenettyOptions &options) noexcept
{
    GEN_ASSERT_MSG_RETURN(options.cq != nullptr, "Invalid qp options, as cq is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(options.sqDepth != 0, "Invalid qp options, as sq depth is 0", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(options.rqDepth != 0, "Invalid qp options, as rq depth is 0", GEN_INVALID_PARAM);
    return GEN_OK;
}

Result UrmaHostQp::Initialize(const GenettyOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Already initialized, genetty(" << id_ << ")");
        return GEN_OK;
    }

    /* step1: verify the options and get the cq created by this backend */
    auto result = VerifyOptions(options);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize genetty failed, as verify options failed, result(" << result << ")");
        return result;
    }

    auto hostCq = mf::RefDynamicCast<UrmaHostCQ>(options.cq);
    if (UNLIKELY(hostCq == nullptr) || UNLIKELY(hostCq->Jfc() == nullptr)) {
        GEN_LOG_ERROR("Initialize genetty failed, as the cq is not a urma host cq, type(" << options.cq->Type() << ")");
        return GEN_INVALID_PARAM;
    }

    /* step2: create the jfs, jfr and jetty on the context of the cq */
    ctx_ = hostCq->HostContext();
    if (UNLIKELY(ctx_ == nullptr || ctx_->Context() == nullptr)) {
        GEN_LOG_ERROR("Initialize genetty failed, as context is null, sq depth(" << options.sqDepth << ")");
        return GEN_NOT_INITIALIZED;
    }

    auto depth = std::min(options.sqDepth, options.rqDepth);
    result = ctx_->Context()->CreateJfsJfrJetty(hostCq->Jfc(), depth, URMA_TM_RC, URMA_RTP, jfs_, jfr_, jetty_);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize genetty failed, as create jfs/jfr/jetty failed, result(" << result << "), sq depth("
                                                                                           << options.sqDepth << ")");
        return result;
    }

    /* step3: assign id */
    options_ = options;
    id_ = GENETTY_ID_GENERATOR.fetch_add(1, std::memory_order_relaxed);
    inited_ = true;

    GEN_LOG_DEBUG("Initialize genetty(" << id_ << ") finished, jetty id(" << jetty_->JettyId() << ")");
    return GEN_OK;
}

void UrmaHostQp::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Not initialized, genetty(" << id_ << ")");
        return;
    }

    if (jetty_ != nullptr) {
        jetty_->Destroy();
        jetty_ = nullptr;
    }

    if (jfs_ != nullptr) {
        jfs_->Destroy();
        jfs_ = nullptr;
    }

    if (jfr_ != nullptr) {
        jfr_->Destroy();
        jfr_ = nullptr;
    }

    inited_ = false;
    GEN_LOG_DEBUG("Destroy genetty(" << id_ << ") finished, sq depth(" << options_.sqDepth << ")");
}

Result UrmaHostQp::Export(uint32_t flags, genetty_link_info_t *info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (UNLIKELY(!inited_ || jetty_ == nullptr)) {
        GEN_LOG_ERROR("Export genetty failed, as genetty is not initialized, genetty(" << id_ << "), flags(" << flags
                                                                                       << ")");
        return GEN_NOT_INITIALIZED;
    }

    UrmaLinkInfo linkInfo;
    linkInfo.Type(type_);
    linkInfo.JettyId(jetty_->RawJettyId());
    linkInfo.Token(jetty_->Token());

    auto result = linkInfo.Export(info, flags);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Export genetty failed, as export link info failed, result(" << result << "), genetty(" << id_
                                                                                   << ")");
        return result;
    }

    GEN_LOG_DEBUG("Export genetty(" << id_ << ") finished, jetty id(" << jetty_->JettyId() << ")");
    return GEN_OK;
}

Result UrmaHostQp::Import(uint32_t flags, genetty_link_info_t *info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (UNLIKELY(!inited_ || jetty_ == nullptr)) {
        GEN_LOG_ERROR("Import genetty failed, as genetty is not initialized, genetty(" << id_ << "), flags(" << flags
                                                                                       << ")");
        return GEN_NOT_INITIALIZED;
    }

    UrmaLinkInfo linkInfo;
    auto result = linkInfo.Import(info, flags);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Import genetty failed, as import link info failed, result(" << result << "), genetty(" << id_
                                                                                   << ")");
        return result;
    }

    auto jettyId = linkInfo.JettyId();
    auto token = linkInfo.Token();
    result = jetty_->ImportRemoteJetty(jettyId, token);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Import genetty failed, as import remote jetty failed, result(" << result << "), genetty(" << id_
                                                                                      << "), jetty id(" << jettyId.id
                                                                                      << ")");
        return result;
    }

    /* the peer eid is needed when importing the memory segment of the peer */
    ctx_->SetPeerEid(jettyId.eid);

    GEN_LOG_DEBUG("Import genetty(" << id_ << ") finished, remote jetty id(" << jettyId.id << ")");
    return GEN_OK;
}

Result UrmaHostQp::PostOne(genetty_batch_op_data_t &opData, genetty_batch_op_options_t &options) noexcept
{
    auto localSeg = ctx_->FindLocalSeg(opData.lkey);
    auto remoteSeg = ctx_->FindRemoteSeg(opData.rkey);
    if (UNLIKELY(localSeg == nullptr || remoteSeg == nullptr)) {
        GEN_LOG_ERROR("Post failed, as the segment is not found, genetty(" << id_ << "), lkey(" << opData.lkey
                                                                          << "), rkey(" << opData.rkey << ")");
        return GEN_INVALID_PARAM;
    }

    urma_sge_t localSge{};
    localSge.addr = reinterpret_cast<uint64_t>(opData.laddress);
    localSge.len = opData.lsize;
    localSge.tseg = localSeg;

    urma_sge_t remoteSge{};
    remoteSge.addr = reinterpret_cast<uint64_t>(opData.raddress);
    remoteSge.len = opData.rsize;
    remoteSge.tseg = remoteSeg;

    urma_jfs_wr_t wr{};
    wr.user_ctx = options.usr_ctx;
    wr.flag.bs.comp_order = 1;
    wr.flag.bs.complete_enable = options.signal != 0;
    wr.next = nullptr;

    if (options.op_type == OP_READ) {
        wr.opcode = URMA_OPC_READ;
        wr.rw.src = {&remoteSge, 1};
        wr.rw.dst = {&localSge, 1};
    } else if (options.op_type == OP_WRITE || options.op_type == OP_WRITE_IMM) {
        wr.opcode = options.op_type == OP_WRITE_IMM ? URMA_OPC_WRITE_IMM : URMA_OPC_WRITE;
        wr.rw.src = {&localSge, 1};
        wr.rw.dst = {&remoteSge, 1};
        wr.rw.notify_data = options.imm;
    } else {
        GEN_LOG_ERROR("Post failed, as op type(" << static_cast<int32_t>(options.op_type)
                                                 << ") is not supported, genetty(" << id_ << ")");
        return GEN_INVALID_PARAM;
    }

    urma_jfs_wr_t *badWr = nullptr;
    auto status = jetty_->PostSendWr(&wr, &badWr);
    if (UNLIKELY(status != URMA_SUCCESS)) {
        GEN_LOG_ERROR("Post failed, genetty(" << id_ << "), op type(" << static_cast<int32_t>(options.op_type)
                                              << "), status(" << status << "), bad wr(" << badWr << ")");
        return GEN_ERROR;
    }

    return GEN_OK;
}

Result UrmaHostQp::BatchPost(genetty_batch_op_data_t *opData, uint32_t batchSize,
                             genetty_batch_op_options_t *options) noexcept
{
    GEN_ASSERT_MSG_RETURN(opData != nullptr, "Invalid op data, it is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Invalid options, it is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(batchSize != 0, "Invalid batch size, it is 0", GEN_INVALID_PARAM);

    std::lock_guard<std::mutex> lock(mutex_);
    if (UNLIKELY(!inited_ || jetty_ == nullptr)) {
        GEN_LOG_ERROR("Batch post failed, as genetty is not initialized, genetty(" << id_ << "), batch size("
                                                                                   << batchSize << ")");
        return GEN_NOT_INITIALIZED;
    }

    for (uint32_t i = 0; i < batchSize; i++) {
        auto result = PostOne(opData[i], *options);
        if (UNLIKELY(result != GEN_OK)) {
            GEN_LOG_ERROR("Batch post failed, genetty(" << id_ << "), index(" << i << "), batch size(" << batchSize
                                                        << ")");
            return result;
        }
    }

    GEN_LOG_DEBUG("Post finished, genetty(" << id_ << "), batch size(" << batchSize << "), op type("
                                            << static_cast<int32_t>(options->op_type) << "), signaled("
                                            << static_cast<uint32_t>(options->signal) << ")");
    return GEN_OK;
}

Result UrmaHostQp::GetDeviceAccessInfo(uint32_t flags, genetty_device_access_info_t *info) noexcept
{
    (void)flags, (void)info;
    GEN_LOG_ERROR("Get genetty device access info failed, as it is not supported by the host urma genetty, genetty("
                  << id_ << ")");
    return GEN_ERROR;
}
} // namespace urma
} // namespace openDCC
} // namespace ock
