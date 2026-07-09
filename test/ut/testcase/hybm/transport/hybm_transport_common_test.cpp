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
#include <sstream>

#include "hybm_transport_common.h"

using namespace ock::mf::transport;

class HybmTransportCommonTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================
// ReadDeviceRdmaMemoryKey / WriteDeviceRdmaMemoryKey
// ============================================================
TEST_F(HybmTransportCommonTest, ReadDeviceRdmaMemoryKey_CopiesFirstKeySlots)
{
    TransportMemoryKey input{};
    TransportMemoryKey output{};
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        input.keys[i] = i + 100; // 100
    }
    ReadDeviceRdmaMemoryKey(input, output);
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        EXPECT_EQ(output.keys[i], input.keys[i]);
    }
    // Only first KEY_SIZE slots should be set, rest remain zero
    for (size_t i = KEY_SIZE; i < sizeof(output.keys) / sizeof(output.keys[0]); ++i) {
        EXPECT_EQ(output.keys[i], 0U);
    }
}

TEST_F(HybmTransportCommonTest, WriteDeviceRdmaMemoryKey_WritesFirstKeySlots)
{
    TransportMemoryKey input{};
    TransportMemoryKey output{};
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        input.keys[i] = i + 200; // 200
    }
    WriteDeviceRdmaMemoryKey(input, output);
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        EXPECT_EQ(output.keys[i], input.keys[i]);
    }
}

// ============================================================
// ReadHcomMemoryKey / WriteHcomMemoryKey
// ============================================================
TEST_F(HybmTransportCommonTest, ReadHcomMemoryKey_CopiesHostKeySlots)
{
    TransportMemoryKey input{};
    TransportMemoryKey output{};
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        input.keys[6 * KEY_SIZE + i] = i + 300; // 6 300
    }
    ReadHcomMemoryKey(input, output);
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        EXPECT_EQ(output.keys[i], input.keys[6 * KEY_SIZE + i]); // 6
    }
}

TEST_F(HybmTransportCommonTest, WriteHcomMemoryKey_WritesToHostKeySlots)
{
    TransportMemoryKey input{};
    TransportMemoryKey output{};
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        input.keys[i] = i + 400; // 400
    }
    WriteHcomMemoryKey(input, output);
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        EXPECT_EQ(output.keys[6 * KEY_SIZE + i], input.keys[i]); // 6
    }
}

// ============================================================
// ReadDeviceUrmaMemoryKey / WriteDeviceUrmaMemoryKey
// ============================================================
TEST_F(HybmTransportCommonTest, ReadDeviceUrmaMemoryKey_CopiesAll32KeySlots)
{
    TransportMemoryKey input{};
    TransportMemoryKey output{};
    for (size_t i = 0; i < KEY_SIZE * 4; ++i) { // 4
        input.keys[i] = i + 500;                // 500
    }
    ReadDeviceUrmaMemoryKey(input, output);
    for (size_t i = 0; i < KEY_SIZE * 4; ++i) { // 4
        EXPECT_EQ(output.keys[i], input.keys[i]);
    }
}

TEST_F(HybmTransportCommonTest, WriteDeviceUrmaMemoryKey_WritesAll32KeySlots)
{
    TransportMemoryKey input{};
    TransportMemoryKey output{};
    for (size_t i = 0; i < KEY_SIZE * 4; ++i) { // 4
        input.keys[i] = i + 600;                // 600
    }
    WriteDeviceUrmaMemoryKey(input, output);
    for (size_t i = 0; i < KEY_SIZE * 4; ++i) { // 4
        EXPECT_EQ(output.keys[i], input.keys[i]);
    }
}

// ============================================================
// TransportMemoryKey::operator<
// ============================================================
TEST_F(HybmTransportCommonTest, TransportMemoryKey_LessThan_ByMemcmp)
{
    TransportMemoryKey a{};
    TransportMemoryKey b{};
    a.keys[0] = 10; // 10
    b.keys[0] = 20; // 20
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
    // Equal keys
    TransportMemoryKey c{};
    TransportMemoryKey d{};
    c.keys[0] = 42; // 42
    d.keys[0] = 42; // 42
    EXPECT_FALSE(c < d);
    EXPECT_FALSE(d < c);
}

// ============================================================
// ostream operators
// ============================================================
TEST_F(HybmTransportCommonTest, TransportOptions_Ostream)
{
    TransportOptions opts{};
    opts.rankId = 1;
    opts.rankCount = 8; // 8
    opts.nic = "eth0";
    std::stringstream ss;
    ss << opts;
    EXPECT_NE(ss.str().find("rankId=1"), std::string::npos);
    EXPECT_NE(ss.str().find("count=8"), std::string::npos);
    EXPECT_NE(ss.str().find("nic=eth0"), std::string::npos);
}

TEST_F(HybmTransportCommonTest, TransportMemoryRegion_Ostream)
{
    TransportMemoryRegion mr{};
    mr.addr = 0x1234;
    mr.size = 0x1000;
    mr.access = 0x7;
    mr.flags = 0x1;
    std::stringstream ss;
    ss << mr;
    EXPECT_NE(ss.str().find("addr=0x1234"), std::string::npos);
    EXPECT_NE(ss.str().find("size=0x1000"), std::string::npos);
}

TEST_F(HybmTransportCommonTest, TransportMemoryKey_Ostream)
{
    TransportMemoryKey key{};
    key.keys[0] = 42; // 42
    key.keys[1] = 99; // 99
    std::stringstream ss;
    ss << key;
    std::string s = ss.str();
    EXPECT_NE(s.find("MemoryKey"), std::string::npos);
    EXPECT_NE(s.find("-42"), std::string::npos);
    EXPECT_NE(s.find("-99"), std::string::npos);
}

TEST_F(HybmTransportCommonTest, TransportRankPrepareInfo_Ostream)
{
    TransportMemoryKey memKey{};
    memKey.keys[0] = 777; // 777
    TransportRankPrepareInfo info("myNic", memKey);
    info.role = HYBM_ROLE_SENDER;
    std::stringstream ss;
    ss << info;
    EXPECT_NE(ss.str().find("myNic"), std::string::npos);
}

TEST_F(HybmTransportCommonTest, HybmTransPrepareOptions_Ostream)
{
    HybmTransPrepareOptions opts{};
    TransportMemoryKey memKey{};
    TransportRankPrepareInfo rankInfo("eth1", memKey);
    opts.options[0] = rankInfo;
    opts.options[1] = rankInfo;
    std::stringstream ss;
    ss << opts;
    EXPECT_NE(ss.str().find("PrepareOptions"), std::string::npos);
}

// ============================================================
// TransportRankPrepareInfo constructors
// ============================================================
TEST_F(HybmTransportCommonTest, TransportRankPrepareInfo_DefaultConstructor)
{
    TransportRankPrepareInfo info;
    EXPECT_EQ(info.role, HYBM_ROLE_PEER);
    EXPECT_TRUE(info.memKeys.empty());
}

TEST_F(HybmTransportCommonTest, TransportRankPrepareInfo_SingleKeyConstructor)
{
    TransportMemoryKey key{};
    key.keys[0] = 123; // 123
    TransportRankPrepareInfo info("eth2", key);
    EXPECT_EQ(info.nic, "eth2");
    ASSERT_EQ(info.memKeys.size(), 1U);
    EXPECT_EQ(info.memKeys[0].keys[0], 123U);
}

TEST_F(HybmTransportCommonTest, TransportRankPrepareInfo_MultiKeyConstructor)
{
    TransportMemoryKey k1{};
    TransportMemoryKey k2{};
    k1.keys[0] = 10; // 10
    k2.keys[0] = 20; // 20
    std::vector<TransportMemoryKey> keys = {k1, k2};
    TransportRankPrepareInfo info("eth3", keys);
    EXPECT_EQ(info.nic, "eth3");
    ASSERT_EQ(info.memKeys.size(), 2U);
    EXPECT_EQ(info.memKeys[0].keys[0], 10U);
    EXPECT_EQ(info.memKeys[1].keys[0], 20U);
}
