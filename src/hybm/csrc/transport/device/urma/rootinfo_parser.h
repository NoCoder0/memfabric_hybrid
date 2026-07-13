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

#ifndef MF_HYBM_ROOTINFO_PARSER_H
#define MF_HYBM_ROOTINFO_PARSER_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "dl_hcomm_api.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

// ---------- ParserReason ----------

enum class ParserReason : uint8_t {
    NONE = 0,
    UNTERMINATED_STRING,
    INVALID_ESCAPE,
    CONTROL_CHAR,
    BAD_STRING_END,
    UNTERMINATED_OBJECT,
    UNTERMINATED_ARRAY,
    MISSING_COMMA,
    TRAILING_COMMA,
    MISSING_COLON,
    BAD_VALUE,
    DEPTH_EXCEEDED,
    DUPLICATE_KEY,
    BAD_DEVICE_ID,
    DEVICE_ID_NEGATIVE,
    DEVICE_ID_FLOAT,
    DEVICE_ID_EXPONENT,
    DEVICE_ID_LEADING_ZERO,
    DEVICE_ID_OVERFLOW,
    DEVICE_ID_MISSING,
    BAD_STRUCTURE,
    TRAILING_GARBAGE,
    NO_MATCH,
    NO_CANDIDATE,
    DEVICE_UNCLOSED,
};

// ---------- ParseResult ----------

struct ParseResult {
    Result result;
    size_t offset;
    ParserReason reason;
};

// ---------- CandidateInfo ----------

struct CandidateInfo {
    bool isCandidate = false;
    const char *eidBegin = nullptr;
    size_t eidLen = 0;
};

// ---------- Constants ----------

constexpr size_t MAX_INPUT_BYTES = 1 * 1024 * 1024; // 1 MiB
constexpr size_t MAX_INPUT_BYTES_PLUS_1 = MAX_INPUT_BYTES + 1U;
constexpr int MAX_PARSE_DEPTH = 32;

// ---------- Parse functions ----------

ParseResult SkipString(const char *begin, const char *end, size_t offset, size_t maxLength);

ParseResult SkipNumber(const char *begin, const char *end, size_t offset);

ParseResult SkipValue(const char *begin, const char *end, size_t offset, int depth);

ParseResult ParseDeviceId(const char *begin, const char *end, size_t offset, uint32_t &value);

ParseResult ParseSingleRankAddr(const char *begin, const char *end, size_t offset, CandidateInfo &out);

Result ParseRootInfoEid(const char *begin, const char *end, uint32_t phyDeviceId, uint32_t rankId,
                        std::array<uint8_t, COMM_ADDR_EID_LEN> &eid);

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_ROOTINFO_PARSER_H
