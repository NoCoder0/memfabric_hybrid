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

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "device/urma/topo_reader.h"
#include "device/urma/rootinfo_parser.h"

using namespace ock::mf;
using namespace ock::mf::transport::device;

namespace {

// ------------------------------------------------------------------
//  EID hex constants
// ------------------------------------------------------------------
constexpr char EID_HEX_OK[] = "0123456789abcdef0123456789abcdef";
constexpr char EID_HEX_LOWER[] = "aabbccddeeff00112233445566778899";
constexpr char EID_HEX_UPPER[] = "AABBCCDDEEFF00112233445566778899";
constexpr char EID_HEX_MIXED[] = "AaBbCcDdEeFf00112233445566778899";
constexpr char EID_HEX_ZERO[] = "00000000000000000000000000000000";

constexpr uint8_t EID_SENTINEL = 0xFF;

// ------------------------------------------------------------------
//  Helpers
// ------------------------------------------------------------------

std::string Cat(std::initializer_list<std::string> parts)
{
    std::string r;
    for (auto &p : parts) {
        r += p;
    }
    return r;
}

// Valid minimal rootinfo JSON (16 bytes of whitespace suffix)
std::string ValidPrefix()
{
    return Cat({R"({"rank_list":[{)", R"("device_id":1,"level_list":[{)", R"("rank_addr_list":[{)",
                R"("addr_type":"EID","ports":[0,1,2,3,4,5],)", R"("addr":")", EID_HEX_OK, R"("}]}]}]})"});
}

void ExpectEid(const RootInfo &ri, const uint8_t expected[COMM_ADDR_EID_LEN])
{
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(ri.eid[i], expected[i]);
    }
}

void ExpectEidSentinel(const RootInfo &ri)
{
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(ri.eid[i], EID_SENTINEL);
    }
}

void ExpectEidSentinelArr(const std::array<uint8_t, COMM_ADDR_EID_LEN> &eid)
{
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(eid[i], EID_SENTINEL);
    }
}

} // anonymous namespace

// ==================== ParseRootInfoStream — success ====================

// Case #1: basic success
TEST(TopoReaderTest, Case1_Success_Basic)
{
    std::string json = Cat({R"({"rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)",
                            R"("addr_type":"EID","ports":[0,1,2,3,4,5],)", R"("addr":")", EID_HEX_OK, R"("}]}]}]})"});
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    const uint8_t expected[COMM_ADDR_EID_LEN] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                                 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    ExpectEid(ri, expected);
}

// Case #2: EID across multiple levels, only one candidate
TEST(TopoReaderTest, Case2_Success_EidAcrossLevels)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{"addr_type":"IP","ports":[0,1,2,3,4,5],)",
        R"("addr":"192.168.1.1"}]},{)",
        R"("rank_addr_list":[{"addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// Case #3: Non-matching devices present
TEST(TopoReaderTest, Case3_Success_NonMatchingDevices)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":99,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_ZERO,
        R"("}]}]},{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(ri.eid[0], 0x01);
    EXPECT_EQ(ri.eid[1], 0x23);
    EXPECT_EQ(ri.eid[14], 0xcd);
    EXPECT_EQ(ri.eid[15], 0xef);
}

// Case #4: Non-EID addr_type
TEST(TopoReaderTest, Case4_Success_NonEidAddrType)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"IP","ports":[0,1,2,3,4,5],"addr":"ignore"},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// Case #5: Non-six-port ports
TEST(TopoReaderTest, Case5_Success_NonSixPortPorts)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2],"addr":"ignore"},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// ==================== ParseRootInfoStream — structure errors ====================

// Case #6: rank_list missing
TEST(TopoReaderTest, Case6_Error_NoRankList)
{
    std::istringstream iss(R"({"other":"data"})");
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

// Case #7: rank_list not array
TEST(TopoReaderTest, Case7_Error_RankListNotArray)
{
    std::istringstream iss(R"({"rank_list":"not_array"})");
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #8: rank_list element not object
TEST(TopoReaderTest, Case8_Error_RankListElementNotObject)
{
    std::istringstream iss(R"({"rank_list":["string_entry"]})");
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #9: device_id missing
TEST(TopoReaderTest, Case9_Error_DeviceIdMissing)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #10: device_id negative
TEST(TopoReaderTest, Case10_Error_DeviceIdNegative)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":-1,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #11: level_list missing
TEST(TopoReaderTest, Case11_Error_NoLevelList)
{
    std::istringstream iss(R"({"rank_list":[{"device_id":0}]})");
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #12: level_list not array
TEST(TopoReaderTest, Case12_Error_LevelListNotArray)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":"not_array"}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #13: rank_addr_list missing
TEST(TopoReaderTest, Case13_Error_NoRankAddrList)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{"not_rank_addr_list":[]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #14: rank_addr_list not array
TEST(TopoReaderTest, Case14_Error_RankAddrListNotArray)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":"not_array"}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #15: rank_addr not object
TEST(TopoReaderTest, Case15_Error_RankAddrNotObject)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":["string_not_object"]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #18: EID but ports missing
TEST(TopoReaderTest, Case18_Error_EidNoPorts)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID",)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #19: EID but ports not array
TEST(TopoReaderTest, Case19_Error_EidPortsNotArray)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":"not_array",)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== uniqueness errors ====================

// Case #20: 0 device matches
TEST(TopoReaderTest, Case20_Error_NoMatchingDevice)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":1,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 99, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #21: >1 device matches (duplicate phyDeviceId)
TEST(TopoReaderTest, Case21_Error_DuplicateDevice)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]},{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #22: 0 candidates
TEST(TopoReaderTest, Case22_Error_NoCandidate)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"IP","ports":[0,1,2,3,4,5],)",
        R"("addr":"192.168.1.1"}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #23: >1 candidates
TEST(TopoReaderTest, Case23_Error_DuplicateCandidate)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    EXPECT_EQ(ri.eid[0], EID_SENTINEL);
}

// ==================== addr format errors ====================

// Case #24: addr missing
TEST(TopoReaderTest, Case24_Error_AddrMissing)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5]}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Six-port EID missing addr followed by valid candidate → still fail (schema error, not non-candidate fallback)
TEST(TopoReaderTest, Case24b_Error_AddrMissingBeforeValid)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5]},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri); // must not modify rootInfo on failure
}

// ParseSingleRankAddr directly: six-port EID missing addr → BAD_VALUE
TEST(TopoReaderTest, RankAddr_SixPortEidMissingAddr_Error)
{
    const char *json = R"({"addr_type":"EID","ports":[0,1,2,3,4,5]})";
    auto len = std::strlen(json);
    CandidateInfo out;
    ParseResult pr = ParseSingleRankAddr(json, json + len, 0, out);
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
    EXPECT_EQ(pr.reason, ParserReason::BAD_VALUE);
    EXPECT_FALSE(out.isCandidate);
}

// Case #25: addr not string
TEST(TopoReaderTest, Case25_Error_AddrNotString)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":12345}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #26: addr length != 32
TEST(TopoReaderTest, Case26_Error_AddrWrongLength)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":"too_short"}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #27: addr non-hex characters
TEST(TopoReaderTest, Case27_Error_AddrNonHex)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":"0123456789abcdef0123456789zzzzzz"}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== hex case variations ====================

// Case #28: Lowercase hex
TEST(TopoReaderTest, Case28_Success_LowercaseHex)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_LOWER,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    const uint8_t expected[COMM_ADDR_EID_LEN] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
                                                 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    ExpectEid(ri, expected);
}

// Case #29: Uppercase hex
TEST(TopoReaderTest, Case29_Success_UppercaseHex)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_UPPER,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    const uint8_t expected[COMM_ADDR_EID_LEN] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
                                                 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    ExpectEid(ri, expected);
}

// Case #30: Mixed case hex
TEST(TopoReaderTest, Case30_Success_MixedCaseHex)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_MIXED,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    const uint8_t expected[COMM_ADDR_EID_LEN] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
                                                 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    ExpectEid(ri, expected);
}

// Case #31: Multiple levels, no extra candidate
TEST(TopoReaderTest, Case31_Success_MultiLevelNoExtra)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"IP","ports":[0,1,2,3,4,5],)",
        R"("addr":"192.168.1.1"}]},{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]},{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2],)",
        R"("addr":"ignored"}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// Case #32: addr all zeros
TEST(TopoReaderTest, Case32_Success_AllZeroEid)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_ZERO,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(ri.eid[i], 0);
    }
}

// ==================== file-level tests ====================

// Case #33: nonexistent path
TEST(TopoReaderTest, Case33_File_Error_NonexistentPath)
{
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoFile("/nonexistent_path_for_test.json", 0, 0, ri);
    EXPECT_EQ(ret, BM_FILE_NOT_ACCESS);
}

// Case #34: temporary valid file
TEST(TopoReaderTest, Case34_File_Success_TempFile)
{
    const char *tmpPath = "/tmp/topo_reader_test_34.json";
    std::remove(tmpPath);
    {
        std::ofstream ofs(tmpPath);
        ASSERT_TRUE(ofs.is_open());
        ofs << Cat({
            R"({"rank_list":[{"device_id":7,"level_list":[{"rank_addr_list":[{)",
            R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
            R"("addr":"aabbccddeeff00112233445566778899"}]}]}]})",
        });
    }
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoFile(tmpPath, 7, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    const uint8_t expected[COMM_ADDR_EID_LEN] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
                                                 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    ExpectEid(ri, expected);
    std::remove(tmpPath);
}

// ==================== seam tests ====================

// Case #35: Valid JSON stream
TEST(TopoReaderTest, Case35_Stream_Success_ValidJson)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    const uint8_t expected[COMM_ADDR_EID_LEN] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                                 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    ExpectEid(ri, expected);
}

// Case #36: badbit on stream
TEST(TopoReaderTest, Case36_Stream_Error_Badbit)
{
    std::istringstream iss("dummy");
    iss.setstate(std::ios::badbit);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_FILE_NOT_ACCESS);
}

// Case #37: Empty stream
TEST(TopoReaderTest, Case37_Stream_Error_Empty)
{
    std::istringstream iss;
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Case #38: Malformed JSON
TEST(TopoReaderTest, Case38_Stream_Error_MalformedJson)
{
    std::istringstream iss("{invalid json}");
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

// ==================== ADR contract tests ====================

// FailDoesNotModifyRootInfo
TEST(TopoReaderTest, ADR_FailDoesNotModifyRootInfo)
{
    // rank_list missing
    {
        std::istringstream iss(R"({"other":"data"})");
        RootInfo ri;
        ri.eid.fill(EID_SENTINEL);
        Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
        EXPECT_EQ(ret, BM_INVALID_PARAM);
        ExpectEidSentinel(ri);
    }
    // no matching device
    {
        std::string json = Cat({
            R"({"rank_list":[{"device_id":1,"level_list":[{"rank_addr_list":[{)",
            R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
            R"("addr":")",
            EID_HEX_OK,
            R"("}]}]}]})",
        });
        std::istringstream iss(json);
        RootInfo ri;
        ri.eid.fill(EID_SENTINEL);
        Result ret = TopoReader::ParseRootInfoStream(iss, 99, 0, ri);
        EXPECT_EQ(ret, BM_INVALID_PARAM);
        ExpectEidSentinel(ri);
    }
    // duplicate EID candidates
    {
        std::string json = Cat({
            R"({"rank_list":[{)",
            R"("device_id":0,"level_list":[{)",
            R"("rank_addr_list":[{)",
            R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
            R"("addr":")",
            EID_HEX_OK,
            R"("},{)",
            R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
            R"("addr":")",
            EID_HEX_OK,
            R"("}]}]}]})",
        });
        std::istringstream iss(json);
        RootInfo ri;
        ri.eid.fill(EID_SENTINEL);
        Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
        EXPECT_EQ(ret, BM_INVALID_PARAM);
        ExpectEidSentinel(ri);
    }
}

// LevelListBeforeDeviceId
TEST(TopoReaderTest, ADR_LevelListBeforeDeviceId)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}],)",
        R"("device_id":0}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// NonEID_NoAddrOk — non-candidate truly has no addr field
TEST(TopoReaderTest, ADR_NonEID_NoAddrOk)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"IP"},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// EID_BadPorts_NoAddrOk — non-candidate has no addr field
TEST(TopoReaderTest, ADR_EID_BadPorts_NoAddrOk)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2]},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// Multi-level multi-candidate
TEST(TopoReaderTest, ADR_MultiLevelMultiCandidate)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]},{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

// Second candidate syntax corruption
TEST(TopoReaderTest, ADR_SecondCandidateSyntaxCorruption)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":12345}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

// Target post-tail corruption
TEST(TopoReaderTest, ADR_TargetPostTailCorruption)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}],)",
        R"("extra_key":broken_value})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

// Extra: device_id exceeds uint32 range
TEST(TopoReaderTest, Extra_Error_DeviceIdOversized)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":5000000000,)",
        R"("level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== DeviceId boundary tests ====================

TEST(TopoReaderTest, DeviceId_NegativeZero_AsZero)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":-0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

TEST(TopoReaderTest, DeviceId_Negative_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":-1,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DeviceId_Float_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1.0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DeviceId_Exponent_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1e5,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DeviceId_LeadingZero_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":00,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DeviceId_Overflow_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":4294967296,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DeviceId_Zero_Ok)
{
    std::string json = Cat({
        R"({"rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// ==================== Duplicate key rejection ====================

TEST(TopoReaderTest, DuplicateKey_RankList_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[],)",
        R"("rank_list":[]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DuplicateKey_DeviceId_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DuplicateKey_LevelList_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[],"level_list":[]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DuplicateKey_RankAddrList_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[],)",
        R"("rank_addr_list":[]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DuplicateKey_AddrType_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr_type":"EID",)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DuplicateKey_Ports_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, DuplicateKey_Addr_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"(","addr":"bad"}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== Trailing garbage ====================

TEST(TopoReaderTest, TrailingGarbage_Rejected)
{
    std::string json = ValidPrefix() + "x";
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, TrailingGarbage_ExtraObject_Rejected)
{
    std::string json =
        Cat({R"({"rank_list":[{"device_id":1,"level_list":[{"rank_addr_list":[{)",
             R"("addr_type":"EID","ports":[0,1,2,3,4,5],)", R"("addr":")", EID_HEX_OK, R"("}]}]}]})", R"({})"});
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

// ==================== Depth tests ====================

// Nesting depth 32: 32 nested arrays within an unknown root field → OK
TEST(TopoReaderTest, Depth32_Success)
{
    // Build nested arrays: [[[...32 deep..."inner"...]]]
    std::string inner = R"("inner")";
    for (int i = 0; i < 32; ++i) {
        inner = "[" + inner + "]";
    }
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}],)",
        R"("deep":)",
        inner,
        "}",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// Nesting depth 33: 33 nested arrays → rejected (depth > MAX_PARSE_DEPTH)
TEST(TopoReaderTest, Depth33_Rejected)
{
    std::string inner = R"("inner")";
    for (int i = 0; i < 33; ++i) {
        inner = "[" + inner + "]";
    }
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}],)",
        R"("deep":)",
        inner,
        "}",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== 1MiB size tests ====================

TEST(TopoReaderTest, FileSize_1MiB_Ok)
{
    std::string prefix = ValidPrefix();
    const size_t pad = MAX_INPUT_BYTES - prefix.size();
    std::string json = prefix + std::string(pad, ' ');
    ASSERT_EQ(json.size(), MAX_INPUT_BYTES);
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

TEST(TopoReaderTest, FileSize_1MiBPlus1_Rejected)
{
    std::string prefix = ValidPrefix();
    const size_t pad = MAX_INPUT_BYTES_PLUS_1 - prefix.size();
    std::string json = prefix + std::string(pad, ' ');
    ASSERT_EQ(json.size(), MAX_INPUT_BYTES_PLUS_1);
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== Unknown field tests ====================

// Unknown field with complex valid JSON value → OK
TEST(TopoReaderTest, UnknownField_ComplexValue_Ok)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}],)",
        R"("extra":{"nested":{"a":1,"b":[true,false,null]}}})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// Unknown field with illegal grammar → error
TEST(TopoReaderTest, UnknownField_IllegalGrammar_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}],)",
        R"("extra":nested value})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== String validation tests ====================

TEST(TopoReaderTest, String_UnescapedControl_Rejected)
{
    // addr_type contains unescaped 0x01 control character
    const std::string ctrlAddr = std::string("EI") + static_cast<char>(0x01) + "D\"";
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":")",
        ctrlAddr,
        ",",
        R"("ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, String_EscapeSequences_Ok)
{
    // Valid escape sequences in a non-key non-target field
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"(","desc":"line1\nline2\t\"ok\"\\"}]}]}],)",
        R"("meta":"foo\"bar"})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

TEST(TopoReaderTest, String_SurrogatePair_Ok)
{
    // Valid surrogate pair \uD800\uDC00 in a non-target field
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}],)",
        R"("x":"\uD800\uDC00"})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

TEST(TopoReaderTest, String_LoneSurrogate_Rejected)
{
    // Lone surrogate \uD800 without pair
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}],)",
        R"("x":"\uD800"})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, String_LowSurrogate_Rejected)
{
    // Lone low surrogate \uDC00
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}],)",
        R"("x":"\uDC00"})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== Ports validation tests ====================

TEST(TopoReaderTest, Ports_ArbitraryJsonValues_Ok)
{
    // ports array elements can be any valid JSON value, not just numbers
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[true,false,null,"str",{},[1,2]],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

TEST(TopoReaderTest, Ports_WrongLength_NotEidCandidate)
{
    // EID type but ports length != 6 → not a candidate (but addr may be present)
    // This should pass because there's another valid candidate
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[1,2,3],"addr":"ignored"},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// ==================== Comma validation tests ====================

TEST(TopoReaderTest, MissingComma_Object_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, TrailingComma_Object_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"(",}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, TrailingComma_Array_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5,],)", // trailing comma in ports
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, IllegalArrayElement_Rejected)
{
    // Non-object element in rank_addr_list
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("},["bad_element"]]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== Non-target device validation ====================

TEST(TopoReaderTest, NonTarget_DeviceIdMissing_Rejected)
{
    // Non-matching device missing device_id → still an error since all
    // rank_list elements must have valid device_id
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("level_list":[]},{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, NonTarget_DeviceIdIllegal_Rejected)
{
    // Non-matching device with illegal device_id
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":"not_a_number","level_list":[]},{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, NonTarget_DeviceIdNegative_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":-5,"level_list":[]},{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== Target success then tail damage ====================

TEST(TopoReaderTest, TargetThenTailRankElement_Damaged)
{
    // Target device's EID is valid, but next rank_list element is garbage
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{"rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]},{)",
        R"("device_id":1,"level_list":broken}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

TEST(TopoReaderTest, TargetThenTailLevel_Damaged)
{
    // Target device has valid EID, but next level in level_list is damaged
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]},{)",
        R"("rank_addr_list":broken}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

TEST(TopoReaderTest, TargetThenTailRankAddr_Damaged)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("},{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":bad_value}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

// ==================== Missing/wrong addr_type in candidate ====================

TEST(TopoReaderTest, MissingAddrType_Rejected)
{
    // rank_addr missing addr_type entirely (but has ports and addr)
    // Since level requires rank_addr_list, and rank_addr is missing addr_type,
    // it can't be classified as EID candidate → no candidate found
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, AddrType_WrongType_Rejected)
{
    // addr_type value is not a string (e.g., number)
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":123,"ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== LevelList order / rank_addr order ====================

TEST(TopoReaderTest, RankAddrFieldArbitraryOrder)
{
    // addr before addr_type, ports before addr, etc.
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"(","addr_type":"EID"}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// ==================== Escaped target field ====================

// ==================== Escaped known key rejection ====================
// Escaped known keys (like device_\u0069d) must be rejected as DUPLICATE_KEY,
// not silently treated as unknown fields.

TEST(TopoReaderTest, EscapedKnownKey_RankList_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[],)",
        R"("ran\u006b_list":[]})", // ran\u006b_list decodes to "rank_list"
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, EscapedKnownKey_DeviceId_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("devic\u0065_id":0,)", // devic\u0065_id decodes to "device_id"
        R"("level_list":[]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, EscapedKnownKey_LevelList_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)", R"("device_id":0,)",
        R"("leve\u006c_list":[]}]})", // leve\u006c_list decodes to "level_list"
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, EscapedKnownKey_RankAddrList_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)", R"("device_id":0,"level_list":[{)",
        R"("ran\u006b_addr_list":[]}]}]})", // ran\u006b_addr_list decodes to "rank_addr_list"
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, EscapedKnownKey_AddrType_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr\u005ftype":"EID",)", // addr\u005ftype decodes to "addr_type"
        R"("ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, EscapedKnownKey_Ports_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID",)",
        R"("por\u0074s":[0,1,2,3,4,5],)", // por\u0074s decodes to "ports"
        R"("addr":")",
        EID_HEX_OK,
        R"("}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, EscapedKnownKey_Addr_Rejected)
{
    std::string json = Cat({
        R"({"rank_list":[{)", R"("device_id":0,"level_list":[{)", R"("rank_addr_list":[{)", R"("addr_type":"EID",)",
        R"("ports":[0,1,2,3,4,5],)", R"("add\u0072":")", EID_HEX_OK,
        R"("}]}]}]})", // add\u0072 decodes to "addr"
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// ==================== Escaped target value (revisited) ====================
// Test that escaped target values in addr_type cause explicit rejection
// distinguishable from "no candidate" by having another valid candidate.

TEST(TopoReaderTest, EscapedTargetValue_WithValidFallback_Ok)
{
    // First entry has valid EID, second has escaped addr_type that decodes to non-EID
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":1,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":")",
        EID_HEX_OK,
        R"("},{)",
        R"("addr_type":"EI\nD","ports":[0,1,2,3,4,5],)",
        R"("addr":"0123456789abcdef0123456789abcdef"}]}]}]})",
    });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_OK);
    const uint8_t expected[COMM_ADDR_EID_LEN] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                                 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    ExpectEid(ri, expected);
}

// ==================== RootInfo sentinel preservation ====================

TEST(TopoReaderTest, Sentinel_1MiBPlus1_Unchanged)
{
    std::string prefix = ValidPrefix();
    const size_t pad = MAX_INPUT_BYTES_PLUS_1 - prefix.size();
    std::string json = prefix + std::string(pad, ' ');
    ASSERT_EQ(json.size(), MAX_INPUT_BYTES_PLUS_1);
    std::istringstream iss(json);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 1, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

TEST(TopoReaderTest, Sentinel_Badbit_Unchanged)
{
    std::istringstream iss("dummy");
    iss.setstate(std::ios::badbit);
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_FILE_NOT_ACCESS);
    ExpectEidSentinel(ri);
}

TEST(TopoReaderTest, Sentinel_EmptyStream_Unchanged)
{
    RootInfo ri;
    ri.eid.fill(EID_SENTINEL);
    // Empty stream → INVALID_PARAM, no eid modification
    std::istringstream iss;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinel(ri);
}

// ==================== ParseRootObject leading whitespace ====================

TEST(TopoReaderTest, LeadingWhitespace_BeforeRootBrace_Ok)
{
    std::string json =
        std::string("  \t\n ") + Cat({
                                     R"({"rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)",
                                     R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
                                     R"("addr":")",
                                     EID_HEX_OK,
                                     R"("}]}]}]})",
                                 });
    std::istringstream iss(json);
    RootInfo ri;
    Result ret = TopoReader::ParseRootInfoStream(iss, 0, 0, ri);
    EXPECT_EQ(ret, BM_OK);
}

// ==================== Nullptr and reverse interval ====================

TEST(TopoReaderTest, Range_NullptrBegin)
{
    uint32_t val = 42;
    ParseResult pr = ParseDeviceId(nullptr, "abc", 0, val);
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
    EXPECT_EQ(val, 42U); // value unchanged
}

TEST(TopoReaderTest, Range_NullptrEnd)
{
    uint32_t val = 42;
    ParseResult pr = ParseDeviceId("abc", nullptr, 0, val);
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
    EXPECT_EQ(val, 42U);
}

TEST(TopoReaderTest, Range_ReverseInterval)
{
    uint32_t val = 42;
    const char *data = "123";
    ParseResult pr = ParseDeviceId(data + 3, data, 0, val); // begin > end
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
    EXPECT_EQ(val, 42U);
}

TEST(TopoReaderTest, Range_SkipString_Nullptr)
{
    ParseResult pr = SkipString(nullptr, "abc", 0, 256);
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, Range_SkipValue_Nullptr)
{
    ParseResult pr = SkipValue(nullptr, "abc", 0, 0);
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
}

TEST(TopoReaderTest, Range_ParseSingleRankAddr_Nullptr)
{
    CandidateInfo out;
    ParseResult pr = ParseSingleRankAddr(nullptr, "abc", 0, out);
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
}

// ==================== ParseDeviceId output guard and reasons ====================

TEST(TopoReaderTest, DeviceId_OutputUnchangedOnFailure)
{
    const char *input = "abc";
    uint32_t val = 0xDEADBEEF;
    ParseResult pr = ParseDeviceId(input, input + 3, 0, val);
    EXPECT_NE(pr.result, BM_OK);
    EXPECT_EQ(val, 0xDEADBEEFU); // unchanged
}

TEST(TopoReaderTest, DeviceId_FloatReason_Specific)
{
    const char *input = "1.5";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 3, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_ID_FLOAT);
    EXPECT_EQ(pr.offset, 1U); // offset of '.'
}

TEST(TopoReaderTest, DeviceId_ExponentReason_Specific)
{
    const char *input = "1e5";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 3, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_ID_EXPONENT);
    EXPECT_EQ(pr.offset, 1U); // offset of 'e'
}

TEST(TopoReaderTest, DeviceId_ExponentReason_CapitalE)
{
    const char *input = "1E5";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 3, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_ID_EXPONENT);
    EXPECT_EQ(pr.offset, 1U); // offset of 'E'
}

TEST(TopoReaderTest, DeviceId_OverflowReason_Specific)
{
    // 4294967296 = UINT32_MAX + 1 → overflow
    const char *input = "4294967296";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 10, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_ID_OVERFLOW);
    // offset should point to the digit that caused overflow
    EXPECT_EQ(pr.offset, 9U); // '6' at position 9 (last digit, 4294967296)
}

TEST(TopoReaderTest, DeviceId_LeadingZeroReason_Specific)
{
    const char *input = "00";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 2, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_ID_LEADING_ZERO);
    EXPECT_EQ(pr.offset, 1U); // second '0'
}

TEST(TopoReaderTest, DeviceId_NegativeZeroFloat_Specific)
{
    const char *input = "-0.5";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 4, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_ID_FLOAT);
    EXPECT_EQ(pr.offset, 2U); // offset of '.'
}

TEST(TopoReaderTest, DeviceId_NegativeZeroExponent_Specific)
{
    const char *input = "-0e5";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 4, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_ID_EXPONENT);
    EXPECT_EQ(pr.offset, 2U); // offset of 'e'
}

TEST(TopoReaderTest, DeviceId_IllegalChar_Offset)
{
    // '-' followed by non-digit 'a' → BAD_DEVICE_ID at offset past '-'
    const char *input = "-a";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 2, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::BAD_DEVICE_ID);
    EXPECT_EQ(pr.offset, 1U); // offset of 'a', the illegal char
}

TEST(TopoReaderTest, DeviceId_IllegalCharNoMinus_Offset)
{
    // Non-digit first char 'a' → BAD_DEVICE_ID at 'a'
    const char *input = "a";
    uint32_t val = 0;
    ParseResult pr = ParseDeviceId(input, input + 1, 0, val);
    EXPECT_EQ(pr.reason, ParserReason::BAD_DEVICE_ID);
    EXPECT_EQ(pr.offset, 0U); // offset of 'a'
}

// ==================== EOF offset tests ====================

TEST(TopoReaderTest, EofOffset_UnterminatedString)
{
    const char *json = "\"abc";
    auto len = static_cast<size_t>(4); // "abc = 4 chars, no closing quote
    ParseResult pr = SkipString(json, json + len, 0, 256);
    EXPECT_EQ(pr.reason, ParserReason::UNTERMINATED_STRING);
    EXPECT_EQ(pr.offset, len); // EOF offset = buffer length
}

TEST(TopoReaderTest, EofOffset_UnterminatedObject)
{
    const char *json = "{\"a\":1";
    auto len = static_cast<size_t>(6);
    ParseResult pr = SkipValue(json, json + len, 0, 0);
    EXPECT_EQ(pr.reason, ParserReason::UNTERMINATED_OBJECT);
    EXPECT_EQ(pr.offset, len); // EOF offset
}

TEST(TopoReaderTest, EofOffset_UnterminatedArray)
{
    const char *json = "[1,2";
    auto len = static_cast<size_t>(4);
    ParseResult pr = SkipValue(json, json + len, 0, 0);
    EXPECT_EQ(pr.reason, ParserReason::UNTERMINATED_ARRAY);
    EXPECT_EQ(pr.offset, len); // EOF offset
}

TEST(TopoReaderTest, EofOffset_UnterminatedStringInObject)
{
    // Object with unterminated string value → EOF offset
    const char *json = "{\"a\":\"bc";
    auto len = static_cast<size_t>(8);
    ParseResult pr = SkipValue(json, json + len, 0, 0);
    EXPECT_EQ(pr.reason, ParserReason::UNTERMINATED_STRING);
    EXPECT_EQ(pr.offset, len);
}

TEST(TopoReaderTest, EofOffset_UnterminatedArrayInObject)
{
    const char *json = "{\"a\":[1,";
    auto len = static_cast<size_t>(8);
    ParseResult pr = SkipValue(json, json + len, 0, 0);
    EXPECT_EQ(pr.reason, ParserReason::UNTERMINATED_ARRAY);
    EXPECT_EQ(pr.offset, len);
}

// ==================== DEVICE_UNCLOSED ====================

TEST(TopoReaderTest, DeviceUnclosed_NoClosingBrace)
{
    // rank_addr object without closing }
    const char *json = R"({"addr_type":"EID","ports":[0,1,2,3,4,5])";
    auto len = std::strlen(json);
    CandidateInfo out;
    ParseResult pr = ParseSingleRankAddr(json, json + len, 0, out);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_UNCLOSED);
    EXPECT_FALSE(out.isCandidate);
}

TEST(TopoReaderTest, DeviceUnclosed_EmptyObjectNoBrace)
{
    const char *json = "{";
    auto len = std::strlen(json);
    CandidateInfo out;
    ParseResult pr = ParseSingleRankAddr(json, json + len, 0, out);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_UNCLOSED);
}

TEST(TopoReaderTest, DeviceUnclosed_PartialObjectEof)
{
    const char *json = R"({"addr_type":"EID")";
    auto len = std::strlen(json);
    CandidateInfo out;
    ParseResult pr = ParseSingleRankAddr(json, json + len, 0, out);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_UNCLOSED);
}

// ==================== EID illegal character offset ====================

TEST(TopoReaderTest, EidIllegalChar_Offset)
{
    // addr contains 'z' (illegal hex) at position after opening quote
    std::string json = Cat({
        R"({"rank_list":[{)",
        R"("device_id":0,"level_list":[{)",
        R"("rank_addr_list":[{)",
        R"("addr_type":"EID","ports":[0,1,2,3,4,5],)",
        R"("addr":"0123456789abcdef0123456789zzzzzz"}]}]}]})",
    });
    // Direct ParseRootInfoEid to get precise failure
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    eid.fill(EID_SENTINEL);
    Result ret = ParseRootInfoEid(json.data(), json.data() + json.size(), 0, 0, eid);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ExpectEidSentinelArr(eid); // eid must be unchanged
}

// ==================== ParseSingleRankAddr missing `}` returns DEVICE_UNCLOSED ====================

TEST(TopoReaderTest, RankAddr_Unclosed_TopLevel)
{
    const char *json = R"({"addr_type":"EID","ports":[0,1,2,3,4,5],"addr":"0123456789abcdef0123456789abcdef")";
    auto len = std::strlen(json);
    CandidateInfo out;
    ParseResult pr = ParseSingleRankAddr(json, json + len, 0, out);
    EXPECT_EQ(pr.reason, ParserReason::DEVICE_UNCLOSED);
    EXPECT_FALSE(out.isCandidate);
}

// ParseSingleRankAddr consumed `}` at end of [begin,end) → i == totalLen must succeed
TEST(TopoReaderTest, RankAddr_ClosedAtBufferEnd_Success)
{
    // Whole buffer is exactly the rank_addr object; after consuming '}' we land at totalLen.
    const char *json = R"({"addr_type":"EID","ports":[0,1,2,3,4,5],"addr":"0123456789abcdef0123456789abcdef"})";
    auto len = std::strlen(json);
    CandidateInfo out;
    ParseResult pr = ParseSingleRankAddr(json, json + len, 0, out);
    EXPECT_EQ(pr.result, BM_OK);
    EXPECT_TRUE(out.isCandidate);
}

// ==================== Trailing comma EOF offset ====================

TEST(TopoReaderTest, TrailingCommaEof_UnterminatedObject)
{
    const char *json = "{\"a\":1,";
    auto len = std::strlen(json);
    ParseResult pr = SkipValue(json, json + len, 0, 0);
    EXPECT_EQ(pr.reason, ParserReason::UNTERMINATED_OBJECT);
    EXPECT_EQ(pr.offset, len);
}

// ==================== RootInfo sentinel on direct API failure ====================

TEST(TopoReaderTest, Sentinel_ParseRootInfoEid_Failure)
{
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    eid.fill(EID_SENTINEL);
    const char *json = R"({"other":"data"})";
    Result ret = ParseRootInfoEid(json, json + std::strlen(json), 0, 0, eid);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(eid[i], EID_SENTINEL);
    }
}

// ==================== SkipString reason/offset UT ====================

TEST(TopoReaderTest, SkipString_UnterminatedEscapeEof)
{
    const char *json = "\"abc\\";
    auto len = std::strlen(json);
    ParseResult pr = SkipString(json, json + len, 0, 256);
    EXPECT_EQ(pr.reason, ParserReason::UNTERMINATED_STRING);
    EXPECT_EQ(pr.offset, len);
}

TEST(TopoReaderTest, SkipString_Nullptr)
{
    ParseResult pr = SkipString(nullptr, "abc", 0, 256);
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
    EXPECT_EQ(pr.reason, ParserReason::BAD_VALUE);
    EXPECT_EQ(pr.offset, 0U);
}

TEST(TopoReaderTest, SkipString_ReverseRange)
{
    const char *data = "abc";
    ParseResult pr = SkipString(data + 3, data, 0, 256);
    EXPECT_EQ(pr.result, BM_INVALID_PARAM);
}

// ==================== Unused reason enum tests ====================
// Verify that certain reasons are actually reachable

TEST(TopoReaderTest, Reason_ControlChar_InKey)
{
    // String with control character at offset 3
    std::string ctrl = "\"ab" + std::string(1, '\x01') + "c\"";
    auto len = ctrl.size();
    ParseResult pr = SkipString(ctrl.data(), ctrl.data() + len, 0, 256);
    EXPECT_EQ(pr.reason, ParserReason::CONTROL_CHAR);
    EXPECT_EQ(pr.offset, 3U); // position of 0x01
}

TEST(TopoReaderTest, Reason_BadStringEnd_NoQuote)
{
    const char *json = "hello";
    ParseResult pr = SkipString(json, json + 5, 0, 256);
    EXPECT_EQ(pr.reason, ParserReason::BAD_STRING_END);
}

bool WriteTempFile(const std::string &path, const std::string &content)
{
    std::ofstream output(path);
    if (!output.is_open()) {
        return false;
    }
    output << content;
    return output.good();
}

std::string RootInfoWithTopo(const std::string &topoPath, const char *ports)
{
    return Cat({R"({"topo_file_path":")", topoPath,
                R"(","rank_list":[{"device_id":0,"level_list":[{"rank_addr_list":[{)", R"("addr_type":"EID","ports":)",
                ports, R"(,"addr":")", EID_HEX_OK, R"("}]}]}]})"});
}

TEST(TopoReaderTest, ParseRootInfo_Atlas850UsesEightPorts)
{
    const std::string topoPath = "/tmp/topo_reader_atlas850.topo";
    const std::string rootInfoPath = "/tmp/topo_reader_atlas850.rootinfo";
    ASSERT_TRUE(WriteTempFile(topoPath, "hardware: Atlas 850"));
    ASSERT_TRUE(WriteTempFile(rootInfoPath, RootInfoWithTopo(topoPath, "[0,1,2,3,4,5,6,7]")));
    RootInfo rootInfo;
    EXPECT_EQ(TopoReader::ParseRootInfo(rootInfoPath, 0, 0, rootInfo), BM_OK);
    EXPECT_EQ(rootInfo.eid[0], 0x01);
    std::remove(topoPath.c_str());
    std::remove(rootInfoPath.c_str());
}

TEST(TopoReaderTest, ParseRootInfo_Atlas950UsesSixPorts)
{
    const std::string topoPath = "/tmp/topo_reader_atlas950.topo";
    const std::string rootInfoPath = "/tmp/topo_reader_atlas950.rootinfo";
    ASSERT_TRUE(WriteTempFile(topoPath, "hardware: Atlas 950 SuperPoD"));
    ASSERT_TRUE(WriteTempFile(rootInfoPath, RootInfoWithTopo(topoPath, "[0,1,2,3,4,5]")));
    RootInfo rootInfo;
    EXPECT_EQ(TopoReader::ParseRootInfo(rootInfoPath, 0, 0, rootInfo), BM_OK);
    EXPECT_EQ(rootInfo.eid[0], 0x01);
    std::remove(topoPath.c_str());
    std::remove(rootInfoPath.c_str());
}

TEST(TopoReaderTest, ParseRootInfo_ModelPortMismatchPreservesEid)
{
    const std::string topoPath = "/tmp/topo_reader_mismatch.topo";
    const std::string rootInfoPath = "/tmp/topo_reader_mismatch.rootinfo";
    ASSERT_TRUE(WriteTempFile(topoPath, "Atlas 850"));
    ASSERT_TRUE(WriteTempFile(rootInfoPath, RootInfoWithTopo(topoPath, "[0,1,2,3,4,5]")));
    RootInfo rootInfo;
    rootInfo.eid.fill(EID_SENTINEL);
    EXPECT_EQ(TopoReader::ParseRootInfo(rootInfoPath, 0, 0, rootInfo), BM_INVALID_PARAM);
    ExpectEidSentinel(rootInfo);
    std::remove(topoPath.c_str());
    std::remove(rootInfoPath.c_str());
}

TEST(TopoReaderTest, ParseRootInfo_TopoFileUnreadablePreservesEid)
{
    const std::string topoPath = "/tmp/topo_reader_missing.topo";
    const std::string rootInfoPath = "/tmp/topo_reader_missing.rootinfo";
    std::remove(topoPath.c_str());
    ASSERT_TRUE(WriteTempFile(rootInfoPath, RootInfoWithTopo(topoPath, "[0,1,2,3,4,5]")));
    RootInfo rootInfo;
    rootInfo.eid.fill(EID_SENTINEL);
    EXPECT_EQ(TopoReader::ParseRootInfo(rootInfoPath, 0, 0, rootInfo), BM_FILE_NOT_ACCESS);
    ExpectEidSentinel(rootInfo);
    std::remove(rootInfoPath.c_str());
}

TEST(TopoReaderTest, ParseRootInfo_TopoFileOversizeRejected)
{
    const std::string topoPath = "/tmp/topo_reader_oversize.topo";
    const std::string rootInfoPath = "/tmp/topo_reader_oversize.rootinfo";
    ASSERT_TRUE(WriteTempFile(topoPath, std::string(MAX_INPUT_BYTES_PLUS_1, 'x')));
    ASSERT_TRUE(WriteTempFile(rootInfoPath, RootInfoWithTopo(topoPath, "[0,1,2,3,4,5]")));
    RootInfo rootInfo;
    rootInfo.eid.fill(EID_SENTINEL);
    EXPECT_EQ(TopoReader::ParseRootInfo(rootInfoPath, 0, 0, rootInfo), BM_INVALID_PARAM);
    ExpectEidSentinel(rootInfo);
    std::remove(topoPath.c_str());
    std::remove(rootInfoPath.c_str());
}

TEST(TopoReaderTest, ParseRootInfo_TopoHardwareMissingPreservesEid)
{
    const std::string topoPath = "/tmp/topo_reader_unknown.topo";
    const std::string rootInfoPath = "/tmp/topo_reader_unknown.rootinfo";
    ASSERT_TRUE(WriteTempFile(topoPath, "hardware: unknown"));
    ASSERT_TRUE(WriteTempFile(rootInfoPath, RootInfoWithTopo(topoPath, "[0,1,2,3,4,5]")));
    RootInfo rootInfo;
    rootInfo.eid.fill(EID_SENTINEL);
    EXPECT_EQ(TopoReader::ParseRootInfo(rootInfoPath, 0, 0, rootInfo), BM_INVALID_PARAM);
    ExpectEidSentinel(rootInfo);
    std::remove(topoPath.c_str());
    std::remove(rootInfoPath.c_str());
}
