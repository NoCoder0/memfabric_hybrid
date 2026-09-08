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

#include <gtest/gtest.h>

#include <functional>
#include <sstream>

#define private public // Test access to heartbeat timeout parsing.
#include "smem_tcp_config_store_server.h"
#undef private
#include "smem_local_memory_backend.h"

namespace ock {
namespace smem {
namespace {

constexpr uint32_t K_WORLD_SIZE = 4;
constexpr uint32_t K_STATE_RECOVERED = 2;
constexpr uint32_t K_STATE_NORMAL = 3;
constexpr uint32_t K_STATE_EXITED = 4;
constexpr uint32_t K_DEFAULT_HEARTBEAT_TIMEOUT_S = 60;
constexpr uint32_t K_HEARTBEAT_TIMEOUT_S = 15;
constexpr uint32_t K_MAX_HEARTBEAT_TIMEOUT_S = 3600;

/* Hand-written mock — no gmock dependency */
class MockBackend : public ConfigStoreBackend {
public:
    bool isDistributed = false;
    bool supportsTTL = false;
    std::string backendName = "MockBackend";

    /* Callbacks for Get — set to override default behavior */
    std::function<StoreErrorCode(const std::string &, std::vector<uint8_t> &)> onGet;

    /* Pre-programmed defaults */
    StoreErrorCode getResult = NOT_EXIST;
    StoreErrorCode putResult = SUCCESS;
    StoreErrorCode deleteResult = SUCCESS;
    StoreErrorCode initResult = SUCCESS;
    StoreErrorCode existResult = NOT_EXIST;

    std::string BackendName() const noexcept override
    {
        return backendName;
    }

    StoreErrorCode Get(const std::string &key, std::vector<uint8_t> &value) const noexcept override
    {
        if (onGet) {
            return onGet(key, value);
        }
        return getResult;
    }

    StoreErrorCode PrefixGet(const std::string &key, PrefixGetMap &value) const noexcept override
    {
        (void)key;
        (void)value;
        return getResult;
    }

    StoreErrorCode Put(const std::string &key, const std::vector<uint8_t> &value, int64_t ttl) noexcept override
    {
        (void)key;
        (void)value;
        (void)ttl;
        return putResult;
    }

    StoreErrorCode Delete(const std::string &key) noexcept override
    {
        (void)key;
        return deleteResult;
    }

    StoreErrorCode Exist(const std::string &key) const noexcept override
    {
        (void)key;
        return existResult;
    }

    void Clear() noexcept override {}

    bool IsDistributed() const noexcept override
    {
        return isDistributed;
    }

    bool SupportsTTL() const noexcept override
    {
        return supportsTTL;
    }

    StoreErrorCode AcquireDistributedLock(const std::string &name) noexcept override
    {
        (void)name;
        return SUCCESS;
    }

    StoreErrorCode ReleaseDistributedLock(const std::string &name) noexcept override
    {
        (void)name;
        return SUCCESS;
    }

    StoreErrorCode TryAcquireDistributedLock(const std::string &name, int64_t ttl) noexcept override
    {
        (void)name;
        (void)ttl;
        return SUCCESS;
    }

    StoreErrorCode Initialize(const std::string &addr, const std::string &port, const std::string &rank) override
    {
        (void)addr;
        (void)port;
        (void)rank;
        return initResult;
    }

    void UnInitialize() override {}
};

} // anonymous namespace

TEST(StoreServerStateTest, EnumValues)
{
    EXPECT_EQ(SS_INITED, 0);
    EXPECT_EQ(SS_RECOVERING, 1);
    EXPECT_EQ(SS_RECOVERED, K_STATE_RECOVERED);
    EXPECT_EQ(SS_NORMAL, K_STATE_NORMAL);
    EXPECT_EQ(SS_EXITED, K_STATE_EXITED);
}

class AccStoreServerTest : public ::testing::Test {
protected:
    StoreBackendPtr backend_;
    AccStoreServerPtr server_;

    void SetUp() override
    {
        auto *rawBackend = new MockBackend();
        rawBackend->isDistributed = false;
        rawBackend->supportsTTL = false;
        rawBackend->backendName = "MockBackend";
        rawBackend->initResult = SUCCESS;
        backend_ = StoreBackendPtr(rawBackend);
        server_ = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, backend_, true);
    }

    void TearDown() override
    {
        if (server_ != nullptr) {
            server_->Shutdown();
        }
    }

    StoreBackendPtr MakeDistributedBackend()
    {
        auto *rawBackend = new MockBackend();
        rawBackend->isDistributed = true;
        rawBackend->supportsTTL = false;
        rawBackend->backendName = "MockDistBackend";
        rawBackend->initResult = SUCCESS;
        rawBackend->getResult = NOT_EXIST;
        rawBackend->putResult = SUCCESS;
        rawBackend->deleteResult = SUCCESS;
        return StoreBackendPtr(rawBackend);
    }
};

TEST_F(AccStoreServerTest, HeartbeatTimeoutParsesSecondsAndZero)
{
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("0"), 0U);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("15"), K_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("3600"), K_MAX_HEARTBEAT_TIMEOUT_S);
}

TEST_F(AccStoreServerTest, HeartbeatTimeoutMissingOrInvalidUsesDefault)
{
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS(""), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("-1"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("abc"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("3601"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
}

TEST_F(AccStoreServerTest, RestoreFromBackend_NonDistributedReturnsOk)
{
    EXPECT_EQ(server_->RestoreFromBackend(), SM_OK);
}

TEST_F(AccStoreServerTest, GetStatus_NonDistributedReturnsTrue)
{
    EXPECT_TRUE(server_->GetStatus());
}

TEST_F(AccStoreServerTest, UpdateStatus_NonDistributedReturnsOk)
{
    EXPECT_EQ(server_->UpdateStatus(true), SM_OK);
    EXPECT_EQ(server_->UpdateStatus(false), SM_OK);
}

TEST_F(AccStoreServerTest, RestoreFromBackend_DistributedBackendWithRanks)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        if (key == KEY_ALIVE_RANK_LIST) {
            out = {'0', ',', '2'};
            return SUCCESS;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->RestoreFromBackend(), SM_OK);
    server->Shutdown();
}

TEST_F(AccStoreServerTest, RestoreFromBackend_DistributedBackendNoRanks)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        (void)out;
        if (key == KEY_ALIVE_RANK_LIST) {
            return NOT_EXIST;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->RestoreFromBackend(), SM_OK);
    server->Shutdown();
}

TEST_F(AccStoreServerTest, GetStatus_DistributedBackendLeaderActive)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        if (key == KEY_LEADER_STATUS) {
            out = {'t', 'r', 'u', 'e'};
            return SUCCESS;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_TRUE(server->GetStatus());
    server->Shutdown();
}

TEST_F(AccStoreServerTest, GetStatus_DistributedBackendLeaderInactive)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        if (key == KEY_LEADER_STATUS) {
            out = {'f', 'a', 'l', 's', 'e'};
            return SUCCESS;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_FALSE(server->GetStatus());
    server->Shutdown();
}

TEST_F(AccStoreServerTest, UpdateStatus_DistributedBackendSetActive)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->putResult = SUCCESS;

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->UpdateStatus(true), SM_OK);
    server->Shutdown();
}

TEST_F(AccStoreServerTest, UpdateStatus_DistributedBackendSetInactive)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->deleteResult = SUCCESS;

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->UpdateStatus(false), SM_OK);
    server->Shutdown();
}

TEST_F(AccStoreServerTest, RestoreFromBackend_DistributedBackendGetFails)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        (void)out;
        if (key == KEY_ALIVE_RANK_LIST) {
            return ERROR;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->RestoreFromBackend(), SM_OK);
    server->Shutdown();
}

} // namespace smem
} // namespace ock
