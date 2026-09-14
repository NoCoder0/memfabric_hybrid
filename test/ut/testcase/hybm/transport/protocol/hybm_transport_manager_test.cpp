/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */

#include <gtest/gtest.h>
#include <memory>

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

// ── Factory creation paths (TransportType overload) ──
TEST(HybmTransportManagerTest, Create_TagManagerNull_ReturnNull)
{
    auto mgr = TransportManager::Create(ock::mf::transport::TT_COMPOSE, nullptr);
    EXPECT_EQ(mgr, nullptr);
}

TEST(HybmTransportManagerTest, Create_InvalidType_ReturnNull)
{
    HybmEntityTagInfoPtr tagMgr = std::make_shared<HybmEntityTagInfo>();
    auto mgr = TransportManager::Create(static_cast<TransportType>(-1), tagMgr);
    EXPECT_EQ(mgr, nullptr);
}

TEST(HybmTransportManagerTest, Create_Compose_ReturnNotNull)
{
    HybmEntityTagInfoPtr tagMgr = std::make_shared<HybmEntityTagInfo>();
    auto mgr = TransportManager::Create(ock::mf::transport::TT_COMPOSE, tagMgr);
    EXPECT_NE(mgr, nullptr);
}

TEST(HybmTransportManagerTest, GetQpInfo_DefaultImpl_ReturnNull)
{
    HybmEntityTagInfoPtr tagMgr = std::make_shared<HybmEntityTagInfo>();
    auto mgr = TransportManager::Create(ock::mf::transport::TT_COMPOSE, tagMgr);
    ASSERT_NE(mgr, nullptr);
    EXPECT_EQ(mgr->GetQpInfo(), nullptr);
}

TEST(HybmTransportManagerTest, Create_SDMA_ReturnNotNull)
{
    HybmEntityTagInfoPtr tagMgr = std::make_shared<HybmEntityTagInfo>();
    auto mgr = TransportManager::Create(ock::mf::transport::TT_SDMA, tagMgr);
    EXPECT_NE(mgr, nullptr);
}

TEST(HybmTransportManagerTest, Create_HCCP_WithTag_ReturnNotNull)
{
    HybmEntityTagInfoPtr tagMgr = std::make_shared<HybmEntityTagInfo>();
    tagMgr->TagInfoInit(hybm_options{});
    auto mgr = TransportManager::Create(ock::mf::transport::TT_HCCP, tagMgr);
    EXPECT_NE(mgr, nullptr);
}

TEST(HybmTransportManagerTest, Create_HCOM_ReturnNotNull)
{
    HybmEntityTagInfoPtr tagMgr = std::make_shared<HybmEntityTagInfo>();
    auto mgr = TransportManager::Create(ock::mf::transport::TT_HCOM, tagMgr);
    EXPECT_NE(mgr, nullptr);
}

} // namespace
