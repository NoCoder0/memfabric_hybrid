/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifdef BUILD_OPENDCC
#include "opendcc_transport_manager.h"
#include <charconv>
#include <chrono>
#include "hybm_logger.h"
#include "mf_ipv4_validator.h"
#include "mf_tls_util.h"
#include "mf_env_define.h"
#include "mf_env_util.h"

namespace ock {
namespace mf {
namespace transport {
namespace {
constexpr int16_t CONTROL_MAGIC = 0x4f44;
constexpr int16_t CONTROL_VERSION = 1;
constexpr int16_t CONTROL_EXPORT = 1;
constexpr int16_t CONTROL_IMPORT = 2;
constexpr int16_t CONTROL_REPLY = 3;
constexpr int16_t CONTROL_READY = 4;
constexpr uint32_t CONTROL_TIMEOUT_MS = 30000;
constexpr uint32_t CONTROL_MAX_BYTES = 32768;
constexpr uint32_t RDMA_TRAFFIC_CLASS = 132;

Result ParseEndpoint(const std::string &nic, std::string &ip, uint16_t &port)
{
    std::string protocol, mask, text;
    BM_ASSERT_LOG_AND_RETURN(NetValidator::ParseNicUrl(nic, protocol, ip, mask, text),
                             "Invalid openDCC control URL: " << nic, BM_INVALID_PARAM);
    uint32_t value = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    BM_ASSERT_LOG_AND_RETURN(parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() && value > 0 &&
                                 value <= UINT16_MAX,
                             "Invalid openDCC control port: " << text, BM_INVALID_PARAM);
    port = static_cast<uint16_t>(value);
    return BM_OK;
}
} // namespace

Result OpenDccTransportManager::StartControl()
{
    acc::AccTcpServerOptions options;
    auto ret = ParseEndpoint(options_.nic, options.listenIp, options.listenPort);
    if (ret != BM_OK) {
        return ret;
    }
    BM_ASSERT_LOG_AND_RETURN(options_.rankId < options_.rankCount &&
                                 options_.rankCount - 1 <= UINT16_MAX - static_cast<uint32_t>(options.listenPort),
                             "openDCC base port leaves no room for rank ports", BM_INVALID_PARAM);
    options.listenPort += options_.rankId;
    options.magic = CONTROL_MAGIC;
    options.version = CONTROL_VERSION;
    options.enableListener = true;
    options.reusePort = false;
    options.maxWorldSize = options_.rankCount;
    server_ = acc::AccTcpServer::Create();
    BM_ASSERT_LOG_AND_RETURN(server_ != nullptr, "Create openDCC control server failed", BM_MALLOC_FAILED);
    server_->RegisterNewLinkHandler([this](const acc::AccConnReq &req, const acc::AccTcpLinkComplexPtr &link) {
        BM_ASSERT_LOG_AND_RETURN(req.rankId < options_.rankCount && req.rankId < options_.rankId,
                                 "Invalid openDCC peer rank", BM_INVALID_PARAM);
        link->UpCtx(req.rankId);
        auto ret = link->EnableNonBlocking();
        BM_ASSERT_LOG_AND_RETURN(ret == 0, "Set openDCC control link nonblocking failed", BM_ERROR);
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &entry = peers_[req.rankId];
            if (entry == nullptr) {
                entry = std::make_shared<Peer>();
            }
            peer = entry;
        }
        std::lock_guard<std::mutex> lock(peer->mutex);
        peer->link = link;
        return BM_OK;
    });
    for (auto type : {CONTROL_EXPORT, CONTROL_IMPORT, CONTROL_REPLY, CONTROL_READY}) {
        server_->RegisterNewRequestHandler(
            type, [this](const acc::AccTcpRequestContext &request) { return HandleControl(request); });
    }
    server_->RegisterLinkBrokenHandler([this](const acc::AccTcpLinkComplexPtr &link) {
        auto peer = FindPeer(link->UpCtx());
        if (peer != nullptr) {
            std::lock_guard<std::mutex> lock(peer->mutex);
            peer->ready = false;
            peer->result = BM_NOT_CONNECTED;
            peer->changed.notify_all();
        }
        return BM_OK;
    });
    const auto &config = options_.tlsOption;
    acc::AccTlsOption tls;
    tls.enableTls = config.tlsEnable;
    tls.tlsTopPath = tls.tlsCaPath = tls.tlsCrlPath = "/";
    tls.tlsCert = config.certPath;
    tls.tlsPk = config.keyPath;
    tls.tlsPkPwd = config.keyPassPath;
    if (config.caPath[0] != 0) {
        tls.tlsCaFile.insert(config.caPath);
    }
    if (config.crlPath[0] != 0) {
        tls.tlsCrlFile.insert(config.crlPath);
    }
    if (tls.enableTls) {
        ret = server_->LoadDynamicLib(config.packagePath);
        BM_ASSERT_LOG_AND_RETURN(ret == 0, "Load openDCC control TLS library failed", BM_DL_FUNCTION_FAILED);
        auto decrypt = config.decrypterLibPath[0] == 0 ? MfTlsUtil::DefaultDecrypter
                                                       : MfTlsUtil::LoadDecryptFunction(config.decrypterLibPath);
        BM_ASSERT_LOG_AND_RETURN(decrypt != nullptr, "Load TLS decrypter failed", BM_DL_FUNCTION_FAILED);
        server_->RegisterDecryptHandler(decrypt);
    }
    ret = server_->Start(options, tls);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "Start openDCC control server failed, ret=" << ret, BM_ERROR);
    options_.nic = "tcp://" + options.listenIp + ":" + std::to_string(options.listenPort);
    return BM_OK;
}

Result OpenDccTransportManager::CreateTunnel(Peer &peer)
{
    if (peer.tunnel != nullptr) {
        return BM_OK;
    }
    hcc_tunnel_options_t options{128, MfEnvUtil::GetOptionalUintOrDefault(env::MF_DEVICE_RDMA_TC, RDMA_TRAFFIC_CLASS)};
    peer.tunnel = DlOpenDccApi::hcc_tunnel_create(communicator_, &options);
    BM_ASSERT_LOG_AND_RETURN(peer.tunnel != nullptr, "Create openDCC peer tunnel failed", BM_DL_FUNCTION_FAILED);
    return BM_OK;
}

int OpenDccTransportManager::HandleControl(const acc::AccTcpRequestContext &request)
{
    auto peer = FindPeer(request.Link()->UpCtx());
    BM_ASSERT_LOG_AND_RETURN(peer != nullptr && request.DataLen() <= CONTROL_MAX_BYTES,
                             "Invalid openDCC control request", BM_INVALID_PARAM);
    std::lock_guard<std::mutex> lock(peer->mutex);
    if (request.MsgType() == CONTROL_REPLY) {
        if (request.SeqNo() == peer->sequence) {
            peer->response.clear();
            if (request.DataLen() != 0) {
                auto data = static_cast<const char *>(request.DataPtr());
                peer->response.assign(data, data + request.DataLen());
            }
            peer->received = request.SeqNo();
            peer->result = request.Header().result;
            peer->changed.notify_all();
        }
        return BM_OK;
    }
    Result ret = peer->removed ? BM_NOT_CONNECTED : CreateTunnel(*peer);
    hcc_link_info_t exported{};
    if (ret == BM_OK && request.MsgType() == CONTROL_IMPORT) {
        hcc_link_info_t info{static_cast<char *>(request.DataPtr()), request.DataLen()};
        ret = ConvertError(DlOpenDccApi::hcc_tunnel_import(&peer->tunnel, 0, &info));
    }
    if (ret == BM_OK && request.MsgType() != CONTROL_READY) {
        ret = ConvertError(DlOpenDccApi::hcc_tunnel_export(&peer->tunnel, 0, &exported));
    }
    /* acc_links requires a nonempty body, including control-only acknowledgements. */
    const uint8_t control = CONTROL_VERSION;
    auto response = exported.length == 0 ? acc::AccDataBuffer::Create(&control, sizeof(control))
                                         : acc::AccDataBuffer::Create(exported.data, exported.length);
    auto sent = request.Link()->NonBlockSend(CONTROL_REPLY, ret, request.SeqNo(), response, nullptr);
    if (exported.data != nullptr) {
        DlOpenDccApi::hcc_tunnel_free_link_info(&exported);
    }
    if (request.MsgType() == CONTROL_READY) {
        peer->ready = ret == BM_OK && sent == 0;
        peer->result = ret != BM_OK ? ret : (sent == 0 ? BM_OK : BM_NOT_CONNECTED);
        peer->changed.notify_all();
    }
    return sent;
}

Result OpenDccTransportManager::Exchange(Peer &peer, int16_t phase, const hcc_link_info_t &info,
                                         std::vector<char> &reply)
{
    std::unique_lock<std::mutex> lock(peer.mutex);
    auto sequence = ++peer.sequence;
    peer.result = BM_OK;
    const uint8_t control = CONTROL_VERSION;
    auto buffer = info.length == 0 ? acc::AccDataBuffer::Create(&control, sizeof(control))
                                   : acc::AccDataBuffer::Create(info.data, info.length);
    auto ret = peer.link->NonBlockSend(phase, 0, sequence, buffer, nullptr);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "Send openDCC control request failed, ret=" << ret, BM_NOT_CONNECTED);
    bool done = peer.changed.wait_for(lock, std::chrono::milliseconds(CONTROL_TIMEOUT_MS), [&]() {
        return peer.received == sequence || peer.removed || peer.result != BM_OK;
    });
    BM_ASSERT_LOG_AND_RETURN(done, "openDCC control request timed out", BM_TIMEOUT);
    BM_ASSERT_LOG_AND_RETURN(!peer.removed && peer.result == BM_OK, "openDCC peer handshake failed", BM_NOT_CONNECTED);
    reply = peer.response;
    return BM_OK;
}

Result OpenDccTransportManager::ConnectRank(uint32_t rank)
{
    std::shared_lock<std::shared_mutex> gate(ioGate_);
    auto peer = FindPeer(rank);
    BM_ASSERT_LOG_AND_RETURN(peer != nullptr, "openDCC rank not prepared: " << rank, BM_INVALID_PARAM);
    std::lock_guard<std::mutex> operation(peer->operation);
    {
        std::unique_lock<std::mutex> lock(peer->mutex);
        if (peer->ready) {
            return BM_OK;
        }
        if (options_.rankId > rank) {
            bool done = peer->changed.wait_for(lock, std::chrono::milliseconds(CONTROL_TIMEOUT_MS),
                                               [&]() { return peer->ready || peer->removed || peer->result != BM_OK; });
            BM_ASSERT_LOG_AND_RETURN(done && peer->ready, "Waiting for openDCC peer failed, rank=" << rank,
                                     BM_NOT_CONNECTED);
            return BM_OK;
        }
        auto ret = CreateTunnel(*peer);
        if (ret != BM_OK) {
            return ret;
        }
    }
    std::string ip;
    uint16_t port = 0;
    auto ret = ParseEndpoint(peer->nic, ip, port);
    if (ret != BM_OK) {
        return ret;
    }
    acc::AccConnReq req;
    req.magic = CONTROL_MAGIC;
    req.version = CONTROL_VERSION;
    req.rankId = options_.rankId;
    ret = server_->ConnectToPeerServer(ip, port, req, peer->link);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "Connect openDCC peer failed, rank=" << rank, BM_NOT_CONNECTED);
    peer->link->UpCtx(rank);
    ret = peer->link->EnableNonBlocking();
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "Set openDCC control link nonblocking failed", BM_ERROR);
    std::vector<char> reply;
    ret = Exchange(*peer, CONTROL_EXPORT, {}, reply);
    if (ret != BM_OK) {
        return ret;
    }
    bool device = context_.provider == DEVICE_RDMA_GENETTY;
    hcc_link_info_t remote{reply.data(), static_cast<uint32_t>(reply.size())};
    if (device) {
        ret = ConvertError(DlOpenDccApi::hcc_tunnel_import(&peer->tunnel, 0, &remote));
        if (ret != BM_OK) {
            return ret;
        }
    }
    hcc_link_info_t local{};
    ret = ConvertError(DlOpenDccApi::hcc_tunnel_export(&peer->tunnel, 0, &local));
    if (ret != BM_OK) {
        return ret;
    }
    ret = Exchange(*peer, CONTROL_IMPORT, local, reply);
    DlOpenDccApi::hcc_tunnel_free_link_info(&local);
    if (ret != BM_OK) {
        return ret;
    }
    if (!device) {
        remote = {reply.data(), static_cast<uint32_t>(reply.size())};
        ret = ConvertError(DlOpenDccApi::hcc_tunnel_import(&peer->tunnel, 0, &remote));
    }
    if (ret == BM_OK) {
        ret = Exchange(*peer, CONTROL_READY, {}, reply);
    }
    std::lock_guard<std::mutex> lock(peer->mutex);
    peer->ready = ret == BM_OK;
    return ret;
}
} // namespace transport
} // namespace mf
} // namespace ock
#endif
