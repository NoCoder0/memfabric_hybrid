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
#include "genetty_verbs_info.h"

#include <arpa/inet.h>

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "common/genetty_base64.h"
#include "common/genetty_kv_serdes.h"
#include "genetty_types.h"

namespace ock {
namespace openDCC {
namespace verbs {
namespace {
std::string FormatGid(const uint8_t *gid)
{
    char buf[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, gid, buf, sizeof(buf)) == nullptr) {
        return "invalid";
    }

    return buf;
}

/* decode the gid from base64 text, the text must carry exactly ROCE_GID_LEN bytes */
Result DecodeGid(const std::string &text, uint32_t qpn, uint8_t *gid) noexcept
{
    if (text.size() != Base64::EncodedLength(ROCE_GID_LEN)) {
        GEN_LOG_ERROR("Decode gid failed, as text length(" << text.size() << ") is invalid, qpn(" << qpn << ")");
        return GEN_INVALID_PARAM;
    }

    if (Base64::Decode(text, gid, ROCE_GID_LEN) != GEN_OK) {
        GEN_LOG_ERROR("Decode gid failed, as base64 decode failed, qpn(" << qpn << ")");
        return GEN_INVALID_PARAM;
    }

    return GEN_OK;
}
} // namespace

Result VerbsLinkInfo::Export(genetty_link_info_t *info, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Invalid param, info is null", GEN_INVALID_PARAM);

    /* step2: encode the gid to base64 text, so that it can be carried by a kv pair */
    std::string gidText;
    if (Base64::Encode(Gid(), ROCE_GID_LEN, gidText) != GEN_OK ||
        gidText.size() != Base64::EncodedLength(ROCE_GID_LEN)) {
        GEN_LOG_ERROR("Export failed, as encode gid failed");
        return GEN_ERROR;
    }

    /* step3: put the member variables as kv pairs, one by one */
    KVWriter writer;
    if (writer.Put(GEN_INFO_VERSION, std::to_string(version_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << GEN_INFO_VERSION << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(GEN_INFO_DEVICE_TYPE, std::to_string(static_cast<uint32_t>(type_))) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << GEN_INFO_DEVICE_TYPE << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(GEN_INFO_DEVICE_VENDOR, std::to_string(static_cast<uint32_t>(vendor_))) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << GEN_INFO_DEVICE_VENDOR << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(VERBS_INFO_QP_QPN, std::to_string(qpn_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << VERBS_INFO_QP_QPN << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(VERBS_INFO_QP_LID, std::to_string(lid_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << VERBS_INFO_QP_LID << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(VERBS_INFO_QP_PORT, std::to_string(static_cast<uint32_t>(portNum_))) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << VERBS_INFO_QP_PORT << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(VERBS_INFO_QP_GID, gidText) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << VERBS_INFO_QP_GID << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(VERBS_INFO_QP_SQ_DEPTH, std::to_string(sq_depth_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << VERBS_INFO_QP_SQ_DEPTH << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(VERBS_INFO_QP_RQ_DEPTH, std::to_string(rq_depth_)) != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as put " << VERBS_INFO_QP_RQ_DEPTH << " failed(" << writer.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (writer.Put(VERBS_INFO_QP_PSN, std::to_string(psn_)) != GEN_OK ||
        writer.Put(VERBS_INFO_QP_MTU, std::to_string(mtu_)) != GEN_OK) {
        GEN_LOG_ERROR("Export PSN/MTU failed");
        return GEN_INVALID_PARAM;
    }

    /* step4: serialize the kv pairs */
    if (writer.Serialize() != GEN_OK) {
        GEN_LOG_ERROR("Export failed, as serialize failed(" << writer.LastError() << ")");
        return GEN_ERROR;
    }

    /* step5: copy the serialized text into the output, the buffer is allocated here */
    auto const text = writer.SerializedData();
    auto buffer = static_cast<char *>(malloc(text.size() + 1));
    if (buffer == nullptr) {
        GEN_LOG_ERROR("Export failed, as malloc " << text.size() << " bytes failed, probably out of memory");
        return GEN_MALLOC_FAILED;
    }

    (void)memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    info->data = buffer;
    info->length = static_cast<uint32_t>(text.size());

    GEN_LOG_DEBUG("Export info finished successfully, " << *this);
    return GEN_OK;
}

Result VerbsLinkInfo::Import(genetty_link_info_t *info, uint32_t flags) noexcept
{
    /* step1: check the parameters */
    if (info == nullptr || info->data == nullptr || info->length == 0) {
        GEN_LOG_ERROR("Import verbs exchange info failed, as info or its data is invalid");
        return GEN_INVALID_PARAM;
    }

    /* step2: parse the kv pairs */
    KVReader reader(std::string(info->data, info->length));
    if (reader.Deserialize() != GEN_OK) {
        GEN_LOG_ERROR("Import verbs exchange info failed, as deserialize failed(" << reader.LastError() << "), length("
                                                                                  << info->length << ")");
        return GEN_INVALID_PARAM;
    }

    /* step3: get the member variables except the gid */
    uint16_t version = 0;
    uint32_t type = 0;
    uint32_t vendor = 0;
    uint32_t qpn = 0;
    uint32_t psn = 0;
    uint32_t mtu = 0;
    uint16_t lid = 0;
    uint32_t portNum = 0;
    uint32_t sqDepth = 0;
    uint32_t rqDepth = 0;
    std::string gidText;
    if (reader.GetUInt32(VERBS_INFO_QP_PSN, psn) != GEN_OK || reader.GetUInt32(VERBS_INFO_QP_MTU, mtu) != GEN_OK ||
        reader.GetUInt16(GEN_INFO_VERSION, version) != GEN_OK ||
        reader.GetUInt32(GEN_INFO_DEVICE_TYPE, type) != GEN_OK ||
        reader.GetUInt32(GEN_INFO_DEVICE_VENDOR, vendor) != GEN_OK ||
        reader.GetUInt32(VERBS_INFO_QP_QPN, qpn) != GEN_OK || reader.GetUInt16(VERBS_INFO_QP_LID, lid) != GEN_OK ||
        reader.GetUInt32(VERBS_INFO_QP_PORT, portNum) != GEN_OK ||
        reader.GetUInt32(VERBS_INFO_QP_SQ_DEPTH, sqDepth) != GEN_OK ||
        reader.GetUInt32(VERBS_INFO_QP_RQ_DEPTH, rqDepth) != GEN_OK ||
        reader.GetString(VERBS_INFO_QP_GID, gidText) != GEN_OK) {
        GEN_LOG_ERROR("Import verbs exchange info failed, as get kv failed(" << reader.LastError() << ")");
        return GEN_INVALID_PARAM;
    }

    if (type >= GENETTY_TYPE_COUNT) {
        GEN_LOG_ERROR("Import verbs exchange info failed, as genetty type(" << type << ") is out of range");
        return GEN_INVALID_PARAM;
    }

    if (vendor > UINT8_MAX) {
        GEN_LOG_ERROR("Import verbs exchange info failed, as vendor(" << vendor << ") is out of range");
        return GEN_INVALID_PARAM;
    }

    if (portNum > UINT8_MAX) {
        GEN_LOG_ERROR("Import verbs exchange info failed, as port number(" << portNum << ") is out of range");
        return GEN_INVALID_PARAM;
    }

    constexpr uint32_t MAX_PSN = 0xffffff;
    constexpr uint32_t MAX_MTU_ENUM = 5;
    GEN_ASSERT_MSG_RETURN(psn <= MAX_PSN && mtu <= MAX_MTU_ENUM,
                          "Import PSN/MTU failed, psn(" << psn << "), mtu(" << mtu << ")", GEN_INVALID_PARAM);

    /* step4: decode the gid from base64 text */
    uint8_t gid[ROCE_GID_LEN] = {};
    if (DecodeGid(gidText, qpn, gid) != GEN_OK) {
        GEN_LOG_ERROR("Import verbs exchange info failed, as decode gid failed, qpn(" << qpn << ")");
        return GEN_INVALID_PARAM;
    }

    /* step5: assign the member variables only when all of them are valid */
    Version(version);
    Type(static_cast<genetty_type_t>(type));
    Vendor(static_cast<VendorType>(vendor));
    Qpn(qpn);
    Psn(psn);
    Mtu(mtu);
    Lid(lid);
    PortNum(static_cast<uint8_t>(portNum));
    SqDepth(sqDepth);
    RqDepth(rqDepth);
    Gid(gid);

    GEN_LOG_DEBUG("Import verbs exchange info finished, " << *this);
    return GEN_OK;
}

/**
 * Functions for verbs info output
 */

std::ostream &operator<<(std::ostream &os, const VerbsLinkInfo &info)
{
    os << "verbs exchange info [version: " << info.Version() << ", qpn: " << info.Qpn() << ", lid: " << info.Lid()
       << ", psn: " << info.Psn() << ", mtu: " << info.Mtu() << ", port: " << static_cast<uint32_t>(info.PortNum())
       << ", sq depth: " << info.SqDepth() << ", rq depth: " << info.RqDepth() << ", gid: " << FormatGid(info.Gid())
       << "]";
    return os;
}
} // namespace verbs
} // namespace openDCC
} // namespace ock
