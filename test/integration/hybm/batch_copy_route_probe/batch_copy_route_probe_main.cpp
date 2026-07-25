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
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "device/urma/device_urma_transport_manager.h"
#include "dl_acl_api.h"
#include "dl_api.h"
#include "hybm.h"
#include "hybm_batch_copy_route.h"
#include "hybm_define.h"

using ock::mf::DlAclApi;
using ock::mf::DlApi;
using ock::mf::Result;
using ock::mf::ACL_MEMCPY_DEVICE_TO_HOST;
using ock::mf::ACL_MEMCPY_HOST_TO_DEVICE;
using ock::mf::HYBM_BATCH_COPY_META_ADDR;
using ock::mf::transport::HybmTransPrepareOptions;
using ock::mf::transport::REG_MR_FLAG_HBM;
using ock::mf::transport::TransportMemoryKey;
using ock::mf::transport::TransportMemoryRegion;
using ock::mf::transport::TransportOptions;
using ock::mf::transport::TransportPrivateData;
using ock::mf::transport::TransportRankPrepareInfo;
using ock::mf::transport::device::DeviceUrmaTransportManager;

namespace {
constexpr uint32_t RANK_COUNT = 2U;
constexpr uint16_t DEFAULT_PORT = 29876U;
constexpr uint64_t HBM_BUFFER_SIZE = 1U << 20U;
constexpr uint64_t BASIC_COPY_SIZE = 4096U;
constexpr uint32_t CONNECT_RETRY_COUNT = 60U;
constexpr uint32_t EXPORTED_MEMORY_COUNT = 2U;

struct ProbeOptions {
    uint32_t rank{RANK_COUNT};
    uint16_t device{0};
    std::string peerIp{};
    uint16_t port{DEFAULT_PORT};
};

struct PeerExchangePayload {
    TransportPrivateData privateData{};
    TransportMemoryKey memoryKeys[EXPORTED_MEMORY_COUNT]{};
};

static_assert(std::is_trivially_copyable<PeerExchangePayload>::value);

struct ProbeResources {
    DeviceUrmaTransportManager manager{};
    void *source{nullptr};
    void *destination{nullptr};
    void *timeoutFlag{nullptr};
    int controlFd{-1};
    bool hybmInitialized{false};

    ~ProbeResources()
    {
        if (controlFd >= 0) {
            (void)close(controlFd);
        }
        (void)manager.CloseDevice();
        if (destination != nullptr) {
            (void)DlAclApi::AclrtFree(destination);
        }
        if (source != nullptr) {
            (void)DlAclApi::AclrtFree(source);
        }
        if (timeoutFlag != nullptr) {
            (void)DlAclApi::AclrtFree(timeoutFlag);
        }
        if (hybmInitialized) {
            hybm_uninit();
        }
    }
};

void PrintUsage(const char *program)
{
    std::cerr << "Usage: " << program
              << " --rank <0|1> --device <id> [--peer-ip <rank0-ip>] [--port <port>]\n"
              << "rank 0 listens on the selected port; rank 1 connects to --peer-ip.\n";
}

bool ParseUint(const char *text, uint64_t maxValue, uint64_t &value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > maxValue) {
        return false;
    }
    value = parsed;
    return true;
}

bool ParseOptions(int argc, char **argv, ProbeOptions &options)
{
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            return false;
        }
        const std::string key = argv[index];
        const char *value = argv[index + 1];
        uint64_t parsed = 0;
        if (key == "--rank" && ParseUint(value, RANK_COUNT - 1U, parsed)) {
            options.rank = static_cast<uint32_t>(parsed);
        } else if (key == "--device" && ParseUint(value, UINT16_MAX, parsed)) {
            options.device = static_cast<uint16_t>(parsed);
        } else if (key == "--port" && ParseUint(value, UINT16_MAX, parsed) && parsed != 0) {
            options.port = static_cast<uint16_t>(parsed);
        } else if (key == "--peer-ip") {
            options.peerIp = value;
        } else {
            return false;
        }
    }
    return options.rank < RANK_COUNT && (options.rank == 0 || !options.peerIp.empty());
}

bool TransferAll(int fd, void *buffer, size_t size, bool sendData)
{
    auto *bytes = static_cast<uint8_t *>(buffer);
    size_t transferred = 0;
    while (transferred < size) {
        const auto ret = sendData ? send(fd, bytes + transferred, size - transferred, MSG_NOSIGNAL)
                                  : recv(fd, bytes + transferred, size - transferred, 0);
        if (ret <= 0) {
            std::cerr << (sendData ? "send" : "recv") << " failed, errno=" << errno << "\n";
            return false;
        }
        transferred += static_cast<size_t>(ret);
    }
    return true;
}

int AcceptRank1(uint16_t port)
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::cerr << "create listen socket failed, errno=" << errno << "\n";
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0) {
        std::cerr << "listen failed, port=" << port << " errno=" << errno << "\n";
        (void)close(listener);
        return -1;
    }
    const int connection = accept(listener, nullptr, nullptr);
    (void)close(listener);
    return connection;
}

int ConnectRank0(const std::string &peerIp, uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, peerIp.c_str(), &address.sin_addr) != 1) {
        std::cerr << "invalid rank0 IPv4 address: " << peerIp << "\n";
        return -1;
    }
    for (uint32_t retry = 0; retry < CONNECT_RETRY_COUNT; ++retry) {
        const int connection = socket(AF_INET, SOCK_STREAM, 0);
        if (connection >= 0 &&
            connect(connection, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) {
            return connection;
        }
        if (connection >= 0) {
            (void)close(connection);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cerr << "connect to rank0 timed out, peer=" << peerIp << " port=" << port << "\n";
    return -1;
}

bool ExchangePayload(int fd, const PeerExchangePayload &local, PeerExchangePayload &remote)
{
    auto sendCopy = local;
    return TransferAll(fd, &sendCopy, sizeof(sendCopy), true) &&
           TransferAll(fd, &remote, sizeof(remote), false);
}

bool Barrier(int fd, uint32_t rank)
{
    uint32_t local = rank;
    uint32_t remote = RANK_COUNT;
    return TransferAll(fd, &local, sizeof(local), true) && TransferAll(fd, &remote, sizeof(remote), false) &&
           remote < RANK_COUNT && remote != rank;
}

std::vector<uint8_t> MakePattern(uint32_t rank)
{
    std::vector<uint8_t> pattern(HBM_BUFFER_SIZE);
    for (size_t index = 0; index < pattern.size(); ++index) {
        pattern[index] = static_cast<uint8_t>((rank * 37U + index) & 0xFFU);
    }
    return pattern;
}

Result AllocateBuffers(const std::vector<uint8_t> &pattern, ProbeResources &resources)
{
    auto ret = DlAclApi::AclrtMalloc(&resources.source, HBM_BUFFER_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != BM_OK) {
        std::cerr << "AclrtMalloc source failed, ret=" << ret << "\n";
        return ret;
    }
    ret = DlAclApi::AclrtMalloc(&resources.destination, HBM_BUFFER_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != BM_OK) {
        std::cerr << "AclrtMalloc destination failed, ret=" << ret << "\n";
        return ret;
    }
    ret = DlAclApi::AclrtMalloc(&resources.timeoutFlag, sizeof(uint64_t), ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != BM_OK) {
        std::cerr << "AclrtMalloc timeout flag failed, ret=" << ret << "\n";
        return ret;
    }
    ret = DlAclApi::AclrtMemcpy(resources.source, HBM_BUFFER_SIZE, pattern.data(), pattern.size(),
                                ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        std::cerr << "copy source pattern failed, ret=" << ret << "\n";
        return ret;
    }
    ret = DlAclApi::AclrtMemset(resources.destination, HBM_BUFFER_SIZE, 0, HBM_BUFFER_SIZE);
    if (ret != BM_OK) {
        std::cerr << "clear destination buffer failed, ret=" << ret << "\n";
        return ret;
    }
    ret = DlAclApi::AclrtMemset(resources.timeoutFlag, sizeof(uint64_t), 0, sizeof(uint64_t));
    if (ret != BM_OK) {
        std::cerr << "clear timeout flag failed, ret=" << ret << "\n";
    }
    return ret;
}

Result PreparePeers(DeviceUrmaTransportManager &manager, uint32_t peerRank, const PeerExchangePayload &remote)
{
    HybmTransPrepareOptions endpointOptions{};
    TransportRankPrepareInfo endpointInfo{};
    endpointInfo.privateData = remote.privateData;
    endpointOptions.options.emplace(peerRank, endpointInfo);
    auto ret = manager.Prepare(endpointOptions);
    if (ret != BM_OK) {
        std::cerr << "endpoint Prepare failed, peerRank=" << peerRank << " ret=" << ret << "\n";
        return ret;
    }
    HybmTransPrepareOptions memoryOptions{};
    TransportRankPrepareInfo memoryInfo{};
    memoryInfo.privateData = remote.privateData;
    memoryInfo.memKeys.assign(std::begin(remote.memoryKeys), std::end(remote.memoryKeys));
    memoryOptions.options.emplace(peerRank, std::move(memoryInfo));
    ret = manager.Prepare(memoryOptions);
    if (ret != BM_OK) {
        std::cerr << "memory Prepare or route publish failed, peerRank=" << peerRank << " ret=" << ret << "\n";
    }
    return ret;
}

Result RunCopyCase(DeviceUrmaTransportManager &manager, void *destination, const std::vector<uint8_t> &expected,
                   uint64_t offset, uint64_t length)
{
    auto ret = DlAclApi::AclrtMemset(destination, HBM_BUFFER_SIZE, 0, HBM_BUFFER_SIZE);
    if (ret != BM_OK) {
        std::cerr << "clear destination failed, ret=" << ret << "\n";
        return ret;
    }
    ret = manager.LaunchBatchCopyRouteProbeForTest(0, 0, offset, reinterpret_cast<uint64_t>(destination), length);
    if (ret != BM_OK) {
        std::cerr << "route probe launch failed, offset=" << offset << " length=" << length << " ret=" << ret << "\n";
        return ret;
    }
    std::vector<uint8_t> actual(length);
    ret = DlAclApi::AclrtMemcpy(actual.data(), actual.size(), destination, length, ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != BM_OK) {
        std::cerr << "copy probe output failed, ret=" << ret << "\n";
        return ret;
    }
    if (!std::equal(actual.begin(), actual.end(), expected.begin() + static_cast<ptrdiff_t>(offset))) {
        std::cerr << "probe data mismatch, offset=" << offset << " length=" << length << "\n";
        return BM_ERROR;
    }
    return BM_OK;
}

Result ValidateRouteHeader(uint32_t expectedMagic)
{
    ock::mf::BatchCopyRouteHeader header{};
    const auto ret = DlAclApi::AclrtMemcpy(&header, sizeof(header),
                                           reinterpret_cast<void *>(HYBM_BATCH_COPY_META_ADDR), sizeof(header),
                                           ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != BM_OK) {
        std::cerr << "read route header failed, ret=" << ret << "\n";
        return ret;
    }
    if (header.magic != expectedMagic ||
        (expectedMagic != 0 && (header.peerCount != 1U || header.rangeCount != EXPORTED_MEMORY_COUNT))) {
        std::cerr << "unexpected route header, magic=0x" << std::hex << header.magic << std::dec
                  << " peerCount=" << header.peerCount << " rangeCount=" << header.rangeCount << "\n";
        return BM_ERROR;
    }
    return BM_OK;
}

Result LaunchProbeIsolated(DeviceUrmaTransportManager &manager, uint32_t peerIndex, uint32_t rangeIndex,
                           uint64_t srcOffset, uint64_t destination, uint64_t length, bool &launched)
{
    launched = false;
    Result result = BM_ERROR;
    std::thread launchThread([&]() {
        result = DlAclApi::AclrtSetDevice(HybmGetInitDeviceId());
        if (result == BM_OK) {
            launched = true;
            result =
                manager.LaunchBatchCopyRouteProbeForTest(peerIndex, rangeIndex, srcOffset, destination, length);
        } else {
            std::cerr << "AclrtSetDevice failed in isolated probe thread, ret=" << result << "\n";
        }
    });
    launchThread.join();
    return result;
}

Result RunNegativeCases(DeviceUrmaTransportManager &manager, void *destination)
{
    const uint64_t dst = reinterpret_cast<uint64_t>(destination);
    if (manager.LaunchBatchCopyRouteProbeForTest(1, 0, 0, dst, 1) != BM_INVALID_PARAM ||
        manager.LaunchBatchCopyRouteProbeForTest(0, EXPORTED_MEMORY_COUNT, 0, dst, 1) != BM_INVALID_PARAM ||
        manager.LaunchBatchCopyRouteProbeForTest(0, 0, HBM_BUFFER_SIZE - 1U, dst, 2) != BM_INVALID_PARAM) {
        std::cerr << "host route probe negative validation failed\n";
        return BM_ERROR;
    }
    const uint32_t zeroMagic = 0;
    auto ret = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(HYBM_BATCH_COPY_META_ADDR), sizeof(zeroMagic),
                                     &zeroMagic, sizeof(zeroMagic), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        std::cerr << "clear route magic failed, ret=" << ret << "\n";
        return ret;
    }
    bool launched = false;
    const auto launchRet = LaunchProbeIsolated(manager, 0, 0, 0, dst, 1, launched);
    const uint32_t validMagic = ock::mf::BATCH_COPY_ROUTE_MAGIC;
    ret = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(HYBM_BATCH_COPY_META_ADDR), sizeof(validMagic),
                                &validMagic, sizeof(validMagic), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        std::cerr << "restore route magic failed, ret=" << ret << "\n";
        return ret;
    }
    if (!launched) {
        return launchRet;
    }
    if (launchRet == BM_OK) {
        std::cerr << "AICPU accepted route with magic=0\n";
        return BM_ERROR;
    }
    return BM_OK;
}

Result RunTimeoutCase(DeviceUrmaTransportManager &manager, void *destination)
{
    ock::mf::BatchCopyRouteTable table{};
    auto ret = DlAclApi::AclrtMemcpy(&table, sizeof(table), reinterpret_cast<void *>(HYBM_BATCH_COPY_META_ADDR),
                                     sizeof(table), ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != BM_OK || table.header.rangeCount < EXPORTED_MEMORY_COUNT ||
        table.ranges[1].peerIndex != 0 || table.ranges[1].srcGvaBegin == 0) {
        std::cerr << "timeout injection route validation failed, ret=" << ret << "\n";
        return ret == BM_OK ? BM_ERROR : ret;
    }
    const auto originalPeer = table.peers[0];
    auto timeoutPeer = originalPeer;
    timeoutPeer.remoteFlagAddr = table.ranges[1].srcGvaBegin;
    const uint64_t peerAddress = HYBM_BATCH_COPY_META_ADDR + offsetof(ock::mf::BatchCopyRouteTable, peers);
    ret = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(peerAddress), sizeof(timeoutPeer), &timeoutPeer,
                                sizeof(timeoutPeer), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        std::cerr << "write timeout peer entry failed, ret=" << ret << "\n";
        return ret;
    }
    bool launched = false;
    const auto launchRet =
        LaunchProbeIsolated(manager, 0, 0, 0, reinterpret_cast<uint64_t>(destination), 1, launched);
    const auto restoreRet = DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(peerAddress), sizeof(originalPeer),
                                                  &originalPeer, sizeof(originalPeer), ACL_MEMCPY_HOST_TO_DEVICE);
    if (restoreRet != BM_OK) {
        std::cerr << "restore route peer entry failed, ret=" << restoreRet << "\n";
        return restoreRet;
    }
    if (!launched) {
        return launchRet;
    }
    if (launchRet == BM_OK) {
        std::cerr << "route probe timeout injection unexpectedly succeeded\n";
        return BM_ERROR;
    }
    return BM_OK;
}

Result InitializeLocalProbe(const ProbeOptions &options, ProbeResources &resources, PeerExchangePayload &local)
{
    const char *probeJson = std::getenv("MF_HYBM_AICPU_KERNEL_JSON");
    if (probeJson == nullptr || probeJson[0] == '\0') {
        std::cerr << "MF_HYBM_AICPU_KERNEL_JSON must point to libcann_hybm_probe_kernel.json\n";
        return BM_NOT_INITIALIZED;
    }
    (void)setenv("MF_HYBM_BATCH_COPY_ROUTE_PROBE", "1", 1);
    auto ret = static_cast<Result>(hybm_init(options.device, HYBM_FLAG_INIT_SHMEM_META));
    if (ret != BM_OK) {
        std::cerr << "hybm_init failed, device=" << options.device << " ret=" << ret << "\n";
        return ret;
    }
    resources.hybmInitialized = true;
    const auto localPattern = MakePattern(options.rank);
    ret = DlApi::LoadExtendLibrary(ock::mf::DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_URMA);
    if (ret == BM_OK) {
        TransportOptions transportOptions{options.rank, RANK_COUNT, HYBM_DOP_TYPE_DEVICE_URMA,
                                          HYBM_TYPE_AICPU_INITIATE, HYBM_ROLE_PEER, "", {}};
        ret = resources.manager.OpenDevice(transportOptions);
    }
    if (ret == BM_OK) {
        ret = AllocateBuffers(localPattern, resources);
    }
    if (ret == BM_OK) {
        const TransportMemoryRegion region{reinterpret_cast<uint64_t>(resources.source), HBM_BUFFER_SIZE,
                                           ock::mf::transport::REG_MR_ACCESS_FLAG_BOTH_READ_WRITE, REG_MR_FLAG_HBM};
        ret = resources.manager.RegisterMemoryRegion(region);
    }
    if (ret == BM_OK) {
        const TransportMemoryRegion timeoutRegion{
            reinterpret_cast<uint64_t>(resources.timeoutFlag), sizeof(uint64_t),
            ock::mf::transport::REG_MR_ACCESS_FLAG_BOTH_READ_WRITE, REG_MR_FLAG_HBM};
        ret = resources.manager.RegisterMemoryRegion(timeoutRegion);
    }
    if (ret == BM_OK) {
        local.privateData = resources.manager.GetPrivateData();
        ret = resources.manager.QueryMemoryKey(reinterpret_cast<uint64_t>(resources.source), local.memoryKeys[0]);
    }
    if (ret == BM_OK) {
        ret = resources.manager.QueryMemoryKey(reinterpret_cast<uint64_t>(resources.timeoutFlag),
                                               local.memoryKeys[1]);
    }
    return ret;
}

Result ExchangeAndPrepare(const ProbeOptions &options, ProbeResources &resources,
                          const PeerExchangePayload &local)
{
    resources.controlFd =
        options.rank == 0 ? AcceptRank1(options.port) : ConnectRank0(options.peerIp, options.port);
    if (resources.controlFd < 0) {
        return BM_ERROR;
    }
    PeerExchangePayload remote{};
    if (!ExchangePayload(resources.controlFd, local, remote)) {
        return BM_ERROR;
    }
    return PreparePeers(resources.manager, 1U - options.rank, remote);
}

Result ExecuteProbeCases(const ProbeOptions &options, ProbeResources &resources)
{
    const auto remotePattern = MakePattern(1U - options.rank);
    auto ret = ValidateRouteHeader(ock::mf::BATCH_COPY_ROUTE_MAGIC);
    if (ret != BM_OK) {
        return ret;
    }
    ret = RunCopyCase(resources.manager, resources.destination, remotePattern, 0, BASIC_COPY_SIZE);
    if (ret != BM_OK) {
        return ret;
    }
    ret = RunCopyCase(resources.manager, resources.destination, remotePattern, 123U, 8192U);
    if (ret != BM_OK) {
        return ret;
    }
    ret = RunCopyCase(resources.manager, resources.destination, remotePattern, HBM_BUFFER_SIZE - BASIC_COPY_SIZE,
                      BASIC_COPY_SIZE);
    if (ret != BM_OK) {
        return ret;
    }
    ret = RunNegativeCases(resources.manager, resources.destination);
    if (ret != BM_OK) {
        return ret;
    }
    return RunTimeoutCase(resources.manager, resources.destination);
}

Result CloseProbe(const ProbeOptions &options, ProbeResources &resources, Result currentResult)
{
    if (!Barrier(resources.controlFd, options.rank) && currentResult == BM_OK) {
        currentResult = BM_ERROR;
    }
    (void)close(resources.controlFd);
    resources.controlFd = -1;
    const auto closeRet = resources.manager.CloseDevice();
    if (currentResult == BM_OK) {
        currentResult = closeRet;
    }
    if (currentResult == BM_OK) {
        currentResult = ValidateRouteHeader(0);
    }
    return currentResult;
}

Result RunProbe(const ProbeOptions &options)
{
    ProbeResources resources{};
    PeerExchangePayload local{};
    auto ret = InitializeLocalProbe(options, resources, local);
    if (ret != BM_OK) {
        return ret;
    }
    ret = ExchangeAndPrepare(options, resources, local);
    if (ret != BM_OK) {
        return ret;
    }
    ret = ExecuteProbeCases(options, resources);
    return CloseProbe(options, resources, ret);
}
} // namespace

int main(int argc, char **argv)
{
    ProbeOptions options{};
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage(argv[0]);
        return 2;
    }
    const auto ret = RunProbe(options);
    if (ret != BM_OK) {
        std::cerr << "batch_copy_route_probe FAILED, rank=" << options.rank << " ret=" << ret << "\n";
        return 1;
    }
    std::cout << "batch_copy_route_probe PASSED, rank=" << options.rank << "\n";
    return 0;
}
