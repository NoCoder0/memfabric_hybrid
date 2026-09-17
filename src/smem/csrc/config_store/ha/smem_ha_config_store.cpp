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
#include "smem_ha_config_store.h"

#include <unistd.h>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <cerrno>
#include <cstring>

#include <array>
#include <chrono>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "smem_config_store_logger.h"
#include "network_endpoint_util.h"
#include "mf_ipv4_validator.h"
#include "mf_str_util.h"
#include "smem_message_packer.h"
#include "smem_tcp_config_store_ssl_helper.h"

namespace ock {
namespace smem {

using namespace ock::mf;

namespace {
constexpr char BACKEND_LOCK_NAME[] = "backend";
}

// ============================================================================
// HaConfigStore - Construction / Destruction
// ============================================================================

HaConfigStore::HaConfigStore(StoreBackendPtr backend, TcpConfigStorePtr clientDelegate, const std::string &endpoints,
                             uint32_t worldSize, uint16_t model)
    : endpoints_(endpoints), worldSize_(worldSize), backendLockName_(BACKEND_LOCK_NAME), model_(model),
      backend_(std::move(backend)), clientDelegate_(std::move(clientDelegate))
{
    // clang-format off
    SM_LOG_DEBUG("HaConfigStore constructing, endpoints: " << endpoints << ", worldSize: " << worldSize <<
                 ", backendLockName: " << backendLockName_ << ", model: " << model);
    // clang-format on
}

void HaConfigStore::Uninitialize() noexcept
{
    stopFlag_.store(true, std::memory_order_release);

    // Join health check thread
    if (healthCheckThread_.joinable()) {
        healthCheckThread_.join();
    }

    // Join re-election thread (guaranteed to terminate since stopFlag_ is set)
    {
        std::lock_guard<std::mutex> lock(reElectionThreadMutex_);
        if (reElectionThread_.joinable()) {
            reElectionThread_.join();
        }
    }

    if (clientDelegate_ != nullptr) {
        SM_LOG_DEBUG("Shutting down clientDelegate_");
        clientDelegate_->Shutdown();
        clientDelegate_ = nullptr;
    }

    {
        std::unique_lock<std::shared_mutex> lock(delegateRwLock_);
        if (serverDelegate_ != nullptr) {
            SM_LOG_DEBUG("Shutting down serverDelegate_");
            serverDelegate_->Shutdown();
            serverDelegate_ = nullptr;
        }
        isLeader_.store(false, std::memory_order_release);
    }

    // Close backend connection after all threads/delegates are stopped
    {
        std::lock_guard<std::mutex> bLock(backendMutex_);
        backend_->UnInitialize();
    }
    SM_LOG_DEBUG("Backend connection closed");
}

HaConfigStore::~HaConfigStore()
{
    SM_LOG_DEBUG("HaConfigStore destructor called, endpoints: " << endpoints_);
    Uninitialize();
    SM_LOG_DEBUG("HaConfigStore destroyed");
}

// ============================================================================
// Startup / Election
// ============================================================================

Result HaConfigStore::Startup(const smem_tls_config &tlsConfig) noexcept
{
    SM_LOG_TRACE("HaConfigStore starting, endpoints: " << endpoints_);

    if (clientDelegate_ == nullptr) {
        SM_LOG_ERROR("clientDelegate_ is null, cannot start");
        return SM_NOT_INITIALIZED;
    }

    tlsConfig_ = tlsConfig;

    // CSM_CLIENT: 只读 leader 地址并作为 follower 连接，不参与选举，避免所有 rank 抢分布式锁。
    if (model_ == CSM_CLIENT) {
        SM_LOG_INFO("HaConfigStore in client-only model, skipping election loop, endpoints: " << endpoints_);
        StartHealthCheckThread();
        // 必须连接 leader，否则 clientDelegate_ 无 TCP 链路；leader 可能未选出，同步等待重试。
        ConnectToLeaderAsFollower();
        return SM_OK;
    }

    SM_LOG_TRACE("Entering election loop");
    RunElectionLoop();
    StartHealthCheckThread();
    SM_LOG_TRACE("HaConfigStore started successfully");
    return SM_OK;
}

bool HaConfigStore::InitBackendConnection() noexcept
{
    SM_LOG_TRACE("Initializing backend connection: " << endpoints_);
    // backendMutex_ 只保护 backend 句柄的初始化/反初始化，临界区必须短，
    // 不得把抢分布式锁等长阻塞操作包含进来，否则会饿死健康检查并拖长降级时延。
    std::lock_guard<std::mutex> lock(backendMutex_);
    int backendRet = backend_->Initialize(endpoints_, "", "");
    if (backendRet != 0) {
        SM_LOG_ERROR("Failed to init backend client, endpoints: " << endpoints_ << ", ret: " << backendRet);
        return false;
    }

    SM_LOG_TRACE("Backend connection initialized, endpoints: " << endpoints_);
    return true;
}

void HaConfigStore::RandomBackoff() noexcept
{
    static thread_local std::mt19937 generator(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^ reinterpret_cast<uintptr_t>(&generator)));
    static thread_local std::uniform_int_distribution<int> distribution(MIN_SLEEP_MS, MAX_SLEEP_MS);

    int sleepDuration = distribution(generator);
    SM_LOG_DEBUG("Random backoff: " << sleepDuration << "ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepDuration));
}

bool HaConfigStore::IsLeaderAlive(std::string &leaderAddr) noexcept
{
    int backendRet;
    {
        std::lock_guard<std::mutex> lock(backendMutex_);
        backendRet = backend_->Get(KEY_LEADER, leaderAddr);
    }
    if (backendRet != SUCCESS) {
        SM_LOG_ERROR("Backend Get failed, key: " << KEY_LEADER << ", ret: " << backendRet);
        return false;
    }

    if (leaderAddr.empty()) {
        SM_LOG_INFO("No leader registered in backend");
        return false;
    }

    // clang-format off
    SM_LOG_TRACE("Backend leader: " << leaderAddr <<
                 ", liveness governed by etcd lease (key auto-deleted on lease expiry)");
    // clang-format on
    return true;
}

std::string HaConfigStore::BuildSelfLeaderAddr() const noexcept
{
    constexpr size_t kTcpSchemeLen = 6;
    const std::string endpoint = NetworkEndpointUtil::BuildEndpoint("tcp", leaderBindIp_, leaderBindPort_);
    if (endpoint.size() <= kTcpSchemeLen) {
        return "";
    }
    return endpoint.substr(kTcpSchemeLen);
}

Result HaConfigStore::TryBecomeLeader() noexcept
{
    constexpr size_t kTcpSchemeLen = 6;
    SM_ASSERT_RETURN(NetworkEndpointUtil::ExtractIpAndPort(endpoints_, leaderBindIp_, leaderBindPort_), SM_ERROR);
    bool isIpv6 = endpoints_.find('[') != std::string::npos;
    SM_ASSERT_RETURN(NetworkEndpointUtil::FindAvailablePort(leaderBindPort_, isIpv6), SM_ERROR);
    SM_ASSERT_RETURN(NetworkEndpointUtil::GetLocalIpWithTarget(leaderBindIp_, leaderBindIp_), SM_ERROR);
    SM_LOG_TRACE("Attempting to become leader, addr: " << leaderBindIp_ << ":" << leaderBindPort_);

    // Select MetaService port from the same port range, excluding the port
    // already bound by the config store leader to avoid a port collision.
    SM_ASSERT_RETURN(NetworkEndpointUtil::FindAvailablePort(metaServiceBindPort_, isIpv6, leaderBindPort_), SM_ERROR);
    SM_LOG_INFO("MetaService port selected: " << metaServiceBindPort_);

    // Start server
    StartServer();
    if (!isLeader_.load(std::memory_order_acquire)) {
        SM_LOG_ERROR("StartServer failed");
        return SM_ERROR;
    }
    SM_LOG_TRACE("AccStoreServer started successfully");

    // Register as leader in backend
    const std::string myEndpoint = NetworkEndpointUtil::BuildEndpoint("tcp", leaderBindIp_, leaderBindPort_);
    SM_ASSERT_RETURN(!myEndpoint.empty(), SM_ERROR);
    std::string myAddr = myEndpoint.substr(kTcpSchemeLen);
    auto registerRet = backend_->Put(KEY_LEADER, myAddr, PUT_LEASE_TTL_SEC);
    if (registerRet != 0) {
        SM_LOG_ERROR("Failed to register leader address in backend: " << myAddr << ", ret: " << registerRet);
        StopServer();
        return SM_ERROR;
    }
    SM_LOG_TRACE("Registered in backend: " << myAddr << ", TTL: " << PUT_LEASE_TTL_SEC << "s");

    std::string metaServiceAddr = leaderBindIp_ + ":" + std::to_string(metaServiceBindPort_);
    auto metaAddrRet = backend_->Put(KEY_META_SERVICE_ADDR, metaServiceAddr, PUT_LEASE_TTL_SEC);
    if (metaAddrRet != 0) {
        SM_LOG_WARN("Failed to register MetaService address in backend: " << metaServiceAddr);
    } else {
        SM_LOG_INFO("Registered MetaService address in backend: " << metaServiceAddr);
    }

    NotifyLeaderChange();

    // Connect client delegate to self
    auto clientRet = ConnectClient(leaderBindIp_, leaderBindPort_);
    if (clientRet != SM_OK) {
        StopServer();
        (void)backend_->Delete(KEY_LEADER);
        isLeader_.store(false, std::memory_order_release);
        return SM_ERROR;
    }
    SM_LOG_TRACE("Self-connection established");
    {
        std::lock_guard<std::mutex> lock(lastLeaderMutex_);
        lastConnectedLeader_ = myAddr;
    }

    NotifyLeaderChange();
    return SM_OK;
}

bool HaConfigStore::HandleLeaderExists(const std::string &leaderAddr) noexcept
{
    if (leaderAddr == BuildSelfLeaderAddr()) {
        // KEY_LEADER 指向本节点：本节点就是 leader，重新登记并继续服务，不能降级为“跟随自己”。
        SM_LOG_INFO("Leader key points to this node, re-acquiring leadership: " << leaderAddr);
        if (TryBecomeLeader() != SM_OK) {
            SM_LOG_ERROR("Re-acquire leadership failed when leader key points to this node, leader: " << leaderAddr);
            return false;
        }
        return true;
    }
    isFirstLeader_ = false;
    SM_LOG_INFO("Found alive leader: " << leaderAddr << ", becoming follower");
    if (BecomeFollower(leaderAddr) != SM_OK) {
        SM_LOG_ERROR("Becoming follower failed, leader: " << leaderAddr);
        // Keep the backend connection alive: closing the process-global etcd client
        // here races with the health-check thread (Get on an uninitialized client),
        // and the next election loop iteration reuses the connection directly.
        return false;
    }
    SM_LOG_INFO("Election loop exiting: became follower of " << leaderAddr);
    return true;
}

bool HaConfigStore::TryAcquireLeadership(bool &becameLeader, uint32_t electionAttempt) noexcept
{
    // 进入重选举即先停服：领导权待定期间不得继续对外服务，避免旧主与新主并存。
    // StopServer 对非 leader 进程是幂等空操作；若本进程恰为旧主，则会立即停 AccStoreServer
    // 并回调通知上层（memcache 侧据此停 MetaNetServer）。
    StopServer();
    std::string leaderAddr;
    {
        DistributedLockGuard lockGuard(backend_, backendLockName_);
        if (!lockGuard.IsLocked()) {
            SM_LOG_ERROR("Failed to acquire distributed lock, will retry");
            return false;
        }
        SM_LOG_INFO("Distributed lock acquired, double-checking leader");
        if (IsLeaderAlive(leaderAddr)) {
            if (leaderAddr == BuildSelfLeaderAddr()) {
                // KEY_LEADER 仍指向本节点（如健康检查瞬时超时后的重选举）：本节点仍是 leader，
                // 重新登记并继续服务；绝不能把自己登记的地址当作其它 leader 降级，否则会出现无主。
                SM_LOG_INFO("Leader key still points to this node, re-acquiring leadership: " << leaderAddr);
                becameLeader = TryBecomeLeader() == SM_OK;
                if (!becameLeader) {
                    SM_LOG_ERROR("Re-acquire leadership failed, attempt=" << electionAttempt
                                                                          << ", addr=" << leaderAddr);
                }
                return becameLeader;
            }
            SM_LOG_INFO("Leader appeared during lock acquisition: " << leaderAddr);
            if (BecomeFollower(leaderAddr) != SM_OK) {
                SM_LOG_ERROR("Becoming follower failed after lock, leader: " << leaderAddr);
                return false;
            }
            SM_LOG_INFO("Election loop exiting: became follower of " << leaderAddr << " after lock");
            becameLeader = false;
            return true;
        }
        SM_LOG_INFO("Proceeding to become leader");
        if (TryBecomeLeader() == SM_OK) {
            SM_LOG_INFO("Became leader after " << electionAttempt << " attempts");
            becameLeader = true;
        } else {
            SM_LOG_ERROR("Become leader failed, attempt=" << electionAttempt << " addr=" << leaderAddr << " released");
            becameLeader = false;
        }
    }
    if (becameLeader) {
        return true;
    }
    return false;
}

void HaConfigStore::RunElectionLoop() noexcept
{
    pthread_setname_np(pthread_self(), "election-loop");
    (void)signal(SIGPIPE, SIG_IGN);
    uint32_t electionAttempt = 0;
    while (!stopFlag_.load(std::memory_order_acquire)) {
        ++electionAttempt;
        SM_LOG_INFO("Election attempt #" << electionAttempt << " starting, endpoints: " << endpoints_);
        RandomBackoff();
        if (stopFlag_.load(std::memory_order_acquire)) {
            SM_LOG_INFO("Stop flag detected, exiting election loop");
            break;
        }
        // 注意：这里不再整段持有 backendMutex_。抢分布式锁最长可阻塞 LockAcquireTimeout，
        // 若与健康检查共用同一把锁，会让旧主在租约失效后仍无法及时降级，造成双主。
        // backendMutex_ 仅在 InitBackendConnection / IsLeaderAlive 的短临界区内持有。
        if (!InitBackendConnection()) {
            SM_LOG_ERROR("Backend connection failed, will retry");
            continue;
        }
        std::string leaderAddr;
        if (IsLeaderAlive(leaderAddr)) {
            if (HandleLeaderExists(leaderAddr)) {
                return;
            }
            continue;
        }
        SM_LOG_INFO("No alive leader found, attempting to acquire lock");
        bool becameLeader = false;
        if (TryAcquireLeadership(becameLeader, electionAttempt)) {
            if (becameLeader) {
                break;
            }
            return;
        }
    }
    SM_LOG_TRACE("Election loop exiting: stop flag set after " << electionAttempt << " attempts");
}

// ============================================================================
// Server Management
// ============================================================================

void HaConfigStore::StartServer() noexcept
{
    SM_LOG_DEBUG("Starting AccStoreServer");
    std::unique_lock<std::shared_mutex> lock(delegateRwLock_);

    if (isLeader_.load(std::memory_order_acquire)) {
        SM_LOG_WARN("Already a leader, skipping server startup");
        return;
    }

    SM_LOG_TRACE("ip: " << leaderBindIp_ << ", port: " << leaderBindPort_ << ", worldSize: " << worldSize_);

    // Recover world size from backend
    std::string worldSizeStr;
    uint32_t recoveredWorldSize = worldSize_;
    int backendRet = backend_->Get(KEY_WORLD_SIZE, worldSizeStr);
    bool recovered = false;
    if (backendRet == 0 && !worldSizeStr.empty()) {
        if (StrUtil::String2Uint(worldSizeStr, recoveredWorldSize)) {
            if (recoveredWorldSize == 0) {
                SM_LOG_WARN("Recovered worldSize is 0, using default: " << worldSize_);
            } else {
                SM_LOG_TRACE("Recovered worldSize from backend: " << recoveredWorldSize);
                recovered = true;
            }
        } else {
            SM_LOG_WARN("Unable to parse worldSize: " << worldSizeStr);
        }
    }
    if (!recovered) {
        recoveredWorldSize = worldSize_;
        SM_LOG_DEBUG("No valid worldSize in backend, using default: " << worldSize_);
    }

    // Create and configure server, skip recover if is the first leader
    serverDelegate_ =
        SmMakeRef<AccStoreServer>(leaderBindIp_, leaderBindPort_, recoveredWorldSize, backend_, isFirstLeader_);
    SM_ASSERT_RET_VOID(serverDelegate_ != nullptr);
    SM_LOG_INFO("AccStoreServer created, ip: " << leaderBindIp_ << ", port: " << leaderBindPort_
                                               << ", worldSize: " << recoveredWorldSize);
    // Update status in backend to not-ready
    backendRet = serverDelegate_->UpdateStatus(false);
    SM_ASSERT_RET_VOID(backendRet == 0);
    SM_LOG_INFO("Backend status set to not-ready");
    // Restore metadata from backend
    SM_ASSERT_RET_VOID(serverDelegate_->RestoreFromBackend() == SM_OK);
    // Re-register cached handlers
    {
        std::unique_lock<std::mutex> stateLock(stateMutex_);
        if (cachedServerBrokenHandler_ != nullptr) {
            SM_LOG_DEBUG("Registering cached broken handler");
            serverDelegate_->RegisterBrokenLinkCHandler(cachedServerBrokenHandler_);
        }
    }

    // Validate server address
    const std::string serverEndpoint = NetworkEndpointUtil::BuildEndpoint("tcp", leaderBindIp_, leaderBindPort_);
    SM_ASSERT_RET_VOID(!serverEndpoint.empty());
    auto parser = SocketAddressParserMgr::getInstance().CreateParser(serverEndpoint);
    SM_ASSERT_RET_VOID(parser);
    // Start the server
    Result startRet = serverDelegate_->Startup(tlsConfig_);
    if (startRet != SM_OK) {
        SM_LOG_ERROR("Startup failed at " << leaderBindIp_ << ":" << leaderBindPort_ << ", ret: " << startRet);
        serverDelegate_ = nullptr;
        return;
    }
    isLeader_.store(true, std::memory_order_release);
    SM_LOG_DEBUG("AccStoreServer started successfully");
}

void HaConfigStore::StopServer() noexcept
{
    SM_LOG_DEBUG("StopServer called");
    std::unique_lock<std::shared_mutex> lock(delegateRwLock_);
    if (!isLeader_.load(std::memory_order_acquire)) {
        SM_LOG_DEBUG("Not a leader, skip");
        return;
    }
    SM_LOG_DEBUG("Stopping AccStoreServer");
    if (serverDelegate_ != nullptr) {
        SM_LOG_DEBUG("Shutting down serverDelegate_");
        serverDelegate_->Shutdown();
        serverDelegate_ = nullptr;
    }
    isLeader_.store(false, std::memory_order_release);
    SM_LOG_DEBUG("AccStoreServer stopped");
    NotifyLeaderChange();
}

// ============================================================================
// Leader change notification
// ============================================================================

void HaConfigStore::NotifyLeaderChange() noexcept
{
    if (!leaderChangeCallback_) {
        return;
    }
    LeaderAddresses addrs;
    addrs.isLeader = IsLeader();
    if (addrs.isLeader) {
        addrs.metaServiceAddr = leaderBindIp_ + ":" + std::to_string(metaServiceBindPort_);
    }
    leaderChangeCallback_(addrs);
}

// ============================================================================
// Re-election
// ============================================================================
void HaConfigStore::ReElectionThreadFunc()
{
    SM_LOG_INFO("Re-election thread started");
    if (!stopFlag_.load(std::memory_order_acquire)) {
        if (model_ == CSM_CLIENT) {
            // client-only: 断链后不参与选举（避免抢分布式锁），直接重连当前 leader
            SM_LOG_INFO("Client-only model, reconnecting to leader instead of election");
            ConnectToLeaderAsFollower();
        } else {
            RunElectionLoop();
        }
    }
    reElectionInProgress_.store(false, std::memory_order_release);
    NotifyLeaderChange();
    SM_LOG_INFO("Re-election thread finished");
}

void HaConfigStore::TriggerReElectionAsync() noexcept
{
    if (stopFlag_.load(std::memory_order_acquire)) {
        SM_LOG_WARN("Stop flag set, skipping re-election");
        return;
    }

    bool expected = false;
    if (!reElectionInProgress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        SM_LOG_INFO("Re-election already in progress, skip duplicate trigger");
        return;
    }

    SM_LOG_INFO("Triggering async re-election");

    std::lock_guard<std::mutex> lock(reElectionThreadMutex_);
    // Move previous thread out so the new thread can join it asynchronously
    std::thread prev = std::move(reElectionThread_);

    reElectionThread_ = std::thread([this, prev = std::move(prev)]() mutable {
        pthread_setname_np(pthread_self(), "ha_elect_th");
        if (prev.joinable()) {
            prev.join();
        }
        ReElectionThreadFunc();
    });
}

// ============================================================================
// Client Connection
// ============================================================================

// CSM_CLIENT 专用：作为 follower 连接当前 leader，leader 未就绪时周期性重试。
void HaConfigStore::ConnectToLeaderAsFollower() noexcept
{
    constexpr uint32_t kClientConnectRetryMs = 2000;
    constexpr uint32_t kClientConnectMaxAttempts = 60; // 最长约 120s
    constexpr int kClientConnectRetryTimes = 3;
    for (uint32_t attempt = 1; attempt <= kClientConnectMaxAttempts; ++attempt) {
        if (stopFlag_.load(std::memory_order_acquire)) {
            SM_LOG_WARN("Stop flag set, abort client leader connect");
            return;
        }
        std::string leaderAddr;
        const bool hasLeader = backend_->Get(KEY_LEADER, leaderAddr) == SUCCESS && !leaderAddr.empty();
        if (hasLeader) {
            std::string ip;
            uint16_t port = 0;
            const bool addrOk = NetworkEndpointUtil::ExtractIpAndPort("tcp://" + leaderAddr, ip, port) && port != 0;
            if (addrOk) {
                const auto connectRet = ConnectClient(ip, port, kClientConnectRetryTimes);
                if (connectRet == SM_OK) {
                    SM_LOG_INFO("Client-only HaConfigStore connected to leader: " << leaderAddr);
                    {
                        std::lock_guard<std::mutex> lock(lastLeaderMutex_);
                        lastConnectedLeader_ = leaderAddr;
                    }
                    NotifyLeaderChange();
                    return;
                }
                SM_LOG_WARN("Client-only connect to leader failed: " << leaderAddr << ", ret: " << connectRet);
                std::this_thread::sleep_for(std::chrono::milliseconds(kClientConnectRetryMs));
                continue;
            }
            SM_LOG_WARN("Invalid leader address from backend: " << leaderAddr);
        } else {
            SM_LOG_INFO("No leader registered yet, attempt " << attempt << "/" << kClientConnectMaxAttempts);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kClientConnectRetryMs));
    }
    // clang-format off
    SM_LOG_ERROR("Client-only HaConfigStore failed to connect to leader after " << kClientConnectMaxAttempts <<
                 " attempts");
    // clang-format on
}

Result HaConfigStore::ConnectClient(const std::string &ip, uint16_t port, int reconnectRetryTimes) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    SM_LOG_TRACE("Target: " << ip << ":" << port);

    auto parser =
        SocketAddressParserMgr::getInstance().CreateParser(NetworkEndpointUtil::BuildEndpoint("tcp", ip, port));
    if (!parser) {
        SM_LOG_ERROR("Failed to create address parser for " << ip << ":" << port);
        return SM_ERROR;
    }

    // Check if already started
    if (clientDelegate_->SetServerInfo(ip, port)) {
        SM_LOG_INFO("Reconnecting to: " << ip << ":" << port);
        Result reconnectRet = clientDelegate_->ReConnectAfterBroken(reconnectRetryTimes);
        if (reconnectRet != SM_OK) {
            SM_LOG_ERROR("ReConnectAfterBroken failed, ret: " << reconnectRet);
        } else {
            SM_LOG_INFO("Reconnection initiated successfully");
        }
        return reconnectRet;
    }
    // First time connection
    SM_LOG_TRACE("First time connection to: " << ip << ":" << port);
    Result clientStartRet = clientDelegate_->ClientStart(tlsConfig_);
    if (clientStartRet != SM_OK) {
        SM_LOG_ERROR("ClientStart failed, ret: " << clientStartRet);
        return clientStartRet;
    }
    SM_LOG_INFO("ClientStart succeeded");
    clientDelegate_->SetConnectStatus(true);
    // Register broken link handler only once
    if (!brokenHandlerRegistered_.exchange(true, std::memory_order_acq_rel)) {
        SM_LOG_DEBUG("Registering broken link handler");
        clientDelegate_->RegisterClientBrokenHandler([this]() -> int {
            SM_LOG_INFO("Connection broken, triggering async re-election");
            TriggerReElectionAsync();
            return 0;
        });
    }
    return SM_OK;
}

Result HaConfigStore::BecomeFollower(const std::string &leaderIpPort) noexcept
{
    SM_LOG_INFO("Leader: " << leaderIpPort);
    std::string fullUrl = "tcp://" + leaderIpPort;
    std::string ip;
    uint16_t port = 0;
    if (!NetworkEndpointUtil::ExtractIpAndPort(fullUrl, ip, port)) {
        SM_LOG_ERROR("Invalid leader address format: " << leaderIpPort);
        // 无法跟随 leader 时必须确保本进程已降级，否则旧主会与新主并存。
        StopServer();
        NotifyLeaderChange();
        return SM_ERROR;
    }

    SM_LOG_TRACE("Connecting to leader, ip: " << ip << ", port: " << port);
    // Bound the reconnect attempts so a dead leader's lease-expiry window (PUT_LEASE_TTL_SEC)
    // is not blocked by a long retry loop: fail fast, return to the election loop, and let
    // the etcd lease expiry drive the next election attempt.
    constexpr int kFollowerConnectRetryTimes = 3;
    auto connectRet = ConnectClient(ip, port, kFollowerConnectRetryTimes);
    if (connectRet != SM_OK) {
        SM_LOG_ERROR("Connect to leader failed, ret: " << connectRet << ", leader: " << leaderIpPort
                                                       << ", demoting local server to avoid dual master");
        // 跟随失败也必须先停服并通知上层：即使连接新主失败，旧主也绝不能继续对外服务。
        StopServer();
        NotifyLeaderChange();
        return connectRet;
    }
    SM_LOG_INFO("Connection initiated to leader");
    {
        std::lock_guard<std::mutex> lock(lastLeaderMutex_);
        lastConnectedLeader_ = leaderIpPort;
    }
    NotifyLeaderChange();
    return SM_OK;
}

void HaConfigStore::CheckLeaderConsistency(const std::string &backendLeader) noexcept
{
    std::string lastLeader;
    {
        std::lock_guard<std::mutex> lock(lastLeaderMutex_);
        lastLeader = lastConnectedLeader_;
    }

    // 未连接|一致，均无需动作
    if (lastLeader.empty()) {
        // 尚未建立过连接（Startup 的同步连接循环仍在重试中）由连接循环自身负责，避免并发触发重复连接。
        SM_LOG_DEBUG("Leader not connected yet, skip consistency check");
        return;
    }
    if (backendLeader == lastLeader) {
        SM_LOG_DEBUG("Leader consistent with connection");
        return;
    }

    // 复核对比值
    std::string recheckLeader;
    int recheckRet;
    {
        std::lock_guard<std::mutex> bLock(backendMutex_);
        recheckRet = backend_->Get(KEY_LEADER, recheckLeader);
    }
    if (recheckRet != SUCCESS) {
        SM_LOG_WARN("Re-check KEY_LEADER failed, ret: " << recheckRet << ", defer consistency handling this round");
        return;
    }
    std::string freshLeader;
    {
        std::lock_guard<std::mutex> lock(lastLeaderMutex_);
        freshLeader = lastConnectedLeader_;
    }
    if (recheckLeader == freshLeader) {
        SM_LOG_DEBUG("Stale snapshot detected (leader=" << freshLeader << "), skip");
        return;
    }

    if (isLeader_.load(std::memory_order_acquire)) {
        // 本进程是 leader，但 KEY_LEADER 已不是自己登记的地址：无 key 的 leader 不得继续服务。
        // 窗口保护仅限“本进程正在登记新 leader”的瞬态：KEY_LEADER 为空（Put 尚未生效）或已
        // 指向自己（已生效但 lastConnectedLeader_ 尚未更新）。一旦 etcd 中出现其它 leader 地址，
        // 无论重选举是否在进行，都必须立即降级，否则旧主会与新主并存。
        const std::string selfAddr = BuildSelfLeaderAddr();
        if (reElectionInProgress_.load(std::memory_order_acquire) &&
            (recheckLeader.empty() || recheckLeader == selfAddr)) {
            SM_LOG_DEBUG("Leader registration in progress, skip self-demotion this round, backend=" << recheckLeader);
            return;
        }
        SM_LOG_WARN("Leader key no longer mine (lease expired or replaced): registered="
                    << freshLeader << ", backend=" << recheckLeader << ", self-demoting");
        StopServer();
        TriggerReElectionAsync();
        return;
    }

    // follower：假死场景下 leader 进程未退出、TCP 连接保持 ESTABLISHED，收不到断链
    // 事件，HandleLinkBroken 不会被触发。发现 KEY_LEADER 与当前连接的 leader 不一致即触发重选举
    SM_LOG_WARN("Leader changed: connected=" << freshLeader << ", backend=" << recheckLeader);
    TriggerReElectionAsync();
}

void HaConfigStore::HealthCheckThreadFunc() noexcept
{
    constexpr auto CHECK_INTERVAL = std::chrono::seconds(HEALTH_CHECK_INTERVAL_SEC);
    pthread_setname_np(pthread_self(), "ha_health_chk");
    SM_LOG_INFO("Health check thread started, interval: " << HEALTH_CHECK_INTERVAL_SEC << "s");
    while (!stopFlag_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(CHECK_INTERVAL);
        if (stopFlag_.load(std::memory_order_acquire)) {
            break;
        }
        SM_LOG_DEBUG("Performing backend connection check");

        std::string testValue;
        int backendRet;
        {
            std::lock_guard<std::mutex> bLock(backendMutex_);
            backendRet = backend_->Get(KEY_LEADER, testValue);
        }
        if (backendRet != SUCCESS) {
            SM_LOG_ERROR("Backend connection timeout, ret: " << backendRet << ", triggering re-election");
            StopServer();
            TriggerReElectionAsync();
        } else {
            CheckLeaderConsistency(testValue);
        }
    }
    SM_LOG_INFO("Health check thread exiting");
    healthCheckRunning_.store(false, std::memory_order_release);
}

void HaConfigStore::StartHealthCheckThread() noexcept
{
    if (healthCheckRunning_.exchange(true, std::memory_order_acq_rel)) {
        SM_LOG_WARN("Health check thread already running");
        return;
    }
    SM_LOG_INFO("Starting health check thread");
    healthCheckThread_ = std::thread(&HaConfigStore::HealthCheckThreadFunc, this);
}

// ============================================================================
// ConfigStore Interface Forwarding (moved from header)
// ============================================================================
Result HaConfigStore::PrefixGet(const std::string &key, std::unordered_map<std::string, std::string> &value) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->PrefixGet(key, value);
}

Result HaConfigStore::Set(const std::string &key, const std::vector<uint8_t> &value) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Set(key, value);
}

Result HaConfigStore::Add(const std::string &key, int64_t increment, int64_t &value) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Add(key, increment, value);
}

Result HaConfigStore::Remove(const std::string &key, bool printKeyNotExist) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Remove(key, printKeyNotExist);
}

Result HaConfigStore::Append(const std::string &key, const std::vector<uint8_t> &value, uint64_t &newSize) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Append(key, value, newSize);
}

Result HaConfigStore::Cas(const std::string &key, const std::vector<uint8_t> &expect, const std::vector<uint8_t> &value,
                          std::vector<uint8_t> &exists) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Cas(key, expect, value, exists);
}

Result
HaConfigStore::Watch(const std::string &key,
                     const std::function<void(int result, const std::string &, const std::vector<uint8_t> &)> &notify,
                     uint32_t &wid) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Watch(key, notify, wid);
}

Result HaConfigStore::Watch(WatchRankType type, const std::function<void(WatchRankType, uint32_t, Result)> &notify,
                            uint32_t &wid) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Watch(type, notify, wid);
}

Result HaConfigStore::Unwatch(uint32_t wid) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Unwatch(wid);
}

Result HaConfigStore::Write(const std::string &key, const std::vector<uint8_t> &value, uint32_t offset) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Write(key, value, offset);
}

Result HaConfigStore::QueryAlive(uint32_t rank, uint32_t &alive) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->QueryAlive(rank, alive);
}

void HaConfigStore::SetRankId(const int32_t &rankId) noexcept
{
    SM_ASSERT_RET_VOID(clientDelegate_ != nullptr);
    clientDelegate_->SetRankId(rankId);
}

std::string HaConfigStore::GetCompleteKey(const std::string &key) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, "");
    return clientDelegate_->GetCompleteKey(key);
}

std::string HaConfigStore::GetCommonPrefix() noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, "");
    return clientDelegate_->GetCommonPrefix();
}

StorePtr HaConfigStore::GetCoreStore() noexcept
{
    // Join/Leave 与异步接口依赖底层 TcpConfigStore（+=SmemGroupManager 能力），此处返回
    // 稳定的 clientDelegate_；它在 leader 切换时原地重连（SetServerInfo+ReConnectAfterBroken），
    // 缓存该指针的调用方能持续跟随新 leader。
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, nullptr);
    return Convert<TcpConfigStore, ConfigStore>(clientDelegate_);
}

HaConfigStore *HaConfigStore::AsHaConfigStore() noexcept
{
    return this;
}

void HaConfigStore::RegisterReconnectHandler(ConfigStoreReconnectHandler callback) noexcept
{
    SM_ASSERT_RET_VOID(clientDelegate_ != nullptr);
    clientDelegate_->RegisterReconnectHandler(callback);
}

Result HaConfigStore::ReConnectAfterBroken(int reconnectRetryTimes) noexcept
{
    // Note: This function triggers async re-election rather than synchronous reconnection.
    // The reconnectRetryTimes parameter is not used as re-election is async.
    (void)reconnectRetryTimes;
    SM_LOG_INFO("ReConnectAfterBroken: triggering async re-election");
    TriggerReElectionAsync();
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return SM_OK;
}

bool HaConfigStore::GetConnectStatus() noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, false);
    return clientDelegate_->GetConnectStatus();
}

void HaConfigStore::SetConnectStatus(bool status) noexcept
{
    SM_ASSERT_RET_VOID(clientDelegate_ != nullptr);
    clientDelegate_->SetConnectStatus(status);
}

void HaConfigStore::RegisterClientBrokenHandler(const ConfigStoreClientBrokenHandler &handler) noexcept
{
    SM_ASSERT_RET_VOID(clientDelegate_ != nullptr);
    clientDelegate_->RegisterClientBrokenHandler(handler);
}

void HaConfigStore::RegisterServerBrokenHandler(const ConfigStoreServerBrokenHandler &handler) noexcept
{
    std::unique_lock<std::mutex> lock(stateMutex_);
    cachedServerBrokenHandler_ = handler;
    lock.unlock();

    std::shared_lock<std::shared_mutex> rwLock(delegateRwLock_);
    if (serverDelegate_ == nullptr) {
        SM_LOG_DEBUG("ServerDelegate is null, handler cached for future leader promotion");
        return;
    }
    serverDelegate_->RegisterBrokenLinkCHandler(handler);
}

uint32_t HaConfigStore::GetRankIdByLinkId(uint32_t linkId) const noexcept
{
    return serverDelegate_ != nullptr ? serverDelegate_->GetRankIdByLinkId(linkId) : UINT32_MAX;
}

Result HaConfigStore::GetReal(const std::string &key, std::vector<uint8_t> &value, int64_t timeoutMs) noexcept
{
    SM_ASSERT_RETURN(clientDelegate_ != nullptr, SM_ERROR);
    return clientDelegate_->Get(key, value, timeoutMs);
}

} // namespace smem
} // namespace ock
