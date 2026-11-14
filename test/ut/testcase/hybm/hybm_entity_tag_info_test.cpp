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

#define private public

#include "hybm_entity_tag_info.h"

#undef private

namespace ock {
namespace mf {
class HybmEntityTagInfoTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        tagInfo_ = std::make_unique<HybmEntityTagInfo>();
        hybm_options opt{};
        opt.rankCount = 8; // 8
        EXPECT_EQ(tagInfo_->TagInfoInit(opt), BM_OK);
    }

    void TearDown() override
    {
        tagInfo_.reset();
    }

    std::unique_ptr<HybmEntityTagInfo> tagInfo_;
};

// ========================
// Test Case 1: TagInfoInit
// ========================
TEST_F(HybmEntityTagInfoTest, TagInfoInit)
{
    hybm_options opt;
    std::string tagStr = "test";
    memset(opt.tag, 0, sizeof(opt.tag));
    memcpy(opt.tag, tagStr.c_str(), tagStr.size());
    std::string tagOpStr = "DEVICE_SDMA";
    memset(opt.tagOpInfo, 0, sizeof(opt.tagOpInfo));
    memcpy(opt.tagOpInfo, tagOpStr.c_str(), tagOpStr.size());
    opt.rankCount = 4; // 4
    EXPECT_EQ(tagInfo_->TagInfoInit(opt), BM_OK);
}

// ========================
// Test Case 24: GetRank2RankOpType - Invalid rank
// ========================
TEST_F(HybmEntityTagInfoTest, GetRank2RankOpType_InvalidRank)
{
    auto opType = tagInfo_->GetRank2RankOpType(999, 1000);
    EXPECT_EQ(opType, HYBM_DOP_TYPE_DEFAULT);
}

// ========================
// Test Case 30: GetOpTypeStr - All known op types
// ========================
TEST_F(HybmEntityTagInfoTest, GetOpTypeStr_AllKnownOpTypes)
{
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_SDMA), "DEVICE_SDMA");
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_DEVICE_RDMA), "DEVICE_RDMA");
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_DEVICE_URMA), "DEVICE_URMA");
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_HOST_RDMA), "HOST_RDMA");
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_HOST_TCP), "HOST_TCP");
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_HOST_URMA), "HOST_URMA");
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_HOST_SHM), "HOST_SHM");
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_MTE), "DEVICE_MTE");
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(HYBM_DOP_TYPE_AIV_SDMA), "AIV_SDMA");
}

// ========================
// Test Case 31: GetOpTypeStr - Unknown op type returns "OP_TYPE_BUTT"
// ========================
TEST_F(HybmEntityTagInfoTest, GetOpTypeStr_UnknownOpType)
{
    EXPECT_EQ(HybmEntityTagInfo::GetOpTypeStr(static_cast<hybm_data_op_type>(0xFF)), "OP_TYPE_BUTT");
}

TEST_F(HybmEntityTagInfoTest, TagInfoInit_EmptyTag_DefaultsToDefault)
{
    hybm_options opt{};
    opt.rankCount = 4; // 4
    auto info = std::make_unique<HybmEntityTagInfo>();
    EXPECT_EQ(info->TagInfoInit(opt), BM_OK);
}

TEST_F(HybmEntityTagInfoTest, TagInfoInit_EmptySegment_ParsesAsEmptyLayer)
{
    hybm_options opt{};
    opt.rankCount = 4; // 4
    opt.rankId = 0;    // 0
    std::string protocol = ":HOST_SHM";
    memcpy(opt.tagOpInfo, protocol.c_str(), protocol.size() + 1);
    auto info = std::make_unique<HybmEntityTagInfo>();
    EXPECT_EQ(info->TagInfoInit(opt), BM_OK);
    const auto layers = info->GetLayersForRank(0);
    ASSERT_EQ(layers.size(), 2U);
    EXPECT_EQ(layers.front(), HYBM_DOP_TYPE_DEFAULT);
    EXPECT_EQ(layers.back(), HYBM_DOP_TYPE_HOST_SHM);
}

TEST_F(HybmEntityTagInfoTest, TagInfoInit_AllEmptySegments_ReturnsInvalid)
{
    hybm_options opt{};
    opt.rankCount = 4; // 4
    std::string protocol = ":";
    memcpy(opt.tagOpInfo, protocol.c_str(), protocol.size() + 1);
    auto info = std::make_unique<HybmEntityTagInfo>();
    EXPECT_EQ(info->TagInfoInit(opt), BM_INVALID_PARAM);
}

TEST_F(HybmEntityTagInfoTest, TagInfoInit_TrailingEmptySegment_ParsesAsEmptyLayer)
{
    hybm_options opt{};
    opt.rankCount = 4;
    opt.rankId = 0;
    std::string protocol = "HOST_SHM:DEVICE_RDMA:";
    memcpy(opt.tagOpInfo, protocol.c_str(), protocol.size() + 1);
    auto info = std::make_unique<HybmEntityTagInfo>();
    EXPECT_EQ(info->TagInfoInit(opt), BM_OK);
    const auto layers = info->GetLayersForRank(0);
    ASSERT_EQ(layers.size(), 3U);
    EXPECT_EQ(layers.front(), HYBM_DOP_TYPE_HOST_SHM);
    EXPECT_EQ(layers[1], HYBM_DOP_TYPE_DEVICE_RDMA);
    EXPECT_EQ(layers.back(), HYBM_DOP_TYPE_DEFAULT);
}

TEST_F(HybmEntityTagInfoTest, GetRank2RankLayers_SkipsEmptyLayer)
{
    hybm_options opt{};
    opt.rankCount = 4; // 4
    opt.rankId = 0;    // 0
    std::string protocol = ":HOST_SHM";
    memcpy(opt.tagOpInfo, protocol.c_str(), protocol.size() + 1);
    auto info = std::make_unique<HybmEntityTagInfo>();
    ASSERT_EQ(info->TagInfoInit(opt), BM_OK);

    EXPECT_EQ(info->AddRankLayers(1, {HYBM_DOP_TYPE_DEFAULT, HYBM_DOP_TYPE_HOST_SHM}), BM_OK);

    const auto layers = info->GetRank2RankLayers(0, 1);
    ASSERT_EQ(layers.size(), 1U);
    EXPECT_EQ(layers.front(), HYBM_DOP_TYPE_HOST_SHM);
}

TEST_F(HybmEntityTagInfoTest, AddRankLayers_AcceptsDifferentEmptyPosition)
{
    hybm_options opt{};
    opt.rankCount = 4; // 4
    opt.rankId = 0;    // 0
    std::string protocol = ":HOST_SHM";
    memcpy(opt.tagOpInfo, protocol.c_str(), protocol.size() + 1);
    auto info = std::make_unique<HybmEntityTagInfo>();
    ASSERT_EQ(info->TagInfoInit(opt), BM_OK);

    EXPECT_EQ(info->AddRankLayers(1, {HYBM_DOP_TYPE_HOST_RDMA, HYBM_DOP_TYPE_HOST_SHM}), BM_OK);
}

TEST_F(HybmEntityTagInfoTest, GetRank2RankLayers_MatchesCommonLayerWhenFirstLayerDiffers)
{
    hybm_options opt{};
    opt.rankCount = 4; // 4
    opt.rankId = 0;    // 0
    std::string protocol = "DEVICE_RDMA:HOST_RDMA:HOST_SHM";
    memcpy(opt.tagOpInfo, protocol.c_str(), protocol.size() + 1);
    auto info = std::make_unique<HybmEntityTagInfo>();
    ASSERT_EQ(info->TagInfoInit(opt), BM_OK);

    EXPECT_EQ(info->AddRankLayers(1, {HYBM_DOP_TYPE_DEVICE_URMA, HYBM_DOP_TYPE_HOST_RDMA, HYBM_DOP_TYPE_HOST_SHM}),
              BM_OK);

    const auto layers = info->GetRank2RankLayers(0, 1);
    ASSERT_EQ(layers.size(), 2U);
    EXPECT_EQ(layers.front(), HYBM_DOP_TYPE_HOST_RDMA);
    EXPECT_EQ(layers.back(), HYBM_DOP_TYPE_HOST_SHM);
}

TEST_F(HybmEntityTagInfoTest, TagInfoInit_LegacyFormat_Rejected)
{
    hybm_options opt{};
    opt.rankCount = 4; // 4
    std::string legacyFormat = "tag0:HOST_RDMA:tag1";
    auto info = std::make_unique<HybmEntityTagInfo>();
    memcpy(opt.tagOpInfo, legacyFormat.c_str(), legacyFormat.size() + 1);
    EXPECT_EQ(info->TagInfoInit(opt), BM_INVALID_PARAM);
}

TEST_F(HybmEntityTagInfoTest, GetRank2RankOpType_LegacyFallback)
{
    hybm_options opt{};
    opt.rankCount = 4; // 4
    opt.bmDataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_DEVICE_RDMA);
    auto info = std::make_unique<HybmEntityTagInfo>();
    ASSERT_EQ(info->TagInfoInit(opt), BM_OK);

    EXPECT_EQ(info->GetRank2RankOpType(0, 1), static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_DEVICE_RDMA));
    EXPECT_EQ(info->AddRankLayers(0, {HYBM_DOP_TYPE_HOST_SHM}), BM_OK);
}

TEST_F(HybmEntityTagInfoTest, AddRankLayers_DifferentLayerCount_Rejected)
{
    ASSERT_EQ(tagInfo_->AddRankLayers(0, {HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_SHM}), BM_OK);
    EXPECT_EQ(tagInfo_->AddRankLayers(1, {HYBM_DOP_TYPE_DEVICE_RDMA}), BM_INVALID_PARAM);
}

TEST_F(HybmEntityTagInfoTest, GetRank2RankLayers_ThreeLayers_PreservesConfiguredOrder)
{
    const std::vector<hybm_data_op_type> layers = {HYBM_DOP_TYPE_HOST_RDMA, HYBM_DOP_TYPE_DEFAULT,
                                                   HYBM_DOP_TYPE_DEVICE_RDMA};
    ASSERT_EQ(tagInfo_->AddRankLayers(0, layers), BM_OK);
    ASSERT_EQ(tagInfo_->AddRankLayers(1, layers), BM_OK);

    const auto matched = tagInfo_->GetRank2RankLayers(0, 1);
    ASSERT_EQ(matched.size(), 2U);
    EXPECT_EQ(matched[0], HYBM_DOP_TYPE_HOST_RDMA);
    EXPECT_EQ(matched[1], HYBM_DOP_TYPE_DEVICE_RDMA);
}

TEST_F(HybmEntityTagInfoTest, ResolvePeerProtocols_SelfRankKeepsLegacyDeviceTransport)
{
    for (auto protocol : {HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_DEVICE_URMA}) {
        HybmEntityTagInfo info;
        hybm_options options{};
        options.bmDataOpType = protocol;
        ASSERT_EQ(info.TagInfoInit(options), BM_OK);
        info.SetRankTopology(0, 0, 1, 4);

        EXPECT_EQ(info.ResolvePeerProtocols({0, 0}), std::vector<hybm_data_op_type>{protocol});
    }
}

TEST_F(HybmEntityTagInfoTest, ResolvePeerProtocols_SelfRankKeepsConfiguredDeviceTransport)
{
    for (auto protocol : {HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_DEVICE_URMA}) {
        HybmEntityTagInfo info;
        ASSERT_EQ(info.AddRankLayers(0, {protocol}), BM_OK);
        info.SetRankTopology(0, 0, 1, 4);

        EXPECT_EQ(info.ResolvePeerProtocols({0, 0}), std::vector<hybm_data_op_type>{protocol});
    }
}

TEST_F(HybmEntityTagInfoTest, ResolvePeerProtocols_UnlayeredCrossNodeKeepsSdma)
{
    /* A3 独立部署不配置 tagOpInfo：两侧都没有分层信息，与历史语义一致，不能按同机/远端白名单裁剪，
     * 否则 SDMA-only 的跨节点对端选不出任何算子。 */
    HybmEntityTagInfo info;
    hybm_options options{};
    options.bmDataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA);
    ASSERT_EQ(info.TagInfoInit(options), BM_OK);
    info.SetRankTopology(0, 1, 2, 0);
    info.SetRankTopology(1, 1, 3, 0);

    EXPECT_EQ(info.ResolvePeerProtocols({0, 1}), std::vector<hybm_data_op_type>{HYBM_DOP_TYPE_SDMA});
}

TEST_F(HybmEntityTagInfoTest, ResolvePeerProtocols_UnlayeredSameNodeKeepsLegacyOps)
{
    HybmEntityTagInfo info;
    hybm_options options{};
    options.bmDataOpType =
        static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_TCP);
    ASSERT_EQ(info.TagInfoInit(options), BM_OK);
    info.SetRankTopology(0, 1, 2, 0);
    info.SetRankTopology(1, 1, 2, 1);

    EXPECT_EQ(info.ResolvePeerProtocols({0, 1}),
              (std::vector<hybm_data_op_type>{HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_TCP}));
}

TEST_F(HybmEntityTagInfoTest, ResolvePeerProtocols_UnlayeredKeepsLegacyOpsAcrossSuperPod)
{
    HybmEntityTagInfo info;
    hybm_options options{};
    options.bmDataOpType =
        static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_TCP);
    ASSERT_EQ(info.TagInfoInit(options), BM_OK);
    info.SetRankTopology(0, 1, 2, 0);
    info.SetRankTopology(1, 9, 3, 0);

    EXPECT_EQ(info.ResolvePeerProtocols({0, 1}),
              (std::vector<hybm_data_op_type>{HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_TCP}));
}

TEST_F(HybmEntityTagInfoTest, ResolvePeerProtocols_LayeredSdmaKeepsSameSuperPodCrossServer)
{
    /* 配置了分层协议且两侧都是 SDMA：跨 server 但同 superPod 必须保留（SDMA 引擎可达），
     * 跨 superPod 才剔除。 */
    HybmEntityTagInfo info;
    hybm_options options{};
    options.bmDataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA);
    ASSERT_EQ(info.TagInfoInit(options), BM_OK);
    ASSERT_EQ(info.AddRankLayers(0, {HYBM_DOP_TYPE_SDMA}), BM_OK);
    ASSERT_EQ(info.AddRankLayers(1, {HYBM_DOP_TYPE_SDMA}), BM_OK);
    info.SetRankTopology(0, 7, 2, 0);
    info.SetRankTopology(1, 7, 3, 1);

    EXPECT_EQ(info.ResolvePeerProtocols({0, 1}), std::vector<hybm_data_op_type>{HYBM_DOP_TYPE_SDMA});

    info.SetRankTopology(1, 9, 3, 1);
    EXPECT_TRUE(info.ResolvePeerProtocols({0, 1}).empty());
}

} // namespace mf
} // namespace ock
