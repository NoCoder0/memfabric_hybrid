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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "verbs/genetty_verbs_info.h"

using namespace ock::openDCC;
using namespace ock::openDCC::verbs;

namespace {
std::string MemoryText(const genetty_memory_info_t &info)
{
    return std::string(info.data, info.length);
}

std::string LinkText(const genetty_link_info_t &info)
{
    return std::string(info.data, info.length);
}

void FillGid(uint8_t *gid, uint8_t seed)
{
    for (size_t i = 0; i < ROCE_GID_LEN; i++) {
        gid[i] = static_cast<uint8_t>(seed + i);
    }
}

std::string ExportMemoryText(MemoryInfo &info)
{
    genetty_memory_info_t out {};
    EXPECT_EQ(info.Export(&out, 0), GEN_OK);
    std::string text = MemoryText(out);
    free(out.data);
    return text;
}

Result ImportMemoryText(MemoryInfo &info, const std::string &text)
{
    genetty_memory_info_t in {};
    in.data = const_cast<char *>(text.c_str());
    in.length = static_cast<uint32_t>(text.size());
    return info.Import(&in, 0);
}

std::string ExportLinkText(VerbsLinkInfo &info)
{
    genetty_link_info_t out {};
    EXPECT_EQ(info.Export(&out, 0), GEN_OK);
    std::string text = LinkText(out);
    free(out.data);
    return text;
}

Result ImportLinkText(VerbsLinkInfo &info, const std::string &text)
{
    genetty_link_info_t in {};
    in.data = const_cast<char *>(text.c_str());
    in.length = static_cast<uint32_t>(text.size());
    return info.Import(&in, 0);
}
} // namespace

class GenettyVerbsInfoTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================
// MemoryInfo
// ============================================================
TEST_F(GenettyVerbsInfoTest, MemoryInfo_SetGet)
{
    MemoryInfo info;
    EXPECT_EQ(info.Address(), 0U);
    EXPECT_EQ(info.Size(), 0U);
    EXPECT_EQ(info.Rkey(), 0U);
    EXPECT_EQ(info.MemType(), MT_INVALID);

    info.Address(0x1000U);
    info.Size(4096);
    info.Rkey(0x1a2bU);
    info.MemType(MT_DEVICE_HBM);

    EXPECT_EQ(info.Address(), 0x1000U);
    EXPECT_EQ(info.Size(), 4096U);
    EXPECT_EQ(info.Rkey(), 0x1a2bU);
    EXPECT_EQ(info.MemType(), MT_DEVICE_HBM);
}

TEST_F(GenettyVerbsInfoTest, MemoryInfo_ExportContent)
{
    MemoryInfo info;
    info.Address(0x2000U);
    info.Size(8192);
    info.Rkey(0x33U);
    info.MemType(MT_DEVICE_HBM);

    std::string text = ExportMemoryText(info);
    EXPECT_NE(text.find(GEN_INFO_MEM_ADDR), std::string::npos);
    EXPECT_NE(text.find(GEN_INFO_MEM_SIZE), std::string::npos);
    EXPECT_NE(text.find(GEN_INFO_MEM_RKEY), std::string::npos);
    EXPECT_NE(text.find(GEN_INFO_MEM_TYPE), std::string::npos);
    EXPECT_NE(text.find("= 8192"), std::string::npos);
    EXPECT_NE(text.find("= 1"), std::string::npos);
}

TEST_F(GenettyVerbsInfoTest, MemoryInfo_RoundTrip)
{
    MemoryInfo source;
    source.Address(0x3000U);
    source.Size(1024);
    source.Rkey(0x55U);
    source.MemType(MT_DEVICE_HBM);

    auto text = ExportMemoryText(source);

    MemoryInfo target;
    EXPECT_EQ(ImportMemoryText(target, text), GEN_OK);
    EXPECT_EQ(target.Address(), source.Address());
    EXPECT_EQ(target.Size(), source.Size());
    EXPECT_EQ(target.Rkey(), source.Rkey());
    EXPECT_EQ(target.MemType(), source.MemType());
    EXPECT_EQ(ExportMemoryText(target), text);
}

TEST_F(GenettyVerbsInfoTest, MemoryInfo_ImportMalformed)
{
    MemoryInfo info;
    EXPECT_EQ(ImportMemoryText(info, "no equal sign\n"), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, MemoryInfo_ImportMissingKey)
{
    MemoryInfo info;
    /* the memory type key is missing, the import must fail instead of using a default one */
    std::string text = "openDCC.verbs.mem.addr = 4096\n"
                       "openDCC.verbs.mem.lkey = 77\n"
                       "openDCC.verbs.mem.size = 8192\n";
    EXPECT_EQ(ImportMemoryText(info, text), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, MemoryInfo_ImportMemTypeOutOfRange)
{
    MemoryInfo info;
    std::string text = "openDCC.verbs.mem.addr = 4096\n"
                       "openDCC.verbs.mem.lkey = 77\n"
                       "openDCC.verbs.mem.size = 8192\n"
                       "openDCC.verbs.mem.type = 9\n";
    EXPECT_EQ(ImportMemoryText(info, text), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, MemoryInfo_ImportInvalidParam)
{
    MemoryInfo info;
    genetty_memory_info_t empty {};
    EXPECT_EQ(info.Import(nullptr, 0), GEN_INVALID_PARAM);
    EXPECT_EQ(info.Import(&empty, 0), GEN_INVALID_PARAM);

    const char *text = "openDCC.verbs.mem.addr = 1\n";
    genetty_memory_info_t zeroLen {};
    zeroLen.data = const_cast<char *>(text);
    zeroLen.length = 0;
    EXPECT_EQ(info.Import(&zeroLen, 0), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, MemoryInfo_ExportInvalidParam)
{
    MemoryInfo info;
    EXPECT_EQ(info.Export(nullptr, 0), GEN_INVALID_PARAM);
}

// ============================================================
// VerbsLinkInfo
// ============================================================
TEST_F(GenettyVerbsInfoTest, LinkInfo_SetGet)
{
    VerbsLinkInfo info;
    EXPECT_EQ(info.Version(), GEN_INFO_VERSION_CONST);
    EXPECT_EQ(info.Qpn(), 0U);
    EXPECT_EQ(info.PortNum(), 0U);

    uint8_t gid[ROCE_GID_LEN] = {};
    FillGid(gid, 0x10);
    info.Version(2);
    info.Qpn(1234);
    info.Lid(7);
    info.PortNum(1);
    info.SqDepth(128);
    info.RqDepth(256);
    info.Gid(gid);

    EXPECT_EQ(info.Version(), 2);
    EXPECT_EQ(info.Qpn(), 1234U);
    EXPECT_EQ(info.Lid(), 7);
    EXPECT_EQ(info.PortNum(), 1);
    EXPECT_EQ(info.SqDepth(), 128U);
    EXPECT_EQ(info.RqDepth(), 256U);
    EXPECT_EQ(memcmp(info.Gid(), gid, ROCE_GID_LEN), 0);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_SetGidNullIgnored)
{
    VerbsLinkInfo info;
    uint8_t gid[ROCE_GID_LEN] = {};
    FillGid(gid, 0x20);
    info.Gid(gid);

    info.Gid(nullptr);
    EXPECT_EQ(memcmp(info.Gid(), gid, ROCE_GID_LEN), 0);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_ExportContent)
{
    VerbsLinkInfo info;
    uint8_t gid[ROCE_GID_LEN] = {};
    FillGid(gid, 0);
    info.Qpn(4321);
    info.Lid(9);
    info.PortNum(1);
    info.SqDepth(64);
    info.RqDepth(128);
    info.Gid(gid);

    std::string text = ExportLinkText(info);
    EXPECT_NE(text.find(GEN_INFO_VERSION), std::string::npos);
    EXPECT_NE(text.find(VERBS_INFO_QP_QPN), std::string::npos);
    EXPECT_NE(text.find(VERBS_INFO_QP_LID), std::string::npos);
    EXPECT_NE(text.find(VERBS_INFO_QP_PORT), std::string::npos);
    EXPECT_NE(text.find(VERBS_INFO_QP_SQ_DEPTH), std::string::npos);
    EXPECT_NE(text.find(VERBS_INFO_QP_RQ_DEPTH), std::string::npos);
    EXPECT_NE(text.find(VERBS_INFO_QP_GID), std::string::npos);
    /* the gid is carried as base64 text rather than the raw bytes */
    EXPECT_NE(text.find("AAECAwQFBgcICQoLDA0ODw=="), std::string::npos);
    EXPECT_NE(text.find("qp.qpn = 4321"), std::string::npos);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_RoundTrip)
{
    VerbsLinkInfo source;
    uint8_t gid[ROCE_GID_LEN] = {};
    FillGid(gid, 0x30);
    source.Type(HOST_RDMA_GENETTY);
    source.Vendor(VT_KUNPENG_CPU);
    source.Psn(12345);
    source.Mtu(4);
    source.Qpn(5678);
    source.Lid(3);
    source.PortNum(2);
    source.SqDepth(16);
    source.RqDepth(32);
    source.Gid(gid);

    auto text = ExportLinkText(source);

    VerbsLinkInfo target;
    EXPECT_EQ(ImportLinkText(target, text), GEN_OK);
    EXPECT_EQ(target.Version(), source.Version());
    EXPECT_EQ(target.Qpn(), source.Qpn());
    EXPECT_EQ(target.Psn(), source.Psn());
    EXPECT_EQ(target.Mtu(), source.Mtu());
    EXPECT_EQ(target.Lid(), source.Lid());
    EXPECT_EQ(target.PortNum(), source.PortNum());
    EXPECT_EQ(target.SqDepth(), source.SqDepth());
    EXPECT_EQ(target.RqDepth(), source.RqDepth());
    EXPECT_EQ(memcmp(target.Gid(), source.Gid(), ROCE_GID_LEN), 0);
    EXPECT_EQ(ExportLinkText(target), text);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_ImportPortOutOfRange)
{
    VerbsLinkInfo source;
    source.PortNum(1);
    auto text = ExportLinkText(source);

    auto pos = text.find("qp.port = 1");
    EXPECT_NE(pos, std::string::npos);
    text.replace(pos, strlen("qp.port = 1"), "qp.port = 300");

    VerbsLinkInfo target;
    EXPECT_EQ(ImportLinkText(target, text), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_ImportBadGidLength)
{
    VerbsLinkInfo source;
    uint8_t gid[ROCE_GID_LEN] = {};
    FillGid(gid, 0);
    source.Gid(gid);
    auto text = ExportLinkText(source);

    auto pos = text.find("AAECAwQFBgcICQoLDA0ODw==");
    EXPECT_NE(pos, std::string::npos);
    text.replace(pos, strlen("AAECAwQFBgcICQoLDA0ODw=="), "AAAA");

    VerbsLinkInfo target;
    EXPECT_EQ(ImportLinkText(target, text), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_ImportMalformed)
{
    VerbsLinkInfo info;
    EXPECT_EQ(ImportLinkText(info, "no equal sign\n"), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_ImportInvalidParam)
{
    VerbsLinkInfo info;
    genetty_link_info_t empty {};
    EXPECT_EQ(info.Import(nullptr, 0), GEN_INVALID_PARAM);
    EXPECT_EQ(info.Import(&empty, 0), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_ExportInvalidParam)
{
    VerbsLinkInfo info;
    EXPECT_EQ(info.Export(nullptr, 0), GEN_INVALID_PARAM);
}

TEST_F(GenettyVerbsInfoTest, LinkInfo_Ostream)
{
    VerbsLinkInfo info;
    uint8_t gid[ROCE_GID_LEN] = {};
    FillGid(gid, 0);
    info.Qpn(1024);
    info.Lid(5);
    info.PortNum(1);
    info.Gid(gid);

    std::ostringstream oss;
    oss << info;
    EXPECT_NE(oss.str().find("verbs exchange info"), std::string::npos);
    EXPECT_NE(oss.str().find("qpn: 1024"), std::string::npos);
    EXPECT_NE(oss.str().find("1:203:405:607:809:a0b:c0d:e0f"), std::string::npos);
}
