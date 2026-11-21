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
#ifndef MF_OPENDCC_GENETTY_KV_SERDES_H
#define MF_OPENDCC_GENETTY_KV_SERDES_H

#include <cstddef>
#include <cstdint>
#include <exception>
#include <ostream>
#include <string>

#include "genetty_def.h"
#include "genetty_define.h"
#include "genetty_logger.h"

#include <map>

namespace ock {
namespace openDCC {
/*
 * KV format, which is most likely same with the format of .ini file
 *
 * # comments
 * ock.openDCC.genetty.version = 0
 *
 * ock.openDCC.a = 1
 * ock.openDDC.b = haha
 *
 */
class KVReader {
public:
    explicit KVReader(const std::string &data) noexcept : data_(data) {}
    ~KVReader() noexcept = default;

    Result Deserialize() noexcept;
    const std::string &LastError() const noexcept;

    Result GetInt16(const std::string &key, int16_t &value) noexcept;
    Result GetUInt16(const std::string &key, uint16_t &value) noexcept;
    Result GetInt32(const std::string &key, int32_t &value) const noexcept;
    Result GetUInt32(const std::string &key, uint32_t &value) const noexcept;
    Result GetInt64(const std::string &key, int64_t &value) const noexcept;
    Result GetUInt64(const std::string &key, uint64_t &value) const noexcept;

    Result GetFloat(const std::string &key, float &value) const noexcept;
    Result GetDouble(const std::string &key, double &value) const noexcept;

    Result GetString(const std::string &key, std::string &value) const noexcept;

private:
    Result ParseLine(const std::string &rawLine, size_t lineNo) noexcept;
    Result GetRawValue(const std::string &key, std::string &value) const noexcept;
    Result GetSignedValue(const std::string &key, int64_t minValue, int64_t maxValue, int64_t &value) const noexcept;
    Result GetUnsignedValue(const std::string &key, uint64_t maxValue, uint64_t &value) const noexcept;
    static bool ParseSigned(const std::string &text, int64_t minValue, int64_t maxValue, int64_t &value) noexcept;
    static bool ParseUnsigned(const std::string &text, uint64_t maxValue, uint64_t &value) noexcept;
    Result ParseReal(const std::string &key, const std::string &text, float &value) const noexcept;
    Result ParseReal(const std::string &key, const std::string &text, double &value) const noexcept;

private:
    friend std::ostream &operator<<(std::ostream &os, const KVReader &reader) noexcept;

    std::string data_;                      /* data to be serialized */
    mutable std::string lastErrorMsg_;      /* the message of the last error */
    std::map<std::string, std::string> kv_; /* deserialized key and value without type */
};

ALWAYS_INLINE const std::string &KVReader::LastError() const noexcept
{
    return lastErrorMsg_;
}

ALWAYS_INLINE Result KVReader::GetRawValue(const std::string &key, std::string &value) const noexcept
{
    auto iter = kv_.find(key);
    if (iter == kv_.end()) {
        lastErrorMsg_ = "key(" + key + ") is not found";
        GEN_LOG_DEBUG("kv error, " << lastErrorMsg_);
        return GEN_INVALID_PARAM;
    }

    value = iter->second;
    return GEN_OK;
}

ALWAYS_INLINE Result KVReader::GetInt16(const std::string &key, int16_t &value) noexcept
{
    int64_t parsed = 0;
    if (GetSignedValue(key, INT16_MIN, INT16_MAX, parsed) != GEN_OK) {
        return GEN_INVALID_PARAM;
    }

    value = static_cast<int16_t>(parsed);
    return GEN_OK;
}

ALWAYS_INLINE Result KVReader::GetUInt16(const std::string &key, uint16_t &value) noexcept
{
    uint64_t parsed = 0;
    if (GetUnsignedValue(key, UINT16_MAX, parsed) != GEN_OK) {
        return GEN_INVALID_PARAM;
    }

    value = static_cast<uint16_t>(parsed);
    return GEN_OK;
}

ALWAYS_INLINE Result KVReader::GetInt32(const std::string &key, int32_t &value) const noexcept
{
    int64_t parsed = 0;
    if (GetSignedValue(key, INT32_MIN, INT32_MAX, parsed) != GEN_OK) {
        return GEN_INVALID_PARAM;
    }

    value = static_cast<int32_t>(parsed);
    return GEN_OK;
}

ALWAYS_INLINE Result KVReader::GetUInt32(const std::string &key, uint32_t &value) const noexcept
{
    uint64_t parsed = 0;
    if (GetUnsignedValue(key, UINT32_MAX, parsed) != GEN_OK) {
        return GEN_INVALID_PARAM;
    }

    value = static_cast<uint32_t>(parsed);
    return GEN_OK;
}

ALWAYS_INLINE Result KVReader::GetInt64(const std::string &key, int64_t &value) const noexcept
{
    return GetSignedValue(key, INT64_MIN, INT64_MAX, value);
}

ALWAYS_INLINE Result KVReader::GetUInt64(const std::string &key, uint64_t &value) const noexcept
{
    return GetUnsignedValue(key, UINT64_MAX, value);
}

ALWAYS_INLINE Result KVReader::GetFloat(const std::string &key, float &value) const noexcept
{
    std::string text;
    if (GetRawValue(key, text) != GEN_OK) {
        return GEN_INVALID_PARAM;
    }

    return ParseReal(key, text, value);
}

ALWAYS_INLINE Result KVReader::GetDouble(const std::string &key, double &value) const noexcept
{
    std::string text;
    if (GetRawValue(key, text) != GEN_OK) {
        return GEN_INVALID_PARAM;
    }

    return ParseReal(key, text, value);
}

ALWAYS_INLINE Result KVReader::GetString(const std::string &key, std::string &value) const noexcept
{
    return GetRawValue(key, value);
}

class KVWriter {
public:
    KVWriter() noexcept = default;
    ~KVWriter() noexcept = default;

    Result Put(const std::string &key, const std::string &value) noexcept;

    Result Serialize() noexcept;
    
    const std::string &SerializedData() const noexcept;

    const std::string &LastError() const noexcept;

private:
    friend std::ostream &operator<<(std::ostream &os, const KVWriter &writer) noexcept;

    std::string data_;                      /* serialized data */
    std::string lastErrorMsg_;              /* the message of the last error */
    std::map<std::string, std::string> kv_; /* the key and value to be serialized */
};

ALWAYS_INLINE Result KVWriter::Serialize() noexcept
{
    lastErrorMsg_.clear();
    data_.clear();

    for (const auto &item : kv_) {
        data_ += item.first + " = " + item.second + '\n';
    }

    GEN_LOG_DEBUG("Serialize kv finished, key count(" << kv_.size() << "), data length(" << data_.size() << ")");
    return GEN_OK;
}

ALWAYS_INLINE const std::string &KVWriter::SerializedData() const noexcept
{
    return data_;
}

ALWAYS_INLINE const std::string &KVWriter::LastError() const noexcept
{
    return lastErrorMsg_;
}
} // namespace openDCC
} // namespace ock

#endif //MF_OPENDCC_GENETTY_KV_SERDES_H
