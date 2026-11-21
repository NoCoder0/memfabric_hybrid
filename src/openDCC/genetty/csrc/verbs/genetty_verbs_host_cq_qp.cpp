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
#include "genetty_verbs_host_cq_qp.h"

#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <new>

#include "genetty_verbs_info.h"

namespace ock {
namespace openDCC {
namespace verbs {
/**
 * Functions of the host cq
 */
Result VerbsHostCQ::VerifyOptions(const GenettyCQOptions &options) noexcept
{
    if (UNLIKELY(options.queueDepth == 0)) {
        GEN_LOG_ERROR("Verify cq options failed, as queue depth is 0");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(options.polling_type >= GCQ_TYPE_COUNT)) {
        GEN_LOG_ERROR("Verify cq options failed, as polling type(" << static_cast<uint32_t>(options.polling_type)
                                                                   << ") is invalid, queue depth(" << options.queueDepth
                                                                   << ")");
        return GEN_INVALID_PARAM;
    }

    return GEN_OK;
}

Result VerbsHostCQ::Initialize(const GenettyCQOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Already initialized, cq(" << id_ << ")");
        return GEN_OK;
    }

    /* step1: verify options */
    auto result = VerifyOptions(options);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize cq failed, as verify options failed, result(" << result << "), queue depth("
                                                                                << options.queueDepth << ")");
        return result;
    }

    /* step2: verify context create the verbs cq */
    if (UNLIKELY(ctx_ == nullptr)) {
        GEN_LOG_ERROR("Initialize cq failed, ctx is null");
        return GEN_ERROR;
    }

    result = ctx_->CreateCq(options.queueDepth, options.polling_type, cq_);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize cq failed, as create verbs cq failed, result("
                      << result << "), queue depth(" << options.queueDepth << "), polling type("
                      << static_cast<uint32_t>(options.polling_type) << ")");
        return result;
    }

    /* step3: assign id */
    options_ = options;
    id_ = GENETTY_CQ_ID_GENERATOR.fetch_add(1, std::memory_order_relaxed);
    inited_ = true;

    GEN_LOG_DEBUG("Initialize cq(" << id_ << ") finished, queue depth(" << options.queueDepth << "), polling type("
                                   << static_cast<uint32_t>(options.polling_type) << ")");
    return GEN_OK;
}

void VerbsHostCQ::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Not initialized, cq(" << id_ << ")");
        return;
    }

    DestroyInner();

    inited_ = false;
}

void VerbsHostCQ::DestroyInner() noexcept
{
    if (cq_ != nullptr) {
        cq_->Destroy();
        cq_ = nullptr;
    }

    GEN_LOG_DEBUG("Destroy cq(" << id_ << ") finished, queue depth(" << options_.queueDepth << ")");
}

int32_t VerbsHostCQ::GetEventFd(int32_t *fd, uint32_t flags) noexcept
{
    if (UNLIKELY(fd == nullptr)) {
        GEN_LOG_ERROR("Get cq event fd failed, as fd is null, cq(" << id_ << ")");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(cq_ == nullptr)) {
        GEN_LOG_ERROR("Get cq event fd failed, as cq is not initialized, cq(" << id_ << ")");
        return GEN_NOT_INITIALIZED;
    }

    auto eventFd = cq_->Fd();
    if (UNLIKELY(eventFd < 0)) {
        GEN_LOG_ERROR("Get cq event fd failed, as the cq has no event channel, cq(" << id_ << "), fd(" << eventFd
                                                                                    << "), flags(" << flags << ")");
        return GEN_ERROR;
    }

    *fd = eventFd;
    GEN_LOG_DEBUG("Get cq event fd(" << eventFd << ") successfully, cq(" << id_ << ")");
    return GEN_OK;
}

int32_t VerbsHostCQ::Poll(int32_t num, genetty_cq_wc_t *wc, uint32_t flags) noexcept
{
    /* caller must ensure the param is valid */
    std::unique_ptr<struct ibv_wc[]> rawWcs(new (std::nothrow) struct ibv_wc[num]());
    if (UNLIKELY(rawWcs == nullptr)) {
        GEN_LOG_ERROR("Poll cq failed, as allocate wc array failed, cq(" << id_ << "), num(" << num << ")");
        return GEN_MALLOC_FAILED;
    }

    auto polled = cq_->Poll(num, rawWcs.get());
    if (UNLIKELY(polled < 0)) {
        GEN_LOG_ERROR("Poll cq failed, cq(" << id_ << "), num(" << num << "), flags(" << flags << ")");
        return GEN_ERROR;
    }

    for (int32_t i = 0; i < polled; i++) {
        const auto &rawWc = rawWcs[i];
        wc[i].status = rawWc.status == IBV_WC_SUCCESS ? OPS_SUCCESS : OPS_ERROR;
        wc[i].genetty_id = 0; /* the genetty id is not carried by the host rdma completion */
        wc[i].usr_ctx = rawWc.wr_id;
        switch (rawWc.opcode) {
            case IBV_WC_RDMA_READ:
                wc[i].type = OP_READ;
                break;
            case IBV_WC_RECV_RDMA_WITH_IMM:
                wc[i].type = OP_WRITE_IMM;
                break;
            default:
                wc[i].type = OP_WRITE;
                break;
        }
    }

    return polled;
}

Result VerbsHostCQ::GetDeviceAccessInfo(uint32_t flags, genetty_cq_device_access_info_t *info) noexcept
{
    if (UNLIKELY(info == nullptr)) {
        GEN_LOG_ERROR("Get cq device access info failed, as info is null, cq(" << id_ << ")");
        return GEN_INVALID_PARAM;
    }

    /* the host rdma cq has no address which can be accessed by the device */
    GEN_LOG_ERROR("Get cq device access info failed, as it is not supported by the host rdma cq, cq("
                  << id_ << "), flags(" << flags << ")");
    return GEN_ERROR;
}

/**
 * Functions of the host genetty qp
 */
Result VerbsHostQp::VerifyOptions(const GenettyOptions &options) noexcept
{
    GEN_ASSERT_MSG_RETURN(options.trafficClass <= UINT8_MAX, "Invalid RoCE traffic class", GEN_INVALID_PARAM);
    if (UNLIKELY(options.cq == nullptr)) {
        GEN_LOG_ERROR("Verify genetty options failed, as cq is null");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(options.sqDepth == 0 || options.rqDepth == 0)) {
        GEN_LOG_ERROR("Verify genetty options failed, as sq depth(" << options.sqDepth << ") or rq depth("
                                                                    << options.rqDepth << ") is 0");
        return GEN_INVALID_PARAM;
    }

    return GEN_OK;
}

Result VerbsHostQp::Initialize(const GenettyOptions &options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        GEN_LOG_DEBUG("Already initialized, genetty(" << id_ << ")");
        return GEN_OK;
    }

    /* step1: verify options */
    auto result = VerifyOptions(options);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize genetty failed, as verify options failed, result("
                      << result << "), sq depth(" << options.sqDepth << "), rq depth(" << options.rqDepth << ")");
        return result;
    }

    /* step2: get the verbs cq created by this backend and check the device it works on */
    auto hostCq = mf::RefDynamicCast<VerbsHostCQ>(options.cq);
    if (UNLIKELY(hostCq == nullptr)) {
        GEN_LOG_ERROR("Initialize genetty failed, as the cq is not a verbs host cq, type(" << options.cq->Type()
                                                                                           << ")");
        return GEN_INVALID_PARAM;
    }

    if (UNLIKELY(hostCq->Type() != Type())) {
        GEN_LOG_ERROR("Initialize genetty failed, as cq type(" << hostCq->Type() << ") is not the same as qp type("
                                                               << Type() << ")");
        return GEN_INVALID_PARAM;
    }

    const auto &verbsCq = hostCq->Cq();
    if (UNLIKELY(verbsCq == nullptr || verbsCq->Context() == nullptr)) {
        GEN_LOG_ERROR("Initialize genetty failed, as the cq is not initialized, cq(" << hostCq->Id() << "), sq depth("
                                                                                     << options.sqDepth << ")");
        return GEN_DEVICE_ERROR;
    }

    /* step3: create the qp */
    result = verbsCq->Context()->CreateQp(options.sqDepth, options.rqDepth, verbsCq, verbsCq, qp_);
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Initialize genetty failed, as create verbs qp failed, result("
                      << result << "), sq depth(" << options.sqDepth << "), rq depth(" << options.rqDepth << ")");
        return result;
    }

    /* step4: assign id */
    options_ = options;
    id_ = GENETTY_ID_GENERATOR.fetch_add(1, std::memory_order_relaxed);
    inited_ = true;

    GEN_LOG_DEBUG("Initialize genetty(" << id_ << ") finished, qp num(" << qp_->QpNumber() << "), sq depth("
                                        << options.sqDepth << ")");
    return GEN_OK;
}

Result VerbsHostQp::Quiesce() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qp_ == nullptr ? GEN_OK : qp_->Quiesce();
}

void VerbsHostQp::Destroy() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        GEN_LOG_DEBUG("Not initialized, genetty(" << id_ << ")");
        return;
    }

    DestroyInner();

    inited_ = false;
}

void VerbsHostQp::DestroyInner() noexcept
{
    uint32_t qpNum = 0;
    if (qp_ != nullptr) {
        qpNum = qp_->QpNumber();
        qp_->Destroy();
        qp_ = nullptr;
    }

    GEN_LOG_DEBUG("Destroy genetty(" << id_ << ") finished, qp num(" << qpNum << ")");
}

Result VerbsHostQp::GetDeviceAccessInfo(uint32_t flags, genetty_device_access_info_t *info) noexcept
{
    if (UNLIKELY(info == nullptr)) {
        GEN_LOG_ERROR("Get genetty device access info failed, as info is null, genetty(" << id_ << ")");
        return GEN_INVALID_PARAM;
    }

    /* the host rdma qp has no queue or door bell which can be accessed by the device */
    GEN_LOG_ERROR("Get genetty device access info failed, as it is not supported by the host rdma qp, genetty("
                  << id_ << "), flags(" << flags << ")");
    return GEN_ERROR;
}

Result VerbsHostQp::Export(uint32_t flags, genetty_link_info_t *info)
{
    /* step1: check params */
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Export failed, as info is null", GEN_INVALID_PARAM);

    /* step2: lock and check if initialized */
    std::lock_guard<std::mutex> lock(mutex_);
    if (UNLIKELY(!inited_ || qp_ == nullptr)) {
        GEN_LOG_ERROR("Export failed, as genetty(" << id_ << ") is not initialized");
        return GEN_NOT_INITIALIZED;
    }

    /* step3: create VerbsLinkInfo and fill its info after get info with VerbsQP::GetLocalInfo */
    VerbsQpInfo qpInfo;
    auto result = qp_->GetLocalInfo(qpInfo);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Export failed, as get local qp info failed, result(" << result << "), genetty(" << id_
                                                                            << "), qp(" << qp_->QpNumber() << ")");
        return result;
    }

    VerbsLinkInfo linkInfo;
    linkInfo.Type(type_);
    linkInfo.Vendor(options_.vendor);
    linkInfo.Qpn(qpInfo.qpNumber);
    linkInfo.Psn(qpInfo.psn);
    linkInfo.Mtu(qpInfo.mtu);
    linkInfo.Lid(qpInfo.lid);
    linkInfo.PortNum(qpInfo.portNum);
    linkInfo.Gid(qpInfo.gid);
    linkInfo.SqDepth(qp_->SendQueueDepth());
    linkInfo.RqDepth(qp_->RecvQueueDepth());

    /* step4: call VerbsLinkInfo to export info */
    result = linkInfo.Export(info, flags);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Export failed, as export link info failed, result(" << result << "), genetty(" << id_ << "), qp("
                                                                           << qpInfo.qpNumber << ")");
        return result;
    }

    GEN_LOG_DEBUG("Export finished, genetty(" << id_ << "), " << linkInfo);
    return GEN_OK;
}

Result VerbsHostQp::Import(uint32_t flags, genetty_link_info_t *info)
{
    /* step1: check params */
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Import failed, as info is null", GEN_INVALID_PARAM);

    /* step2: lock and check if initialized */
    std::lock_guard<std::mutex> lock(mutex_);
    if (UNLIKELY(!inited_ || qp_ == nullptr)) {
        GEN_LOG_ERROR("Import failed, as genetty(" << id_ << ") is not initialized");
        return GEN_NOT_INITIALIZED;
    }

    /* step3: create VerbsLinkInfo and do import info to VerbsLinkInfo */
    VerbsLinkInfo linkInfo;
    auto result = linkInfo.Import(info, flags);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Import failed, as import link info failed, result(" << result << "), genetty(" << id_ << ")");
        return result;
    }

    /* step4: call VerbsQP::Import to modify qp status to RTR and RTS */
    VerbsQpInfo remote;
    remote.qpNumber = linkInfo.Qpn();
    remote.psn = linkInfo.Psn();
    remote.mtu = linkInfo.Mtu();
    remote.lid = linkInfo.Lid();
    remote.portNum = linkInfo.PortNum();
    memcpy(remote.gid, linkInfo.Gid(), ROCE_GID_LEN);

    const auto &device = qp_->Context()->Device();
    if (UNLIKELY(device == nullptr)) {
        GEN_LOG_ERROR("Import failed, as device is null, genetty(" << id_ << "), remote qp(" << remote.qpNumber << ")");
        return GEN_DEVICE_ERROR;
    }

    auto activeMtu = static_cast<uint32_t>(device->PortAttributes().active_mtu);
    GEN_ASSERT_MSG_RETURN(remote.mtu != 0 && (linkInfo.Type() != DEVICE_RDMA_GENETTY || remote.mtu <= activeMtu),
                          "Import failed, unsupported path MTU(" << remote.mtu << ")", GEN_INVALID_PARAM);
    remote.mtu = std::min(remote.mtu, activeMtu);

    result = qp_->Init(qp_->Context()->Device()->PortIndex());
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Import failed, as init local qp failed, result(" << result << "), genetty(" << id_ << "), qp("
                                                                        << qp_->QpNumber() << "), port("
                                                                        << device->PortIndex() << ")");
        return result;
    }

    constexpr uint8_t DEVICE_SERVICE_LEVEL = 4;
    auto serviceLevel = linkInfo.Type() == DEVICE_RDMA_GENETTY ? DEVICE_SERVICE_LEVEL : 0;
    result = qp_->Import(remote, static_cast<uint8_t>(options_.trafficClass), serviceLevel);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Import failed, as connect qp failed, result(" << result << "), genetty(" << id_ << "), local qp("
                                                                     << qp_->QpNumber() << "), remote qp("
                                                                     << remote.qpNumber << ")");
        return result;
    }

    GEN_LOG_DEBUG("Import finished, genetty(" << id_ << "), " << linkInfo);
    return GEN_OK;
}

Result VerbsHostQp::BatchPost(genetty_batch_op_data_t *op_data, uint32_t batchSize,
                              genetty_batch_op_options_t *options) noexcept
{
    /* step1: verify the parameters */
    GEN_ASSERT_MSG_RETURN(op_data != nullptr, "Invalid op data, it is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Invalid options, it is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(batchSize != 0, "Invalid batch size, it is 0", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(inited_, "Not initialized", GEN_NOT_INITIALIZED);
    GEN_ASSERT_MSG_RETURN(options->imm <= UINT32_MAX, "Invalid imm, should <=UINT32_MAX", GEN_INVALID_PARAM);

    std::lock_guard<std::mutex> lock(mutex_);
    GEN_ASSERT_MSG_RETURN(qp_ != nullptr && batchSize <= options_.sqDepth && options->op_type >= OP_READ &&
                              options->op_type < OP_COUNT,
                          "Invalid Host RDMA post options", GEN_INVALID_PARAM);
    std::vector<ibv_sge> sges(batchSize);
    std::vector<ibv_send_wr> requests(batchSize);
    for (uint32_t i = 0; i < batchSize; ++i) {
        const auto &data = op_data[i];
        GEN_ASSERT_MSG_RETURN(data.laddress != nullptr && data.raddress != nullptr && data.lsize != 0 &&
                                  data.lsize <= data.rsize,
                              "Invalid Host RDMA memory range", GEN_INVALID_PARAM);
        sges[i] = {reinterpret_cast<uint64_t>(data.laddress), data.lsize, data.lkey};
        auto &wr = requests[i];
        wr.wr_id = options->usr_ctx;
        wr.sg_list = &sges[i];
        wr.num_sge = 1;
        wr.opcode = options->op_type == OP_READ
                        ? IBV_WR_RDMA_READ
                        : (options->op_type == OP_WRITE ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_WRITE_WITH_IMM);
        wr.send_flags = options->signal != 0 ? IBV_SEND_SIGNALED : 0;
        wr.imm_data = htonl(static_cast<uint32_t>(options->imm));
        wr.wr.rdma.remote_addr = reinterpret_cast<uint64_t>(data.raddress);
        wr.wr.rdma.rkey = data.rkey;
        wr.next = i + 1 < batchSize ? &requests[i + 1] : nullptr;
    }
    ibv_send_wr *bad = nullptr;
    if (qp_->PostSend(requests.data(), &bad) != 0) {
        return GEN_POST_FAILED;
    }

    GEN_LOG_DEBUG("Post finished, qp(" << id_ << "), batch size(" << batchSize << "), op type("
                                       << static_cast<int32_t>(options->op_type) << "), signaled("
                                       << static_cast<uint32_t>(options->signal) << ")");
    return GEN_OK;
}
} // namespace verbs
} // namespace openDCC
} // namespace ock
