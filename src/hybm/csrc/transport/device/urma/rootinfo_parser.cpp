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
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "hybm_logger.h"
#include "hybm_types.h"
#include "rootinfo_parser.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {

// ---------- helpers ----------

constexpr size_t kStringMaxLen = 256;
constexpr size_t kMaxDecodedKeyLen = 32;

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

// ---------- range validation ----------

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

// ---------- escaped known-key detection ----------

// ---------- decode one escape char (for IsEscapedKnownKey) ----------

bool DecodeSimpleEscape(const char esc, char &out)
{
    switch (esc) {
        case '"':
            out = '"';
            return true;
        case '\\':
            out = '\\';
            return true;
        case '/':
            out = '/';
            return true;
        case 'b':
            out = '\b';
            return true;
        case 'f':
            out = '\f';
            return true;
        case 'n':
            out = '\n';
            return true;
        case 'r':
            out = '\r';
            return true;
        case 't':
            out = '\t';
            return true;
        default:
            return false;
    }
}

bool DecodeUnicodeEscape(const char *raw, size_t rawLen, size_t &i, char &out)
{
    if (i + 5U >= rawLen) {
        return false;
    }
    unsigned v = 0U;
    for (size_t k = 2U; k < 6U; ++k) {
        if (!IsHexDigit(raw[i + k])) {
            return false;
        }
        v = (v << 4) | static_cast<unsigned>(HexVal(raw[i + k]));
    }
    if (v > 127U) {
        return false; // known keys are pure ASCII
    }
    out = static_cast<char>(v);
    i += 6U;
    return true;
}

bool DecodeOneEscape(const char *raw, size_t rawLen, size_t &i, char &out)
{
    if (i + 1U >= rawLen) {
        return false;
    }
    const char esc = raw[i + 1U];
    if (DecodeSimpleEscape(esc, out)) {
        i += 2U;
        return true;
    }
    if (esc == 'u') {
        return DecodeUnicodeEscape(raw, rawLen, i, out);
    }
    return false;
}

bool IsEscapedKnownKey(const char *raw, size_t rawLen)
{
    bool hasBs = false;
    for (size_t chk = 0; chk < rawLen; ++chk) {
        if (raw[chk] == '\\') {
            hasBs = true;
            break;
        }
    }
    if (!hasBs) {
        return false;
    }
    char decoded[kMaxDecodedKeyLen];
    size_t d = 0;
    for (size_t i = 0; i < rawLen && d < kMaxDecodedKeyLen;) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\\') {
            if (!DecodeOneEscape(raw, rawLen, i, decoded[d])) {
                return false;
            }
            ++d;
        } else if (c < 0x20U) {
            return false;
        } else {
            decoded[d++] = raw[i++];
        }
    }
    if (d >= kMaxDecodedKeyLen) {
        return false;
    }
    decoded[d] = '\0';
    return (d == 4 && std::memcmp(decoded, "addr", 4) == 0) || (d == 5 && std::memcmp(decoded, "ports", 5) == 0) ||
           (d == 9 && (std::memcmp(decoded, "rank_list", 9) == 0 || std::memcmp(decoded, "device_id", 9) == 0 ||
                       std::memcmp(decoded, "addr_type", 9) == 0)) ||
           (d == 10 && std::memcmp(decoded, "level_list", 10) == 0) ||
           (d == 14 && std::memcmp(decoded, "rank_addr_list", 14) == 0);
}

// ---------- handle unicode escape in string (surrogate pair for HandleStringEscape) ----------

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

// ---------- escape handler for SkipString ----------

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

// ---------- digit accumulation for ParseDeviceId ----------

ParseResult AccumulateDeviceDigits(const char *begin, const char *end, size_t &i, bool negative, uint64_t &val)
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

// ---------- zero-prefix handler for ParseDeviceId ----------

ParseResult ParseDeviceIdZeroPrefix(const char *begin, const char *end, size_t i, bool negative, uint32_t &value)
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

// ---------- array element separator ----------

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
        return MakeOk(SIZE_MAX); // signal container closed
    }
    return MakeError(i, ParserReason::MISSING_COMMA);
}

// ---------- object member separator ----------

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
        return MakeOk(SIZE_MAX); // signal container closed
    }
    return MakeError(i, ParserReason::MISSING_COMMA);
}

// ---------- ParseEidHex ----------

void ParseEidHex(const char *hexBegin, size_t hexLen, std::array<uint8_t, COMM_ADDR_EID_LEN> &eid)
{
    for (size_t k = 0; k < COMM_ADDR_EID_LEN && k * 2U + 1U < hexLen; ++k) {
        const unsigned hi = static_cast<unsigned>(HexVal(hexBegin[k * 2]));
        const unsigned lo = static_cast<unsigned>(HexVal(hexBegin[k * 2 + 1U]));
        eid[k] = static_cast<uint8_t>((hi * 16U + lo) & 0xFFU);
    }
}

// ---------- escape detection for target values ----------

bool HasBackslash(const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '\\') {
            return true;
        }
    }
    return false;
}

// ---------- ports parser for ParseSingleRankAddr ----------

struct PortsResult {
    size_t offset;
    size_t count;
};

// ---------- port separator helper for ParsePortsArray ----------

ParseResult ParsePortsSeparator(const char *begin, const char *end, size_t &pi, bool pComma)
{
    pi = SkipSpaces(begin, end, pi);
    if (pi >= static_cast<size_t>(end - begin)) {
        return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
    }
    if (!pComma) {
        return MakeOk(pi);
    }
    if (begin[pi] == ',') {
        ++pi;
        pi = SkipSpaces(begin, end, pi);
        if (pi >= static_cast<size_t>(end - begin)) {
            return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
        }
        if (begin[pi] == ']') {
            return MakeError(pi, ParserReason::TRAILING_COMMA);
        }
        return MakeOk(pi);
    }
    if (begin[pi] == ']') {
        return MakeOk(SIZE_MAX);
    }
    return MakeError(pi, ParserReason::MISSING_COMMA);
}

ParseResult ParsePortsArray(const char *begin, const char *end, size_t offset, PortsResult &pr)
{
    pr = {offset, 0};
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len || begin[offset] != '[') {
        return MakeError(offset, ParserReason::BAD_VALUE);
    }
    size_t pi = offset + 1U;
    pi = SkipSpaces(begin, end, pi);
    if (pi < len && begin[pi] == ']') {
        pr = {pi + 1U, 0};
        return MakeOk(pi + 1U);
    }
    bool pComma = false;
    while (pi < len) {
        auto sr = ParsePortsSeparator(begin, end, pi, pComma);
        if (sr.result != BM_OK) {
            return sr;
        }
        if (sr.offset == SIZE_MAX) {
            break;
        }
        pi = sr.offset;
        pComma = false;
        auto pv = SkipValue(begin, end, pi, 0);
        if (pv.result != BM_OK) {
            return pv;
        }
        pi = pv.offset;
        ++pr.count;
        pComma = true;
    }
    pi = SkipSpaces(begin, end, pi);
    if (pi >= len || begin[pi] != ']') {
        return MakeError(pi, ParserReason::BAD_VALUE);
    }
    pr.offset = pi + 1U;
    return MakeOk(pi + 1U);
}

} // anonymous namespace

// ==============================
//  SkipString
// ==============================

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

// ==============================
//  SkipNumber
// ==============================

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

// ==============================
//  SkipLiteral
// ==============================

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

// ==============================
//  SkipObject  (strict JSON)
// ==============================

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

// ==============================
//  SkipArray  (strict JSON)
// ==============================

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

// ==============================
//  SkipValue
// ==============================

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

// ==============================
//  ParseDeviceId
// ==============================

ParseResult ParseDeviceId(const char *begin, const char *end, size_t offset, uint32_t &value)
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
    // Handle '0' prefix variant
    if (begin[i] == '0') {
        return ParseDeviceIdZeroPrefix(begin, end, i, negative, value);
    }
    if (begin[i] < '1' || begin[i] > '9') {
        return MakeError(i, ParserReason::BAD_DEVICE_ID);
    }
    ++i;
    uint64_t val = static_cast<uint64_t>(begin[i - 1U] - '0');
    auto dr = AccumulateDeviceDigits(begin, end, i, negative, val);
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

namespace {

// ---------- candidate handler for ParseRankAddrList ----------

ParseResult ParseRankAddrListCandidate(const char *begin, size_t &i, CandidateInfo &cand, size_t &candidateCount,
                                       std::array<uint8_t, COMM_ADDR_EID_LEN> &tempEid)
{
    if (!cand.isCandidate) {
        return MakeOk(i);
    }
    ++candidateCount;
    if (candidateCount == 1 && cand.eidBegin != nullptr && cand.eidLen > 0) {
        constexpr size_t kHexLen = COMM_ADDR_EID_LEN * 2U;
        if (cand.eidLen != kHexLen) {
            return MakeError(i, ParserReason::BAD_VALUE);
        }
        for (size_t k = 0; k < kHexLen; ++k) {
            if (!IsHexDigit(cand.eidBegin[k])) {
                return MakeError(static_cast<size_t>(cand.eidBegin - begin) + k, ParserReason::BAD_VALUE);
            }
        }
        ParseEidHex(cand.eidBegin, cand.eidLen, tempEid);
    }
    return MakeOk(i);
}

// ==============================
//  ParseRankAddrList
// ==============================

ParseResult ParseRankAddrList(const char *begin, const char *end, size_t offset,
                              std::array<uint8_t, COMM_ADDR_EID_LEN> &tempEid, size_t &candidateCount)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len || begin[offset] != '[') {
        return MakeError(offset, ParserReason::BAD_STRUCTURE);
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
        if (begin[i] != '{') {
            return MakeError(i, ParserReason::BAD_VALUE);
        }
        CandidateInfo cand;
        auto pr = ParseSingleRankAddr(begin, end, i, cand);
        if (pr.result != BM_OK) {
            return pr;
        }
        i = pr.offset;
        auto cr = ParseRankAddrListCandidate(begin, i, cand, candidateCount, tempEid);
        if (cr.result != BM_OK) {
            return cr;
        }
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
}

// ---------- read key + colon (shared by object-level parsers) ----------

struct KeyAndColon {
    size_t keyOff;
    size_t keyLen;
    size_t pos;
};

ParseResult ReadKeyAndColon(const char *begin, const char *end, size_t &i, KeyAndColon &kc)
{
    auto kr = SkipString(begin, end, i, kStringMaxLen);
    if (kr.result != BM_OK) {
        return kr;
    }
    kc.keyOff = i + 1U;
    kc.keyLen = kr.offset - i - 2U;
    kc.pos = kr.offset;
    if (IsEscapedKnownKey(begin + kc.keyOff, kc.keyLen)) {
        return MakeError(kc.pos, ParserReason::DUPLICATE_KEY);
    }
    kc.pos = SkipSpaces(begin, end, kc.pos);
    if (kc.pos >= GetLen(begin, end) || begin[kc.pos] != ':') {
        return MakeError(kc.pos, ParserReason::MISSING_COLON);
    }
    ++kc.pos;
    kc.pos = SkipSpaces(begin, end, kc.pos);
    return MakeOk(kc.pos);
}

// ---------- helpers for ParseSingleLevel ----------

ParseResult ParseSingleLevelClose(size_t offset, size_t &i, uint8_t seenBits)
{
    ++i;
    if ((seenBits & 1U) == 0U) {
        return MakeError(offset, ParserReason::BAD_STRUCTURE);
    }
    return MakeOk(i);
}

ParseResult ParseSingleLevelField(const char *begin, const char *end, const KeyAndColon &kc, size_t &i,
                                  uint8_t &seenBits, std::array<uint8_t, COMM_ADDR_EID_LEN> &tempEid,
                                  size_t &candidateCount)
{
    if (kc.keyLen == 14 && std::memcmp(begin + kc.keyOff, "rank_addr_list", 14) == 0) {
        if (seenBits & 1U) {
            return MakeError(i, ParserReason::DUPLICATE_KEY);
        }
        seenBits |= 1U;
        auto ar = ParseRankAddrList(begin, end, i, tempEid, candidateCount);
        if (ar.result != BM_OK) {
            return ar;
        }
        i = ar.offset;
    } else {
        auto vr = SkipValue(begin, end, i, 0);
        if (vr.result != BM_OK) {
            return vr;
        }
        i = vr.offset;
    }
    return MakeOk(i);
}

// ==============================
//  ParseSingleLevel
// ==============================

ParseResult ParseSingleLevel(const char *begin, const char *end, size_t offset,
                             std::array<uint8_t, COMM_ADDR_EID_LEN> &tempEid, size_t &candidateCount)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len || begin[offset] != '{') {
        return MakeError(offset, ParserReason::BAD_STRUCTURE);
    }
    size_t i = offset + 1U;
    uint8_t seenBits = 0U;
    bool expectComma = false;
    while (i < len) {
        i = SkipSpaces(begin, end, i);
        if (i >= len) {
            return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
        }
        if (begin[i] == '}') {
            return ParseSingleLevelClose(offset, i, seenBits);
        }
        if (expectComma) {
            auto sr = SkipObjectSeparator(begin, end, i, expectComma);
            if (sr.result != BM_OK) {
                return sr;
            }
            i = sr.offset;
            expectComma = false;
            continue;
        }
        KeyAndColon kc;
        auto kr = ReadKeyAndColon(begin, end, i, kc);
        if (kr.result != BM_OK) {
            return kr;
        }
        i = kc.pos;
        auto lr = ParseSingleLevelField(begin, end, kc, i, seenBits, tempEid, candidateCount);
        if (lr.result != BM_OK) {
            return lr;
        }
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
}

// ==============================
//  ParseLevelList
// ==============================

ParseResult ParseLevelList(const char *begin, const char *end, size_t offset,
                           std::array<uint8_t, COMM_ADDR_EID_LEN> &tempEid, size_t &candidateCount)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len || begin[offset] != '[') {
        return MakeError(offset, ParserReason::BAD_STRUCTURE);
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
        auto lr = ParseSingleLevel(begin, end, i, tempEid, candidateCount);
        if (lr.result != BM_OK) {
            return lr;
        }
        i = lr.offset;
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
}

// ---------- helpers for ParseSingleDevice ----------

ParseResult ParseDeviceObjectClose(const char *begin, size_t offset, size_t &i, bool hasDevId, uint32_t devId,
                                   uint32_t phyDeviceId, size_t llStart, size_t llEnd, size_t &targetLlBegin,
                                   size_t &targetLlEnd, size_t &matchCount)
{
    ++i;
    if (!hasDevId) {
        return MakeError(offset, ParserReason::DEVICE_ID_MISSING);
    }
    if (devId == phyDeviceId) {
        ++matchCount;
        targetLlBegin = llStart;
        targetLlEnd = llEnd;
    }
    return MakeOk(i);
}

ParseResult ParseSingleDeviceField(const char *begin, const char *end, const KeyAndColon &kc, size_t &i,
                                   uint8_t &seenBits, bool &hasDevId, uint32_t &devId, size_t &llStart, size_t &llEnd)
{
    const auto len = static_cast<size_t>(end - begin);
    if (kc.keyLen == 9 && std::memcmp(begin + kc.keyOff, "device_id", 9) == 0) {
        if (seenBits & 1U) {
            return MakeError(i, ParserReason::DUPLICATE_KEY);
        }
        seenBits |= 1U;
        auto dr = ParseDeviceId(begin, end, i, devId);
        if (dr.result != BM_OK) {
            return dr;
        }
        hasDevId = true;
        i = dr.offset;
    } else if (kc.keyLen == 10 && std::memcmp(begin + kc.keyOff, "level_list", 10) == 0) {
        if (seenBits & 2U) {
            return MakeError(i, ParserReason::DUPLICATE_KEY);
        }
        seenBits |= 2U;
        if (i >= len || begin[i] != '[') {
            return MakeError(i, ParserReason::BAD_STRUCTURE);
        }
        llStart = i;
        auto ar = SkipArray(begin, end, i, 0);
        if (ar.result != BM_OK) {
            return ar;
        }
        llEnd = ar.offset;
        i = ar.offset;
    } else {
        auto vr = SkipValue(begin, end, i, 0);
        if (vr.result != BM_OK) {
            return vr;
        }
        i = vr.offset;
    }
    return MakeOk(i);
}

// ==============================
//  ParseSingleDevice
// ==============================

ParseResult ParseSingleDevice(const char *begin, const char *end, size_t offset, uint32_t phyDeviceId,
                              size_t &targetLlBegin, size_t &targetLlEnd, size_t &matchCount)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len || begin[offset] != '{') {
        return MakeError(offset, ParserReason::BAD_STRUCTURE);
    }
    size_t i = offset + 1U;
    bool hasDevId = false;
    uint32_t devId = 0;
    size_t llStart = 0;
    size_t llEnd = 0;
    uint8_t seenBits = 0U;
    bool expectComma = false;
    while (i < len) {
        i = SkipSpaces(begin, end, i);
        if (i >= len) {
            return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
        }
        if (begin[i] == '}') {
            return ParseDeviceObjectClose(begin, offset, i, hasDevId, devId, phyDeviceId, llStart, llEnd, targetLlBegin,
                                          targetLlEnd, matchCount);
        }
        if (expectComma) {
            auto sr = SkipObjectSeparator(begin, end, i, expectComma);
            if (sr.result != BM_OK) {
                return sr;
            }
            i = sr.offset;
            expectComma = false;
            continue;
        }
        KeyAndColon kc;
        auto kr = ReadKeyAndColon(begin, end, i, kc);
        if (kr.result != BM_OK) {
            return kr;
        }
        i = kc.pos;
        auto dr = ParseSingleDeviceField(begin, end, kc, i, seenBits, hasDevId, devId, llStart, llEnd);
        if (dr.result != BM_OK) {
            return dr;
        }
        i = dr.offset;
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
}
// ==============================

ParseResult ParseRankList(const char *begin, const char *end, size_t offset, uint32_t phyDeviceId,
                          size_t &targetLlBegin, size_t &targetLlEnd, size_t &matchCount)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    if (offset >= len || begin[offset] != '[') {
        return MakeError(offset, ParserReason::BAD_STRUCTURE);
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
        if (begin[i] != '{') {
            return MakeError(i, ParserReason::BAD_VALUE);
        }
        auto dr = ParseSingleDevice(begin, end, i, phyDeviceId, targetLlBegin, targetLlEnd, matchCount);
        if (dr.result != BM_OK) {
            return dr;
        }
        i = dr.offset;
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_ARRAY);
}

// ---------- helpers for ParseRootObject ----------

ParseResult ParseRootObjectClose(const char *begin, const char *end, size_t offset, size_t &i, bool foundRankList)
{
    ++i;
    if (!foundRankList) {
        return MakeError(offset, ParserReason::BAD_STRUCTURE);
    }
    i = SkipSpaces(begin, end, i);
    if (i < static_cast<size_t>(end - begin)) {
        return MakeError(i, ParserReason::TRAILING_GARBAGE);
    }
    return MakeOk(i);
}

ParseResult ParseRootObjectField(const char *begin, const char *end, const KeyAndColon &kc, size_t &i,
                                 uint8_t &seenBits, bool &foundRankList, uint32_t phyDeviceId, size_t &targetLlBegin,
                                 size_t &targetLlEnd, size_t &matchCount)
{
    if (kc.keyLen == 9 && std::memcmp(begin + kc.keyOff, "rank_list", 9) == 0) {
        if (seenBits & 1U) {
            return MakeError(i, ParserReason::DUPLICATE_KEY);
        }
        seenBits |= 1U;
        foundRankList = true;
        auto rr = ParseRankList(begin, end, i, phyDeviceId, targetLlBegin, targetLlEnd, matchCount);
        if (rr.result != BM_OK) {
            return rr;
        }
        i = rr.offset;
    } else {
        auto vr = SkipValue(begin, end, i, 0);
        if (vr.result != BM_OK) {
            return vr;
        }
        i = vr.offset;
    }
    return MakeOk(i);
}

// ==============================
//  ParseRootObject
// ==============================

ParseResult ParseRootObject(const char *begin, const char *end, size_t offset, uint32_t phyDeviceId,
                            size_t &targetLlBegin, size_t &targetLlEnd, size_t &matchCount)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    const auto len = static_cast<size_t>(end - begin);
    size_t i = SkipSpaces(begin, end, offset);
    if (i >= len || begin[i] != '{') {
        return MakeError(i >= len ? len : i, ParserReason::BAD_STRUCTURE);
    }
    i = i + 1U;
    bool foundRankList = false;
    uint8_t seenBits = 0U;
    bool expectComma = false;
    while (i < len) {
        i = SkipSpaces(begin, end, i);
        if (i >= len) {
            return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
        }
        if (begin[i] == '}') {
            return ParseRootObjectClose(begin, end, offset, i, foundRankList);
        }
        if (expectComma) {
            auto sr = SkipObjectSeparator(begin, end, i, expectComma);
            if (sr.result != BM_OK) {
                return sr;
            }
            i = sr.offset;
            expectComma = false;
            continue;
        }
        KeyAndColon kc;
        auto kr = ReadKeyAndColon(begin, end, i, kc);
        if (kr.result != BM_OK) {
            return kr;
        }
        i = kc.pos;
        auto rr = ParseRootObjectField(begin, end, kc, i, seenBits, foundRankList, phyDeviceId, targetLlBegin,
                                       targetLlEnd, matchCount);
        if (rr.result != BM_OK) {
            return rr;
        }
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::UNTERMINATED_OBJECT);
}

// ---------- rank-addr field handler ----------

struct RankAddrState {
    uint8_t seenBits = 0U;
    bool hasEidType = false;
    bool hasSixPorts = false;
    const char *addrStr = nullptr;
    size_t addrEndOffset = 0;
};

ParseResult HandleRankAddrAddrType(const char *begin, const char *end, size_t &i, RankAddrState &st)
{
    if (st.seenBits & 1U) {
        return MakeError(i, ParserReason::DUPLICATE_KEY);
    }
    st.seenBits |= 1U;
    if (i >= GetLen(begin, end) || begin[i] != '"') {
        return MakeError(i, ParserReason::BAD_VALUE);
    }
    const char *typeBegin = begin + i + 1U;
    auto sr = SkipString(begin, end, i, kStringMaxLen);
    if (sr.result != BM_OK) {
        return sr;
    }
    const size_t typeLen = sr.offset - i - 2U;
    if (typeLen == 3 && std::memcmp(typeBegin, "EID", 3) == 0 && !HasBackslash(typeBegin, typeLen)) {
        st.hasEidType = true;
    }
    i = sr.offset;
    return MakeOk(i);
}

ParseResult HandleRankAddrPorts(const char *begin, const char *end, size_t &i, RankAddrState &st)
{
    if (st.seenBits & 2U) {
        return MakeError(i, ParserReason::DUPLICATE_KEY);
    }
    st.seenBits |= 2U;
    PortsResult pr;
    auto prr = ParsePortsArray(begin, end, i, pr);
    if (prr.result != BM_OK) {
        return prr;
    }
    if (pr.count == 6) {
        st.hasSixPorts = true;
    }
    i = pr.offset;
    return MakeOk(i);
}

ParseResult HandleRankAddrAddr(const char *begin, const char *end, size_t &i, RankAddrState &st)
{
    if (st.seenBits & 4U) {
        return MakeError(i, ParserReason::DUPLICATE_KEY);
    }
    st.seenBits |= 4U;
    if (i >= GetLen(begin, end) || begin[i] != '"') {
        return MakeError(i, ParserReason::BAD_VALUE);
    }
    st.addrStr = begin + i + 1U;
    auto sr = SkipString(begin, end, i, kStringMaxLen);
    if (sr.result != BM_OK) {
        return sr;
    }
    st.addrEndOffset = sr.offset;
    i = sr.offset;
    return MakeOk(i);
}

ParseResult HandleRankAddrField(const char *begin, const char *end, const KeyAndColon &kc, size_t &i, RankAddrState &st)
{
    if (kc.keyLen == 9 && std::memcmp(begin + kc.keyOff, "addr_type", 9) == 0) {
        return HandleRankAddrAddrType(begin, end, i, st);
    }
    if (kc.keyLen == 5 && std::memcmp(begin + kc.keyOff, "ports", 5) == 0) {
        return HandleRankAddrPorts(begin, end, i, st);
    }
    if (kc.keyLen == 4 && std::memcmp(begin + kc.keyOff, "addr", 4) == 0) {
        return HandleRankAddrAddr(begin, end, i, st);
    }
    auto vr = SkipValue(begin, end, i, 0);
    if (vr.result != BM_OK) {
        return vr;
    }
    i = vr.offset;
    return MakeOk(i);
}

} // anonymous namespace

// ---------- EID candidate evaluation for ParseSingleRankAddr ----------

ParseResult ParseSingleRankAddrFinalize(const char *begin, size_t i, size_t totalLen, const RankAddrState &st,
                                        CandidateInfo &out)
{
    if (i > totalLen) {
        return MakeError(GetLen(begin, begin + totalLen), ParserReason::DEVICE_UNCLOSED);
    }
    // six-port EID missing addr is schema error, not silent non-candidate
    if (st.hasEidType && st.hasSixPorts && st.addrStr == nullptr) {
        return MakeError(i, ParserReason::BAD_VALUE);
    }
    if (st.hasEidType && st.hasSixPorts && st.addrStr != nullptr && st.addrEndOffset > 0) {
        const size_t addrLen = st.addrEndOffset - 1U - static_cast<size_t>(st.addrStr - begin);
        if (addrLen > 0) {
            out.isCandidate = true;
            out.eidBegin = st.addrStr;
            out.eidLen = addrLen;
        }
    }
    return MakeOk(i);
}

// ==============================
//  ParseSingleRankAddr
// ==============================

ParseResult ParseSingleRankAddr(const char *begin, const char *end, size_t offset, CandidateInfo &out)
{
    if (!IsValidRange(begin, end)) {
        return RangeError();
    }
    out = CandidateInfo{};
    const auto totalLen = static_cast<size_t>(end - begin);
    if (offset >= totalLen || begin[offset] != '{') {
        return MakeError(offset, ParserReason::BAD_STRUCTURE);
    }
    size_t i = offset + 1U;
    RankAddrState st;
    bool expectComma = false;
    while (i < totalLen) {
        i = SkipSpaces(begin, end, i);
        if (i >= totalLen) {
            break;
        }
        if (begin[i] == '}') {
            ++i;
            return ParseSingleRankAddrFinalize(begin, i, totalLen, st, out);
        }
        if (expectComma) {
            auto sr = SkipObjectSeparator(begin, end, i, expectComma);
            if (sr.result != BM_OK) {
                return sr;
            }
            i = sr.offset;
            expectComma = false;
            continue;
        }
        KeyAndColon kc;
        auto kr = ReadKeyAndColon(begin, end, i, kc);
        if (kr.result != BM_OK) {
            return kr;
        }
        i = kc.pos;
        kr = HandleRankAddrField(begin, end, kc, i, st);
        if (kr.result != BM_OK) {
            return kr;
        }
        expectComma = true;
    }
    return MakeError(GetLen(begin, end), ParserReason::DEVICE_UNCLOSED);
}

// ---------- validation helpers for ParseRootInfoEid ----------

Result ParseRootInfoEidValidateDevice(size_t matchCount, uint32_t phyDeviceId, uint32_t rankId, size_t targetLlBegin,
                                      size_t targetLlEnd)
{
    if (matchCount == 0) {
        BM_LOG_ERROR("RootInfoParser: no matching device, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    if (matchCount > 1) {
        BM_LOG_ERROR("RootInfoParser: duplicate device matches=" << matchCount << " phyDeviceId=" << phyDeviceId
                                                                 << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    if (targetLlBegin == 0 || targetLlEnd == 0) {
        BM_LOG_ERROR("RootInfoParser: level_list missing, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result ParseRootInfoEidValidateCandidate(size_t candidateCount, uint32_t phyDeviceId, uint32_t rankId)
{
    if (candidateCount == 0) {
        BM_LOG_ERROR("RootInfoParser: no EID candidate, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    if (candidateCount > 1) {
        BM_LOG_ERROR("RootInfoParser: duplicate EID candidates=" << candidateCount << " phyDeviceId=" << phyDeviceId
                                                                 << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

// ==============================
//  ParseRootInfoEid
// ==============================

Result ParseRootInfoEid(const char *begin, const char *end, uint32_t phyDeviceId, uint32_t rankId,
                        std::array<uint8_t, COMM_ADDR_EID_LEN> &eid)
{
    if (!IsValidRange(begin, end)) {
        BM_LOG_ERROR("RootInfoParser: invalid range, phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    size_t targetLlBegin = 0;
    size_t targetLlEnd = 0;
    size_t matchCount = 0;
    auto pr = ParseRootObject(begin, end, 0, phyDeviceId, targetLlBegin, targetLlEnd, matchCount);
    if (pr.result != BM_OK) {
        BM_LOG_ERROR("RootInfoParser: parse failed offset=" << pr.offset << " reason=" << static_cast<int>(pr.reason)
                                                            << " phyDeviceId=" << phyDeviceId << " rankId=" << rankId);
        return pr.result;
    }
    Result vr = ParseRootInfoEidValidateDevice(matchCount, phyDeviceId, rankId, targetLlBegin, targetLlEnd);
    if (vr != BM_OK) {
        return vr;
    }
    std::array<uint8_t, COMM_ADDR_EID_LEN> tempEid{};
    tempEid.fill(0xFF);
    size_t candidateCount = 0;
    auto lr = ParseLevelList(begin, end, targetLlBegin, tempEid, candidateCount);
    if (lr.result != BM_OK) {
        BM_LOG_ERROR("RootInfoParser: level_list parse failed offset="
                     << lr.offset << " reason=" << static_cast<int>(lr.reason) << " phyDeviceId=" << phyDeviceId
                     << " rankId=" << rankId);
        return BM_INVALID_PARAM;
    }
    vr = ParseRootInfoEidValidateCandidate(candidateCount, phyDeviceId, rankId);
    if (vr != BM_OK) {
        return vr;
    }
    eid = tempEid;
    return BM_OK;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
