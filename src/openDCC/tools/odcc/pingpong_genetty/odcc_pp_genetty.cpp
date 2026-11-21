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
#include "odcc_pp_genetty.h"

#include <arpa/inet.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "under_api/acl/genetty_dl_acl_api.h"
#include "verbs/genetty_verbs_device_ctx_pool.h"
#include "verbs/genetty_verbs_info.h"

namespace ock {
namespace openDCC {
namespace {
constexpr char PP_VENDOR[] = "kunpeng";
constexpr char PP_DEVICE_VENDOR[] = "ascend";
constexpr uint32_t PP_MAX_INFO_LENGTH = 65536;
constexpr uint32_t PP_MAX_MEMORY_SIZE = 16 * 1024 * 1024;
constexpr uint32_t PP_PROTOCOL_VERSION = 1;
constexpr uint32_t PP_QUEUE_DEPTH = 8192;
constexpr uint32_t PP_RECEIVE_DEPTH = 128;
constexpr uint32_t PP_TRAFFIC_CLASS = 132;
constexpr uint32_t PP_WRITE_STAGE = 1;
constexpr uint32_t PP_READ_STAGE = 2;
constexpr uint32_t PP_READY_STAGE = 3;
constexpr uint32_t PP_OPERATION_COUNT = 2;
constexpr uint32_t PP_PATTERN_MODULUS = 251;
constexpr uint32_t PP_REPLY_SALT = 97;

uint8_t Pattern(uint32_t round, size_t index, bool reply) noexcept
{
    return static_cast<uint8_t>((round + index + (reply ? PP_REPLY_SALT : 0)) % PP_PATTERN_MODULUS);
}

int CheckPattern(const std::vector<uint8_t> &buffer, uint32_t round, bool reply) noexcept
{
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] != Pattern(round, i, reply)) {
            GEN_LOG_ERROR("Pingpong data mismatch, round(" << round << "), offset(" << i << "), actual("
                                                           << static_cast<uint32_t>(buffer[i]) << ")");
            return -1;
        }
    }
    return 0;
}
} // namespace

void SubCommandPp::SetRules() noexcept
{
    /* step1: set endpoint and control connection rules */
    paramRules_[PP_PARAM_SERVER] =
        Param(PP_PARAM_SERVER, PDT_STR_ENUM, false, PP_SERVER_FALSE, PP_SERVER_ENUMS, "run as a server or a client");
    paramRules_[PP_PARAM_TYPE] =
        Param(PP_PARAM_TYPE, PDT_STR_ENUM, false, PP_TYPE_ROCE, PP_TYPE_ENUMS, "transport type, roce or ub");
    paramRules_[PP_PARAM_ENDPOINT] = Param(PP_PARAM_ENDPOINT, PDT_STR_ENUM, false, PP_ENDPOINT_HOST,
                                          PP_ENDPOINT_ENUMS, "local endpoint, host or device");
    paramRules_[PP_PARAM_IP] = Param(PP_PARAM_IP, PDT_STR, false, PP_DEFAULT_IP, "", "TCP listen or server IP");
    paramRules_[PP_PARAM_PORT] = Param(PP_PARAM_PORT, PDT_INT64, false, PP_DEFAULT_PORT, 1, UINT16_MAX, "TCP port");
    paramRules_["rdma-ip"] = Param("rdma-ip", PDT_STR, false, "", "", "local RDMA IP (optional /mask is stripped)");
    paramRules_["device-id"] = Param("device-id", PDT_INT64, false, UINT16_MAX, 0, UINT16_MAX,
                                     "local logical device id, 65535 means automatic selection");
    /* step2: set data validation and timeout rules */
    paramRules_["size"] = Param("size", PDT_INT64, false, 1024, 1, PP_MAX_MEMORY_SIZE, "bytes per operation");
    paramRules_["iterations"] = Param("iterations", PDT_INT64, false, 10, 1, INT32_MAX, "pingpong rounds");
    paramRules_["timeout-ms"] = Param("timeout-ms", PDT_INT64, false, 60000, 1, INT32_MAX, "timeout in milliseconds");
    paramRules_["path-mtu"] = Param("path-mtu", PDT_INT64, false, 0, 0, 4096,
                                    "actual Device QP MTU, required only when HCCP cannot report it");
    examples_.push_back(std::string(PROGRAM_NAME) + " " + SUB_CMD_PP +
                        " --type=roce --endpoint=host --server=true --ip=0.0.0.0 --rdma-ip=192.168.1.2");
    examples_.push_back(std::string(PROGRAM_NAME) + " " + SUB_CMD_PP +
                        " --type=roce --endpoint=device --ip=192.168.10.2 --device-id=0");
}

int SubCommandPp::DoInitialize() noexcept
{
    isServer_ = paramRules_[PP_PARAM_SERVER].strRule.value == PP_SERVER_TRUE;
    type_ = paramRules_[PP_PARAM_TYPE].strRule.value;
    endpoint_ = paramRules_[PP_PARAM_ENDPOINT].strRule.value;
    ip_ = paramRules_[PP_PARAM_IP].strRule.value;
    port_ = static_cast<uint16_t>(paramRules_[PP_PARAM_PORT].int64Rule.value);
    rdmaIp_ = paramRules_["rdma-ip"].strRule.value;
    deviceId_ = static_cast<uint16_t>(paramRules_["device-id"].int64Rule.value);
    memorySize_ = static_cast<uint64_t>(paramRules_["size"].int64Rule.value);
    iterations_ = static_cast<uint32_t>(paramRules_["iterations"].int64Rule.value);
    timeoutMs_ = static_cast<uint32_t>(paramRules_["timeout-ms"].int64Rule.value);
    auto mtu = static_cast<uint32_t>(paramRules_["path-mtu"].int64Rule.value);
    const uint32_t sizes[] = {0, 256, 512, 1024, 2048, 4096};
    auto found = std::find(std::begin(sizes), std::end(sizes), mtu);
    GEN_ASSERT_MSG_RETURN(found != std::end(sizes), "Invalid path MTU(" << mtu << ")", -1);
    pathMtu_ = static_cast<uint32_t>(found - std::begin(sizes));
    GEN_ASSERT_MSG_RETURN(rdmaIp_.empty() || deviceId_ == UINT16_MAX, "Choose either rdma-ip or device-id", -1);
    GEN_ASSERT_MSG_RETURN(endpoint_ != PP_ENDPOINT_DEVICE || (type_ == PP_TYPE_ROCE && !isServer_),
                          "Device pingpong currently requires a RoCE client and a Host server", -1);
    return 0;
}

int SubCommandPp::DoExecute() noexcept
{
    /* step1: establish the control channel and validate both endpoint settings */
    int result = CreateSocket();
    if (result == 0) {
        result = ExchangeSettings();
    }
    /* step2: create and connect the queues, then register and exchange the memory */
    if (result == 0) {
        result = CreateGenetty();
    }
    if (result == 0) {
        result = ExchangeGenettyInfo();
    }
    if (result == 0) {
        result = CreateLocalMemory();
    }
    if (result == 0) {
        result = ExchangeMemoryInfo();
    }
    if (result == 0) {
        result = PingPong();
    }
    /* step3: stop the queues before deregistering or freeing their memory */
    if (genetty_ != nullptr) {
        genetty_destroy(genetty_, 0);
        genetty_ = nullptr;
    }
    if (cq_ != nullptr) {
        genetty_cq_destroy(cq_, 0);
        cq_ = nullptr;
    }
    if (DestroyLocalMemory() != 0) {
        result = -1;
    }
    DestroyGenetty();
    if (endpoint_ == PP_ENDPOINT_DEVICE && (verbs::DeviceRdmaContext::RetryPendingCleanup() != GEN_OK ||
                                            (localAddress_ == nullptr && AclApi::AclFinalize() != 0))) {
        result = -1;
    }
    DestroySocket();
    if (result == 0) {
        std::cout << "Pingpong passed, rounds(" << iterations_ << "), bytes(" << memorySize_ << ")" << std::endl;
    }
    return result;
}

int SubCommandPp::CreateSocket() noexcept
{
    if (isServer_) {
        server_ = new (std::nothrow) SocketServer();
        GEN_ASSERT_MSG_RETURN(server_ != nullptr, "Allocate socket server failed", -1);
        std::cout << "Listening on " << ip_ << ":" << port_ << std::endl;
        GEN_ASSERT_MSG_RETURN(server_->ListenAndAccept(ip_, port_, timeoutMs_) == 0,
                              "Listen/accept failed, ip(" << ip_ << "), errno(" << errno << ")", -1);
    } else {
        client_ = new (std::nothrow) SocketClient();
        GEN_ASSERT_MSG_RETURN(client_ != nullptr, "Allocate socket client failed", -1);
        GEN_ASSERT_MSG_RETURN(client_->Connect(ip_, port_, timeoutMs_) == 0,
                              "Connect failed, ip(" << ip_ << "), errno(" << errno << ")", -1);
    }
    return 0;
}

int SubCommandPp::TransferBytes(void *data, size_t size, bool sending) noexcept
{
    size_t transferred = 0;
    if (isServer_) {
        transferred = sending ? server_->Send(data, size) : server_->Receive(data, size);
    } else {
        transferred = sending ? client_->Send(data, size) : client_->Receive(data, size);
    }
    GEN_ASSERT_MSG_RETURN(transferred == size,
                          "TCP transfer failed, sending(" << sending << "), bytes(" << transferred << "/" << size
                                                          << "), errno(" << errno << ")",
                          -1);
    return 0;
}

int SubCommandPp::SendFrame(char *data, uint32_t size) noexcept
{
    GEN_ASSERT_MSG_RETURN(size != 0 && size <= PP_MAX_INFO_LENGTH && data != nullptr,
                          "Invalid outgoing frame length(" << size << ")", -1);
    uint32_t length = htonl(size);
    return TransferBytes(&length, sizeof(length), true) == 0 ? TransferBytes(data, size, true) : -1;
}

int SubCommandPp::ReceiveFrame(std::vector<char> &data) noexcept
{
    uint32_t length = 0;
    if (TransferBytes(&length, sizeof(length), false) != 0) {
        return -1;
    }
    length = ntohl(length);
    GEN_ASSERT_MSG_RETURN(length != 0 && length <= PP_MAX_INFO_LENGTH,
                          "Invalid incoming frame length(" << length << ")", -1);
    data.resize(length);
    return TransferBytes(data.data(), data.size(), false);
}

int SubCommandPp::ExchangeFrame(char *data, uint32_t size, std::vector<char> &peer) noexcept
{
    /* Receive first on the server to avoid both endpoints blocking in send. */
    if (isServer_) {
        return ReceiveFrame(peer) == 0 ? SendFrame(data, size) : -1;
    }
    return SendFrame(data, size) == 0 ? ReceiveFrame(peer) : -1;
}

int SubCommandPp::ExchangeSettings() noexcept
{
    uint32_t settings[] = {htonl(PP_PROTOCOL_VERSION), htonl(endpoint_ == PP_ENDPOINT_DEVICE),
                           htonl(type_ == PP_TYPE_ROCE), htonl(static_cast<uint32_t>(memorySize_)), htonl(iterations_)};
    std::vector<char> data;
    if (ExchangeFrame(reinterpret_cast<char *>(settings), sizeof(settings), data) != 0) {
        return -1;
    }
    GEN_ASSERT_MSG_RETURN(data.size() == sizeof(settings), "Invalid peer settings length", -1);
    uint32_t peer[sizeof(settings) / sizeof(settings[0])]{};
    memcpy(peer, data.data(), sizeof(peer));
    peerEndpoint_ = ntohl(peer[1]);
    GEN_ASSERT_MSG_RETURN(peer[0] == settings[0] && peer[2] == settings[2] && peer[3] == settings[3] &&
                              peer[4] == settings[4] && peerEndpoint_ <= 1,
                          "Peer protocol, transport, size or iterations do not match", -1);
    GEN_ASSERT_MSG_RETURN(peerEndpoint_ == 0 || (isServer_ && type_ == PP_TYPE_ROCE),
                          "Only a Device client and Host server are supported for heterogeneous pingpong", -1);
    return 0;
}

int SubCommandPp::CreateGenetty() noexcept
{
    /* step1: initialize the backend selected by transport and endpoint */
    bool device = endpoint_ == PP_ENDPOINT_DEVICE;
    PROTOCOL protocol = type_ == PP_TYPE_UB ? GEN_PROT_HOST_URMA : GEN_PROT_HOST_RDMA;
    auto type = type_ == PP_TYPE_UB ? HOST_URMA_GENETTY : HOST_RDMA_GENETTY;
    if (device) {
        protocol = GEN_PROT_DEVICE_RDMA;
        type = DEVICE_RDMA_GENETTY;
    }
    auto result = genetty_init(protocol, device ? PP_DEVICE_VENDOR : PP_VENDOR, 0);
    GEN_ASSERT_MSG_RETURN(result == GEN_OK, "Initialize genetty failed, result(" << result << ")", -1);
    initialized_ = true;
    /* step2: select a local RDMA device independently of the TCP server address */
    if (!rdmaIp_.empty()) {
        context_ = genetty_context_get_by_ip(type, const_cast<char *>(rdmaIp_.c_str()), 0);
    } else {
        context_ = genetty_context_get_by_id(type, deviceId_, 0);
    }
    GEN_ASSERT_MSG_RETURN(context_ != nullptr, "Select local RDMA context failed", -1);
    /* step3: create a CQ and its QP */
    genetty_cq_options_t cqOptions{};
    genetty_cq_options_init(&cqOptions);
    cqOptions.context = context_;
    cqOptions.queue_depth = PP_QUEUE_DEPTH;
    cq_ = genetty_cq_create(&cqOptions);
    GEN_ASSERT_MSG_RETURN(cq_ != nullptr, "Create pingpong CQ failed", -1);
    genetty_options_t options{};
    genetty_options_init(&options);
    options.cq_handle = cq_;
    options.sq_depth = PP_QUEUE_DEPTH;
    options.rq_depth = PP_RECEIVE_DEPTH;
    options.traffic_class = PP_TRAFFIC_CLASS;
    genetty_ = genetty_create(&options);
    GEN_ASSERT_MSG_RETURN(genetty_ != nullptr, "Create pingpong QP failed", -1);
    return 0;
}

int SubCommandPp::ExportConnectedInfo() noexcept
{
    genetty_link_info_t info{};
    if (genetty_export(genetty_, 0, &info) != GEN_OK) {
        return -1;
    }
    verbs::VerbsLinkInfo link;
    auto result = link.Import(&info, 0);
    genetty_link_info_free(&info);
    if (result != GEN_OK) {
        return -1;
    }
    GEN_ASSERT_MSG_RETURN(link.Mtu() != 0 || pathMtu_ != 0,
                          "HCCP did not report path MTU; supply the actual Device QP MTU with --path-mtu", -1);
    GEN_ASSERT_MSG_RETURN(pathMtu_ == 0 || link.Mtu() == 0 || pathMtu_ == link.Mtu(),
                          "Explicit MTU conflicts with Device QP MTU(" << link.Mtu() << ")", -1);
    if (link.Mtu() == 0) {
        link.Mtu(pathMtu_);
    }
    if (link.Export(&info, 0) != GEN_OK) {
        return -1;
    }
    result = SendFrame(info.data, info.length);
    genetty_link_info_free(&info);
    return result;
}

int SubCommandPp::ExchangeGenettyInfo() noexcept
{
    /* step1: exchange the initial QPN, GID, PSN and Host active MTU */
    genetty_link_info_t local{};
    if (genetty_export(genetty_, 0, &local) != GEN_OK) {
        return -1;
    }
    std::vector<char> data;
    auto result = ExchangeFrame(local.data, local.length, data);
    genetty_link_info_free(&local);
    if (result != 0) {
        return -1;
    }
    /* step2: HCCP determines the Device path MTU during QP modification */
    if (peerEndpoint_ != 0 && ReceiveFrame(data) != 0) {
        return -1;
    }
    genetty_link_info_t peer{data.data(), static_cast<uint32_t>(data.size())};
    if (type_ == PP_TYPE_ROCE) {
        verbs::VerbsLinkInfo link;
        if (link.Import(&peer, 0) != GEN_OK) {
            return -1;
        }
        auto expected = peerEndpoint_ == 0 ? HOST_RDMA_GENETTY : DEVICE_RDMA_GENETTY;
        GEN_ASSERT_MSG_RETURN(link.Type() == expected, "Peer link type does not match its endpoint", -1);
    }
    if (genetty_import(genetty_, &peer, 0) != GEN_OK) {
        return -1;
    }
    if (endpoint_ == PP_ENDPOINT_DEVICE && ExportConnectedInfo() != 0) {
        return -1;
    }
    /* step3: wait for both endpoints to finish connecting before posting data */
    return Barrier(PP_READY_STAGE, 0);
}

int SubCommandPp::SelectDevice() noexcept
{
    if (endpoint_ != PP_ENDPOINT_DEVICE) {
        return 0;
    }
    auto context = static_cast<verbs::DeviceVerbsContext *>(context_);
    return AclApi::AclrtSetDevice(static_cast<int32_t>(context->wrapCtx_->UserDeviceId())) == 0 ? 0 : -1;
}

int SubCommandPp::CreateLocalMemory() noexcept
{
    /* step1: allocate separate registered write and read ranges */
    auto size = memorySize_ * PP_OPERATION_COUNT;
    if (endpoint_ == PP_ENDPOINT_DEVICE) {
        if (SelectDevice() != 0 || AclApi::AclrtMalloc(&localAddress_, size, ACL_MEM_MALLOC_HUGE_FIRST) != 0) {
            return -1;
        }
    } else {
        localAddress_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (localAddress_ == MAP_FAILED) {
            localAddress_ = nullptr;
            GEN_LOG_ERROR("Allocate pingpong DRAM failed, size(" << size << "), errno(" << errno << ")");
            return -1;
        }
    }
    /* step2: initialize both ranges before registering memory */
    std::vector<uint8_t> zero(memorySize_, 0);
    if (CopyLocal(zero.data(), 0, true) != 0 || CopyLocal(zero.data(), memorySize_, true) != 0) {
        return -1;
    }
    local_.type = endpoint_ == PP_ENDPOINT_DEVICE ? MT_DEVICE_HBM : MT_HOST_DRAM;
    local_.address = reinterpret_cast<uint64_t>(localAddress_);
    local_.size = size;
    if (genetty_memory_register(context_, &local_, 0) != GEN_OK) {
        return -1;
    }
    registered_ = true;
    return 0;
}

int SubCommandPp::ExchangeMemoryInfo() noexcept
{
    genetty_memory_info_t local{};
    if (genetty_memory_info_encode(&local_, 0, &local) != GEN_OK) {
        return -1;
    }
    std::vector<char> data;
    auto result = ExchangeFrame(local.data, local.length, data);
    genetty_memory_free_info(&local);
    if (result != 0) {
        return -1;
    }
    genetty_memory_info_t peer{data.data(), static_cast<uint32_t>(data.size())};
    if (genetty_memory_info_decode(&peer, &remote_, 0) != GEN_OK) {
        return -1;
    }
    auto expected = peerEndpoint_ == 0 ? MT_HOST_DRAM : MT_DEVICE_HBM;
    GEN_ASSERT_MSG_RETURN(remote_.type == expected && remote_.address != 0 && remote_.size == local_.size &&
                              remote_.address <= UINT64_MAX - remote_.size,
                          "Invalid peer memory range", -1);
    return genetty_memory_import(context_, &remote_, 0) == GEN_OK ? 0 : -1;
}

int SubCommandPp::CopyLocal(void *host, uint64_t offset, bool toLocal) noexcept
{
    void *local = static_cast<char *>(localAddress_) + offset;
    if (endpoint_ == PP_ENDPOINT_DEVICE) {
        if (SelectDevice() != 0) {
            return -1;
        }
        int kind = toLocal ? ACL_MEMCPY_HOST_TO_DEVICE : ACL_MEMCPY_DEVICE_TO_HOST;
        return AclApi::AclrtMemcpy(toLocal ? local : host, memorySize_, toLocal ? host : local, memorySize_, kind) == 0
                   ? 0
                   : -1;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    memcpy(toLocal ? local : host, toLocal ? host : local, memorySize_);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return 0;
}

int SubCommandPp::Barrier(uint32_t stage, uint32_t round) noexcept
{
    uint32_t token[] = {htonl(stage), htonl(round)};
    std::vector<char> peer;
    if (ExchangeFrame(reinterpret_cast<char *>(token), sizeof(token), peer) != 0) {
        return -1;
    }
    GEN_ASSERT_MSG_RETURN(peer.size() == sizeof(token) && memcmp(peer.data(), token, sizeof(token)) == 0,
                          "Unexpected control token, stage(" << stage << "), round(" << round << ")", -1);
    return 0;
}

int SubCommandPp::PostAndWait(genetty_op_type_t operation, uint64_t requestId) noexcept
{
    genetty_batch_op_data_t data{};
    data.laddress = static_cast<char *>(localAddress_) + (operation == OP_READ ? memorySize_ : 0);
    data.lsize = static_cast<uint32_t>(memorySize_);
    data.lkey = local_.lkey;
    data.raddress = reinterpret_cast<void *>(remote_.address);
    data.rsize = static_cast<uint32_t>(memorySize_);
    data.rkey = remote_.rkey;
    genetty_batch_op_options_t options{};
    options.op_type = operation;
    options.signal = 1;
    options.kick_db = 1;
    options.usr_ctx = requestId;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
    if (genetty_batch_post(genetty_, &data, 1, &options) != GEN_OK) {
        return -1;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        genetty_cq_wc_t wc{};
        auto count = genetty_cq_poll(cq_, 1, &wc, 0);
        if (count < 0) {
            return -1;
        }
        if (count != 0) {
            GEN_ASSERT_MSG_RETURN(count == 1 && wc.status == OPS_SUCCESS && wc.type == operation &&
                                      wc.usr_ctx == requestId,
                                  "Unexpected CQ completion, request(" << requestId << "), status(" << wc.status
                                                                       << "), type(" << wc.type << ")",
                                  -1);
            return 0;
        }
        std::this_thread::yield();
    }
    GEN_LOG_ERROR("CQ timeout, request(" << requestId << "), operation(" << operation << "), timeout ms(" << timeoutMs_
                                         << ")");
    return -1;
}

int SubCommandPp::ClientRound(uint32_t round, std::vector<uint8_t> &buffer) noexcept
{
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = Pattern(round, i, false);
    }
    auto request = static_cast<uint64_t>(round) * PP_OPERATION_COUNT;
    if (CopyLocal(buffer.data(), 0, true) != 0 || PostAndWait(OP_WRITE, request) != 0 ||
        Barrier(PP_WRITE_STAGE, round) != 0) {
        return -1;
    }
    /* The server has verified WRITE data and prepared different READ data. */
    if (PostAndWait(OP_READ, request + 1) != 0 || CopyLocal(buffer.data(), memorySize_, false) != 0 ||
        CheckPattern(buffer, round, true) != 0) {
        return -1;
    }
    return Barrier(PP_READ_STAGE, round);
}

int SubCommandPp::ServerRound(uint32_t round, std::vector<uint8_t> &buffer) noexcept
{
    /* step1: wait until the client has completed its RDMA WRITE */
    std::vector<char> data;
    uint32_t token[] = {htonl(PP_WRITE_STAGE), htonl(round)};
    if (ReceiveFrame(data) != 0) {
        return -1;
    }
    GEN_ASSERT_MSG_RETURN(data.size() == sizeof(token) && memcmp(data.data(), token, sizeof(token)) == 0,
                          "Unexpected WRITE token, round(" << round << ")", -1);
    if (CopyLocal(buffer.data(), 0, false) != 0 || CheckPattern(buffer, round, false) != 0) {
        return -1;
    }
    /* step2: prepare a distinct pattern for the client's RDMA READ */
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = Pattern(round, i, true);
    }
    if (CopyLocal(buffer.data(), 0, true) != 0 || SendFrame(reinterpret_cast<char *>(token), sizeof(token)) != 0) {
        return -1;
    }
    return Barrier(PP_READ_STAGE, round);
}

int SubCommandPp::PingPong() noexcept
{
    if (Barrier(PP_READY_STAGE, 0) != 0) {
        return -1;
    }
    std::vector<uint8_t> buffer(memorySize_);
    for (uint32_t round = 0; round < iterations_; ++round) {
        auto result = isServer_ ? ServerRound(round, buffer) : ClientRound(round, buffer);
        if (result != 0) {
            return -1;
        }
    }
    return 0;
}

int SubCommandPp::DestroyLocalMemory() noexcept
{
    if (registered_) {
        if (genetty_memory_unregister(context_, &local_, 0) != GEN_OK) {
            GEN_LOG_ERROR("Memory deregistration failed; keeping the backing allocation alive");
            return -1;
        }
        registered_ = false;
    }
    if (localAddress_ == nullptr) {
        return 0;
    }
    if (endpoint_ == PP_ENDPOINT_DEVICE) {
        if (SelectDevice() != 0 || AclApi::AclrtFree(localAddress_) != 0) {
            return -1;
        }
    } else if (munmap(localAddress_, memorySize_ * PP_OPERATION_COUNT) != 0) {
        GEN_LOG_ERROR("Free pingpong DRAM failed, errno(" << errno << ")");
        return -1;
    }
    localAddress_ = nullptr;
    return 0;
}

void SubCommandPp::DestroyGenetty() noexcept
{
    if (context_ != nullptr) {
        genetty_context_return(context_, 0);
        context_ = nullptr;
    }
    if (initialized_) {
        genetty_uninit(0);
        initialized_ = false;
    }
}

void SubCommandPp::DestroySocket() noexcept
{
    if (client_ != nullptr) {
        client_->Close();
        delete client_;
        client_ = nullptr;
    }
    if (server_ != nullptr) {
        server_->Close();
        delete server_;
        server_ = nullptr;
    }
}
} // namespace openDCC
} // namespace ock
