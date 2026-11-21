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
#ifndef MF_GENETTY_BASE64_H
#define MF_GENETTY_BASE64_H

#include "genetty_def.h"
#include "genetty_define.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ock {
namespace openDCC {
/*
 * Base64 codec with the standard alphabet, that is A-Z a-z 0-9 + /, and the '=' padding.
 *
 * The encoded text is always padded, so the text length is always a multiple of 4. The decode side
 * requires the padded form, and rejects any character outside the alphabet or any misplaced padding.
 */
class Base64 {
public:
    /**
     * @brief Encode the given binary data to base64 text
     *
     * @param data         [in] binary data to be encoded, can be null only when length is 0
     * @param length       [in] length of the binary data in bytes
     * @param output       [out] encoded text, empty when the input data is empty
     * @return GEN_OK if successful, GEN_INVALID_PARAM if the parameters are invalid
     */
    static Result Encode(const uint8_t *data, uint32_t length, std::string &output) noexcept;

    /**
     * @brief Decode the given base64 text to binary data
     *
     * @param src          [in] base64 text to be decoded, which must be padded
     * @param data         [out] buffer to hold the decoded data, untouched when failed
     * @param length       [in] capacity of the output buffer in bytes
     * @return GEN_OK if successful, GEN_INVALID_PARAM if the parameters are invalid, the text is
     *         malformed, or the output buffer is too small
     */
    static Result Decode(const std::string &src, uint8_t *data, uint32_t length) noexcept;

    /**
     * @brief Get the base64 text length of the given binary data
     *
     * @param length       [in] length of the binary data in bytes
     * @return the encoded text length, that is 4 characters per 3 bytes plus the padding
     */
    static constexpr size_t EncodedLength(size_t length) noexcept;

private:
    /**
     * @brief Translate one base64 character to its 6 bits value
     *
     * @param ch               [in] character to be translated
     * @return the 6 bits value, or -1 when the character is not in the alphabet
     */
    static int DecodeChar(char ch) noexcept;
};

ALWAYS_INLINE int Base64::DecodeChar(char ch) noexcept
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }

    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }

    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }

    if (ch == '+') {
        return 62;
    }

    if (ch == '/') {
        return 63;
    }

    return -1;
}

ALWAYS_INLINE Result Base64::Encode(const uint8_t *data, uint32_t length, std::string &output) noexcept
{
    static constexpr char CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /* step1: check the parameters, a null data is allowed only when the length is 0 */
    output.clear();
    if (length > 0 && data == nullptr) {
        return GEN_INVALID_PARAM;
    }

    if (length == 0) {
        return GEN_OK;
    }

    /* step2: reserve the output, every 3 input bytes are encoded to 4 characters */
    output.reserve(EncodedLength(length));

    /* step3: encode the full groups of 3 bytes */
    size_t pos = 0;
    while ((pos + 3) <= length) {
        uint32_t block = (static_cast<uint32_t>(data[pos]) << 16) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
                         static_cast<uint32_t>(data[pos + 2]);
        output.push_back(CHARS[(block >> 18) & 0x3F]);
        output.push_back(CHARS[(block >> 12) & 0x3F]);
        output.push_back(CHARS[(block >> 6) & 0x3F]);
        output.push_back(CHARS[block & 0x3F]);
        pos += 3;
    }

    /* step4: encode the last group of 1 or 2 bytes, filled with '=' padding */
    size_t remain = static_cast<size_t>(length) - pos;
    if (remain == 1) {
        auto block = static_cast<uint32_t>(data[pos]) << 16;
        output.push_back(CHARS[(block >> 18) & 0x3F]);
        output.push_back(CHARS[(block >> 12) & 0x3F]);
        output.append("==");
    } else if (remain == 2) {
        auto block = (static_cast<uint32_t>(data[pos]) << 16) | (static_cast<uint32_t>(data[pos + 1]) << 8);
        output.push_back(CHARS[(block >> 18) & 0x3F]);
        output.push_back(CHARS[(block >> 12) & 0x3F]);
        output.push_back(CHARS[(block >> 6) & 0x3F]);
        output.push_back('=');
    }

    return GEN_OK;
}

ALWAYS_INLINE Result Base64::Decode(const std::string &src, uint8_t *data, uint32_t length) noexcept
{
    /* step1: check the text length, the encoded text is always padded so it is a multiple of 4 */
    if ((src.size() % 4) != 0) {
        return GEN_INVALID_PARAM;
    }

    size_t padding = 0;
    if (!src.empty() && src[src.size() - 1] == '=') {
        padding = 1;
        if (src.size() >= 2 && src[src.size() - 2] == '=') {
            padding = 2;
        }
    }

    /* a single left over character can not be decoded to any byte */
    size_t validSize = src.size() - padding;
    if ((validSize % 4) == 1) {
        return GEN_INVALID_PARAM;
    }

    /* step2: check the output buffer, it must be able to hold all decoded bytes */
    size_t needSize = (validSize / 4) * 3;
    if ((validSize % 4) == 2) {
        needSize += 1;
    } else if ((validSize % 4) == 3) {
        needSize += 2;
    }

    if (needSize > length) {
        return GEN_INVALID_PARAM;
    }

    if (needSize > 0 && data == nullptr) {
        return GEN_INVALID_PARAM;
    }

    /* step3: check all characters before writing anything, so that the output stays untouched when failed */
    for (size_t pos = 0; pos < validSize; pos++) {
        if (DecodeChar(src[pos]) < 0) {
            return GEN_INVALID_PARAM;
        }
    }

    /* step4: decode the text group by group, the last group carries 1 or 2 bytes when padded */
    size_t outPos = 0;
    for (size_t pos = 0; pos < validSize; pos += 4) {
        size_t remain = validSize - pos;
        uint32_t block = 0;
        for (size_t i = 0; i < 4; i++) {
            int value = (i < remain) ? DecodeChar(src[pos + i]) : 0;
            block = (block << 6) | static_cast<uint32_t>(value);
        }

        data[outPos++] = static_cast<uint8_t>((block >> 16) & 0xFF);
        if (remain > 2) {
            data[outPos++] = static_cast<uint8_t>((block >> 8) & 0xFF);
        }
        if (remain > 3) {
            data[outPos++] = static_cast<uint8_t>(block & 0xFF);
        }
    }

    return GEN_OK;
}

ALWAYS_INLINE constexpr size_t Base64::EncodedLength(size_t length) noexcept
{
    return ((length + 2) / 3) * 4;
}
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_BASE64_H
