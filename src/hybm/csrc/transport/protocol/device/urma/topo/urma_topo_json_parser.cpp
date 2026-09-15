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

#include <climits>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "hybm_logger.h"
#include "hybm_types.h"
#include "urma_topo_json_parser.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {

constexpr size_t kStringMaxLen = 256;

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

size_t SkipSpaces(const char *begin, const char *end, size_t offset)
{
    while (offset < static_cast<size_t>(end - begin) && IsSpace(begin[offset])) {
        ++offset;
    }
    return offset;
}

bool IsHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexVal(char c)
{
    if (c >= '0' && c <= '9') {
        return static_cast<int>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<int>(c - 'a' + 10);
    }
    return static_cast<int>(c - 'A' + 10);
}

ParseResult MakeResult(Result r, size_t offset, ParserReason reason = ParserReason::NONE)
{
    return {r, offset, reason};
}

ParseResult MakeOk(size_t offset)
{
    return {BM_OK, offset, ParserReason::NONE};
}

ParseResult MakeError(size_t offset, ParserReason reason = ParserReason::BAD_VALUE)
{
    return {BM_INVALID_PARAM, offset, reason};
}

bool IsValidRange(const char *begin, const char *end)
{
    return begin != nullptr && end != nullptr && end >= begin;
}

size_t GetLen(const char *begin, const char *end)
{
    return static_cast<size_t>(end - begin);
}

ParseResult RangeError()
{
    return {BM_INVALID_PARAM, 0, ParserReason::BAD_VALUE};
}

ParseResult HandleUnicodeEscape(const char *begin, const char *end, size_t &i, size_t &strLen, size_t maxLength)
{
    if (i + 4U >= static_cast<size_t>(end - begin)) {
        return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_STRING);
    }
    for (size_t k = 1U; k <= 4U; ++k) {
        if (!IsHexDigit(begin[i + k])) {
            return MakeError(i + k, ParserReason::INVALID_ESCAPE);
        }
    }
    unsigned v = 0U;
    for (size_t k = 0U; k < 4U; ++k) {
        v = (v << 4) | static_cast<unsigned>(HexVal(begin[i + 1U + k]));
    }
    i += 5U;
    ++strLen;
    if (v >= 0xD800U && v <= 0xDBFFU) {
        if (i + 6U > static_cast<size_t>(end - begin) || begin[i] != '\\' || begin[i + 1U] != 'u') {
            return MakeError(i, ParserReason::INVALID_ESCAPE);
        }
        for (size_t k = 2U; k < 6U; ++k) {
            if (!IsHexDigit(begin[i + k])) {
                return MakeError(i + k, ParserReason::INVALID_ESCAPE);
            }
        }
        unsigned low = 0U;
        for (size_t k = 0U; k < 4U; ++k) {
            low = (low << 4) | static_cast<unsigned>(HexVal(begin[i + 2U + k]));
        }
        if (low < 0xDC00U || low > 0xDFFFU) {
            return MakeError(i, ParserReason::INVALID_ESCAPE);
        }
        i += 6U;
        ++strLen;
    } else if (v >= 0xDC00U && v <= 0xDFFFU) {
        return MakeError(i, ParserReason::INVALID_ESCAPE);
    }
    if (strLen > maxLength) {
        return MakeError(i, ParserReason::BAD_VALUE);
    }
    return MakeOk(i);
}

ParseResult HandleStringEscape(const char *begin, const char *end, size_t &i, size_t &strLen, size_t maxLength)
{
    ++i;
    if (i >= static_cast<size_t>(end - begin)) {
        return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_STRING);
    }
    const char esc = begin[i];
    switch (esc) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't': {
            ++i;
            ++strLen;
            if (strLen > maxLength) {
                return MakeError(i, ParserReason::BAD_VALUE);
            }
            return MakeOk(i);
        }
        case 'u':
            return HandleUnicodeEscape(begin, end, i, strLen, maxLength);
        default:
            return MakeError(i, ParserReason::INVALID_ESCAPE);
    }
}

ParseResult AccumulateUint32Digits(const char *begin, const char *end, size_t &i, bool negative, uint64_t &val)
{
    const auto len = static_cast<size_t>(end - begin);
    while (i < len && begin[i] >= '0' && begin[i] <= '9') {
        if (val > UINT64_MAX / 10ULL) {
            return MakeError(i, ParserReason::DEVICE_ID_OVERFLOW);
        }
        val = val * 10ULL + static_cast<uint64_t>(begin[i] - '0');
        if (val > static_cast<uint64_t>(UINT32_MAX) + (negative ? 1ULL : 0ULL)) {
            return MakeError(i, ParserReason::DEVICE_ID_OVERFLOW);
        }
        ++i;
    }
    return MakeOk(i);
}

ParseResult ParseJsonUint32ZeroPrefix(const char *begin, const char *end, size_t i, bool negative, uint32_t &value)
{
    const auto len = static_cast<size_t>(end - begin);
    if (negative && i + 1U < len && begin[i + 1U] >= '0' && begin[i + 1U] <= '9') {
        return MakeError(i + 1U, ParserReason::DEVICE_ID_LEADING_ZERO);
    }
    if (negative) {
        if (i + 1U < len && begin[i + 1U] == '.') {
            return MakeError(i + 1U, ParserReason::DEVICE_ID_FLOAT);
        }
        if (i + 1U < len && (begin[i + 1U] == 'e' || begin[i + 1U] == 'E')) {
            return MakeError(i + 1U, ParserReason::DEVICE_ID_EXPONENT);
        }
        value = 0U;
        return MakeOk(i + 1U);
    }
    if (i + 1U >= len || begin[i + 1U] == ',' || begin[i + 1U] == '}' || begin[i + 1U] == ']' ||
        IsSpace(begin[i + 1U])) {
        value = 0U;
        return MakeOk(i + 1U);
    }
    if (begin[i + 1U] == '.') {
        return MakeError(i + 1U, ParserReason::DEVICE_ID_FLOAT);
    }
    if (begin[i + 1U] == 'e' || begin[i + 1U] == 'E') {
        return MakeError(i + 1U, ParserReason::DEVICE_ID_EXPONENT);
    }
    return MakeError(i + 1U, ParserReason::DEVICE_ID_LEADING_ZERO);
}

ParseResult ParseJsonUint32(const char *begin, const char *end, size_t offset, uint32_t &value)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len) {
        return MakeError(offset, ParserReason::BAD_DEVICE_ID);
    }
    size_t i = offset;
    bool negative = false;
    if (begin[i] == '-') {
        negative = true;
        ++i;
    }
    if (i >= len || begin[i] < '0' || begin[i] > '9') {
        return MakeError(negative ? i : offset, ParserReason::BAD_DEVICE_ID);
    }
    if (begin[i] == '0') {
        return ParseJsonUint32ZeroPrefix(begin, end, i, negative, value);
    }
    if (begin[i] < '1' || begin[i] > '9') {
        return MakeError(i, ParserReason::BAD_DEVICE_ID);
    }
    ++i;
    uint64_t val = static_cast<uint64_t>(begin[i - 1U] - '0');
    auto dr = AccumulateUint32Digits(begin, end, i, negative, val);
    if (dr.result != BM_OK) {
        return dr;
    }
    if (i < len && begin[i] == '.') {
        return MakeError(i, ParserReason::DEVICE_ID_FLOAT);
    }
    if (i < len && (begin[i] == 'e' || begin[i] == 'E')) {
        return MakeError(i, ParserReason::DEVICE_ID_EXPONENT);
    }
    if (negative && val > 0U) {
        return MakeError(i, ParserReason::DEVICE_ID_NEGATIVE);
    }
    if (val > UINT32_MAX) {
        return MakeError(i, ParserReason::DEVICE_ID_OVERFLOW);
    }
    value = static_cast<uint32_t>(val);
    return MakeOk(i);
}

ParseResult SkipArraySeparator(const char *begin, const char *end, size_t &i, bool expectComma)
{
    if (!expectComma) {
        return MakeOk(i);
    }
    i = SkipSpaces(begin, end, i);
    if (i >= GetLen(begin, end)) {
        return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
    }
    if (begin[i] == ',') {
        ++i;
        i = SkipSpaces(begin, end, i);
        if (i >= GetLen(begin, end)) {
            return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
        }
        if (begin[i] == ']') {
            return MakeError(i, ParserReason::TRAILING_COMMA);
        }
        return MakeOk(i);
    }
    if (begin[i] == ']') {
        return MakeOk(SIZE_MAX);
    }
    return MakeError(i, ParserReason::MISSING_COMMA);
}

ParseResult SkipObjectSeparator(const char *begin, const char *end, size_t &i, bool expectComma)
{
    if (!expectComma) {
        return MakeOk(i);
    }
    i = SkipSpaces(begin, end, i);
    if (i >= GetLen(begin, end)) {
        return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
    }
    if (begin[i] == ',') {
        ++i;
        i = SkipSpaces(begin, end, i);
        if (i >= GetLen(begin, end)) {
            return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
        }
        if (begin[i] == '}') {
            return MakeError(i, ParserReason::TRAILING_COMMA);
        }
        return MakeOk(i);
    }
    if (begin[i] == '}') {
        return MakeOk(SIZE_MAX);
    }
    return MakeError(i, ParserReason::MISSING_COMMA);
}

} // anonymous namespace

size_t SkipWs(const char *begin, const char *end, size_t offset)
{
    while (offset < static_cast<size_t>(end - begin)) {
        const char c = begin[offset];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        ++offset;
    }
    return offset;
}

Result ReadFileToString(const std::string &path, std::string &buf)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        BM_LOG_INFO("ReadFileToString: cannot open file=" << path);
        return BM_FILE_NOT_ACCESS;
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) {
        BM_LOG_INFO("ReadFileToString: cannot determine file size, path=" << path);
        return BM_FILE_NOT_ACCESS;
    }
    if (static_cast<size_t>(size) > MAX_INPUT_BYTES) {
        BM_LOG_INFO("ReadFileToString: file exceeds 1MiB, path=" << path << " size=" << size);
        return BM_INVALID_PARAM;
    }
    input.seekg(0, std::ios::beg);
    buf.resize(static_cast<size_t>(size));
    if (buf.empty()) {
        BM_LOG_INFO("ReadFileToString: empty file, path=" << path);
        return BM_INVALID_PARAM;
    }
    input.read(&buf[0], size);
    if (input.bad()) {
        BM_LOG_INFO("ReadFileToString: read failed, path=" << path);
        return BM_FILE_NOT_ACCESS;
    }
    return BM_OK;
}

Result ExtractStrField(const std::string &json, const char *quotedKey, std::string &out)
{
    size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) {
        return BM_INVALID_PARAM;
    }
    pos += std::strlen(quotedKey);
    const char *begin = json.data();
    const char *end = begin + json.size();
    pos = SkipWs(begin, end, pos);
    if (pos >= json.size() || json[pos] != ':') {
        return BM_INVALID_PARAM;
    }
    pos = SkipWs(begin, end, pos + 1);
    if (pos >= json.size() || json[pos] != '"') {
        return BM_INVALID_PARAM;
    }
    const size_t strStart = pos + 1;
    size_t strEnd = strStart;
    while (strEnd < json.size() && json[strEnd] != '"') {
        if (json[strEnd] == '\\' && strEnd + 1 < json.size()) {
            ++strEnd;
        }
        ++strEnd;
    }
    if (strEnd >= json.size()) {
        return BM_INVALID_PARAM;
    }
    out.assign(json, strStart, strEnd - strStart);
    return BM_OK;
}

Result ExtractIntField(const std::string &json, const char *quotedKey, int32_t &out)
{
    size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) {
        return BM_INVALID_PARAM;
    }
    pos += std::strlen(quotedKey);
    const char *begin = json.data();
    const char *end = begin + json.size();
    pos = SkipWs(begin, end, pos);
    if (pos >= json.size() || json[pos] != ':') {
        return BM_INVALID_PARAM;
    }
    pos = SkipWs(begin, end, pos + 1);
    uint32_t val = 0;
    auto pr = ParseJsonUint32(begin, end, pos, val);
    if (pr.result != BM_OK) {
        return BM_INVALID_PARAM;
    }
    out = static_cast<int32_t>(val);
    return BM_OK;
}

size_t FindQuotedKeyInRange(const char *buf, size_t start, size_t stop, const char *quotedKey, size_t keyLen)
{
    for (size_t i = start; i + keyLen <= stop; ++i) {
        if (buf[i] == quotedKey[0] && std::memcmp(buf + i, quotedKey, keyLen) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

Result ExtractIntInRange(const char *buf, size_t start, size_t stop, const char *quotedKey, int32_t &out)
{
    const size_t keyLen = std::strlen(quotedKey);
    size_t pos = FindQuotedKeyInRange(buf, start, stop, quotedKey, keyLen);
    if (pos == SIZE_MAX) {
        return BM_INVALID_PARAM;
    }
    pos += keyLen;
    const char *begin = buf;
    const char *end = buf + stop;
    pos = SkipWs(begin, end, pos);
    if (pos >= stop || buf[pos] != ':') {
        return BM_INVALID_PARAM;
    }
    pos = SkipWs(begin, end, pos + 1);
    uint32_t val = 0;
    auto pr = ParseJsonUint32(begin, end, pos, val);
    if (pr.result != BM_OK) {
        return BM_INVALID_PARAM;
    }
    out = static_cast<int32_t>(val);
    return BM_OK;
}

Result ParseJsonArrayHeader(const std::string &json, const char *quotedKey, size_t &pos)
{
    const char *begin = json.data();
    const char *end = begin + json.size();
    pos = json.find(quotedKey);
    if (pos == std::string::npos) {
        return BM_INVALID_PARAM;
    }
    pos = SkipWs(begin, end, pos + std::strlen(quotedKey));
    if (pos >= json.size() || json[pos] != ':') {
        return BM_INVALID_PARAM;
    }
    pos = SkipWs(begin, end, pos + 1);
    if (pos >= json.size() || json[pos] != '[') {
        return BM_INVALID_PARAM;
    }
    ++pos;
    pos = SkipWs(begin, end, pos);
    return BM_OK;
}

Result ParseStrArray(const std::string &json, const char *quotedKey, std::vector<std::string> &out)
{
    size_t pos = 0;
    auto ret = ParseJsonArrayHeader(json, quotedKey, pos);
    if (ret != BM_OK) {
        return BM_OK;
    }
    const char *begin = json.data();
    const char *end = begin + json.size();
    bool expectComma = false;
    while (pos < json.size() && json[pos] != ']') {
        if (expectComma) {
            if (json[pos] != ',') {
                return BM_INVALID_PARAM;
            }
            ++pos;
            pos = SkipWs(begin, end, pos);
        }
        if (pos >= json.size() || json[pos] != '"') {
            return BM_INVALID_PARAM;
        }
        const size_t strStart = pos + 1;
        size_t strEnd = strStart;
        while (strEnd < json.size() && json[strEnd] != '"') {
            if (json[strEnd] == '\\' && strEnd + 1 < json.size()) {
                ++strEnd;
            }
            ++strEnd;
        }
        if (strEnd >= json.size()) {
            return BM_INVALID_PARAM;
        }
        out.emplace_back(json, strStart, strEnd - strStart);
        pos = strEnd + 1;
        pos = SkipWs(begin, end, pos);
        expectComma = true;
    }
    return BM_OK;
}

ParseResult SkipString(const char *begin, const char *end, size_t offset, size_t maxLength)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    if (offset >= GetLen(begin, end) || begin[offset] != '"') {
        return MakeError(offset, ParserReason::BAD_STRING_END);
    }
    size_t i = offset + 1U;
    size_t strLen = 0;
    while (i < GetLen(begin, end)) {
        const unsigned char c = static_cast<unsigned char>(begin[i]);
        if (c == '"') {
            return MakeOk(i + 1U);
        }
        if (c < 0x20U) {
            return MakeError(i, ParserReason::CONTROL_CHAR);
        }
        if (c != '\\') {
            ++i;
            ++strLen;
            if (strLen > maxLength) {
                return MakeError(i, ParserReason::BAD_VALUE);
            }
            continue;
        }
        auto er = HandleStringEscape(begin, end, i, strLen, maxLength);
        if (er.result != BM_OK) {
            return er;
        }
        i = er.offset;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_STRING);
}

ParseResult SkipNumber(const char *begin, const char *end, size_t offset)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len) {
        return MakeError(offset, ParserReason::BAD_VALUE);
    }
    size_t i = offset;
    if (begin[i] == '-') {
        ++i;
    }
    if (i >= len) {
        return MakeError(offset, ParserReason::BAD_VALUE);
    }
    if (begin[i] == '0') {
        ++i;
    } else if (begin[i] >= '1' && begin[i] <= '9') {
        ++i;
        while (i < len && begin[i] >= '0' && begin[i] <= '9') {
            ++i;
        }
    } else {
        return MakeError(offset, ParserReason::BAD_VALUE);
    }
    if (i < len && begin[i] == '.') {
        ++i;
        if (i >= len || begin[i] < '0' || begin[i] > '9') {
            return MakeError(offset, ParserReason::BAD_VALUE);
        }
        while (i < len && begin[i] >= '0' && begin[i] <= '9') {
            ++i;
        }
    }
    if (i < len && (begin[i] == 'e' || begin[i] == 'E')) {
        ++i;
        if (i < len && (begin[i] == '+' || begin[i] == '-')) {
            ++i;
        }
        if (i >= len || begin[i] < '0' || begin[i] > '9') {
            return MakeError(offset, ParserReason::BAD_VALUE);
        }
        while (i < len && begin[i] >= '0' && begin[i] <= '9') {
            ++i;
        }
    }
    return MakeOk(i);
}

ParseResult SkipLiteral(const char *begin, const char *end, size_t offset)
{
    const auto len = static_cast<size_t>(end - begin);
    if (offset + 4U <= len && std::memcmp(begin + offset, "true", 4) == 0) {
        return MakeOk(offset + 4U);
    }
    if (offset + 5U <= len && std::memcmp(begin + offset, "false", 5) == 0) {
        return MakeOk(offset + 5U);
    }
    if (offset + 4U <= len && std::memcmp(begin + offset, "null", 4) == 0) {
        return MakeOk(offset + 4U);
    }
    return MakeError(offset, ParserReason::BAD_VALUE);
}

ParseResult SkipObject(const char *begin, const char *end, size_t offset, int depth)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len || begin[offset] != '{') {
        return MakeError(offset, ParserReason::BAD_VALUE);
    }
    size_t i = offset + 1U;
    i = SkipSpaces(begin, end, i);
    if (i < len && begin[i] == '}') {
        return MakeOk(i + 1U);
    }
    bool expectComma = false;
    while (i < len) {
        i = SkipSpaces(begin, end, i);
        if (i >= len) {
            return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
        }
        auto sr = SkipObjectSeparator(begin, end, i, expectComma);
        if (sr.result != BM_OK) {
            return sr;
        }
        if (sr.offset == SIZE_MAX) {
            return MakeOk(i + 1U);
        }
        i = sr.offset;
        expectComma = false;
        auto kr = SkipString(begin, end, i, kStringMaxLen);
        if (kr.result != BM_OK) {
            return kr;
        }
        i = kr.offset;
        i = SkipSpaces(begin, end, i);
        if (i >= len || begin[i] != ':') {
            return MakeError(i, ParserReason::MISSING_COLON);
        }
        ++i;
        auto vr = SkipValue(begin, end, i, depth + 1);
        if (vr.result != BM_OK) {
            return vr;
        }
        i = vr.offset;
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
}

ParseResult SkipArray(const char *begin, const char *end, size_t offset, int depth)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len || begin[offset] != '[') {
        return MakeError(offset, ParserReason::BAD_VALUE);
    }
    size_t i = offset + 1U;
    i = SkipSpaces(begin, end, i);
    if (i < len && begin[i] == ']') {
        return MakeOk(i + 1U);
    }
    bool expectComma = false;
    while (i < len) {
        i = SkipSpaces(begin, end, i);
        if (i >= len) {
            return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
        }
        auto sr = SkipArraySeparator(begin, end, i, expectComma);
        if (sr.result != BM_OK) {
            return sr;
        }
        if (sr.offset == SIZE_MAX) {
            return MakeOk(i + 1U);
        }
        i = sr.offset;
        expectComma = false;
        auto vr = SkipValue(begin, end, i, depth + 1);
        if (vr.result != BM_OK) {
            return vr;
        }
        i = vr.offset;
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
}

ParseResult SkipValue(const char *begin, const char *end, size_t offset, int depth)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    if (depth > MAX_PARSE_DEPTH) {
        return MakeError(offset, ParserReason::DEPTH_EXCEEDED);
    }
    offset = SkipSpaces(begin, end, offset);
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len) {
        return MakeError(offset, ParserReason::BAD_VALUE);
    }
    const char c = begin[offset];
    switch (c) {
        case '{':
            return SkipObject(begin, end, offset, depth);
        case '[':
            return SkipArray(begin, end, offset, depth);
        case '"':
            return SkipString(begin, end, offset, SIZE_MAX);
        case 't':
        case 'f':
        case 'n':
            return SkipLiteral(begin, end, offset);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return SkipNumber(begin, end, offset);
            }
            return MakeError(offset, ParserReason::BAD_VALUE);
    }
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
