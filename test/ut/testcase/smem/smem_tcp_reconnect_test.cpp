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
#include <gtest/gtest.h>
#include <atomic>
#include <future>

#define private public
#include "smem_tcp_config_store.h"
#undef private
#include "acc_tcp_server_default.h"
#include "acc_tcp_link_complex_default.h"
#include "smem_etcd_store_backend.h"

using namespace ock::smem;
using namespace ock::acc;

namespace {
class ReconnectServer : public AccTcpServerDefault {
public:
    std::atomic<int> calls{0};
    std::function<int(AccTcpLinkComplexPtr &)> connect;
    int ConnectToPeerServer(const std::string &, uint16_t, const AccConnReq &, uint32_t,
                            AccTcpLinkComplexPtr &link) override
    {
        ++calls;
        return connect(link);
    }
};

class RetryLink : public AccTcpLinkComplexDefault {
public:
    RetryLink() : AccTcpLinkComplexDefault(-1, "retry", 2, nullptr)
    {
        established_ = 1;
    }
    int sends{0};
    int NonBlockSend(int16_t, uint32_t, const AccDataBufferPtr &, const AccDataBufferPtr &) override
    {
        ++sends;
        return ACC_OK;
    }
};

TEST(TcpReconnectCasTest, DirectAndAutomaticReconnectShareGuardAndNotifyBeforeConnect)
{
    TcpConfigStore store(nullptr, "127.0.0.1", 19000, ConfigStoreModel::CSM_CLIENT, true, 2, 0);
    auto server = AccMakeRef<ReconnectServer>();
    store.accClient_ = server.Get();
    auto oldLink = AccMakeRef<AccTcpLinkComplexDefault>(-1, "broken", 1, nullptr);
    store.accClientLink_ = oldLink.Get();
    int notifications = 0;
    auto observer = std::make_shared<ConfigStoreClientBrokenHandler>([&] {
        ++notifications;
        return SM_OK;
    });
    store.RegisterClientBrokenObserver(observer);

    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    server->connect = [&](AccTcpLinkComplexPtr &) {
        if (server->calls.load() == 1) {
            EXPECT_EQ(notifications, 1);
            EXPECT_TRUE(store.reconnecting_.load());
            entered.set_value();
            released.wait();
        }
        return ACC_ERROR;
    };
    auto first = std::async(std::launch::async, [&] { return store.ReConnectAfterBroken(1); });
    auto status = entered.get_future().wait_for(std::chrono::seconds(5));
    if (status != std::future_status::ready) {
        release.set_value();
        FAIL() << "reconnect did not reach ConnectToPeerServer";
    }
    EXPECT_EQ(store.ReConnectAfterBroken(1), SM_NOT_CONNECTED);
    auto data = AccDataBuffer::Create(1U);
    EXPECT_EQ(store.LocalNonBlockSend(0, 0, data, nullptr), ACC_LINK_ERROR);
    store.TryReconnectByHeartbeat();
    EXPECT_EQ(server->calls.load(), 1);
    EXPECT_EQ(notifications, 1);
    EXPECT_TRUE(store.reconnecting_.load());
    release.set_value();
    EXPECT_EQ(first.get(), ACC_ERROR);
    EXPECT_FALSE(store.reconnecting_.load());
    store.NotifyClientBrokenObservers(oldLink.Get(), true);
    EXPECT_EQ(notifications, 1);
}

TEST(TcpReconnectCasTest, FailureAndSuccessReleaseGuardAndKeepObserverDeduplicated)
{
    TcpConfigStore store(nullptr, "127.0.0.1", 19000, ConfigStoreModel::CSM_CLIENT, true, 2, 0);
    auto server = AccMakeRef<ReconnectServer>();
    store.accClient_ = server.Get();
    auto oldLink = AccMakeRef<AccTcpLinkComplexDefault>(-1, "broken", 1, nullptr);
    store.accClientLink_ = oldLink.Get();
    int notifications = 0;
    auto observer = std::make_shared<ConfigStoreClientBrokenHandler>([&] {
        ++notifications;
        return SM_OK;
    });
    store.RegisterClientBrokenObserver(observer);
    auto link = AccMakeRef<RetryLink>();
    int reconnectCallbacks = 0;
    store.RegisterReconnectHandler([&] {
        EXPECT_TRUE(store.reconnecting_.load());
        EXPECT_EQ(store.accClientLink_.Get(), link.Get());
        ++reconnectCallbacks;
        return SM_OK;
    });
    server->connect = [&](AccTcpLinkComplexPtr &output) {
        EXPECT_TRUE(store.reconnecting_.load());
        EXPECT_EQ(notifications, 1);
        if (server->calls.load() == 1) {
            return ACC_ERROR;
        }
        output = link.Get();
        return ACC_OK;
    };
    EXPECT_EQ(store.ReConnectAfterBroken(1), ACC_ERROR);
    EXPECT_FALSE(store.reconnecting_.load());
    EXPECT_EQ(store.ReConnectAfterBroken(1), ACC_OK);
    EXPECT_FALSE(store.reconnecting_.load());
    EXPECT_TRUE(store.GetConnectStatus());
    EXPECT_EQ(store.ReConnectAfterBroken(1), ACC_OK);
    EXPECT_FALSE(store.reconnecting_.load());
    EXPECT_EQ(server->calls.load(), 3);
    EXPECT_EQ(reconnectCallbacks, 2);
    store.NotifyClientBrokenObservers(oldLink.Get(), true);
    EXPECT_EQ(notifications, 1);
}

TEST(TcpReconnectCasTest, SendFailureRetainsOriginalRetryForLocalAndDistributedBackends)
{
    for (bool distributed : {false, true}) {
        StoreBackendPtr backend = distributed ? SmMakeRef<SmemEtcdStoreBackend>().Get() : nullptr;
        TcpConfigStore store(backend, "127.0.0.1", 19000, ConfigStoreModel::CSM_CLIENT, true, 2, 0);
        auto server = AccMakeRef<ReconnectServer>();
        store.accClient_ = server.Get();
        store.accClientLink_ = AccMakeRef<AccTcpLinkComplexDefault>(-1, "broken", 1, nullptr).Get();
        auto link = AccMakeRef<RetryLink>();
        server->connect = [&](AccTcpLinkComplexPtr &output) {
            output = link.Get();
            return ACC_OK;
        };
        auto data = AccDataBuffer::Create(1U);
        EXPECT_EQ(store.LocalNonBlockSend(0, 0, data, nullptr), ACC_OK) << distributed;
        EXPECT_EQ(server->calls.load(), 1);
        EXPECT_EQ(link->sends, 1);
        EXPECT_FALSE(store.reconnecting_.load());
    }
}
} // namespace
