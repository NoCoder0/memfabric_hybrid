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
namespace {

/* Test constants — avoid magic numbers (CodeCheck G.CNS.02) */
constexpr uint16_t K_TEST_PORT = 29500;
constexpr uint32_t K_TEST_WORLD_SIZE = 4;
constexpr int32_t K_TEST_RECONNECT_RETRIES = 5;
constexpr int32_t K_TEST_RECONNECT_RETRIES_FEWER = 3;

/* Hand-written mock for ConfigStoreBackend — same pattern as test_ha_config_store.cpp */
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
    std::function<StoreErrorCode(const std::string &, const std::vector<uint8_t> &, int64_t)> onPut;
    std::function<StoreErrorCode(const std::string &)> onDelete;
    std::function<StoreErrorCode(const std::string &)> onAcquire;
    std::function<void()> onUninit;

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
        if (onPut) {
            return onPut(key, value, ttl);
        }
        return putResult;
    }

    StoreErrorCode Delete(const std::string &key) noexcept override
    {
        if (onDelete) {
            return onDelete(key);
        }
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
        if (onAcquire) {
            return onAcquire(name);
        }
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
        if (onAcquire) {
            return onAcquire(name);
        }
        return SUCCESS;
    }

    StoreErrorCode Initialize(const std::string &addr, const std::string &port, const std::string &rank) override
    {
        (void)addr;
        (void)port;
        (void)rank;
        return initResult;
    }

    void UnInitialize() noexcept override
    {
        if (onUninit) {
            onUninit();
        }
    }
};

} // anonymous namespace

/* ================================================================== */
/*  Test fixture                                                      */
/* ================================================================== */
class HaConfigStoreDetailTest : public ::testing::Test {
protected:
    StoreBackendPtr backend_;
    TcpConfigStorePtr client_;
    HaConfigStorePtr ha_;

    void SetUp() override
    {
        auto *rawBackend = new MockBackend();
        rawBackend->isDistributed = true;
        rawBackend->backendName = "HATestBackend";
        rawBackend->initResult = SUCCESS;
        backend_ = StoreBackendPtr(rawBackend);
        client_ = SmMakeRef<TcpConfigStore>(backend_, "127.0.0.1", K_TEST_PORT, false, true, K_TEST_WORLD_SIZE);
    }

    void TearDown() override
    {
        ha_ = nullptr;
    }
};

/* ================================================================== */
/*  Startup / Uninitialize                                             */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, Startup_NullClientReturnsNotInitialized)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    smem_tls_config tlsConfig{};
    EXPECT_EQ(ha_->Startup(tlsConfig), SM_NOT_INITIALIZED);
}

TEST_F(HaConfigStoreDetailTest, Uninitialize_WithNoServerNoCrash)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_ = nullptr;
    SUCCEED();
}

/* ================================================================== */
/*  RegisterServerBrokenHandler                                        */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, RegisterServerBrokenHandler_NoServerDelegate_CachesHandler)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    bool handlerCalled = false;
    ConfigStoreServerBrokenHandler handler = [&](uint32_t, StoreBackendPtr &) { handlerCalled = true; };
    ha_->RegisterServerBrokenHandler(handler);
    /* Handler is cached, not called yet (no server delegate) */
    EXPECT_FALSE(handlerCalled);
}

/* ================================================================== */
/*  ReConnectAfterBroken                                               */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, ReConnectAfterBroken_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    EXPECT_EQ(ha_->ReConnectAfterBroken(K_TEST_RECONNECT_RETRIES), SM_ERROR);
}

TEST_F(HaConfigStoreDetailTest, ReConnectAfterBroken_WithClient_TriggersReElection)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    Result ret = ha_->ReConnectAfterBroken(K_TEST_RECONNECT_RETRIES_FEWER);
    EXPECT_EQ(ret, SM_OK);
}

/* ================================================================== */
/*  SetConnectStatus / GetConnectStatus                                */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, SetGetConnectStatus_NullClient)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_->SetConnectStatus(true);
    EXPECT_FALSE(ha_->GetConnectStatus());
}

TEST_F(HaConfigStoreDetailTest, SetGetConnectStatus_WithClient)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_->SetConnectStatus(true);
    EXPECT_TRUE(ha_->GetConnectStatus());
    ha_->SetConnectStatus(false);
    EXPECT_FALSE(ha_->GetConnectStatus());
}

/* ================================================================== */
/*  GetReal                                                            */
/* ================================================================== */

/* ================================================================== */
/*  Watch overload                                                     */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, WatchRankType_NullClientReturnsError)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    uint32_t wid = 0;
    EXPECT_EQ(ha_->Watch(WATCH_RANK_LINK_DOWN, nullptr, wid), SM_ERROR);
}

/* ================================================================== */
/*  RegisterReconnectHandler                                           */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, RegisterReconnectHandler_NullClientNoCrash)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_->RegisterReconnectHandler(nullptr);
    SUCCEED();
}

/* ================================================================== */
/*  RegisterClientBrokenHandler                                        */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, RegisterClientBrokenHandler_NullClientNoCrash)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_->RegisterClientBrokenHandler(nullptr);
    SUCCEED();
}

/* ================================================================== */
/*  SetRankId                                                          */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, SetRankId_NullClientNoCrash)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, nullptr, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
    ha_->SetRankId(0);
    SUCCEED();
}

/* ================================================================== */
/*  Construction with instance ID                                      */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, ConstructorWithInstanceId)
{
    ha_ = SmMakeRef<HaConfigStore>(backend_, client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE, "my_instance");
    EXPECT_NE(ha_, nullptr);
}

/* ================================================================== */
/*  Shutdown ordering: destructor calls Uninitialize                   */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, DestructorCallsUninitialize)
{
    auto *rawBackend = new MockBackend();
    bool uninitCalled = false;
    rawBackend->onUninit = [&]() { uninitCalled = true; };
    StoreBackendPtr backendPtr(rawBackend);
    {
        auto haStore = SmMakeRef<HaConfigStore>(std::move(backendPtr), client_, "127.0.0.1:29500", K_TEST_WORLD_SIZE);
        (void)haStore;
    }
    EXPECT_TRUE(uninitCalled);
}

/* ================================================================== */
/*  TcpConfigStore server broken handler (null accServer path)         */
/* ================================================================== */

TEST_F(HaConfigStoreDetailTest, ClientRegisterServerBrokenHandler_NoServer_NoOp)
{
    bool handlerCalled = false;
    ConfigStoreServerBrokenHandler handler = [&](uint32_t, StoreBackendPtr &) { handlerCalled = true; };
    client_->RegisterServerBrokenHandler(handler);
    /* No server started (accServer_ is null), handler is silently dropped */
    EXPECT_FALSE(handlerCalled);
}

} // namespace smem
} // namespace ock
