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
#ifndef MF_OPENDCC_ODCC_PP_H
#define MF_OPENDCC_ODCC_PP_H

#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "genetty.h"
#include "odcc_cmd.h"
#include "odcc_cmd_list.h"
#include "odcc_cmd_socket.h"

namespace ock {
namespace openDCC {
constexpr char PP_PARAM_SERVER[] = "server";
constexpr char PP_PARAM_IP[] = "ip";
constexpr char PP_PARAM_PORT[] = "port";
constexpr char PP_PARAM_TYPE[] = "type";
constexpr char PP_PARAM_ENDPOINT[] = "endpoint";

constexpr char PP_SERVER_TRUE[] = "true";
constexpr char PP_SERVER_FALSE[] = "false";
constexpr char PP_SERVER_ENUMS[] = "true|false";

constexpr char PP_TYPE_ROCE[] = "roce";
constexpr char PP_TYPE_UB[] = "ub";
constexpr char PP_TYPE_ENUMS[] = "roce|ub";

constexpr char PP_ENDPOINT_HOST[] = "host";
constexpr char PP_ENDPOINT_DEVICE[] = "device";
constexpr char PP_ENDPOINT_ENUMS[] = "host|device";

constexpr char PP_DEFAULT_IP[] = "127.0.0.1";
constexpr uint16_t PP_DEFAULT_PORT = 9010;

class SubCommandPp : public SubCommand {
public:
    SubCommandPp(const std::string &name, const ParamMap &params) noexcept : SubCommand(name, params) {}

protected:
    void SetRules() noexcept override;

    int DoInitialize() noexcept override;

    int DoExecute() noexcept override;

    int CreateSocket() noexcept;

    int CreateGenetty() noexcept;

    int ExchangeGenettyInfo() noexcept;

    int CreateLocalMemory() noexcept;

    int ExchangeMemoryInfo() noexcept;

    int PingPong() noexcept;

    void DestroyGenetty() noexcept;

    void DestroySocket() noexcept;

    int DestroyLocalMemory() noexcept;
    int SelectDevice() noexcept;
    int ExchangeSettings() noexcept;
    int SendFrame(char *data, uint32_t size) noexcept;
    int ReceiveFrame(std::vector<char> &data) noexcept;
    int ExchangeFrame(char *data, uint32_t size, std::vector<char> &peer) noexcept;
    int ExportConnectedInfo() noexcept;
    int TransferBytes(void *data, size_t size, bool sending) noexcept;
    int Barrier(uint32_t stage, uint32_t round) noexcept;
    int CopyLocal(void *host, uint64_t offset, bool toLocal) noexcept;
    int PostAndWait(genetty_op_type_t operation, uint64_t requestId) noexcept;
    int ClientRound(uint32_t round, std::vector<uint8_t> &buffer) noexcept;
    int ServerRound(uint32_t round, std::vector<uint8_t> &buffer) noexcept;

private:
    bool isServer_ = false;                /* run as a server or a client */
    std::string type_ = PP_TYPE_ROCE; /* transport type, roce or ub */
    std::string endpoint_ = PP_ENDPOINT_HOST; /* local endpoint, host or device */
    std::string ip_ = PP_DEFAULT_IP;  /* ip to listen or connect */
    uint16_t port_ = PP_DEFAULT_PORT; /* port to listen or connect */
    SocketClient *client_ = nullptr;       /* socket client */
    SocketServer *server_ = nullptr;       /* socket server */
    genetty_context_t context_ = nullptr;  /* gentty context handle */
    genetty_t genetty_ = nullptr;          /* genetty handle */
    genetty_cq_t cq_ = nullptr;            /* cq handle */
    void *localAddress_ = nullptr;         /* virtual address of local memory */
    uint64_t memorySize_ = 1024;           /* size of remote memory */
    std::string rdmaIp_;                   /* local RDMA IP, independent of the TCP address */
    uint16_t deviceId_ = UINT16_MAX;       /* local logical device id */
    uint32_t iterations_ = 10;             /* pingpong rounds */
    uint32_t timeoutMs_ = 60000;           /* control and completion timeout */
    uint32_t pathMtu_ = 0;                 /* explicit MTU for HCCP without pathMtu reporting */
    uint32_t peerEndpoint_ = 0;            /* zero for host, one for device */
    bool initialized_ = false;             /* owns a genetty initialization */
    bool registered_ = false;              /* local memory is registered */
    genetty_memory_segment_t local_{};     /* registered local write/read buffers */
    genetty_memory_segment_t remote_{};    /* imported peer memory */
};

inline SubCommand *CreatePp(const ParamMap &params)
{
    return new (std::nothrow) SubCommandPp(SUB_CMD_PP, params);
}
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_ODCC_PP_H
