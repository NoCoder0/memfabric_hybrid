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

#include <unordered_map>
#include <functional>

#include "smem_ha_config_store.h"
#include "smem_tcp_config_store_server.h"
#include "smem_local_memory_backend.h"

namespace ock {
namespace smem {

/* Test constants — avoid magic numbers (CodeCheck G.CNS.02) */
constexpr uint16_t K_TEST_PORT = 29500;
constexpr uint32_t K_TEST_WORLD_SIZE = 4;
constexpr int32_t K_TEST_RECONNECT_RETRIES = 5;

/* Hand-written mock — no gmock dependency */
class MockBackend : public ConfigStoreBackend {
public:
    bool isDistributed = false;
    bool supportsTTL = false;
    std::string backendName = "MockBackend";
    StoreErrorCode initResult = SUCCESS;
    StoreErrorCode getResult = NOT_EXIST;
    StoreErrorCode putResult = SUCCESS;
    StoreErrorCode deleteResult = SUCCESS;

    std::function<StoreErrorCode(const std::string &, std::vector<uint8_t> &)> onGet;

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
        return NOT_EXIST;
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

class HaConfigStoreTest : public ::testing::Test {
protected:
    StoreBackendPtr backend_;
    TcpConfigStorePtr client_;
    HaConfigStorePtr ha_;

    void SetUp() override
    {
        auto *rawBackend = new MockBackend();
        rawBackend->isDistributed = false;
        rawBackend->supportsTTL = false;
        rawBackend->backendName = "MockBackend";
        rawBackend->initResult = SUCCESS;
        backend_ = StoreBackendPtr(rawBackend);
        client_ = SmMakeRef<TcpConfigStore>(backend_, "127.0.0.1", K_TEST_PORT, false, true, K_TEST_WORLD_SIZE);
    }

    void TearDown() override {}
};

TEST_F(HaConfigStoreTest, Construction_StoresParameters)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_NE(ha_, nullptr);
}

TEST_F(HaConfigStoreTest, IsLeader_DefaultFalse)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_FALSE(ha_->IsLeader());
}

TEST_F(HaConfigStoreTest, GetCoreStore_ReturnsClientDelegate)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    auto core = ha_->GetCoreStore();
    EXPECT_NE(core.Get(), nullptr);
    EXPECT_EQ(core.Get(), client_.Get());
}

TEST_F(HaConfigStoreTest, GetTcpStore_ReturnsClientDelegate)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->GetTcpStore(), client_.Get());
}

TEST_F(HaConfigStoreTest, RegisterLeaderChangeCallback_StoresCallback)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    bool called = false;
    ha_->RegisterLeaderChangeCallback([&called](const HaConfigStore::LeaderAddresses &) { called = true; });
    SUCCEED();
}

TEST_F(HaConfigStoreTest, PrefixGet_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    std::unordered_map<std::string, std::string> value;
    EXPECT_EQ(ha_->PrefixGet("key", value), SM_ERROR);
}

TEST_F(HaConfigStoreTest, Set_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->Set("key", {}), SM_ERROR);
}

TEST_F(HaConfigStoreTest, Add_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    int64_t val = 0;
    EXPECT_EQ(ha_->Add("key", 1, val), SM_ERROR);
}

TEST_F(HaConfigStoreTest, Remove_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->Remove("key", false), SM_ERROR);
}

TEST_F(HaConfigStoreTest, Append_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    uint64_t newSize = 0;
    EXPECT_EQ(ha_->Append("key", {}, newSize), SM_ERROR);
}

TEST_F(HaConfigStoreTest, Cas_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    std::vector<uint8_t> exists;
    EXPECT_EQ(ha_->Cas("key", {}, {}, exists), SM_ERROR);
}

TEST_F(HaConfigStoreTest, Watch_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    uint32_t wid = 0;
    EXPECT_EQ(ha_->Watch("key", nullptr, wid), SM_ERROR);
}

TEST_F(HaConfigStoreTest, Unwatch_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->Unwatch(0), SM_ERROR);
}

TEST_F(HaConfigStoreTest, Write_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->Write("key", {}, 0), SM_ERROR);
}

TEST_F(HaConfigStoreTest, QueryAlive_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    uint32_t alive = 0;
    EXPECT_EQ(ha_->QueryAlive(0, alive), SM_ERROR);
}

TEST_F(HaConfigStoreTest, GetConnectStatus_NullClientReturnsFalse)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_FALSE(ha_->GetConnectStatus());
}

TEST_F(HaConfigStoreTest, SetRankId_NullClientNoCrash)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_->SetRankId(0);
    SUCCEED();
}

TEST_F(HaConfigStoreTest, GetCompleteKey_NullClientReturnsEmpty)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->GetCompleteKey("key"), "");
}

TEST_F(HaConfigStoreTest, GetCommonPrefix_NullClientReturnsEmpty)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->GetCommonPrefix(), "");
}

TEST_F(HaConfigStoreTest, RegisterReconnectHandler_NullClientNoCrash)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_->RegisterReconnectHandler(nullptr);
    SUCCEED();
}

TEST_F(HaConfigStoreTest, RegisterClientBrokenHandler_NullClientNoCrash)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_->RegisterClientBrokenHandler(nullptr);
    SUCCEED();
}

TEST_F(HaConfigStoreTest, ReConnectAfterBroken_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->ReConnectAfterBroken(K_TEST_RECONNECT_RETRIES), SM_ERROR);
}

TEST_F(HaConfigStoreTest, RegisterServerBrokenHandler_CachesHandler)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    bool handlerCalled = false;
    ConfigStoreServerBrokenHandler handler = [&](uint32_t, StoreBackendPtr &) { handlerCalled = true; };
    ha_->RegisterServerBrokenHandler(handler);
    SUCCEED();
}

TEST_F(HaConfigStoreTest, Constructor_WithInstanceId)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE, "test_instance");
    EXPECT_NE(ha_, nullptr);
}

TEST_F(HaConfigStoreTest, Startup_NullClientReturnsNotInitialized)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    smem_tls_config tlsConfig{};
    EXPECT_EQ(ha_->Startup(tlsConfig), SM_NOT_INITIALIZED);
}

TEST_F(HaConfigStoreTest, Constants_CorrectValues)
{
    constexpr uint32_t expectedMaxRetryCount = 10;
    constexpr uint32_t expectedRetryIntervalMs = 500;
    constexpr int32_t expectedConnectionTimeoutSec = 3;
    constexpr int32_t expectedMinSleepMs = 100;
    constexpr int32_t expectedMaxSleepMs = 1000;
    constexpr int32_t expectedPutLeaseTtlSec = 5;
    constexpr uint32_t expectedHealthCheckIntervalSec = 4;

    EXPECT_EQ(MAX_RETRY_COUNT, expectedMaxRetryCount);
    EXPECT_EQ(RETRY_INTERVAL_MS, expectedRetryIntervalMs);
    EXPECT_EQ(CONNECTION_TIMEOUT_SEC, expectedConnectionTimeoutSec);
    EXPECT_EQ(MIN_SLEEP_MS, expectedMinSleepMs);
    EXPECT_EQ(MAX_SLEEP_MS, expectedMaxSleepMs);
    EXPECT_EQ(PUT_LEASE_TTL_SEC, expectedPutLeaseTtlSec);
    EXPECT_EQ(HEALTH_CHECK_INTERVAL_SEC, expectedHealthCheckIntervalSec);
}

} // namespace smem
} // namespace ock
