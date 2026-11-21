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
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "hcc.h"
#include "hcc_p2p.h"
#include "odcc_cmd_socket.h"
#include "under_api/acl/genetty_dl_acl_api.h"

using ock::openDCC::AclApi;
using ock::openDCC::SocketClient;
using ock::openDCC::SocketServer;
namespace {
constexpr uint32_t TIMEOUT_MS = 30000;
constexpr size_t HALF = 128 * 1024;
constexpr size_t OFFSET = 64;
constexpr size_t BYTES = 64 * 1024;
constexpr uint8_t GUARD = 0xcc;
void Check(bool ok, const char *stage)
{
    if (!ok) {
        std::fprintf(stderr, "HCC test failed: %s\n", stage);
        std::abort();
    }
}
uint32_t Number(const char *text)
{
    uint32_t value = 0;
    auto end = text + std::strlen(text);
    auto result = std::from_chars(text, end, value);
    Check(result.ec == std::errc() && result.ptr == end, "numeric argument");
    return value;
}
struct Connection {
    explicit Connection(bool isHost) : host(isHost) {}
    bool host;
    SocketClient client;
    SocketServer server;
    void Send(void *data, size_t length)
    {
        Check((host ? server.Send(data, length) : client.Send(data, length)) == length, "control send");
    }
    void Receive(void *data, size_t length)
    {
        Check((host ? server.Receive(data, length) : client.Receive(data, length)) == length, "control receive");
    }
    void Frame(char *data, uint32_t length)
    {
        auto size = htonl(length);
        Send(&size, sizeof(size));
        Send(data, length);
    }
    std::vector<char> Frame()
    {
        uint32_t length;
        Receive(&length, sizeof(length));
        length = ntohl(length);
        Check(length != 0 && length <= 32768, "control frame length");
        std::vector<char> data(length);
        Receive(data.data(), data.size());
        return data;
    }
    ~Connection()
    {
        client.Close();
        server.Close();
    }
};
void ExportLink(Connection &socket, hcc_tunnel_t tunnel)
{
    hcc_link_info_t info{};
    Check(hcc_tunnel_export(&tunnel, 0, &info) == HCC_OK, "export link");
    socket.Frame(info.data, info.length);
    hcc_tunnel_free_link_info(&info);
}
void ImportLink(Connection &socket, hcc_tunnel_t tunnel)
{
    auto data = socket.Frame();
    hcc_link_info_t info{data.data(), static_cast<uint32_t>(data.size())};
    Check(hcc_tunnel_import(&tunnel, 0, &info) == HCC_OK, "import link");
}
void Transfer(hcc_tunnel_t tunnel, genetty_op_type_t operation, const hcc_mem_segment_t &local,
              const hcc_mem_segment_t &remote)
{
    std::array<hcc_copy_desc_t, 2> batch{};
    for (size_t i = 0; i < batch.size(); ++i) {
        uint64_t offset = OFFSET + i * BYTES / batch.size();
        batch[i] = {local.address + offset + (operation == OP_READ ? HALF : 0), remote.address + offset,
                    BYTES / batch.size(), local.lkey, remote.rkey};
    }
    hcc_request_t request = nullptr;
    Check(hcc_tunnel_submit(tunnel, operation, batch.data(), batch.size(), &request) == HCC_OK, "submit batch");
    auto ret = hcc_request_wait(request, TIMEOUT_MS);
    if (ret != HCC_OK) {
        Check(hcc_tunnel_quiesce(tunnel) == HCC_OK, "stop DMA after failure");
    }
    Check(hcc_request_release(&request) == HCC_OK, "release request");
    Check(ret == HCC_OK, "RDMA completion");
    std::printf("%s completed: local=0x%lx size=%zu offset=%zu\n", operation == OP_WRITE ? "WRITE" : "READ",
                batch[0].local_address, BYTES, OFFSET);
}
void CheckPattern(const std::vector<uint8_t> &data, size_t offset, uint8_t pattern)
{
    for (size_t i = 0; i < data.size(); ++i) {
        Check(data[i] == (i >= offset && i < offset + BYTES ? pattern : GUARD), "data and guards");
    }
}
void Run(hcc_t comm, bool host, const char *ip, uint16_t port, uint8_t pattern)
{
    Connection socket{host};
    Check((host ? socket.server.ListenAndAccept(ip, port, TIMEOUT_MS) : socket.client.Connect(ip, port, TIMEOUT_MS)) ==
              0,
          "control connect");
    hcc_tunnel_options_t options{128, 132};
    auto tunnel = hcc_tunnel_create(comm, &options);
    Check(tunnel != nullptr, "create tunnel");
    if (host) {
        ExportLink(socket, tunnel);
        ImportLink(socket, tunnel);
    } else {
        ImportLink(socket, tunnel);
        ExportLink(socket, tunnel);
    }
    std::vector<uint8_t> data(HALF * 2, GUARD);
    void *buffer = data.data();
    if (!host) {
        Check(AclApi::AclrtMalloc(&buffer, data.size(), ock::openDCC::ACL_MEM_MALLOC_HUGE_FIRST) == 0, "allocate HBM");
        Check(AclApi::AclrtMemcpy(buffer, data.size(), data.data(), data.size(),
                                  ock::openDCC::ACL_MEMCPY_HOST_TO_DEVICE) == 0,
              "initialize HBM");
    }
    hcc_mem_segment_t local{host ? MT_HOST_DRAM : MT_DEVICE_HBM, reinterpret_cast<uint64_t>(buffer), data.size(), 0, 0};
    Check(hcc_memory_register(&comm, &local, 0) == HCC_OK, "register memory after link");
    hcc_memory_info_t encoded{};
    Check(hcc_memory_info_encode(&local, 0, &encoded) == HCC_OK, "encode memory");
    socket.Frame(encoded.data, encoded.length);
    hcc_memory_free_info(&encoded);
    auto peer = socket.Frame();
    hcc_memory_info_t info{peer.data(), static_cast<uint32_t>(peer.size())};
    hcc_mem_segment_t remote{};
    Check(hcc_memory_info_decode(&info, &remote, 0) == HCC_OK, "decode memory");
    if (host) {
        socket.Receive(&pattern, 1);
        CheckPattern(data, OFFSET, pattern);
        std::fill(data.begin() + OFFSET, data.begin() + OFFSET + BYTES, pattern + 1);
        socket.Send(&pattern, 1);
        socket.Receive(&pattern, 1);
    } else {
        std::fill(data.begin() + OFFSET, data.begin() + OFFSET + BYTES, pattern);
        Check(AclApi::AclrtMemcpy(buffer, data.size(), data.data(), data.size(),
                                  ock::openDCC::ACL_MEMCPY_HOST_TO_DEVICE) == 0,
              "prepare HBM WRITE");
        Transfer(tunnel, OP_WRITE, local, remote);
        socket.Send(&pattern, 1);
        socket.Receive(&pattern, 1);
        Transfer(tunnel, OP_READ, local, remote);
        Check(AclApi::AclrtMemcpy(data.data(), data.size(), buffer, data.size(),
                                  ock::openDCC::ACL_MEMCPY_DEVICE_TO_HOST) == 0,
              "verify HBM READ");
        std::vector<uint8_t> first(data.begin(), data.begin() + HALF);
        std::vector<uint8_t> second(data.begin() + HALF, data.end());
        CheckPattern(first, OFFSET, pattern);
        CheckPattern(second, OFFSET, pattern + 1);
        socket.Send(&pattern, 1);
    }
    Check(hcc_tunnel_destroy(&tunnel, 0) == HCC_OK, "destroy tunnel");
    Check(hcc_memory_unregister(&comm, &local, 0) == HCC_OK, "unregister memory");
    if (!host) {
        Check(AclApi::AclrtFree(buffer) == 0, "free HBM");
    }
    std::printf("PASS peer=%s:%u pattern=%u\n", ip, port, pattern);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 5 && argc != 7) {
        std::fprintf(stderr,
                     "Usage: %s host <listen-ip> <port> <CX7-RoCE-IP>\n"
                     "       %s device <host-ip> <port> <user-device-id> [<second-host-ip> <port>]\n",
                     argv[0], argv[0]);
        return 1;
    }
    bool host = std::strcmp(argv[1], "host") == 0;
    Check(host || std::strcmp(argv[1], "device") == 0, "role");
    Check(!host || argc == 5, "Host accepts one peer");
    auto port = Number(argv[3]);
    Check(port > 0 && port <= UINT16_MAX, "port");
    hcc_options_t options{};
    hcc_options_init(&options);
    if (host) {
        Check(std::strlen(argv[4]) < sizeof(options.ip), "CX7 IP length");
        std::strcpy(options.ip, argv[4]);
    } else {
        Check(AclApi::Init() == 0, "ACL init");
        auto device = Number(argv[4]);
        Check(device <= INT32_MAX && AclApi::AclrtSetDevice(device) == 0, "select user device");
        int32_t logic = -1;
        Check(AclApi::AclrtGetLogicDevIdByUserDevId(device, &logic) == 0 && logic >= 0 && logic < UINT16_MAX,
              "logical device ID");
        options.provider = DEVICE_RDMA_GENETTY;
        options.logic_device_id = logic;
    }
    Check(hcc_init(host ? GEN_PROT_HOST_RDMA : GEN_PROT_DEVICE_RDMA, host ? "kunpeng" : "ascend", 0) == HCC_OK,
          "HCC init");
    auto comm = hcc_create(&options);
    Check(comm != nullptr, "create communicator");
    Run(comm, host, argv[2], port, 0x19);
    if (argc == 7) {
        auto secondPort = Number(argv[6]);
        Check(secondPort > 0 && secondPort <= UINT16_MAX, "second port");
        Run(comm, false, argv[5], secondPort, 0x39);
    }
    Check(hcc_destroy(&comm, 0) == HCC_OK, "destroy communicator");
    hcc_uninit(0);
    if (!host) {
        Check(AclApi::AclFinalize() == 0, "ACL finalize");
    }
    return 0;
}
