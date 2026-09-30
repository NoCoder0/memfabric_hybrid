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

namespace {
struct TestPayload {
    uint64_t field0;
    uint32_t field1;
    uint32_t field2;
};
} // namespace

// ============================================================
// WriteStructToKey / ReadStructFromKey
// ============================================================
TEST_F(HybmTransportCommonTest, WriteStructToKey_RoundTrip)
{
    TestPayload payload{0x1122334455667788UL, 0xAABBCCDDUL, 0x01020304UL};
    TransportMemoryKey key{};
    WriteStructToKey(payload, key);
    EXPECT_EQ(key.ByteSize(), sizeof(TestPayload));

    TestPayload parsed{};
    ASSERT_TRUE(ReadStructFromKey(key, parsed));
    EXPECT_EQ(parsed.field0, payload.field0);
    EXPECT_EQ(parsed.field1, payload.field1);
    EXPECT_EQ(parsed.field2, payload.field2);
}

TEST_F(HybmTransportCommonTest, ReadStructFromKey_TooSmallFails)
{
    TransportMemoryKey key{};
    key.keys.assign(1, 0);
    TestPayload parsed{};
    EXPECT_FALSE(ReadStructFromKey(key, parsed));
}

// ============================================================
// WriteDeviceMemoryKey / WriteHostMemoryKey / ReadDeviceMemoryKey / ReadHostMemoryKey
// ============================================================
TEST_F(HybmTransportCommonTest, Envelope_RoundTripBothSides)
{
    TransportMemoryKey deviceKey{};
    deviceKey.keys.assign({100, 101, 102});
    TransportMemoryKey hostKey{};
    hostKey.keys.assign({200, 201});

    TransportMemoryKey merged{};
    WriteDeviceMemoryKey(deviceKey, merged);
    WriteHostMemoryKey(hostKey, merged);
    // Envelope header + both payloads
    EXPECT_EQ(merged.WordCount(), TRANS_KEY_ENVELOPE_WORDS + 3U + 2U);

    TransportMemoryKey deviceOut{};
    TransportMemoryKey hostOut{};
    ReadDeviceMemoryKey(merged, deviceOut);
    ReadHostMemoryKey(merged, hostOut);
    EXPECT_EQ(deviceOut.keys, deviceKey.keys);
    EXPECT_EQ(hostOut.keys, hostKey.keys);
}

TEST_F(HybmTransportCommonTest, Envelope_EmptyDeviceKeepsHostPayload)
{
    TransportMemoryKey deviceKey{};
    TransportMemoryKey hostKey{};
    hostKey.keys.assign({300, 301, 302, 303});

    TransportMemoryKey merged{};
    WriteDeviceMemoryKey(deviceKey, merged);
    WriteHostMemoryKey(hostKey, merged);

    TransportMemoryKey deviceOut{};
    TransportMemoryKey hostOut{};
    ReadDeviceMemoryKey(merged, deviceOut);
    ReadHostMemoryKey(merged, hostOut);
    EXPECT_TRUE(deviceOut.keys.empty());
    EXPECT_EQ(hostOut.keys, hostKey.keys);
}

TEST_F(HybmTransportCommonTest, ReadDeviceMemoryKey_TooShortReturnsEmpty)
{
    TransportMemoryKey merged{};
    merged.keys.assign(1, 0); // fewer than envelope header words
    TransportMemoryKey deviceOut{};
    deviceOut.keys.assign(1, 0xABCDUL);
    ReadDeviceMemoryKey(merged, deviceOut);
    EXPECT_TRUE(deviceOut.keys.empty());
}

TEST_F(HybmTransportCommonTest, ReadHostMemoryKey_InconsistentLengthsReturnsEmpty)
{
    TransportMemoryKey merged{};
    // device word count says 3, but no device payload follows
    merged.keys.assign({3, 1, 0});
    TransportMemoryKey hostOut{};
    hostOut.keys.assign(1, 0xABCDUL);
    ReadHostMemoryKey(merged, hostOut);
    EXPECT_TRUE(hostOut.keys.empty());
}

// ============================================================
// TransportMemoryKey::operator</operator==
// ============================================================
TEST_F(HybmTransportCommonTest, TransportMemoryKey_LessThanAndEquality)
{
    TransportMemoryKey a{};
    TransportMemoryKey b{};
    a.keys.assign(1, 10);
    b.keys.assign(1, 20);
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
    EXPECT_TRUE(a != b);

    // Equal keys
    TransportMemoryKey c{};
    TransportMemoryKey d{};
    c.keys.assign(1, 42);
    d.keys.assign(1, 42);
    EXPECT_FALSE(c < d);
    EXPECT_FALSE(d < c);
    EXPECT_TRUE(c == d);

    // Prefix order: a (1 word) sorts before e (2 words sharing the prefix)
    TransportMemoryKey e{};
    e.keys.assign({10, 11});
    EXPECT_TRUE(a < e);
    EXPECT_FALSE(e < a);
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
    key.keys.assign({42, 99});
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
    memKey.keys.assign(1, 777);
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
    key.keys.assign(1, 123);
    TransportRankPrepareInfo info("eth2", key);
    EXPECT_EQ(info.nic, "eth2");
    ASSERT_EQ(info.memKeys.size(), 1U);
    EXPECT_EQ(info.memKeys[0].keys[0], 123U);
}

TEST_F(HybmTransportCommonTest, TransportRankPrepareInfo_MultiKeyConstructor)
{
    TransportMemoryKey k1{};
    TransportMemoryKey k2{};
    k1.keys.assign(1, 10);
    k2.keys.assign(1, 20);
    std::vector<TransportMemoryKey> keys = {k1, k2};
    TransportRankPrepareInfo info("eth3", keys);
    EXPECT_EQ(info.nic, "eth3");
    ASSERT_EQ(info.memKeys.size(), 2U);
    EXPECT_EQ(info.memKeys[0].keys[0], 10U);
    EXPECT_EQ(info.memKeys[1].keys[0], 20U);
}
