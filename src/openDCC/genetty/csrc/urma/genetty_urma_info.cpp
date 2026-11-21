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
#include "genetty_urma_info.h"

#include <cstdlib>
#include <cstring>

#include "common/genetty_base64.h"
#include "common/genetty_kv_serdes.h"

namespace ock {
namespace openDCC {
namespace urma {
Result UrmaLinkInfo::Export(genetty_link_info_t *info, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Invalid param, info is null", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    /* step2: encode the eid to base64 text, so that it can be carried by a kv pair */
    std::string eidText;
    if (Base64::Encode(jettyId_.eid.raw, sizeof(urma_eid_t), eidText) != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as encode eid failed");
        return GEN_ERROR;
    }

    /* step3: put the member variables as kv pairs, one by one */
    KVWriter writer;
    if (writer.Put(GEN_INFO_VERSION, std::to_string(version_)) != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as put " << GEN_INFO_VERSION << " failed(" << writer.LastError()
                                                              << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(GEN_INFO_DEVICE_TYPE, std::to_string(static_cast<uint32_t>(type_))) != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as put " << GEN_INFO_DEVICE_TYPE << " failed("
                                                              << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(GEN_INFO_DEVICE_VENDOR, std::to_string(static_cast<uint32_t>(vendor_))) != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as put " << GEN_INFO_DEVICE_VENDOR << " failed("
                                                              << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(URMA_INFO_JETTY_EID, eidText) != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as put " << URMA_INFO_JETTY_EID << " failed("
                                                              << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(URMA_INFO_JETTY_UASID, std::to_string(jettyId_.uasid)) != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as put " << URMA_INFO_JETTY_UASID << " failed("
                                                              << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(URMA_INFO_JETTY_ID, std::to_string(jettyId_.id)) != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as put " << URMA_INFO_JETTY_ID << " failed("
                                                              << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(URMA_INFO_JETTY_TOKEN, std::to_string(token_)) != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as put " << URMA_INFO_JETTY_TOKEN << " failed("
                                                              << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    /* step4: serialize the kv pairs */
    if (writer.Serialize() != GEN_OK) {
        GEN_LOG_ERROR("Export urma link info failed, as serialize failed(" << writer.LastError() << ")");
        return GEN_ERROR;
    }

    /* step5: copy the serialized text into the output, the buffer is allocated here */
    const auto &text = writer.SerializedData();
    auto buffer = static_cast<char *>(malloc(text.size() + 1));
    if (buffer == nullptr) {
        GEN_LOG_ERROR("Export urma link info failed, as malloc " << text.size() << " bytes failed");
        return GEN_MALLOC_FAILED;
    }

    memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    info->data = buffer;
    info->length = static_cast<uint32_t>(text.size());

    GEN_LOG_DEBUG("Export urma link info finished, jetty id(" << jettyId_.id << "), token(" << token_ << ")");
    return GEN_OK;
}

Result UrmaLinkInfo::Import(genetty_link_info_t *info, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Invalid param, info is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(info->data != nullptr, "Invalid param, info data is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(info->length != 0, "Invalid param, info length is 0", GEN_INVALID_PARAM);
    (void)flags; /* the flags is reserved for now */

    /* step2: parse the kv pairs */
    KVReader reader(std::string(info->data, info->length));
    if (reader.Deserialize() != GEN_OK) {
        GEN_LOG_ERROR("Import urma link info failed, as deserialize failed(" << reader.LastError() << "), length("
                                                                             << info->length << ")");
        return GEN_INVALID_PARAM;
    }

    /* step3: get all the values and assign them only when all of them are valid */
    uint16_t version = 0;
    uint32_t type = 0;
    uint32_t vendor = 0;
    std::string eidText;
    uint32_t uasid = 0;
    uint32_t jettyId = 0;
    uint32_t token = 0;
    if (reader.GetUInt16(GEN_INFO_VERSION, version) != GEN_OK ||
        reader.GetUInt32(GEN_INFO_DEVICE_TYPE, type) != GEN_OK ||
        reader.GetUInt32(GEN_INFO_DEVICE_VENDOR, vendor) != GEN_OK ||
        reader.GetString(URMA_INFO_JETTY_EID, eidText) != GEN_OK ||
        reader.GetUInt32(URMA_INFO_JETTY_UASID, uasid) != GEN_OK ||
        reader.GetUInt32(URMA_INFO_JETTY_ID, jettyId) != GEN_OK ||
        reader.GetUInt32(URMA_INFO_JETTY_TOKEN, token) != GEN_OK) {
        GEN_LOG_ERROR("Import urma link info failed, as get kv failed(" << reader.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (type >= GENETTY_TYPE_COUNT) {
        GEN_LOG_ERROR("Import urma link info failed, as genetty type(" << type << ") is out of range");
        return GEN_INVALID_PARAM;
    }

    if (vendor > UINT8_MAX) {
        GEN_LOG_ERROR("Import urma link info failed, as vendor(" << vendor << ") is out of range");
        return GEN_INVALID_PARAM;
    }

    if (eidText.size() != Base64::EncodedLength(sizeof(urma_eid_t))) {
        GEN_LOG_ERROR("Import urma link info failed, as eid text length(" << eidText.size() << ") is invalid");
        return GEN_INVALID_PARAM;
    }

    /* step4: decode the eid from the base64 text */
    urma_jetty_id_t peerJettyId{};
    if (Base64::Decode(eidText, peerJettyId.eid.raw, sizeof(urma_eid_t)) != GEN_OK) {
        GEN_LOG_ERROR("Import urma link info failed, as decode eid failed, length(" << info->length << ")");
        return GEN_INVALID_PARAM;
    }

    /* step5: assign the values */
    version_ = version;
    type_ = static_cast<genetty_type_t>(type);
    vendor_ = static_cast<VendorType>(vendor);
    peerJettyId.uasid = uasid;
    peerJettyId.id = jettyId;
    jettyId_ = peerJettyId;
    token_ = token;

    GEN_LOG_DEBUG("Import urma link info finished, jetty id(" << jettyId << "), token(" << token << ")");
    return GEN_OK;
}
} // namespace urma
} // namespace openDCC
} // namespace ock
