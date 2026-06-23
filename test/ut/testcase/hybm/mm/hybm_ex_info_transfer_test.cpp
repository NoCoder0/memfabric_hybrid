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
#include <cstring>

#include "hybm_ex_info_transfer.h"

using namespace ock::mf;

struct TestPodType {
    uint32_t a;
    uint64_t b;
    char c[8];
};

static_assert(std::is_trivial<TestPodType>::value, "TestPodType must be trivial");

class ExInfoTransferTest : public testing::Test {
public:
    void SetUp() override
    {
        bzero(&exchangeInfo_, sizeof(exchangeInfo_));
    }
    void TearDown() override {}

protected:
    hybm_exchange_info exchangeInfo_;
};

// ==================== ExchangeInfoWriter Tests ====================

TEST_F(ExInfoTransferTest, Writer_NullInfo_ReturnsError)
{
    ExchangeInfoWriter writer(nullptr);
    int data = 42; // 42
    EXPECT_NE(writer.Append(&data, sizeof(data)), 0);
    EXPECT_NE(writer.Append(data), 0);
}

TEST_F(ExInfoTransferTest, Writer_Append_Success)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    EXPECT_EQ(exchangeInfo_.descLen, 0U);

    uint32_t val = 0x12345678;
    ASSERT_EQ(writer.Append(&val, sizeof(val)), 0);
    EXPECT_EQ(exchangeInfo_.descLen, sizeof(val));

    uint32_t readBack = 0;
    std::copy_n(exchangeInfo_.desc, sizeof(readBack), reinterpret_cast<uint8_t *>(&readBack));
    EXPECT_EQ(readBack, val);
}

TEST_F(ExInfoTransferTest, Writer_AppendMultiple)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    uint32_t a = 0x1111;
    uint64_t b = 0x22222222;
    ASSERT_EQ(writer.Append(&a, sizeof(a)), 0);
    ASSERT_EQ(writer.Append(&b, sizeof(b)), 0);
    EXPECT_EQ(exchangeInfo_.descLen, sizeof(a) + sizeof(b));
}

TEST_F(ExInfoTransferTest, Writer_AppendOverflow)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    ASSERT_EQ(writer.Append(exchangeInfo_.desc, sizeof(exchangeInfo_.desc)), 0);
    char c = 'x';
    EXPECT_NE(writer.Append(&c, sizeof(c)), 0);
}

TEST_F(ExInfoTransferTest, Writer_AppendTemplate)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    TestPodType pod{1, 2, "hello"}; // 1, 2
    EXPECT_EQ(writer.Append(pod), 0);
    EXPECT_EQ(exchangeInfo_.descLen, sizeof(TestPodType));
}

// ==================== ExchangeInfoReader Tests ====================

TEST_F(ExInfoTransferTest, Reader_NullInfo_ReturnsError)
{
    ExchangeInfoReader reader(nullptr);
    EXPECT_EQ(reader.LeftBytes(), 0U);
    EXPECT_TRUE(reader.LeftToString().empty());

    int data = 0;
    EXPECT_NE(reader.Read(&data, sizeof(data)), 0);
    EXPECT_NE(reader.Test(&data, sizeof(data)), 0);
}

TEST_F(ExInfoTransferTest, Reader_ReadAfterWrite)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    uint32_t written = 0xDEADBEEF;
    ASSERT_EQ(writer.Append(&written, sizeof(written)), 0);

    ExchangeInfoReader reader(&exchangeInfo_);
    EXPECT_GT(reader.LeftBytes(), 0U);
    uint32_t readBack = 0;
    EXPECT_EQ(reader.Read(&readBack, sizeof(readBack)), 0);
    EXPECT_EQ(readBack, written);
    EXPECT_EQ(reader.LeftBytes(), 0U);
}

TEST_F(ExInfoTransferTest, Reader_ReadMultiple)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    uint32_t a = 0xAAAA;
    uint64_t b = 0xBBBBBBBBBBBB;
    ASSERT_EQ(writer.Append(&a, sizeof(a)), 0);
    ASSERT_EQ(writer.Append(&b, sizeof(b)), 0);

    ExchangeInfoReader reader(&exchangeInfo_);
    uint32_t readA = 0;
    uint64_t readB = 0;
    EXPECT_EQ(reader.Read(&readA, sizeof(readA)), 0);
    EXPECT_EQ(readA, a);
    EXPECT_EQ(reader.Read(&readB, sizeof(readB)), 0);
    EXPECT_EQ(readB, b);
    EXPECT_EQ(reader.LeftBytes(), 0U);
}

TEST_F(ExInfoTransferTest, Reader_ReadBeyondData)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    uint32_t val = 42; // 42
    ASSERT_EQ(writer.Append(&val, sizeof(val)), 0);

    ExchangeInfoReader reader(&exchangeInfo_);
    uint64_t bigBuf = 0;
    EXPECT_NE(reader.Read(&bigBuf, sizeof(bigBuf)), 0);
    EXPECT_NE(reader.Test(&bigBuf, sizeof(bigBuf)), 0);
}

TEST_F(ExInfoTransferTest, Reader_TestDoesNotAdvanceOffset)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    uint32_t val = 0x12345678;
    ASSERT_EQ(writer.Append(&val, sizeof(val)), 0);

    ExchangeInfoReader reader(&exchangeInfo_);
    uint32_t read1 = 0;
    ASSERT_EQ(reader.Test(&read1, sizeof(read1)), 0);
    EXPECT_EQ(read1, val);

    // After Test, offset unchanged, Read should get same value
    uint32_t read2 = 0;
    ASSERT_EQ(reader.Read(&read2, sizeof(read2)), 0);
    EXPECT_EQ(read2, val);
}

TEST_F(ExInfoTransferTest, Reader_LeftToString)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    uint32_t val = 0x12345678;
    ASSERT_EQ(writer.Append(&val, sizeof(val)), 0);

    ExchangeInfoReader reader(&exchangeInfo_);
    auto remaining = reader.LeftToString();
    EXPECT_EQ(remaining.size(), sizeof(val));
    EXPECT_EQ(reader.LeftBytes(), 0U);
}

TEST_F(ExInfoTransferTest, Reader_LeftToStringAfterPartialRead)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    uint32_t a = 0xAAAA;
    uint32_t b = 0xBBBB;
    ASSERT_EQ(writer.Append(&a, sizeof(a)), 0);
    ASSERT_EQ(writer.Append(&b, sizeof(b)), 0);

    ExchangeInfoReader reader(&exchangeInfo_);
    uint32_t readA = 0;
    ASSERT_EQ(reader.Read(&readA, sizeof(readA)), 0);

    auto remaining = reader.LeftToString();
    ASSERT_EQ(remaining.size(), sizeof(b));
    uint32_t readB = 0;
    std::copy_n(remaining.data(), sizeof(readB), reinterpret_cast<uint8_t *>(&readB));
    EXPECT_EQ(readB, b);
}

TEST_F(ExInfoTransferTest, Reader_ReadTemplate)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    TestPodType pod{10, 20, "test"};
    ASSERT_EQ(writer.Append(pod), 0);

    ExchangeInfoReader reader(&exchangeInfo_);
    TestPodType readPod{};
    EXPECT_EQ(reader.Read(readPod), 0);
    EXPECT_EQ(readPod.a, 10U);
    EXPECT_EQ(readPod.b, 20U);
    EXPECT_STREQ(readPod.c, "test");
}

TEST_F(ExInfoTransferTest, Reader_TestTemplate)
{
    ExchangeInfoWriter writer(&exchangeInfo_);
    TestPodType pod{100, 200, "test2"};
    ASSERT_EQ(writer.Append(pod), 0);

    ExchangeInfoReader reader(&exchangeInfo_);
    TestPodType testPod{};
    EXPECT_EQ(reader.Test(testPod), 0);
    EXPECT_EQ(testPod.a, 100U);
    EXPECT_EQ(testPod.b, 200U);

    TestPodType readPod{};
    ASSERT_EQ(reader.Read(readPod), 0);
    EXPECT_EQ(readPod.a, 100U);
}

// ==================== LiteralExInfoTranslater Tests ====================

TEST_F(ExInfoTransferTest, LiteralSerializeDeserializeRoundTrip)
{
    TestPodType original{42, 0x12345678ABCD, "poddata"};
    std::string encoded;
    LiteralExInfoTranslater<TestPodType> translater;
    ASSERT_EQ(translater.Serialize(original, encoded), 0);
    EXPECT_EQ(encoded.size(), sizeof(TestPodType));

    TestPodType decoded{};
    EXPECT_EQ(translater.Deserialize(encoded, decoded), 0);
    EXPECT_EQ(decoded.a, original.a);
    EXPECT_EQ(decoded.b, original.b);
    EXPECT_STREQ(decoded.c, original.c);
}

TEST_F(ExInfoTransferTest, LiteralDeserializeWrongSize)
{
    LiteralExInfoTranslater<TestPodType> translater;
    TestPodType decoded{};
    EXPECT_NE(translater.Deserialize("too short", decoded), 0);
}
