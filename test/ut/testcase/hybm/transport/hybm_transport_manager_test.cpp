/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include <gtest/gtest.h>

#include "hybm_transport_manager.h"
#include "hybm_define.h"

using namespace ock::mf;
using namespace ock::mf::transport;

namespace {

class TestTransportManager : public TransportManager {
public:
    Result OpenDevice(const TransportOptions &) override
    {
        return BM_OK;
    }
    Result CloseDevice() override
    {
        return BM_OK;
    }
    Result RegisterMemoryRegion(const TransportMemoryRegion &) override
    {
        return BM_OK;
    }
    Result UnregisterMemoryRegion(uint64_t) override
    {
        return BM_OK;
    }
    bool QueryHasRegistered(uint64_t, uint64_t) override
    {
        return false;
    }
    Result QueryMemoryKey(uint64_t, TransportMemoryKey &) override
    {
        return BM_OK;
    }
    void UpdateMemoryKey(TransportMemoryKey &, void *) override {}
    Result Prepare(const HybmTransPrepareOptions &) override
    {
        return prepareResult;
    }
    Result RemoveRanks(const std::vector<uint32_t> &) override
    {
        return BM_OK;
    }
    Result Connect() override
    {
        return connectResult;
    }
    Result AsyncConnect() override
    {
        return BM_OK;
    }
    Result WaitForConnected(int64_t) override
    {
        return BM_OK;
    }
    Result UpdateRankOptions(const HybmTransPrepareOptions &) override
    {
        return updateRankResult;
    }
    const std::string &GetNic() const override
    {
        return nic_;
    }
    const TransportPrivateData GetPrivateData() const override
    {
        return {};
    }
    const void *GetQpInfo() const override
    {
        return qpInfo_;
    }
    Result ReadRemote(uint32_t, uint64_t, uint64_t, uint64_t) override
    {
        return BM_OK;
    }
    Result WriteRemote(uint32_t, uint64_t, uint64_t, uint64_t) override
    {
        return BM_OK;
    }
    Result ReadRemoteAsync(uint32_t, uint64_t, uint64_t, uint64_t) override
    {
        return BM_OK;
    }
    Result WriteRemoteAsync(uint32_t, uint64_t, uint64_t, uint64_t) override
    {
        return BM_OK;
    }
    Result ReadRemoteBatchAsync(uint32_t, const CopyDescriptor &) override
    {
        return BM_OK;
    }
    Result WriteRemoteBatchAsync(uint32_t, const CopyDescriptor &) override
    {
        return BM_OK;
    }
    Result Synchronize(uint32_t) override
    {
        return BM_OK;
    }

    Result prepareResult{BM_OK};
    Result connectResult{BM_OK};
    Result updateRankResult{BM_OK};
    const void *qpInfo_{nullptr};
    std::string nic_{"test_nic"};
};

TEST(HybmTransportManagerTest, GetSdmaWorkSpaceAddr_Default)
{
    TestTransportManager mgr;
    EXPECT_EQ(mgr.GetSdmaWorkSpaceAddr(), 0UL);
}

TEST(HybmTransportManagerTest, GetQpInfo_Null)
{
    TestTransportManager mgr;
    EXPECT_EQ(mgr.GetQpInfo(), nullptr);
}

TEST(HybmTransportManagerTest, ConnectWithOptions_NotConnected)
{
    TestTransportManager mgr;
    HybmTransPrepareOptions opts{};
    EXPECT_EQ(mgr.ConnectWithOptions(opts), BM_OK);
    // Second call → already connected → UpdateRankOptions
    EXPECT_EQ(mgr.ConnectWithOptions(opts), BM_OK);
}

TEST(HybmTransportManagerTest, ConnectWithOptions_PrepareFails)
{
    TestTransportManager mgr;
    mgr.prepareResult = BM_ERROR;
    HybmTransPrepareOptions opts{};
    EXPECT_NE(mgr.ConnectWithOptions(opts), BM_OK);
}

TEST(HybmTransportManagerTest, ConnectWithOptions_ConnectFails)
{
    TestTransportManager mgr;
    mgr.connectResult = BM_ERROR;
    HybmTransPrepareOptions opts{};
    EXPECT_NE(mgr.ConnectWithOptions(opts), BM_OK);
}

TEST(HybmTransportManagerTest, Remove_NotSupported)
{
    TestTransportManager mgr;
    EXPECT_NE(mgr.Remove({1, 2}), BM_OK);
}

TEST(HybmTransportManagerTest, Create_IndirectEnv)
{
    setenv("MF_TRANSPORT_MANAGER", "INDIRECT", 1);
    auto mgr = TransportManager::Create(HybmGvaVersion::HYBM_GVA_V4);
    EXPECT_NE(mgr, nullptr);
    unsetenv("MF_TRANSPORT_MANAGER");
}

TEST(HybmTransportManagerTest, Create_V4Version)
{
    auto mgr = TransportManager::Create(HybmGvaVersion::HYBM_GVA_V4);
    EXPECT_NE(mgr, nullptr);
}

} // namespace
