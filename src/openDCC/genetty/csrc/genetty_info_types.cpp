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
#include "genetty_info_types.h"

#include <cstdlib>
#include <cstring>

#include "common/genetty_kv_serdes.h"

namespace ock {
namespace openDCC {
Result MemoryInfo::Export(genetty_memory_info_t *info, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Invalid param, info is null", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    /* step2: put the member variables as kv pairs, one by one */
    KVWriter writer;
    if (writer.Put(GEN_INFO_VERSION, std::to_string(version_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << GEN_INFO_VERSION << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(GEN_INFO_MEM_TYPE, std::to_string(static_cast<uint32_t>(type_))) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << GEN_INFO_MEM_TYPE << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(GEN_INFO_MEM_ADDR, std::to_string(address_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << GEN_INFO_MEM_ADDR << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(GEN_INFO_MEM_SIZE, std::to_string(size_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << GEN_INFO_MEM_SIZE << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(GEN_INFO_MEM_RKEY, std::to_string(rkey_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << GEN_INFO_MEM_RKEY << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    /* step3: serialize the kv pairs */
    if (writer.Serialize() != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as serialize failed(" << writer.LastError() << ")");
        return GEN_ERROR;
    }

    /* step4: copy the serialized text into the output, the buffer is allocated here */
    const auto &text = writer.SerializedData();
    auto buffer = static_cast<char *>(malloc(text.size() + 1));
    if (buffer == nullptr) {
        GEN_LOG_ERROR("Export failed, as malloc " << text.size() << " bytes failed, probably out of memory");
        return GEN_MALLOC_FAILED;
    }

    memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    info->data = buffer;
    info->length = static_cast<uint32_t>(text.size());

    GEN_LOG_DEBUG("Export memory info finished, mem type(" << type_ << "), address(" << address_ << "), size("
                                                           << size_ << "), key(" << rkey_ << ")");
    return GEN_OK;
}

Result MemoryInfo::Import(genetty_memory_info_t *info, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Invalid param, info is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(info->data != nullptr, "Invalid param, info->data is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(info->length != 0, "Invalid param, info->length is 0", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    /* step2: parse the kv pairs */
    KVReader reader(std::string(info->data, info->length));
    if (reader.Deserialize() != GEN_OK) {
        GEN_LOG_ERROR("Import info failed, as deserialize failed(" << reader.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    /* step3: get all the values and assign them only when all of them are valid */
    uint16_t version = 0;
    uint32_t memType = 0;
    uint64_t address = 0;
    uint64_t size = 0;
    uint32_t key = 0;
    if (reader.GetUInt16(GEN_INFO_VERSION, version) != GEN_OK ||
        reader.GetUInt32(GEN_INFO_MEM_TYPE, memType) != GEN_OK ||
        reader.GetUInt64(GEN_INFO_MEM_ADDR, address) != GEN_OK || reader.GetUInt64(GEN_INFO_MEM_SIZE, size) != GEN_OK ||
        reader.GetUInt32(GEN_INFO_MEM_RKEY, key) != GEN_OK) {
        GEN_LOG_ERROR("Import failed, as get kv failed(" << reader.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (memType >= MT_COUNT) {
        GEN_LOG_ERROR("Import failed, as device type(" << memType << ") is out of range");
        return GEN_INVALID_PARAM;
    }

    /* step4: assign the values */
    version_ = version;
    type_ = static_cast<genetty_mem_type_t>(memType);
    address_ = address;
    size_ = size;
    rkey_ = key;

    GEN_LOG_DEBUG("Import memory info finished, mem type(" << type_ << "), address(" << address_ << "), size("
                                                           << size_ << "), key(" << rkey_ << ")");
    return GEN_OK;
}

} // namespace openDCC
} // namespace ock
