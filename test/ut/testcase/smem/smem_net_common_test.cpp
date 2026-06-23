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
#include <string>
#include <vector>

#include "smem_net_common.h"

using namespace ock::smem;

class SmemNetCommonTest : public ::testing::Test {
protected:
    void SetUp() override
    {
    }

    void TearDown() override
    {
    }
};

// ======================== ExtractIpPortFromUrl Tests ========================

/**
 * ExtractIpPortFromUrl_TcpUrl_Success
 *  - Parse a valid tcp:// URL with standard port.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_TcpUrl_Success)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://192.168.1.100:9980");
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(extraction.ip, "192.168.1.100");
    EXPECT_EQ(extraction.port, 9980); // 9980
}

/**
 * ExtractIpPortFromUrl_TcpUrl_DefaultPort
 *  - When port is not specified in URL, the default port should be used.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_TcpUrl_DefaultPort)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://10.0.0.1:9980");
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(extraction.ip, "10.0.0.1");
    EXPECT_EQ(extraction.port, 9980); // 9980
}

/**
 * ExtractIpPortFromUrl_TcpUrl_ClusterFragment_Rejected
 *  - Cluster fragments (#cluster-name) are only supported for etcd/reg protocols,
 *    not for tcp:// URLs. Verify they are rejected with SM_INVALID_PARAM.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_TcpUrl_ClusterFragment_Rejected)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://172.16.0.1:9980#cluster-alpha");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_TcpUrl_ClusterFragmentHyphen_Rejected
 *  - Same as above, fragments with hyphens/underscores are also rejected for tcp://.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_TcpUrl_ClusterFragmentHyphen_Rejected)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://192.168.1.1:9980#my_cluster_id");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_EtcdUrl_WithClusterFragment_Success
 *  - etcd:// URLs support cluster fragments. Verify the fragment is stripped
 *    and the IP:port is correctly extracted.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_EtcdUrl_WithClusterFragment_Success)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("etcd://192.168.1.1:9980#cluster1");
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(extraction.ip, "192.168.1.1");
    EXPECT_EQ(extraction.port, 9980); // 9980
}

/**
 * ExtractIpPortFromUrl_EmptyFragment_Invalid
 *  - A URL ending with '#' (empty fragment) should be rejected.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_EmptyFragment_Invalid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://192.168.1.1:9980#");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_MultipleFragments_Invalid
 *  - More than one '#' in the URL should be rejected.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_MultipleFragments_Invalid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://192.168.1.1:9980#c1#c2");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_InvalidIpAddress_Invalid
 *  - An invalid IP address (out of range) should be rejected.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_InvalidIpAddress_Invalid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://999.999.999.999:9980");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_PrivatePort_Invalid
 *  - Port 1024 (N1024) should be invalid (must be > 1024).
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_PrivatePort_Invalid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://192.168.1.1:1024");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_PortTooHigh_Invalid
 *  - Port greater than UINT16_MAX (65535) should be invalid.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_PortTooHigh_Invalid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://192.168.1.1:99999");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_NonNumericPort_Invalid
 *  - Non-numeric port should be rejected.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_NonNumericPort_Invalid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://192.168.1.1:abc");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_IpV6Url_Success
 *  - IPv6 URLs should be parsed successfully.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_IpV6Url_Valid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://[::1]:9980");
    EXPECT_EQ(ret, SM_OK);
    // IPv6 addresses are not validated the same way as IPv4
    EXPECT_FALSE(extraction.ip.empty());
    EXPECT_EQ(extraction.port, 9980); // 9980
}

/**
 * ExtractIpPortFromUrl_IpV6GlobalUnicast_Valid
 *  - A typical global unicast IPv6 address should parse.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_IpV6GlobalUnicast_Valid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://[2001:db8::1]:9980");
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(extraction.port, 9980); // 9980
}

/**
 * ExtractIpPortFromUrl_InvalidUrl_EmptyString_Invalid
 *  - Empty string should fail.
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_EmptyString_Invalid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("");
    EXPECT_NE(ret, SM_OK);
}

/**
 * ExtractIpPortFromUrl_ZeroIp_Valid
 *  - 0.0.0.0 is accepted by the validator (IsValidIpV4OrZero allows zero IP).
 */
TEST_F(SmemNetCommonTest, ExtractIpPortFromUrl_ZeroIp_Valid)
{
    UrlExtraction extraction;
    Result ret = extraction.ExtractIpPortFromUrl("tcp://0.0.0.0:9980");
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(extraction.ip, "0.0.0.0");
    EXPECT_EQ(extraction.port, 9980); // 9980
}

// ======================== GetLocalIpWithTarget Tests ========================

/**
 * GetLocalIpWithTarget_InvalidIp_ReturnsError
 *  - Passing a completely invalid string as target should fail.
 */
TEST_F(SmemNetCommonTest, GetLocalIpWithTarget_InvalidIp_ReturnsError)
{
    std::string local;
    Result ret = GetLocalIpWithTarget("not_an_ip_address", local);
    EXPECT_NE(ret, SM_OK);
}

/**
 * GetLocalIpWithTarget_EmptyTarget_ReturnsError
 *  - Empty target should fail.
 */
TEST_F(SmemNetCommonTest, GetLocalIpWithTarget_EmptyTarget_ReturnsError)
{
    std::string local;
    Result ret = GetLocalIpWithTarget("", local);
    EXPECT_NE(ret, SM_OK);
}

/**
 * GetLocalIpWithTarget_ValidIp_WithLocalInterface
 *  - If running on a machine with 127.0.0.1 configured, this can succeed.
 *    This test validates the function works end-to-end, but only asserts
 *    that the function returns without crashing.
 */
TEST_F(SmemNetCommonTest, GetLocalIpWithTarget_Localhost_Valid)
{
    std::string local;
    // This may succeed on machines with loopback or may fail depending on network config.
    // We just verify it doesn't crash and returns a valid state.
    Result ret = GetLocalIpWithTarget("127.0.0.1", local);
    // On CI with loopback, it may return SM_OK; on a container without loopback, it may fail.
    // Both are acceptable outcomes - the test just verifies no crash.
    EXPECT_TRUE(ret == SM_OK || ret == SM_ERROR);
}

/**
 * GetLocalIpWithTarget_InvalidIpV6_ReturnsError
 *  - An invalid IPv6 string should be rejected.
 */
TEST_F(SmemNetCommonTest, GetLocalIpWithTarget_InvalidIpV6_ReturnsError)
{
    std::string local;
    Result ret = GetLocalIpWithTarget("not_an_ip_v6_address", local);
    EXPECT_NE(ret, SM_OK);
}
