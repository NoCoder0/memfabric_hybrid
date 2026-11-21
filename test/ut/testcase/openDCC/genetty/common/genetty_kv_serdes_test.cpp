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
#include <sstream>
#include <string>

#include "common/genetty_kv_serdes.h"

using namespace ock::openDCC;

class GenettyKvSerdesTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================
// KVWriter
// ============================================================
TEST_F(GenettyKvSerdesTest, Writer_PutAndSerialize)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("int16", "-123"), GEN_OK);
    EXPECT_EQ(writer.Put("uint32", "4294967295"), GEN_OK);
    EXPECT_EQ(writer.Put("double", "-2.25"), GEN_OK);
    EXPECT_EQ(writer.Put("str", "hello-world"), GEN_OK);
    EXPECT_EQ(writer.Serialize(), GEN_OK);
    EXPECT_TRUE(writer.LastError().empty());

    std::ostringstream oss;
    oss << writer;
    EXPECT_NE(oss.str().find("int16 = -123\n"), std::string::npos);
    EXPECT_NE(oss.str().find("uint32 = 4294967295\n"), std::string::npos);
    EXPECT_NE(oss.str().find("double = -2.25\n"), std::string::npos);
    EXPECT_NE(oss.str().find("str = hello-world\n"), std::string::npos);
    EXPECT_NE(oss.str().find("key count: 4"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Writer_SerializeSortedByKey)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("b", "2"), GEN_OK);
    EXPECT_EQ(writer.Put("a", "1"), GEN_OK);
    EXPECT_EQ(writer.Serialize(), GEN_OK);

    std::ostringstream oss;
    oss << writer;
    auto posA = oss.str().find("a = 1");
    auto posB = oss.str().find("b = 2");
    EXPECT_NE(posA, std::string::npos);
    EXPECT_NE(posB, std::string::npos);
    EXPECT_LT(posA, posB);
}

TEST_F(GenettyKvSerdesTest, Writer_PutTrimsKeyAndValue)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("  key  ", "  value  "), GEN_OK);
    EXPECT_EQ(writer.Serialize(), GEN_OK);

    std::ostringstream oss;
    oss << writer;
    EXPECT_NE(oss.str().find("key = value\n"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Writer_PutSameKeyOverwrites)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("k", "1"), GEN_OK);
    EXPECT_EQ(writer.Put("k", "2"), GEN_OK);
    EXPECT_EQ(writer.Serialize(), GEN_OK);

    std::ostringstream oss;
    oss << writer;
    EXPECT_NE(oss.str().find("k = 2\n"), std::string::npos);
    EXPECT_EQ(oss.str().find("k = 1\n"), std::string::npos);
}

/* the existed key must be replaced instead of being inserted again */
TEST_F(GenettyKvSerdesTest, Writer_PutExistingKeyReplaced)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("k", "old-value"), GEN_OK);
    EXPECT_TRUE(writer.LastError().empty());

    EXPECT_EQ(writer.Put("k", "new value"), GEN_OK);
    EXPECT_TRUE(writer.LastError().empty());

    EXPECT_EQ(writer.Put("other", "v"), GEN_OK);
    EXPECT_EQ(writer.Serialize(), GEN_OK);

    std::ostringstream oss;
    oss << writer;
    EXPECT_NE(oss.str().find("k = new value\n"), std::string::npos);
    EXPECT_EQ(oss.str().find("old-value"), std::string::npos);
    EXPECT_NE(oss.str().find("other = v\n"), std::string::npos);
    /* the replaced key must not be counted twice */
    EXPECT_NE(oss.str().find("key count: 2"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Writer_PutEmptyKeyRejected)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("", "v"), GEN_INVALID_PARAM);
    EXPECT_FALSE(writer.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Writer_PutBlankKeyRejected)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("   ", "v"), GEN_INVALID_PARAM);
    EXPECT_FALSE(writer.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Writer_PutInvalidKeyRejected)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("a=b", "v"), GEN_INVALID_PARAM);
    EXPECT_FALSE(writer.LastError().empty());
    EXPECT_EQ(writer.Put("#a", "v"), GEN_INVALID_PARAM);
    EXPECT_FALSE(writer.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Writer_PutLineBreakValueRejected)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("k", "a\nb"), GEN_INVALID_PARAM);
    EXPECT_FALSE(writer.LastError().empty());
    EXPECT_EQ(writer.Put("k", "a\rb"), GEN_INVALID_PARAM);
    EXPECT_FALSE(writer.LastError().empty());
}

/* the key must not contain blank, while the value can */
TEST_F(GenettyKvSerdesTest, Writer_PutBlankInKeyRejected)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("a b", "v"), GEN_INVALID_PARAM);
    EXPECT_FALSE(writer.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Writer_PutBlankInValueAccepted)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("k", "hello world"), GEN_OK);
    EXPECT_TRUE(writer.LastError().empty());
    EXPECT_EQ(writer.Serialize(), GEN_OK);

    std::ostringstream oss;
    oss << writer;
    EXPECT_NE(oss.str().find("k = hello world\n"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Writer_PutSuccessClearsError)
{
    KVWriter writer;
    EXPECT_EQ(writer.Put("", "v"), GEN_INVALID_PARAM);
    EXPECT_FALSE(writer.LastError().empty());
    EXPECT_EQ(writer.Put("k", "v"), GEN_OK);
    EXPECT_TRUE(writer.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Writer_ErrorIsPerObject)
{
    KVWriter first;
    KVWriter second;
    EXPECT_EQ(first.Put("", "v"), GEN_INVALID_PARAM);
    EXPECT_FALSE(first.LastError().empty());
    EXPECT_TRUE(second.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Writer_SerializeEmpty)
{
    KVWriter writer;
    EXPECT_EQ(writer.Serialize(), GEN_OK);
    EXPECT_TRUE(writer.LastError().empty());

    std::ostringstream oss;
    oss << writer;
    EXPECT_NE(oss.str().find("key count: 0"), std::string::npos);
}

// ============================================================
// KVReader
// ============================================================
TEST_F(GenettyKvSerdesTest, Reader_DeserializeSkipCommentAndBlank)
{
    KVReader reader("# version = 1\n\n   # indented comment\na = 1\r\nb=2\n   c   =   hello   \n");
    EXPECT_EQ(reader.Deserialize(), GEN_OK);
    EXPECT_TRUE(reader.LastError().empty());

    int32_t a = 0;
    int32_t b = 0;
    std::string c;
    EXPECT_EQ(reader.GetInt32("a", a), GEN_OK);
    EXPECT_EQ(a, 1);
    EXPECT_EQ(reader.GetInt32("b", b), GEN_OK);
    EXPECT_EQ(b, 2);
    EXPECT_EQ(reader.GetString("c", c), GEN_OK);
    EXPECT_EQ(c, "hello");

    std::ostringstream oss;
    oss << reader;
    EXPECT_NE(oss.str().find("key count: 3"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Reader_GetAllTypes)
{
    KVReader reader("i16 = -123\nu16 = 65535\ni32 = -2147483648\nu32 = 4294967295\n"
                    "i64 = -9223372036854775808\nu64 = 18446744073709551615\n"
                    "f = 3.5\nd = -2.25\ns = hello-world\n");
    EXPECT_EQ(reader.Deserialize(), GEN_OK);

    int16_t i16 = 0;
    uint16_t u16 = 0;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    float f = 0;
    double d = 0;
    std::string s;

    EXPECT_EQ(reader.GetInt16("i16", i16), GEN_OK);
    EXPECT_EQ(i16, -123);
    EXPECT_EQ(reader.GetUInt16("u16", u16), GEN_OK);
    EXPECT_EQ(u16, 65535);
    EXPECT_EQ(reader.GetInt32("i32", i32), GEN_OK);
    EXPECT_EQ(i32, INT32_MIN);
    EXPECT_EQ(reader.GetUInt32("u32", u32), GEN_OK);
    EXPECT_EQ(u32, UINT32_MAX);
    EXPECT_EQ(reader.GetInt64("i64", i64), GEN_OK);
    EXPECT_EQ(i64, INT64_MIN);
    EXPECT_EQ(reader.GetUInt64("u64", u64), GEN_OK);
    EXPECT_EQ(u64, UINT64_MAX);
    EXPECT_EQ(reader.GetFloat("f", f), GEN_OK);
    EXPECT_FLOAT_EQ(f, 3.5F);
    EXPECT_EQ(reader.GetDouble("d", d), GEN_OK);
    EXPECT_DOUBLE_EQ(d, -2.25);
    EXPECT_EQ(reader.GetString("s", s), GEN_OK);
    EXPECT_EQ(s, "hello-world");
    EXPECT_TRUE(reader.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Reader_GetMissingKeyFailed)
{
    KVReader reader("a = 1\n");
    EXPECT_EQ(reader.Deserialize(), GEN_OK);

    int32_t value = 0;
    EXPECT_EQ(reader.GetInt32("missing", value), GEN_INVALID_PARAM);
    EXPECT_NE(reader.LastError().find("missing"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Reader_GetInvalidNumberFailed)
{
    KVReader reader("garbage = 12abc\nnotnum = abc\nsmall = 40000\nnegative = -1\n");
    EXPECT_EQ(reader.Deserialize(), GEN_OK);

    int16_t i16 = 0;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    EXPECT_EQ(reader.GetInt32("garbage", i32), GEN_INVALID_PARAM);
    EXPECT_FALSE(reader.LastError().empty());
    EXPECT_EQ(reader.GetInt32("notnum", i32), GEN_INVALID_PARAM);
    EXPECT_FALSE(reader.LastError().empty());
    EXPECT_EQ(reader.GetInt16("small", i16), GEN_INVALID_PARAM);
    EXPECT_FALSE(reader.LastError().empty());
    EXPECT_EQ(reader.GetUInt32("negative", u32), GEN_INVALID_PARAM);
    EXPECT_FALSE(reader.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Reader_GetInvalidRealFailed)
{
    KVReader reader("f = 1.5x\nd = abc\n");
    EXPECT_EQ(reader.Deserialize(), GEN_OK);

    float f = 0;
    double d = 0;
    EXPECT_EQ(reader.GetFloat("f", f), GEN_INVALID_PARAM);
    EXPECT_FALSE(reader.LastError().empty());
    EXPECT_EQ(reader.GetDouble("d", d), GEN_INVALID_PARAM);
    EXPECT_FALSE(reader.LastError().empty());
}

TEST_F(GenettyKvSerdesTest, Reader_DeserializeMissingEqualFailed)
{
    KVReader reader("no-equal-sign\n");
    EXPECT_EQ(reader.Deserialize(), GEN_INVALID_PARAM);
    EXPECT_NE(reader.LastError().find("missing '='"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Reader_DeserializeEmptyKeyFailed)
{
    KVReader reader(" = 1\n");
    EXPECT_EQ(reader.Deserialize(), GEN_INVALID_PARAM);
    EXPECT_NE(reader.LastError().find("key is empty"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Reader_DeserializeBlankInKeyFailed)
{
    KVReader reader("a b = 1\n");
    EXPECT_EQ(reader.Deserialize(), GEN_INVALID_PARAM);
    EXPECT_NE(reader.LastError().find("contains blank"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Reader_GetValueWithBlank)
{
    KVReader reader("k = hello world\n");
    EXPECT_EQ(reader.Deserialize(), GEN_OK);
    EXPECT_TRUE(reader.LastError().empty());

    std::string value;
    EXPECT_EQ(reader.GetString("k", value), GEN_OK);
    EXPECT_EQ(value, "hello world");
}

TEST_F(GenettyKvSerdesTest, Reader_DeserializeDuplicateKeyFailed)
{
    KVReader reader("a = 1\na = 2\n");
    EXPECT_EQ(reader.Deserialize(), GEN_INVALID_PARAM);
    EXPECT_NE(reader.LastError().find("duplicated key"), std::string::npos);
}

TEST_F(GenettyKvSerdesTest, Reader_DeserializeFailureDropsPartialData)
{
    KVReader reader("a = 1\nbad-line\n");
    EXPECT_EQ(reader.Deserialize(), GEN_INVALID_PARAM);
    EXPECT_FALSE(reader.LastError().empty());

    int32_t value = 0;
    EXPECT_EQ(reader.GetInt32("a", value), GEN_INVALID_PARAM);
}

TEST_F(GenettyKvSerdesTest, Reader_DeserializeEmptyDataOk)
{
    KVReader reader("");
    EXPECT_EQ(reader.Deserialize(), GEN_OK);
    EXPECT_TRUE(reader.LastError().empty());

    int32_t value = 0;
    EXPECT_EQ(reader.GetInt32("a", value), GEN_INVALID_PARAM);
}

TEST_F(GenettyKvSerdesTest, Reader_DeserializeTwice)
{
    KVReader reader("a = 1\n");
    EXPECT_EQ(reader.Deserialize(), GEN_OK);
    EXPECT_EQ(reader.Deserialize(), GEN_OK);

    int32_t value = 0;
    EXPECT_EQ(reader.GetInt32("a", value), GEN_OK);
    EXPECT_EQ(value, 1);

    std::ostringstream oss;
    oss << reader;
    EXPECT_NE(oss.str().find("a = 1"), std::string::npos);
}
