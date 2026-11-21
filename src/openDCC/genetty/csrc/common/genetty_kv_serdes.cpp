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
#include "genetty_kv_serdes.h"

#include <exception>

#include "genetty_functions.h"

namespace ock {
namespace openDCC {
/**
 * Functions for KVReader
 */
Result KVReader::Deserialize() noexcept
{
    /* step1: clean old things */
    kv_.clear();
    lastErrorMsg_.clear();

    /* step2: parse one line by one line */
    size_t lineNo = 0;
    size_t begin = 0;
    while (begin <= data_.size()) {
        auto end = data_.find('\n', begin);
        auto length = (end == std::string::npos) ? std::string::npos : (end - begin);
        std::string line = data_.substr(begin, length);
        begin = (end == std::string::npos) ? (data_.size() + 1) : (end + 1);
        lineNo++;

        auto result = ParseLine(line, lineNo);
        if (result != GEN_OK) {
            /* do not keep the partial result, so that the reader can be reused */
            kv_.clear();
            return result;
        }
    }

    GEN_LOG_DEBUG("Deserialize kv finished, parsed " << lineNo << " lines, key count(" << kv_.size() << ")");
    return GEN_OK;
}

Result KVReader::ParseLine(const std::string &rawLine, size_t lineNo) noexcept
{
    /* step1: trim the line */
    std::string line = Func::Trim(rawLine);

    /* step2: skip the empty line and comment line */
    if (line.empty() || line[0] == '#') {
        return GEN_OK;
    }

    /* step3: check if there is = in the line */
    auto separator = line.find('=');
    if (separator == std::string::npos) {
        lastErrorMsg_ = "line " + std::to_string(lineNo) + ": missing '=' in '" + line + "'";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    /* step4: trim key and value and check if is empty*/
    std::string key = Func::Trim(line.substr(0, separator));
    std::string value = Func::Trim(line.substr(separator + 1));
    if (key.empty()) {
        lastErrorMsg_ = "line " + std::to_string(lineNo) + ": key is empty in '" + line + "'";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    /* the key must not contain blank, while the value can */
    if (key.find(' ') != std::string::npos) {
        lastErrorMsg_ = "line " + std::to_string(lineNo) + ": key '" + key + "' contains blank";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    /* step5: check if there is duplicated key */
    if (kv_.find(key) != kv_.end()) {
        lastErrorMsg_ = "line " + std::to_string(lineNo) + ": duplicated key '" + key + "'";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    kv_.emplace(key, value);
    return GEN_OK;
}

bool KVReader::ParseSigned(const std::string &text, int64_t minValue, int64_t maxValue, int64_t &value) noexcept
{
    if (text.empty()) {
        return false;
    }

    size_t pos = 0;
    int64_t parsed = 0;
    try {
        parsed = std::stoll(text, &pos);
    } catch (const std::exception &) {
        /* not a number, or out of the range of int64_t */
        return false;
    }

    if (pos != text.size() || parsed < minValue || parsed > maxValue) {
        return false;
    }

    value = parsed;
    return true;
}

bool KVReader::ParseUnsigned(const std::string &text, uint64_t maxValue, uint64_t &value) noexcept
{
    /* std::stoull() negates a leading '-' instead of rejecting it, so filter it out first */
    if (text.empty() || text[0] == '-') {
        return false;
    }

    size_t pos = 0;
    uint64_t parsed = 0;
    try {
        parsed = std::stoull(text, &pos);
    } catch (const std::exception &) {
        /* not a number, or out of the range of uint64_t */
        return false;
    }

    if (pos != text.size() || parsed > maxValue) {
        return false;
    }

    value = parsed;
    return true;
}

Result KVReader::GetSignedValue(const std::string &key, int64_t minValue, int64_t maxValue,
                                int64_t &value) const noexcept
{
    std::string text;
    if (GetRawValue(key, text) != GEN_OK) {
        return GEN_INVALID_PARAM;
    }

    if (!ParseSigned(text, minValue, maxValue, value)) {
        lastErrorMsg_ = "key(" + key + ") value(" + text + ") is not a valid signed integer";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    return GEN_OK;
}

Result KVReader::GetUnsignedValue(const std::string &key, uint64_t maxValue, uint64_t &value) const noexcept
{
    std::string text;
    if (GetRawValue(key, text) != GEN_OK) {
        return GEN_INVALID_PARAM;
    }

    if (!ParseUnsigned(text, maxValue, value)) {
        lastErrorMsg_ = "key(" + key + ") value(" + text + ") is not a valid unsigned integer";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    return GEN_OK;
}

Result KVReader::ParseReal(const std::string &key, const std::string &text, float &value) const noexcept
{
    size_t pos = 0;
    try {
        value = std::stof(text, &pos);
    } catch (const std::exception &e) {
        lastErrorMsg_ = "key(" + key + ") value(" + text + ") is not a valid float, " + e.what();
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    if (pos != text.size()) {
        lastErrorMsg_ = "key(" + key + ") value(" + text + ") has trailing character";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    return GEN_OK;
}

Result KVReader::ParseReal(const std::string &key, const std::string &text, double &value) const noexcept
{
    size_t pos = 0;
    try {
        value = std::stod(text, &pos);
    } catch (const std::exception &e) {
        lastErrorMsg_ = "key(" + key + ") value(" + text + ") is not a valid double, " + e.what();
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    if (pos != text.size()) {
        lastErrorMsg_ = "key(" + key + ") value(" + text + ") has trailing character";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    return GEN_OK;
}

/**
 * Functions for KVWriter
 */
Result KVWriter::Put(const std::string &key, const std::string &value) noexcept
{
    /* record every call, so that LastError() reflects the last operation */
    lastErrorMsg_.clear();

    /* trim first, so that the stored key and value can always round trip */
    std::string trimmedKey = Func::Trim(key);
    std::string trimmedValue = Func::Trim(value);

    if (trimmedKey.empty()) {
        lastErrorMsg_ = "key is empty";
        GEN_LOG_DEBUG("put kv failed, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    if (trimmedKey.find_first_of("=\r\n ") != std::string::npos || trimmedKey[0] == '#') {
        lastErrorMsg_ = "key(" + trimmedKey + ") contains invalid character";
        GEN_LOG_DEBUG("put kv failed, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    if (trimmedValue.find_first_of("\r\n") != std::string::npos) {
        lastErrorMsg_ = "key(" + trimmedKey + ") value contains line break";
        GEN_LOG_DEBUG("put kv failed, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    auto iter = kv_.find(trimmedKey);
    if (iter != kv_.end()) {
        GEN_LOG_INFO("There is already kv pair exist, key(" << trimmedKey << "), value(" << iter->second
                                                           << "), it will be replaced with key(" << trimmedKey
                                                           << "), value(" << trimmedValue << ")");
        iter->second = trimmedValue;
        return GEN_OK;
    }

    kv_[trimmedKey] = trimmedValue;
    return GEN_OK;
}

/**
 * Functions for stream output
 */
std::ostream &operator<<(std::ostream &os, const KVReader &reader) noexcept
{
    os << "kv reader [key count: " << reader.kv_.size();
    for (const auto &item : reader.kv_) {
        os << ", " << item.first << " = " << item.second;
    }

    os << "; data: " << reader.data_;

    if (!reader.lastErrorMsg_.empty()) {
        os << ", last error: " << reader.lastErrorMsg_;
    }

    os << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const KVWriter &writer) noexcept
{
    os << "kv writer [key count: " << writer.kv_.size() << ", data: '" << writer.data_ << "'";
    if (!writer.lastErrorMsg_.empty()) {
        os << ", last error: " << writer.lastErrorMsg_;
    }

    os << "]";
    return os;
}
} // namespace openDCC
} // namespace ock
