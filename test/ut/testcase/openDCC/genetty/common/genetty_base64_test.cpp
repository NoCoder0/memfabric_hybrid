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
#include <initializer_list>
#include <string>
#include <vector>

#include "common/genetty_base64.h"

using namespace ock::openDCC;

namespace {
std::vector<uint8_t> Base64Bytes(std::initializer_list<uint8_t> list)
{
    return std::vector<uint8_t>(list);
}

std::vector<uint8_t> MakeSequence(size_t len)
{
    std::vector<uint8_t> data(len);
    for (size_t i = 0; i < len; i++) {
        data[i] = static_cast<uint8_t>((i * 37 + len) & 0xFF);
    }

    return data;
}

std::string EncodeText(const std::vector<uint8_t> &data)
{
    std::string text;
    EXPECT_EQ(Base64::Encode(data.data(), static_cast<uint32_t>(data.size()), text), GEN_OK);
    return text;
}

size_t PaddingOf(const std::string &text)
{
    if (!text.empty() && text[text.size() - 1] == '=') {
        return (text.size() >= 2 && text[text.size() - 2] == '=') ? 2 : 1;
    }

    return 0;
}

bool DecodeText(const std::string &text, std::vector<uint8_t> &out)
{
    /* the decoded data is always smaller than the text, so the text length is a safe capacity */
    std::vector<uint8_t> buffer(text.size() + 4);
    if (Base64::Decode(text, buffer.data(), static_cast<uint32_t>(buffer.size())) != GEN_OK) {
        return false;
    }

    buffer.resize((text.size() / 4) * 3 - PaddingOf(text));
    out = buffer;
    return true;
}
} // namespace

class GenettyBase64Test : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================
// Base64::Encode
// ============================================================
TEST_F(GenettyBase64Test, Encode_Rfc4648Vectors)
{
    EXPECT_EQ(EncodeText(Base64Bytes({})), "");
    EXPECT_EQ(EncodeText(Base64Bytes({'f'})), "Zg==");
    EXPECT_EQ(EncodeText(Base64Bytes({'f', 'o'})), "Zm8=");
    EXPECT_EQ(EncodeText(Base64Bytes({'f', 'o', 'o'})), "Zm9v");
    EXPECT_EQ(EncodeText(Base64Bytes({'f', 'o', 'o', 'b'})), "Zm9vYg==");
    EXPECT_EQ(EncodeText(Base64Bytes({'f', 'o', 'o', 'b', 'a'})), "Zm9vYmE=");
    EXPECT_EQ(EncodeText(Base64Bytes({'f', 'o', 'o', 'b', 'a', 'r'})), "Zm9vYmFy");
}

TEST_F(GenettyBase64Test, Encode_BinaryData)
{
    EXPECT_EQ(EncodeText(Base64Bytes({0x00, 0xFF, 0x10})), "AP8Q");
    EXPECT_EQ(EncodeText(Base64Bytes({0xFF, 0xFF, 0xFF})), "////");
    EXPECT_EQ(EncodeText(Base64Bytes({0x00, 0x00, 0x00})), "AAAA");
}

TEST_F(GenettyBase64Test, Encode_InvalidParam)
{
    std::string text = "stale";

    /* a null data is rejected when the length is not 0 */
    EXPECT_EQ(Base64::Encode(nullptr, 8, text), GEN_INVALID_PARAM);
    EXPECT_TRUE(text.empty());
}

TEST_F(GenettyBase64Test, Encode_ZeroLength)
{
    std::string text = "stale";
    EXPECT_EQ(Base64::Encode(nullptr, 0, text), GEN_OK);
    EXPECT_TRUE(text.empty());

    text = "stale";
    uint8_t value = 0x01;
    EXPECT_EQ(Base64::Encode(&value, 0, text), GEN_OK);
    EXPECT_TRUE(text.empty());
}

TEST_F(GenettyBase64Test, EncodedLength_KnownValues)
{
    EXPECT_EQ(Base64::EncodedLength(0), 0U);
    EXPECT_EQ(Base64::EncodedLength(1), 4U);
    EXPECT_EQ(Base64::EncodedLength(2), 4U);
    EXPECT_EQ(Base64::EncodedLength(3), 4U);
    EXPECT_EQ(Base64::EncodedLength(4), 8U);
    EXPECT_EQ(Base64::EncodedLength(5), 8U);
    EXPECT_EQ(Base64::EncodedLength(6), 8U);
    EXPECT_EQ(Base64::EncodedLength(16), 24U);
    EXPECT_EQ(Base64::EncodedLength(256), 344U);
}

TEST_F(GenettyBase64Test, Encode_LengthIsMultipleOfFour)
{
    for (size_t len = 0; len <= 64; len++) {
        auto text = EncodeText(MakeSequence(len));
        EXPECT_EQ(text.size() % 4, 0U);
        EXPECT_EQ(text.size(), Base64::EncodedLength(len));
    }
}

TEST_F(GenettyBase64Test, Encode_AllByteValues)
{
    auto data = MakeSequence(256);
    auto text = EncodeText(data);
    EXPECT_EQ(text.size(), 344U);

    std::vector<uint8_t> decoded;
    EXPECT_TRUE(DecodeText(text, decoded));
    EXPECT_EQ(decoded, data);
}

// ============================================================
// Base64::Decode
// ============================================================
TEST_F(GenettyBase64Test, Decode_Rfc4648Vectors)
{
    std::vector<uint8_t> data;

    EXPECT_TRUE(DecodeText("", data));
    EXPECT_TRUE(data.empty());

    EXPECT_TRUE(DecodeText("Zg==", data));
    EXPECT_EQ(data, Base64Bytes({'f'}));

    EXPECT_TRUE(DecodeText("Zm8=", data));
    EXPECT_EQ(data, Base64Bytes({'f', 'o'}));

    EXPECT_TRUE(DecodeText("Zm9v", data));
    EXPECT_EQ(data, Base64Bytes({'f', 'o', 'o'}));

    EXPECT_TRUE(DecodeText("Zm9vYmFy", data));
    EXPECT_EQ(data, Base64Bytes({'f', 'o', 'o', 'b', 'a', 'r'}));
}

TEST_F(GenettyBase64Test, Decode_BinaryData)
{
    std::vector<uint8_t> data;
    EXPECT_TRUE(DecodeText("AP8Q", data));
    EXPECT_EQ(data, Base64Bytes({0x00, 0xFF, 0x10}));
}

TEST_F(GenettyBase64Test, Decode_RoundTripAllLengths)
{
    for (size_t len = 0; len <= 100; len++) {
        auto src = MakeSequence(len);
        std::vector<uint8_t> decoded;
        EXPECT_TRUE(DecodeText(EncodeText(src), decoded));
        EXPECT_EQ(decoded, src);
    }
}

TEST_F(GenettyBase64Test, Decode_InvalidLength)
{
    uint8_t buffer[16] = {};
    EXPECT_EQ(Base64::Decode("Zg=", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("Z", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("Zm9vY", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("Zg===", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
}

TEST_F(GenettyBase64Test, Decode_InvalidCharacter)
{
    uint8_t buffer[16] = {};
    EXPECT_EQ(Base64::Decode("A*AA", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("AA A", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("AA\nA", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("-AAA", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
}

TEST_F(GenettyBase64Test, Decode_MisplacedPadding)
{
    uint8_t buffer[16] = {};
    EXPECT_EQ(Base64::Decode("=AAA", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("AA=A", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("AB==AA==", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("Z===", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
}

TEST_F(GenettyBase64Test, Decode_BufferTooSmall)
{
    uint8_t buffer[8] = {};

    /* "Zm9vYmFy" carries 6 bytes, so a 5 bytes buffer is not enough */
    EXPECT_EQ(Base64::Decode("Zm9vYmFy", buffer, 5), GEN_INVALID_PARAM);
    EXPECT_EQ(Base64::Decode("Zm9vYmFy", buffer, 6), GEN_OK);
    EXPECT_EQ(Base64::Decode("Zm9vYmFy", buffer, sizeof(buffer)), GEN_OK);
}

TEST_F(GenettyBase64Test, Decode_NullBuffer)
{
    /* the empty text needs no buffer at all */
    EXPECT_EQ(Base64::Decode("", nullptr, 0), GEN_OK);

    /* a non empty text always needs a buffer to hold the decoded bytes */
    EXPECT_EQ(Base64::Decode("Zg==", nullptr, 4), GEN_INVALID_PARAM);
}

TEST_F(GenettyBase64Test, Decode_FailureKeepsBufferUntouched)
{
    uint8_t buffer[8] = {};
    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = 0xA5;
    }

    EXPECT_EQ(Base64::Decode("A*AA", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    for (size_t i = 0; i < sizeof(buffer); i++) {
        EXPECT_EQ(buffer[i], 0xA5);
    }

    EXPECT_EQ(Base64::Decode("Zg=", buffer, sizeof(buffer)), GEN_INVALID_PARAM);
    for (size_t i = 0; i < sizeof(buffer); i++) {
        EXPECT_EQ(buffer[i], 0xA5);
    }
}
